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
// Tests the HDF5 pass-through VOL connector (third_party/hdf5/src/H5VLpassthru.c)
// across info/wrap management, native VOL forwarding, and asynchronous/blob/
// optional/token forwarding over a mock underlying VOL connector.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLpassthru.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

struct MockRequest {
  bool fail_wait = false;
  bool in_progress = false;
  bool notified = false;
  bool canceled = false;
  int specific_calls = 0;
  int optional_calls = 0;
};

struct MockWrapCtx {
  int tag = 77;
};

struct MockObject {
  int kind = 0;
};

struct MockVolState {
  bool fail_ops = false;
  bool emit_req = false;
  bool return_in_progress_once = false;
  bool fail_wrap = false;
  bool fail_unwrap = false;

  int attr_read_calls = 0;
  int attr_write_calls = 0;
  int attr_get_calls = 0;
  int attr_specific_calls = 0;
  int attr_optional_calls = 0;
  int dset_read_calls = 0;
  size_t last_dset_read_count = 0;
  int dset_write_calls = 0;
  size_t last_dset_write_count = 0;
  int dset_get_calls = 0;
  int dset_specific_calls = 0;
  int dset_optional_calls = 0;
  int dt_get_calls = 0;
  int dt_specific_calls = 0;
  int dt_optional_calls = 0;
  int file_get_calls = 0;
  int file_specific_calls = 0;
  bool saw_reopen = false;
  int file_optional_calls = 0;
  int group_get_calls = 0;
  int group_specific_calls = 0;
  void* last_mount_child = nullptr;
  int group_optional_calls = 0;
  int link_create_calls = 0;
  void* last_hard_curr_obj = nullptr;
  int link_copy_calls = 0;
  void* last_link_copy_src = nullptr;
  void* last_link_copy_dst = nullptr;
  int link_move_calls = 0;
  void* last_link_move_src = nullptr;
  void* last_link_move_dst = nullptr;
  int link_get_calls = 0;
  int link_specific_calls = 0;
  int link_optional_calls = 0;
  int obj_copy_calls = 0;
  int obj_get_calls = 0;
  int obj_specific_calls = 0;
  int obj_optional_calls = 0;
  int introspect_opt_calls = 0;
  int blob_put_calls = 0;
  int blob_get_calls = 0;
  int blob_specific_calls = 0;
  int blob_optional_calls = 0;
  int token_cmp_calls = 0;
  int token_to_str_calls = 0;
  int token_from_str_calls = 0;
  int generic_optional_calls = 0;
};

MockVolState g_mock_state{};
const H5VL_class_t* g_mock_cls_ptr = nullptr;

void MaybeEmitReq(void** req) {
  if (req != nullptr && g_mock_state.emit_req) {
    auto* r = new MockRequest{};
    r->in_progress = g_mock_state.return_in_progress_once;
    *req = r;
  }
}

void* MockGetObject(const void* obj) {
  return const_cast<void*>(obj);
}

herr_t MockGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  *wrap_ctx = new MockWrapCtx{};
  return 0;
}

void* MockWrapObject(void* obj, H5I_type_t /*obj_type*/, void* /*wrap_ctx*/) {
  if (g_mock_state.fail_wrap) return nullptr;
  return obj;
}

void* MockUnwrapObject(void* obj) {
  if (g_mock_state.fail_unwrap) return nullptr;
  return obj;
}

herr_t MockFreeWrapCtx(void* wrap_ctx) {
  delete static_cast<MockWrapCtx*>(wrap_ctx);
  return 0;
}

void* MockAttrCreate(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     const char* /*name*/, hid_t /*type_id*/, hid_t /*space_id*/,
                     hid_t /*acpl_id*/, hid_t /*aapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{1};
}

void* MockAttrOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                   const char* /*name*/, hid_t /*aapl_id*/, hid_t /*dxpl_id*/,
                   void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{2};
}

herr_t MockAttrRead(void* /*attr*/, hid_t /*mem_type_id*/, void* /*buf*/,
                    hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.attr_read_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockAttrWrite(void* /*attr*/, hid_t /*mem_type_id*/, const void* /*buf*/,
                     hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.attr_write_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockAttrGet(void* /*obj*/, H5VL_attr_get_args_t* /*args*/,
                   hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.attr_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockAttrSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_attr_specific_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** req) {
  ++g_mock_state.attr_specific_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockAttrOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                        hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.attr_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockAttrClose(void* attr, hid_t /*dxpl_id*/, void** req) {
  MaybeEmitReq(req);
  delete static_cast<MockObject*>(attr);
  return 0;
}

void* MockDatasetCreate(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        const char* /*name*/, hid_t /*lcpl_id*/,
                        hid_t /*type_id*/, hid_t /*space_id*/,
                        hid_t /*dcpl_id*/, hid_t /*dapl_id*/, hid_t /*dxpl_id*/,
                        void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{3};
}

void* MockDatasetOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                      const char* /*name*/, hid_t /*dapl_id*/,
                      hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{4};
}

herr_t MockDatasetRead(size_t count, void* /*dset*/[], hid_t /*mem_type_id*/[],
                       hid_t /*mem_space_id*/[], hid_t /*file_space_id*/[],
                       hid_t /*dxpl_id*/, void* /*buf*/[], void** req) {
  ++g_mock_state.dset_read_calls;
  g_mock_state.last_dset_read_count = count;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatasetWrite(size_t count, void* /*dset*/[], hid_t /*mem_type_id*/[],
                        hid_t /*mem_space_id*/[], hid_t /*file_space_id*/[],
                        hid_t /*dxpl_id*/, const void* /*buf*/[], void** req) {
  ++g_mock_state.dset_write_calls;
  g_mock_state.last_dset_write_count = count;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatasetGet(void* /*obj*/, H5VL_dataset_get_args_t* /*args*/,
                      hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dset_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatasetSpecific(void* /*obj*/, H5VL_dataset_specific_args_t* /*args*/,
                           hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dset_specific_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatasetOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                           hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dset_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatasetClose(void* dset, hid_t /*dxpl_id*/, void** req) {
  MaybeEmitReq(req);
  delete static_cast<MockObject*>(dset);
  return 0;
}

void* MockDatatypeCommit(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                         const char* /*name*/, hid_t /*type_id*/,
                         hid_t /*lcpl_id*/, hid_t /*tcpl_id*/, hid_t /*tapl_id*/,
                         hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{5};
}

void* MockDatatypeOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                       const char* /*name*/, hid_t /*tapl_id*/,
                       hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{6};
}

herr_t MockDatatypeGet(void* /*obj*/, H5VL_datatype_get_args_t* /*args*/,
                       hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dt_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatatypeSpecific(void* /*obj*/,
                            H5VL_datatype_specific_args_t* /*args*/,
                            hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dt_specific_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatatypeOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                            hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.dt_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockDatatypeClose(void* dt, hid_t /*dxpl_id*/, void** req) {
  MaybeEmitReq(req);
  delete static_cast<MockObject*>(dt);
  return 0;
}

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{7};
}

void* MockFileOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                   hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{8};
}

herr_t MockFileGet(void* /*obj*/, H5VL_file_get_args_t* args, hid_t /*dxpl_id*/,
                   void** req) {
  ++g_mock_state.file_get_calls;
  if (args->op_type == H5VL_FILE_GET_CONT_INFO &&
      args->args.get_cont_info.info != nullptr) {
    args->args.get_cont_info.info->version = H5VL_CONTAINER_INFO_VERSION;
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = 8;
    args->args.get_cont_info.info->blob_id_size = 8;
  }
  MaybeEmitReq(req);
  return 0;
}

herr_t MockFileSpecific(void* /*obj*/, H5VL_file_specific_args_t* args,
                        hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.file_specific_calls;
  if (args->op_type == H5VL_FILE_REOPEN) {
    g_mock_state.saw_reopen = true;
    *args->args.reopen.file =
        g_mock_state.fail_ops ? nullptr : new MockObject{88};
  }
  MaybeEmitReq(req);
  return 0;
}

herr_t MockFileOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                        hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.file_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** req) {
  MaybeEmitReq(req);
  delete static_cast<MockObject*>(file);
  return 0;
}

void* MockGroupCreate(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                      const char* /*name*/, hid_t /*lcpl_id*/, hid_t /*gcpl_id*/,
                      hid_t /*gapl_id*/, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{9};
}

void* MockGroupOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                    const char* /*name*/, hid_t /*gapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  MaybeEmitReq(req);
  return new MockObject{10};
}

herr_t MockGroupGet(void* /*obj*/, H5VL_group_get_args_t* /*args*/,
                    hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.group_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockGroupSpecific(void* /*obj*/, H5VL_group_specific_args_t* args,
                         hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.group_specific_calls;
  if (args->op_type == H5VL_GROUP_MOUNT) {
    g_mock_state.last_mount_child = args->args.mount.child_file;
  }
  MaybeEmitReq(req);
  return 0;
}

herr_t MockGroupOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                         hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.group_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockGroupClose(void* grp, hid_t /*dxpl_id*/, void** req) {
  MaybeEmitReq(req);
  delete static_cast<MockObject*>(grp);
  return 0;
}

herr_t MockLinkCreate(H5VL_link_create_args_t* args, void* /*obj*/,
                      const H5VL_loc_params_t* /*loc_params*/, hid_t /*lcpl_id*/,
                      hid_t /*lapl_id*/, hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.link_create_calls;
  if (args->op_type == H5VL_LINK_CREATE_HARD) {
    g_mock_state.last_hard_curr_obj = args->args.hard.curr_obj;
  }
  MaybeEmitReq(req);
  return 0;
}

herr_t MockLinkCopy(void* src_obj, const H5VL_loc_params_t* /*loc_params1*/,
                    void* dst_obj, const H5VL_loc_params_t* /*loc_params2*/,
                    hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  ++g_mock_state.link_copy_calls;
  g_mock_state.last_link_copy_src = src_obj;
  g_mock_state.last_link_copy_dst = dst_obj;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockLinkMove(void* src_obj, const H5VL_loc_params_t* /*loc_params1*/,
                    void* dst_obj, const H5VL_loc_params_t* /*loc_params2*/,
                    hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  ++g_mock_state.link_move_calls;
  g_mock_state.last_link_move_src = src_obj;
  g_mock_state.last_link_move_dst = dst_obj;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockLinkGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                   H5VL_link_get_args_t* /*args*/, hid_t /*dxpl_id*/,
                   void** req) {
  ++g_mock_state.link_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockLinkSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_link_specific_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** req) {
  ++g_mock_state.link_specific_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockLinkOptional(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_optional_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** req) {
  ++g_mock_state.link_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

void* MockObjectOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5I_type_t* opened_type, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_state.fail_ops) return nullptr;
  if (opened_type) *opened_type = H5I_GROUP;
  MaybeEmitReq(req);
  return new MockObject{11};
}

herr_t MockObjectCopy(void* /*src_obj*/,
                      const H5VL_loc_params_t* /*src_loc_params*/,
                      const char* /*src_name*/, void* /*dst_obj*/,
                      const H5VL_loc_params_t* /*dst_loc_params*/,
                      const char* /*dst_name*/, hid_t /*ocpypl_id*/,
                      hid_t /*lcpl_id*/, hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.obj_copy_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockObjectGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5VL_object_get_args_t* /*args*/, hid_t /*dxpl_id*/,
                     void** req) {
  ++g_mock_state.obj_get_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockObjectSpecific(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_object_specific_args_t* /*args*/,
                          hid_t /*dxpl_id*/, void** req) {
  ++g_mock_state.obj_specific_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockObjectOptional(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_optional_args_t* /*args*/, hid_t /*dxpl_id*/,
                          void** req) {
  ++g_mock_state.obj_optional_calls;
  MaybeEmitReq(req);
  return 0;
}

herr_t MockIntrospectGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                const H5VL_class_t** conn_cls) {
  *conn_cls = g_mock_cls_ptr;
  return 0;
}

herr_t MockIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  *cap_flags = H5VL_CAP_FLAG_ASYNC | H5VL_CAP_FLAG_THREADSAFE;
  return 0;
}

herr_t MockIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/,
                              int /*opt_type*/, uint64_t* flags) {
  ++g_mock_state.introspect_opt_calls;
  *flags = H5VL_OPT_QUERY_SUPPORTED;
  return 0;
}

herr_t MockRequestWait(void* req, uint64_t /*timeout*/,
                       H5VL_request_status_t* status) {
  auto* r = static_cast<MockRequest*>(req);
  if (r->in_progress) {
    r->in_progress = false;
    *status = H5VL_REQUEST_STATUS_IN_PROGRESS;
    return 0;
  }
  *status = H5VL_REQUEST_STATUS_SUCCEED;
  delete r;
  return 0;
}

herr_t MockRequestNotify(void* req, H5VL_request_notify_t cb, void* ctx) {
  auto* r = static_cast<MockRequest*>(req);
  r->notified = true;
  if (cb) {
    cb(ctx, H5VL_REQUEST_STATUS_SUCCEED);
  }
  delete r;
  return 0;
}

herr_t MockRequestCancel(void* req, H5VL_request_status_t* status) {
  auto* r = static_cast<MockRequest*>(req);
  r->canceled = true;
  *status = H5VL_REQUEST_STATUS_CANCELED;
  delete r;
  return 0;
}

herr_t MockRequestSpecific(void* req, H5VL_request_specific_args_t* args) {
  auto* r = static_cast<MockRequest*>(req);
  ++r->specific_calls;
  if (args && args->op_type == H5VL_REQUEST_GET_EXEC_TIME) {
    if (args->args.get_exec_time.exec_ts) {
      *args->args.get_exec_time.exec_ts = 100;
    }
    if (args->args.get_exec_time.exec_time) {
      *args->args.get_exec_time.exec_time = 25;
    }
  }
  return 0;
}

herr_t MockRequestOptional(void* req, H5VL_optional_args_t* /*args*/) {
  auto* r = static_cast<MockRequest*>(req);
  ++r->optional_calls;
  return 0;
}

herr_t MockRequestFree(void* req) {
  delete static_cast<MockRequest*>(req);
  return 0;
}

herr_t MockBlobPut(void* /*obj*/, const void* /*buf*/, size_t /*size*/,
                   void* blob_id, void* /*ctx*/) {
  ++g_mock_state.blob_put_calls;
  if (blob_id) {
    static_cast<uint8_t*>(blob_id)[0] = 0xAB;
  }
  return 0;
}

herr_t MockBlobGet(void* /*obj*/, const void* /*blob_id*/, void* buf,
                   size_t size, void* /*ctx*/) {
  ++g_mock_state.blob_get_calls;
  if (buf && size > 0) {
    std::memset(buf, 0x5A, size);
  }
  return 0;
}

herr_t MockBlobSpecific(void* /*obj*/, void* /*blob_id*/,
                        H5VL_blob_specific_args_t* /*args*/) {
  ++g_mock_state.blob_specific_calls;
  return 0;
}

herr_t MockBlobOptional(void* /*obj*/, void* /*blob_id*/,
                        H5VL_optional_args_t* /*args*/) {
  ++g_mock_state.blob_optional_calls;
  return 0;
}

herr_t MockTokenCmp(void* /*obj*/, const H5O_token_t* token1,
                    const H5O_token_t* token2, int* cmp_value) {
  ++g_mock_state.token_cmp_calls;
  *cmp_value = std::memcmp(token1, token2, sizeof(H5O_token_t));
  return 0;
}

herr_t MockTokenToStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                      const H5O_token_t* /*token*/, char** token_str) {
  ++g_mock_state.token_to_str_calls;
  char* out = static_cast<char*>(H5allocate_memory(5, false));
  std::memcpy(out, "tok0", 5);
  *token_str = out;
  return 0;
}

herr_t MockTokenFromStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                        const char* /*token_str*/, H5O_token_t* token) {
  ++g_mock_state.token_from_str_calls;
  std::memset(token, 0, sizeof(H5O_token_t));
  token->__data[0] = 0x42;
  return 0;
}

herr_t MockGenericOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                           hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_mock_state.generic_optional_calls;
  return 0;
}

H5VL_class_t MakeFullMockVolClass() {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = 502;
  cls.name = "google_passthru_mock_under_vol";
  cls.conn_version = 1;
  cls.cap_flags = H5VL_CAP_FLAG_ASYNC | H5VL_CAP_FLAG_THREADSAFE;

  cls.wrap_cls.get_object = MockGetObject;
  cls.wrap_cls.get_wrap_ctx = MockGetWrapCtx;
  cls.wrap_cls.wrap_object = MockWrapObject;
  cls.wrap_cls.unwrap_object = MockUnwrapObject;
  cls.wrap_cls.free_wrap_ctx = MockFreeWrapCtx;

  cls.attr_cls.create = MockAttrCreate;
  cls.attr_cls.open = MockAttrOpen;
  cls.attr_cls.read = MockAttrRead;
  cls.attr_cls.write = MockAttrWrite;
  cls.attr_cls.get = MockAttrGet;
  cls.attr_cls.specific = MockAttrSpecific;
  cls.attr_cls.optional = MockAttrOptional;
  cls.attr_cls.close = MockAttrClose;

  cls.dataset_cls.create = MockDatasetCreate;
  cls.dataset_cls.open = MockDatasetOpen;
  cls.dataset_cls.read = MockDatasetRead;
  cls.dataset_cls.write = MockDatasetWrite;
  cls.dataset_cls.get = MockDatasetGet;
  cls.dataset_cls.specific = MockDatasetSpecific;
  cls.dataset_cls.optional = MockDatasetOptional;
  cls.dataset_cls.close = MockDatasetClose;

  cls.datatype_cls.commit = MockDatatypeCommit;
  cls.datatype_cls.open = MockDatatypeOpen;
  cls.datatype_cls.get = MockDatatypeGet;
  cls.datatype_cls.specific = MockDatatypeSpecific;
  cls.datatype_cls.optional = MockDatatypeOptional;
  cls.datatype_cls.close = MockDatatypeClose;

  cls.file_cls.create = MockFileCreate;
  cls.file_cls.open = MockFileOpen;
  cls.file_cls.get = MockFileGet;
  cls.file_cls.specific = MockFileSpecific;
  cls.file_cls.optional = MockFileOptional;
  cls.file_cls.close = MockFileClose;

  cls.group_cls.create = MockGroupCreate;
  cls.group_cls.open = MockGroupOpen;
  cls.group_cls.get = MockGroupGet;
  cls.group_cls.specific = MockGroupSpecific;
  cls.group_cls.optional = MockGroupOptional;
  cls.group_cls.close = MockGroupClose;

  cls.link_cls.create = MockLinkCreate;
  cls.link_cls.copy = MockLinkCopy;
  cls.link_cls.move = MockLinkMove;
  cls.link_cls.get = MockLinkGet;
  cls.link_cls.specific = MockLinkSpecific;
  cls.link_cls.optional = MockLinkOptional;

  cls.object_cls.open = MockObjectOpen;
  cls.object_cls.copy = MockObjectCopy;
  cls.object_cls.get = MockObjectGet;
  cls.object_cls.specific = MockObjectSpecific;
  cls.object_cls.optional = MockObjectOptional;

  cls.introspect_cls.get_conn_cls = MockIntrospectGetConnCls;
  cls.introspect_cls.get_cap_flags = MockIntrospectGetCapFlags;
  cls.introspect_cls.opt_query = MockIntrospectOptQuery;

  cls.request_cls.wait = MockRequestWait;
  cls.request_cls.notify = MockRequestNotify;
  cls.request_cls.cancel = MockRequestCancel;
  cls.request_cls.specific = MockRequestSpecific;
  cls.request_cls.optional = MockRequestOptional;
  cls.request_cls.free = MockRequestFree;

  cls.blob_cls.put = MockBlobPut;
  cls.blob_cls.get = MockBlobGet;
  cls.blob_cls.specific = MockBlobSpecific;
  cls.blob_cls.optional = MockBlobOptional;

  cls.token_cls.cmp = MockTokenCmp;
  cls.token_cls.to_str = MockTokenToStr;
  cls.token_cls.from_str = MockTokenFromStr;

  cls.optional = MockGenericOptional;
  return cls;
}

TEST(H5VlPassthruTest, InitTermInfoCopyCmpFreeAndStringSerialization) {
  ASSERT_GE(H5open(), 0);

  // Initialize and terminate the pass-through connector directly.
  EXPECT_GE(H5VLinitialize(H5VL_PASSTHRU, H5P_DEFAULT), 0);
  EXPECT_GE(H5VLterminate(H5VL_PASSTHRU), 0);

  // Obtain application-level VOL IDs so H5Iis_valid() returns true inside
  // pass-through info_copy and introspect_get_cap_flags.
  const hid_t native_vol_id = H5VLget_connector_id_by_name(H5VL_NATIVE_NAME);
  ASSERT_NE(native_vol_id, H5I_INVALID_HID);
  absl::Cleanup close_native = [native_vol_id] { H5VLclose(native_vol_id); };

  const hid_t pt_vol_id = H5VLget_connector_id_by_name(H5VL_PASSTHRU_NAME);
  ASSERT_NE(pt_vol_id, H5I_INVALID_HID);
  absl::Cleanup close_pt = [pt_vol_id] { H5VLclose(pt_vol_id); };

  H5VL_pass_through_info_t pt_native{native_vol_id, nullptr};

  // FAPL property copy and equality exercise info_copy, info_cmp, info_free.
  ScopedPlist fapl1(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl1.valid());
  ASSERT_GE(H5Pset_vol(fapl1.get(), H5VL_PASSTHRU, &pt_native), 0);

  ScopedPlist fapl2(H5Pcopy(fapl1.get()));
  ASSERT_TRUE(fapl2.valid());
  EXPECT_GT(H5Pequal(fapl1.get(), fapl2.get()), 0);

  // Register a minimal secondary VOL connector so under_vol_id differs.
  H5VL_class_t alt_cls{};
  alt_cls.version = H5VL_VERSION;
  alt_cls.value = 501;
  alt_cls.name = "google_passthru_alt_vol";
  alt_cls.conn_version = 1;
  const hid_t alt_vol_id = H5VLregister_connector(&alt_cls, H5P_DEFAULT);
  ASSERT_NE(alt_vol_id, H5I_INVALID_HID);
  absl::Cleanup unregister_alt = [alt_vol_id] {
    H5VLunregister_connector(alt_vol_id);
  };

  H5VL_pass_through_info_t pt_alt{alt_vol_id, nullptr};
  int cmp = 0;
  ASSERT_GE(H5VLcmp_connector_info(&cmp, H5VL_PASSTHRU, &pt_native, &pt_alt),
            0);
  EXPECT_NE(cmp, 0);

  // Nested pass-through info exercises info->under_vol_info copy, cmp, free.
  H5VL_pass_through_info_t pt_nested1{pt_vol_id, &pt_native};
  H5VL_pass_through_info_t pt_nested2{pt_vol_id, &pt_alt};

  void* copied_nested = nullptr;
  ASSERT_GE(H5VLcopy_connector_info(H5VL_PASSTHRU, &copied_nested, &pt_nested1),
            0);
  ASSERT_NE(copied_nested, nullptr);
  absl::Cleanup free_copied_nested = [copied_nested] {
    H5VLfree_connector_info(H5VL_PASSTHRU, copied_nested);
  };

  cmp = 1;
  ASSERT_GE(
      H5VLcmp_connector_info(&cmp, H5VL_PASSTHRU, &pt_nested1, copied_nested),
      0);
  EXPECT_EQ(cmp, 0);

  cmp = 0;
  ASSERT_GE(
      H5VLcmp_connector_info(&cmp, H5VL_PASSTHRU, &pt_nested1, &pt_nested2),
      0);
  EXPECT_NE(cmp, 0);

  // Error branches in info_copy and introspect_get_cap_flags.
  H5VL_pass_through_info_t bad_vol_info{H5I_INVALID_HID, nullptr};
  void* bad_copy = nullptr;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLcopy_connector_info(H5VL_PASSTHRU, &bad_copy, &bad_vol_info),
              0);
    EXPECT_EQ(bad_copy, nullptr);
  }

  uint64_t cap_flags = 0;
  ASSERT_GE(H5Pget_vol_cap_flags(fapl1.get(), &cap_flags), 0);
  uint64_t cap_flags_direct = 0;
  ASSERT_GE(H5VLintrospect_get_cap_flags(&pt_native, H5VL_PASSTHRU,
                                         &cap_flags_direct),
            0);
  EXPECT_EQ(cap_flags, cap_flags_direct);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VLintrospect_get_cap_flags(nullptr, H5VL_PASSTHRU, &cap_flags_direct),
        0);
    EXPECT_LT(H5VLintrospect_get_cap_flags(&bad_vol_info, H5VL_PASSTHRU,
                                           &cap_flags_direct),
              0);
  }

  // info_to_str and str_to_info round-trip over Native (under_vol_string==NULL).
  char* info_str = nullptr;
  ASSERT_GE(H5VLconnector_info_to_str(&pt_native, H5VL_PASSTHRU, &info_str), 0);
  ASSERT_NE(info_str, nullptr);
  absl::Cleanup free_info_str = [info_str] { H5free_memory(info_str); };
  EXPECT_STREQ(info_str, "under_vol=0;under_info={}");

  void* parsed_info = nullptr;
  ASSERT_GE(H5VLconnector_str_to_info(info_str, H5VL_PASSTHRU, &parsed_info),
            0);
  ASSERT_NE(parsed_info, nullptr);
  absl::Cleanup free_parsed = [parsed_info] {
    H5VLfree_connector_info(H5VL_PASSTHRU, parsed_info);
  };
  cmp = 1;
  ASSERT_GE(H5VLcmp_connector_info(&cmp, H5VL_PASSTHRU, &pt_native, parsed_info),
            0);
  EXPECT_EQ(cmp, 0);
}

TEST(H5VlPassthruTest, FileAndGroupLifecycleOverNative) {
  ASSERT_GE(H5open(), 0);
  const hid_t native_vol_id = H5VLget_connector_id_by_name(H5VL_NATIVE_NAME);
  ASSERT_NE(native_vol_id, H5I_INVALID_HID);
  absl::Cleanup close_native = [native_vol_id] { H5VLclose(native_vol_id); };

  const hid_t pt_vol_id = H5VLget_connector_id_by_name(H5VL_PASSTHRU_NAME);
  ASSERT_NE(pt_vol_id, H5I_INVALID_HID);
  absl::Cleanup close_pt = [pt_vol_id] { H5VLclose(pt_vol_id); };

  H5VL_pass_through_info_t pt_native{native_vol_id, nullptr};

  // Null-info error paths on file create / open / is_accessible / delete.
  {
    ScopedErrorSilence silence;
    ScopedPlist null_fapl = MakeCoreFapl();
    ASSERT_TRUE(null_fapl.valid());
    ASSERT_GE(H5Pset_vol(null_fapl.get(), H5VL_PASSTHRU, nullptr), 0);
    EXPECT_LT(H5Fcreate("null_info.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                        null_fapl.get()),
              0);
    EXPECT_LT(H5Fopen("null_info.h5", H5F_ACC_RDONLY, null_fapl.get()), 0);

    ScopedPlist sec2_null(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(sec2_null.valid());
    ASSERT_GE(H5Pset_vol(sec2_null.get(), H5VL_PASSTHRU, nullptr), 0);
    EXPECT_LT(H5Fis_accessible("null_info.h5", sec2_null.get()), 0);

    H5VL_file_specific_args_t del_args{};
    del_args.op_type = H5VL_FILE_DELETE;
    del_args.args.del.filename = "null_info.h5";
    del_args.args.del.fapl_id = sec2_null.get();
    EXPECT_LT(H5VLfile_specific(nullptr, H5VL_PASSTHRU, &del_args, H5P_DEFAULT,
                                nullptr),
              0);
  }

  // In-memory Core VFD file operations through pass-through VOL.
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  ASSERT_GE(H5Pset_vol(core_fapl.get(), H5VL_PASSTHRU, &pt_native), 0);

  ScopedFile file(
      H5Fcreate("pt_core.h5", H5F_ACC_TRUNC, H5P_DEFAULT, core_fapl.get()));
  ASSERT_TRUE(file.valid());

  EXPECT_NE(H5VLobject(file.get()), nullptr);

  bool is_native = false;
  ASSERT_GE(H5VLobject_is_native(file.get(), &is_native), 0);
  EXPECT_TRUE(is_native);

  // Nested pass-through over pass-through over native exercises recursive
  // introspect_get_conn_cls and nested wrap contexts.
  {
    H5VL_pass_through_info_t pt_nested{pt_vol_id, &pt_native};
    ScopedPlist nested_fapl = MakeCoreFapl();
    ASSERT_TRUE(nested_fapl.valid());
    ASSERT_GE(H5Pset_vol(nested_fapl.get(), H5VL_PASSTHRU, &pt_nested), 0);
    ScopedFile nested_file(H5Fcreate("pt_nested.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                     nested_fapl.get()));
    ASSERT_TRUE(nested_file.valid());

    bool nested_is_native = false;
    ASSERT_GE(H5VLobject_is_native(nested_file.get(), &nested_is_native), 0);
    EXPECT_TRUE(nested_is_native);

    ScopedGroup nested_grp(H5Gcreate2(nested_file.get(), "/ngrp", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(nested_grp.valid());
    int visit_cnt = 0;
    auto visit_cb = [](hid_t /*obj*/, const char* /*name*/,
                       const H5O_info2_t* /*info*/, void* op_data) -> herr_t {
      ++(*static_cast<int*>(op_data));
      return 0;
    };
    EXPECT_GE(H5Ovisit3(nested_file.get(), H5_INDEX_NAME, H5_ITER_NATIVE,
                        visit_cb, &visit_cnt, H5O_INFO_BASIC),
              0);
    EXPECT_GE(visit_cnt, 2);
  }

  // File getters & flush & reopen.
  ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
  EXPECT_TRUE(got_fapl.valid());
  ScopedPlist got_fcpl(H5Fget_create_plist(file.get()));
  EXPECT_TRUE(got_fcpl.valid());

  unsigned long fileno = 0;
  EXPECT_GE(H5Fget_fileno(file.get(), &fileno), 0);
  unsigned intent = 0;
  EXPECT_GE(H5Fget_intent(file.get(), &intent), 0);
  EXPECT_EQ(intent, H5F_ACC_RDWR);

  std::array<char, 64> fname{};
  EXPECT_GT(H5Fget_name(file.get(), fname.data(), fname.size()), 0);
  EXPECT_GE(H5Fget_obj_count(file.get(), H5F_OBJ_ALL), 1);

  std::array<hid_t, 4> obj_ids{};
  EXPECT_GE(H5Fget_obj_ids(file.get(), H5F_OBJ_FILE, obj_ids.size(),
                           obj_ids.data()),
            1);

  hsize_t fsize = 0;
  EXPECT_GE(H5Fget_filesize(file.get(), &fsize), 0);

  H5F_info2_t finfo{};
  EXPECT_GE(H5Fget_info2(file.get(), &finfo), 0);

  EXPECT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
  ScopedFile reopened(H5Freopen(file.get()));
  EXPECT_TRUE(reopened.valid());

  // Group lifecycle and H5Fmount / H5Funmount.
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup mnt_pt(
      H5Gcreate2(file.get(), "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt_pt.valid());

  H5G_info_t ginfo{};
  EXPECT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  ScopedPlist gcpl(H5Gget_create_plist(grp.get()));
  EXPECT_TRUE(gcpl.valid());
  EXPECT_GE(H5Gflush(grp.get()), 0);
  EXPECT_GE(H5Grefresh(grp.get()), 0);

  ScopedGroup grp_reopened(H5Gopen2(file.get(), "/grp", H5P_DEFAULT));
  EXPECT_TRUE(grp_reopened.valid());

  ScopedFile child_file(H5Fcreate("pt_child.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                    core_fapl.get()));
  ASSERT_TRUE(child_file.valid());
  ScopedGroup child_grp(H5Gcreate2(child_file.get(), "/in_child", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_grp.valid());

  EXPECT_GE(H5Fmount(file.get(), "/mnt", child_file.get(), H5P_DEFAULT), 0);
  ScopedGroup mounted_grp(H5Gopen2(file.get(), "/mnt/in_child", H5P_DEFAULT));
  EXPECT_TRUE(mounted_grp.valid());
  mounted_grp.reset();
  EXPECT_GE(H5Funmount(file.get(), "/mnt"), 0);

  // Underlying group/file failures return NULL cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Gopen2(file.get(), "/no_such_group", H5P_DEFAULT), 0);
    EXPECT_LT(
        H5Fcreate("pt_core.h5", H5F_ACC_EXCL, H5P_DEFAULT, core_fapl.get()), 0);
  }

  // On-disk sec2 file for H5Fis_accessible, H5Fopen, H5Fdelete.
  TempFile disk_file("passthru_disk.h5");
  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(sec2_fapl.valid());
  ASSERT_GE(H5Pset_vol(sec2_fapl.get(), H5VL_PASSTHRU, &pt_native), 0);

  {
    ScopedFile dfile(H5Fcreate(disk_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                               sec2_fapl.get()));
    ASSERT_TRUE(dfile.valid());
  }

  EXPECT_GT(H5Fis_accessible(disk_file.c_str(), sec2_fapl.get()), 0);

  {
    ScopedFile opened(
        H5Fopen(disk_file.c_str(), H5F_ACC_RDONLY, sec2_fapl.get()));
    ASSERT_TRUE(opened.valid());
  }

  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Fopen("/nonexistent_dir_xyz/nofile.h5", H5F_ACC_RDONLY,
                sec2_fapl.get()),
        0);
  }

  EXPECT_GE(H5Fdelete(disk_file.c_str(), sec2_fapl.get()), 0);
}

TEST(H5VlPassthruTest, DatasetDatatypeAttributeLinkObjectAndTokenOverNative) {
  ASSERT_GE(H5open(), 0);
  const hid_t native_vol_id = H5VLget_connector_id_by_name(H5VL_NATIVE_NAME);
  ASSERT_NE(native_vol_id, H5I_INVALID_HID);
  absl::Cleanup close_native = [native_vol_id] { H5VLclose(native_vol_id); };

  H5VL_pass_through_info_t pt_native{native_vol_id, nullptr};

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), H5VL_PASSTHRU, &pt_native), 0);

  ScopedFile file(
      H5Fcreate("pt_objects.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  // Datasets: chunked & extendable, single & multi read/write, optional, error.
  std::array<hsize_t, 1> dims{4};
  std::array<hsize_t, 1> maxdims{H5S_UNLIMITED};
  std::array<hsize_t, 1> chunk_dims{2};
  ScopedSpace space1(H5Screate_simple(1, dims.data(), maxdims.data()));
  ScopedSpace space2(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space1.valid() && space2.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk_dims.data()), 0);

  ScopedDataset dset1(H5Dcreate2(file.get(), "/ds1", H5T_NATIVE_INT32,
                                 space1.get(), H5P_DEFAULT, dcpl.get(),
                                 H5P_DEFAULT));
  ScopedDataset dset2(H5Dcreate2(file.get(), "/ds2", H5T_NATIVE_INT32,
                                 space2.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
  ASSERT_TRUE(dset1.valid() && dset2.valid());

  std::vector<int32_t> write_buf1{10, 20, 30, 40};
  std::vector<int32_t> write_buf2{1, 2, 3, 4};
  EXPECT_GE(H5Dwrite(dset1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, write_buf1.data()),
            0);

  std::vector<int32_t> read_buf1(4, 0);
  EXPECT_GE(H5Dread(dset1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_buf1.data()),
            0);
  EXPECT_EQ(read_buf1, write_buf1);

  // Multi-dataset read/write hits count > 1 malloc/free path.
  std::array<hid_t, 2> dset_ids{dset1.get(), dset2.get()};
  std::array<hid_t, 2> mem_types{H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> mem_spaces{H5S_ALL, H5S_ALL};
  std::array<hid_t, 2> file_spaces{H5S_ALL, H5S_ALL};
  std::array<const void*, 2> wbufs{write_buf1.data(), write_buf2.data()};
  EXPECT_GE(H5Dwrite_multi(2, dset_ids.data(), mem_types.data(),
                           mem_spaces.data(), file_spaces.data(), H5P_DEFAULT,
                           wbufs.data()),
            0);

  std::vector<int32_t> r1(4, 0);
  std::vector<int32_t> r2(4, 0);
  std::array<void*, 2> rbufs{r1.data(), r2.data()};
  EXPECT_GE(H5Dread_multi(2, dset_ids.data(), mem_types.data(),
                          mem_spaces.data(), file_spaces.data(), H5P_DEFAULT,
                          rbufs.data()),
            0);
  EXPECT_EQ(r1, write_buf1);
  EXPECT_EQ(r2, write_buf2);

  ScopedPlist got_dapl(H5Dget_access_plist(dset1.get()));
  EXPECT_TRUE(got_dapl.valid());
  ScopedPlist got_dcpl(H5Dget_create_plist(dset1.get()));
  EXPECT_TRUE(got_dcpl.valid());
  ScopedSpace got_dspace(H5Dget_space(dset1.get()));
  EXPECT_TRUE(got_dspace.valid());
  ScopedType got_dtype(H5Dget_type(dset1.get()));
  EXPECT_TRUE(got_dtype.valid());
  H5D_space_status_t space_status = H5D_SPACE_STATUS_NOT_ALLOCATED;
  EXPECT_GE(H5Dget_space_status(dset1.get(), &space_status), 0);
  EXPECT_GT(H5Dget_storage_size(dset1.get()), 0);

  hsize_t nchunks = 0;
  EXPECT_GE(H5Dget_num_chunks(dset1.get(), H5S_ALL, &nchunks), 0);
  EXPECT_EQ(nchunks, 2);

  std::array<hsize_t, 1> new_dims{6};
  EXPECT_GE(H5Dset_extent(dset1.get(), new_dims.data()), 0);
  EXPECT_GE(H5Dflush(dset1.get()), 0);
  EXPECT_GE(H5Drefresh(dset1.get()), 0);

  ScopedDataset reopened_dset(H5Dopen2(file.get(), "/ds1", H5P_DEFAULT));
  EXPECT_TRUE(reopened_dset.valid());

  // Datatypes: commit, get_create_plist, flush, refresh, open.
  ScopedType ctype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(ctype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/committed_i32", ctype.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedPlist got_tcpl(H5Tget_create_plist(ctype.get()));
  EXPECT_TRUE(got_tcpl.valid());
  EXPECT_GE(H5Tflush(ctype.get()), 0);
  EXPECT_GE(H5Trefresh(ctype.get()), 0);
  ScopedType opened_ctype(H5Topen2(file.get(), "/committed_i32", H5P_DEFAULT));
  EXPECT_TRUE(opened_ctype.valid());

  // Attributes: create, write, read, get info/name/space/size/type/acpl,
  // exists, rename, iterate, delete_by_idx, delete.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  {
    ScopedAttr attr(H5Acreate2(dset1.get(), "attr_a", H5T_NATIVE_INT32,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int32_t aval = 99;
    EXPECT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &aval), 0);
    int32_t aread = 0;
    EXPECT_GE(H5Aread(attr.get(), H5T_NATIVE_INT32, &aread), 0);
    EXPECT_EQ(aread, 99);

    ScopedPlist acpl(H5Aget_create_plist(attr.get()));
    EXPECT_TRUE(acpl.valid());
    H5A_info_t ainfo{};
    EXPECT_GE(H5Aget_info(attr.get(), &ainfo), 0);
    std::array<char, 32> aname{};
    EXPECT_GT(H5Aget_name(attr.get(), aname.size(), aname.data()), 0);
    EXPECT_STREQ(aname.data(), "attr_a");
    ScopedSpace aspace(H5Aget_space(attr.get()));
    EXPECT_TRUE(aspace.valid());
    ScopedType atype(H5Aget_type(attr.get()));
    EXPECT_TRUE(atype.valid());
    EXPECT_GT(H5Aget_storage_size(attr.get()), 0);
  }

  ScopedAttr opened_attr(H5Aopen(dset1.get(), "attr_a", H5P_DEFAULT));
  EXPECT_TRUE(opened_attr.valid());
  opened_attr.reset();

  EXPECT_GT(H5Aexists(dset1.get(), "attr_a"), 0);
  EXPECT_GE(H5Arename(dset1.get(), "attr_a", "attr_b"), 0);

  ScopedAttr attr_c(H5Acreate2(dset1.get(), "attr_c", H5T_NATIVE_INT32,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr_c.valid());
  attr_c.reset();

  int attr_count = 0;
  hsize_t aidx = 0;
  auto attr_iter = [](hid_t loc_id, const char* /*name*/,
                      const H5A_info_t* /*info*/, void* op_data) -> herr_t {
    EXPECT_NE(H5Iget_type(loc_id), H5I_BADID);
    ++(*static_cast<int*>(op_data));
    return 0;
  };
  EXPECT_GE(H5Aiterate2(dset1.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &aidx,
                        attr_iter, &attr_count),
            0);
  EXPECT_EQ(attr_count, 2);

  EXPECT_GE(H5Adelete_by_idx(dset1.get(), ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_GE(H5Adelete(dset1.get(), "attr_c"), 0);

  // Links: hard, soft, copy, move, info, name, val, exists, iterate, delete.
  ScopedGroup src_grp(
      H5Gcreate2(file.get(), "/links", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_grp.valid());
  EXPECT_GE(H5Lcreate_hard(file.get(), "/ds1", src_grp.get(), "hard_ds",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lcreate_soft("/ds1", src_grp.get(), "soft_ds", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lcopy(src_grp.get(), "soft_ds", src_grp.get(), "soft_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lmove(src_grp.get(), "soft_copy", src_grp.get(), "soft_moved",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_GT(H5Lexists(src_grp.get(), "soft_moved", H5P_DEFAULT), 0);
  H5L_info2_t linfo{};
  EXPECT_GE(H5Lget_info2(src_grp.get(), "soft_moved", &linfo, H5P_DEFAULT), 0);
  std::array<char, 64> lval{};
  EXPECT_GE(H5Lget_val(src_grp.get(), "soft_moved", lval.data(), lval.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_STREQ(lval.data(), "/ds1");

  std::array<char, 64> lname{};
  EXPECT_GT(H5Lget_name_by_idx(src_grp.get(), ".", H5_INDEX_NAME,
                               H5_ITER_NATIVE, 0, lname.data(), lname.size(),
                               H5P_DEFAULT),
            0);

  int link_count = 0;
  hsize_t lidx = 0;
  auto link_iter = [](hid_t /*group*/, const char* /*name*/,
                      const H5L_info2_t* /*info*/, void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return 0;
  };
  EXPECT_GE(H5Literate2(src_grp.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &lidx,
                        link_iter, &link_count),
            0);
  EXPECT_EQ(link_count, 3);
  EXPECT_GE(H5Ldelete(src_grp.get(), "soft_moved", H5P_DEFAULT), 0);

  // Objects & tokens: open, open_by_token, copy, get_info3, refcount, exists,
  // visit3, flush, refresh, comment (object optional), token_cmp/str.
  EXPECT_GE(H5Ocopy(file.get(), "/ds2", file.get(), "/ds2_copy", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_GT(H5Oexists_by_name(file.get(), "/ds2_copy", H5P_DEFAULT), 0);

  ScopedObject obj(H5Oopen(file.get(), "/ds2_copy", H5P_DEFAULT));
  ASSERT_TRUE(obj.valid());

  H5O_info2_t oinfo{};
  EXPECT_GE(H5Oget_info3(obj.get(), &oinfo, H5O_INFO_BASIC), 0);

  int tok_cmp = 1;
  EXPECT_GE(H5Otoken_cmp(file.get(), &oinfo.token, &oinfo.token, &tok_cmp), 0);
  EXPECT_EQ(tok_cmp, 0);

  char* tok_str = nullptr;
  ASSERT_GE(H5Otoken_to_str(file.get(), &oinfo.token, &tok_str), 0);
  ASSERT_NE(tok_str, nullptr);
  absl::Cleanup free_tok_str = [tok_str] { H5free_memory(tok_str); };
  H5O_token_t parsed_tok{};
  EXPECT_GE(H5Otoken_from_str(file.get(), tok_str, &parsed_tok), 0);

  ScopedObject obj_by_tok(H5Oopen_by_token(file.get(), parsed_tok));
  EXPECT_TRUE(obj_by_tok.valid());
  obj_by_tok.reset();

  EXPECT_GE(H5Oincr_refcount(obj.get()), 0);
  EXPECT_GE(H5Odecr_refcount(obj.get()), 0);
  EXPECT_GE(H5Oset_comment(obj.get(), "passthru_comment"), 0);
  std::array<char, 64> comment_buf{};
  EXPECT_GT(H5Oget_comment(obj.get(), comment_buf.data(), comment_buf.size()),
            0);
  EXPECT_STREQ(comment_buf.data(), "passthru_comment");
  EXPECT_GE(H5Oflush(obj.get()), 0);
  EXPECT_GE(H5Orefresh(obj.get()), 0);

  int obj_visit_count = 0;
  auto obj_iter = [](hid_t /*obj*/, const char* /*name*/,
                     const H5O_info2_t* /*info*/, void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return 0;
  };
  EXPECT_GE(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_NATIVE, obj_iter,
                      &obj_visit_count, H5O_INFO_BASIC),
            0);
  EXPECT_GT(obj_visit_count, 0);

  uint64_t opt_flags = 0;
  EXPECT_GE(H5VLquery_optional(file.get(), H5VL_SUBCLS_DATASET, 0, &opt_flags),
            0);

  // Underlying open failures return NULL cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dopen2(file.get(), "/missing_ds", H5P_DEFAULT), 0);
    EXPECT_LT(H5Topen2(file.get(), "/missing_dt", H5P_DEFAULT), 0);
    EXPECT_LT(H5Aopen(dset1.get(), "missing_attr", H5P_DEFAULT), 0);
    EXPECT_LT(H5Oopen(file.get(), "/missing_obj", H5P_DEFAULT), 0);
  }
}

TEST(H5VlPassthruTest, MockUnderlyingConnectorForAsyncBlobOptionalAndWrap) {
  ASSERT_GE(H5open(), 0);
  g_mock_state = MockVolState{};

  const H5VL_class_t mock_cls = MakeFullMockVolClass();
  g_mock_cls_ptr = &mock_cls;
  absl::Cleanup clear_cls_ptr = [] { g_mock_cls_ptr = nullptr; };

  const hid_t mock_vol_id = H5VLregister_connector(&mock_cls, H5P_DEFAULT);
  ASSERT_NE(mock_vol_id, H5I_INVALID_HID);
  absl::Cleanup unregister_mock = [mock_vol_id] {
    H5VLunregister_connector(mock_vol_id);
  };

  H5VL_pass_through_info_t pt_mock_info{mock_vol_id, nullptr};
  ScopedPlist pt_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(pt_fapl.valid());
  ASSERT_GE(H5Pset_vol(pt_fapl.get(), H5VL_PASSTHRU, &pt_mock_info), 0);

  // Introspect cap_flags over mock connector.
  uint64_t caps = 0;
  ASSERT_GE(H5VLintrospect_get_cap_flags(&pt_mock_info, H5VL_PASSTHRU, &caps),
            0);
  EXPECT_EQ(caps, H5VL_CAP_FLAG_ASYNC | H5VL_CAP_FLAG_THREADSAFE);

  // Create and open wrapped objects synchronously, then asynchronously with
  // wrapped async request tokens exercised through wait / notify / cancel /
  // specific / optional / free.
  auto consume_req_free = [](void* req) {
    ASSERT_NE(req, nullptr);
    H5VL_request_specific_args_t spec{};
    uint64_t ts = 0;
    uint64_t dt = 0;
    spec.op_type = H5VL_REQUEST_GET_EXEC_TIME;
    spec.args.get_exec_time.exec_ts = &ts;
    spec.args.get_exec_time.exec_time = &dt;
    EXPECT_GE(H5VLrequest_specific(req, H5VL_PASSTHRU, &spec), 0);
    EXPECT_EQ(ts, 100);
    EXPECT_EQ(dt, 25);

    H5VL_optional_args_t opt{};
    opt.op_type = 1;
    EXPECT_GE(H5VLrequest_optional(req, H5VL_PASSTHRU, &opt), 0);
    EXPECT_GE(H5VLrequest_free(req, H5VL_PASSTHRU), 0);
  };

  g_mock_state.emit_req = true;

  void* req = nullptr;
  void* pt_file = H5VLfile_create("mfile", H5F_ACC_TRUNC, H5P_DEFAULT,
                                  pt_fapl.get(), H5P_DEFAULT, &req);
  ASSERT_NE(pt_file, nullptr);
  consume_req_free(req);

  // Test wait (IN_PROGRESS then SUCCEED).
  g_mock_state.return_in_progress_once = true;
  req = nullptr;
  void* pt_file2 = H5VLfile_open("mfile", H5F_ACC_RDWR, pt_fapl.get(),
                                 H5P_DEFAULT, &req);
  ASSERT_NE(pt_file2, nullptr);
  ASSERT_NE(req, nullptr);
  H5VL_request_status_t rstatus = H5VL_REQUEST_STATUS_FAIL;
  EXPECT_GE(H5VLrequest_wait(req, H5VL_PASSTHRU, 0, &rstatus), 0);
  EXPECT_EQ(rstatus, H5VL_REQUEST_STATUS_IN_PROGRESS);
  EXPECT_GE(H5VLrequest_wait(req, H5VL_PASSTHRU, 0, &rstatus), 0);
  EXPECT_EQ(rstatus, H5VL_REQUEST_STATUS_SUCCEED);
  g_mock_state.return_in_progress_once = false;

  // Test notify.
  req = nullptr;
  H5VL_file_get_args_t fget_args{};
  H5VL_file_cont_info_t cinfo{};
  fget_args.op_type = H5VL_FILE_GET_CONT_INFO;
  fget_args.args.get_cont_info.info = &cinfo;
  EXPECT_GE(
      H5VLfile_get(pt_file, H5VL_PASSTHRU, &fget_args, H5P_DEFAULT, &req), 0);
  EXPECT_EQ(cinfo.token_size, 8);
  bool notify_called = false;
  auto notify_cb = [](void* ctx, H5VL_request_status_t status) -> herr_t {
    if (status == H5VL_REQUEST_STATUS_SUCCEED) {
      *static_cast<bool*>(ctx) = true;
    }
    return 0;
  };
  EXPECT_GE(H5VLrequest_notify(req, H5VL_PASSTHRU, notify_cb, &notify_called),
            0);
  EXPECT_TRUE(notify_called);

  // Test cancel.
  req = nullptr;
  H5VL_optional_args_t opt_args{};
  opt_args.op_type = 42;
  EXPECT_GE(
      H5VLfile_optional(pt_file, H5VL_PASSTHRU, &opt_args, H5P_DEFAULT, &req),
      0);
  rstatus = H5VL_REQUEST_STATUS_SUCCEED;
  EXPECT_GE(H5VLrequest_cancel(req, H5VL_PASSTHRU, &rstatus), 0);
  EXPECT_EQ(rstatus, H5VL_REQUEST_STATUS_CANCELED);

  // File specific: reopen (returns wrapped reopened file) + flush.
  void* reopened_file = nullptr;
  H5VL_file_specific_args_t fspec{};
  fspec.op_type = H5VL_FILE_REOPEN;
  fspec.args.reopen.file = &reopened_file;
  req = nullptr;
  EXPECT_GE(
      H5VLfile_specific(pt_file, H5VL_PASSTHRU, &fspec, H5P_DEFAULT, &req), 0);
  EXPECT_TRUE(g_mock_state.saw_reopen);
  ASSERT_NE(reopened_file, nullptr);
  consume_req_free(req);
  EXPECT_GE(H5VLfile_close(reopened_file, H5VL_PASSTHRU, H5P_DEFAULT, nullptr),
            0);

  // Reopen where underlying *file == NULL hits reopen false branch cleanly.
  g_mock_state.fail_ops = true;
  reopened_file = nullptr;
  req = nullptr;
  EXPECT_GE(
      H5VLfile_specific(pt_file, H5VL_PASSTHRU, &fspec, H5P_DEFAULT, &req), 0);
  EXPECT_EQ(reopened_file, nullptr);
  consume_req_free(req);
  g_mock_state.fail_ops = false;

  // Introspection over mock VOL (CURR, TERM, opt_query).
  const H5VL_class_t* conn_cls = nullptr;
  EXPECT_GE(H5VLintrospect_get_conn_cls(pt_file, H5VL_PASSTHRU,
                                        H5VL_GET_CONN_LVL_CURR, &conn_cls),
            0);
  EXPECT_NE(conn_cls, nullptr);
  EXPECT_STREQ(conn_cls->name, H5VL_PASSTHRU_NAME);

  conn_cls = nullptr;
  EXPECT_GE(H5VLintrospect_get_conn_cls(pt_file, H5VL_PASSTHRU,
                                        H5VL_GET_CONN_LVL_TERM, &conn_cls),
            0);
  EXPECT_EQ(conn_cls, &mock_cls);

  uint64_t opt_flags = 0;
  EXPECT_GE(H5VLintrospect_opt_query(pt_file, H5VL_PASSTHRU, H5VL_SUBCLS_FILE,
                                     1, &opt_flags),
            0);
  EXPECT_EQ(opt_flags, H5VL_OPT_QUERY_SUPPORTED);

  // Wrap context / wrap / unwrap / free_wrap_ctx (both success and fail paths).
  void* wrap_ctx = nullptr;
  ASSERT_GE(H5VLget_wrap_ctx(pt_file, H5VL_PASSTHRU, &wrap_ctx), 0);
  ASSERT_NE(wrap_ctx, nullptr);

  auto* raw_for_wrap = new MockObject{70};
  void* wrapped =
      H5VLwrap_object(raw_for_wrap, H5I_GROUP, H5VL_PASSTHRU, wrap_ctx);
  ASSERT_NE(wrapped, nullptr);

  g_mock_state.fail_unwrap = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VLunwrap_object(wrapped, H5VL_PASSTHRU), nullptr);
  }
  g_mock_state.fail_unwrap = false;

  void* unwrapped = H5VLunwrap_object(wrapped, H5VL_PASSTHRU);
  EXPECT_EQ(unwrapped, raw_for_wrap);
  delete static_cast<MockObject*>(unwrapped);

  g_mock_state.fail_wrap = true;
  {
    ScopedErrorSilence silence;
    int dummy = 0;
    EXPECT_EQ(H5VLwrap_object(&dummy, H5I_GROUP, H5VL_PASSTHRU, wrap_ctx),
              nullptr);
  }
  g_mock_state.fail_wrap = false;
  EXPECT_GE(H5VLfree_wrap_ctx(wrap_ctx, H5VL_PASSTHRU), 0);

  // Attributes with req != nullptr.
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_FILE;
  loc.type = H5VL_OBJECT_BY_SELF;
  req = nullptr;
  void* pt_attr =
      H5VLattr_create(pt_file, &loc, H5VL_PASSTHRU, "a", H5T_NATIVE_INT32,
                      H5S_ALL, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, &req);
  ASSERT_NE(pt_attr, nullptr);
  consume_req_free(req);

  req = nullptr;
  void* pt_attr2 = H5VLattr_open(pt_file, &loc, H5VL_PASSTHRU, "a", H5P_DEFAULT,
                                 H5P_DEFAULT, &req);
  ASSERT_NE(pt_attr2, nullptr);
  consume_req_free(req);

  int buf = 0;
  req = nullptr;
  EXPECT_GE(
      H5VLattr_write(pt_attr, H5VL_PASSTHRU, H5T_NATIVE_INT32, &buf,
                     H5P_DEFAULT, &req),
      0);
  consume_req_free(req);
  req = nullptr;
  EXPECT_GE(
      H5VLattr_read(pt_attr, H5VL_PASSTHRU, H5T_NATIVE_INT32, &buf,
                    H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  H5VL_attr_get_args_t aget{};
  aget.op_type = H5VL_ATTR_GET_ACPL;
  req = nullptr;
  EXPECT_GE(H5VLattr_get(pt_attr, H5VL_PASSTHRU, &aget, H5P_DEFAULT, &req), 0);
  consume_req_free(req);

  H5VL_attr_specific_args_t aspec{};
  bool exists = false;
  aspec.op_type = H5VL_ATTR_EXISTS;
  aspec.args.exists.name = "a";
  aspec.args.exists.exists = &exists;
  req = nullptr;
  EXPECT_GE(
      H5VLattr_specific(pt_attr, &loc, H5VL_PASSTHRU, &aspec, H5P_DEFAULT,
                        &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(
      H5VLattr_optional(pt_attr, H5VL_PASSTHRU, &opt_args, H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLattr_close(pt_attr, H5VL_PASSTHRU, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_GE(H5VLattr_close(pt_attr2, H5VL_PASSTHRU, H5P_DEFAULT, nullptr), 0);

  // Datasets with req != nullptr, count == 1 and count == 2.
  req = nullptr;
  void* pt_dset = H5VLdataset_create(pt_file, &loc, H5VL_PASSTHRU, "d",
                                     H5P_DEFAULT, H5T_NATIVE_INT32, H5S_ALL,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                     &req);
  ASSERT_NE(pt_dset, nullptr);
  consume_req_free(req);

  req = nullptr;
  void* pt_dset2 = H5VLdataset_open(pt_file, &loc, H5VL_PASSTHRU, "d",
                                    H5P_DEFAULT, H5P_DEFAULT, &req);
  ASSERT_NE(pt_dset2, nullptr);
  consume_req_free(req);

  std::array<void*, 2> dsets{pt_dset, pt_dset2};
  std::array<hid_t, 2> types{H5T_NATIVE_INT32, H5T_NATIVE_INT32};
  std::array<hid_t, 2> spaces{H5S_ALL, H5S_ALL};
  std::array<const void*, 2> wptrs{&buf, &buf};
  std::array<void*, 2> rptrs{&buf, &buf};

  req = nullptr;
  EXPECT_GE(H5VLdataset_write(1, dsets.data(), H5VL_PASSTHRU, types.data(),
                              spaces.data(), spaces.data(), H5P_DEFAULT,
                              wptrs.data(), &req),
            0);
  consume_req_free(req);
  req = nullptr;
  EXPECT_GE(H5VLdataset_write(2, dsets.data(), H5VL_PASSTHRU, types.data(),
                              spaces.data(), spaces.data(), H5P_DEFAULT,
                              wptrs.data(), &req),
            0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLdataset_read(1, dsets.data(), H5VL_PASSTHRU, types.data(),
                             spaces.data(), spaces.data(), H5P_DEFAULT,
                             rptrs.data(), &req),
            0);
  consume_req_free(req);
  req = nullptr;
  EXPECT_GE(H5VLdataset_read(2, dsets.data(), H5VL_PASSTHRU, types.data(),
                             spaces.data(), spaces.data(), H5P_DEFAULT,
                             rptrs.data(), &req),
            0);
  consume_req_free(req);

  H5VL_dataset_get_args_t dget{};
  dget.op_type = H5VL_DATASET_GET_DCPL;
  req = nullptr;
  EXPECT_GE(H5VLdataset_get(pt_dset, H5VL_PASSTHRU, &dget, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  H5VL_dataset_specific_args_t dspec{};
  dspec.op_type = H5VL_DATASET_FLUSH;
  dspec.args.flush.dset_id = H5I_INVALID_HID;
  req = nullptr;
  EXPECT_GE(
      H5VLdataset_specific(pt_dset, H5VL_PASSTHRU, &dspec, H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLdataset_optional(pt_dset, H5VL_PASSTHRU, &opt_args,
                                 H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLdataset_close(pt_dset, H5VL_PASSTHRU, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_GE(H5VLdataset_close(pt_dset2, H5VL_PASSTHRU, H5P_DEFAULT, nullptr),
            0);

  // Datatypes with req != nullptr.
  req = nullptr;
  void* pt_dt = H5VLdatatype_commit(pt_file, &loc, H5VL_PASSTHRU, "t",
                                    H5T_NATIVE_INT32, H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT, &req);
  ASSERT_NE(pt_dt, nullptr);
  consume_req_free(req);

  req = nullptr;
  void* pt_dt2 = H5VLdatatype_open(pt_file, &loc, H5VL_PASSTHRU, "t",
                                   H5P_DEFAULT, H5P_DEFAULT, &req);
  ASSERT_NE(pt_dt2, nullptr);
  consume_req_free(req);

  H5VL_datatype_get_args_t tget{};
  tget.op_type = H5VL_DATATYPE_GET_TCPL;
  req = nullptr;
  EXPECT_GE(H5VLdatatype_get(pt_dt, H5VL_PASSTHRU, &tget, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  H5VL_datatype_specific_args_t tspec{};
  tspec.op_type = H5VL_DATATYPE_FLUSH;
  req = nullptr;
  EXPECT_GE(
      H5VLdatatype_specific(pt_dt, H5VL_PASSTHRU, &tspec, H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(
      H5VLdatatype_optional(pt_dt, H5VL_PASSTHRU, &opt_args, H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLdatatype_close(pt_dt, H5VL_PASSTHRU, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_GE(H5VLdatatype_close(pt_dt2, H5VL_PASSTHRU, H5P_DEFAULT, nullptr), 0);

  // Groups with req != nullptr, including H5VL_GROUP_MOUNT.
  req = nullptr;
  void* pt_grp = H5VLgroup_create(pt_file, &loc, H5VL_PASSTHRU, "g",
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT, &req);
  ASSERT_NE(pt_grp, nullptr);
  consume_req_free(req);

  req = nullptr;
  void* pt_grp2 = H5VLgroup_open(pt_file, &loc, H5VL_PASSTHRU, "g", H5P_DEFAULT,
                                 H5P_DEFAULT, &req);
  ASSERT_NE(pt_grp2, nullptr);
  consume_req_free(req);

  H5VL_group_get_args_t gget{};
  gget.op_type = H5VL_GROUP_GET_GCPL;
  req = nullptr;
  EXPECT_GE(H5VLgroup_get(pt_grp, H5VL_PASSTHRU, &gget, H5P_DEFAULT, &req), 0);
  consume_req_free(req);

  H5VL_group_specific_args_t gspec{};
  gspec.op_type = H5VL_GROUP_MOUNT;
  gspec.args.mount.name = "m";
  gspec.args.mount.child_file = pt_file2;
  gspec.args.mount.fmpl_id = H5P_DEFAULT;
  req = nullptr;
  EXPECT_GE(
      H5VLgroup_specific(pt_grp, H5VL_PASSTHRU, &gspec, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_NE(g_mock_state.last_mount_child, nullptr);
  EXPECT_NE(g_mock_state.last_mount_child, pt_file2);

  gspec.op_type = H5VL_GROUP_FLUSH;
  req = nullptr;
  EXPECT_GE(
      H5VLgroup_specific(pt_grp, H5VL_PASSTHRU, &gspec, H5P_DEFAULT, &req), 0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(
      H5VLgroup_optional(pt_grp, H5VL_PASSTHRU, &opt_args, H5P_DEFAULT, &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLgroup_close(pt_grp, H5VL_PASSTHRU, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_GE(H5VLgroup_close(pt_grp2, H5VL_PASSTHRU, H5P_DEFAULT, nullptr), 0);

  // Links with req != nullptr, plus obj==NULL & curr_obj!=NULL hard link, and
  // src_obj==NULL & dst_obj!=NULL link copy/move.
  H5VL_link_create_args_t lcreate{};
  lcreate.op_type = H5VL_LINK_CREATE_HARD;
  lcreate.args.hard.curr_obj = pt_file2;
  lcreate.args.hard.curr_loc_params = loc;
  req = nullptr;
  EXPECT_GE(H5VLlink_create(&lcreate, nullptr, &loc, H5VL_PASSTHRU, H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);
  EXPECT_NE(g_mock_state.last_hard_curr_obj, nullptr);
  EXPECT_NE(g_mock_state.last_hard_curr_obj, pt_file2);

  lcreate.op_type = H5VL_LINK_CREATE_SOFT;
  lcreate.args.soft.target = "/target";
  req = nullptr;
  EXPECT_GE(H5VLlink_create(&lcreate, pt_file, &loc, H5VL_PASSTHRU, H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLlink_copy(nullptr, &loc, pt_file, &loc, H5VL_PASSTHRU,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);
  EXPECT_EQ(g_mock_state.last_link_copy_src, nullptr);
  EXPECT_NE(g_mock_state.last_link_copy_dst, nullptr);

  req = nullptr;
  EXPECT_GE(H5VLlink_copy(pt_file, &loc, nullptr, &loc, H5VL_PASSTHRU,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_NE(g_mock_state.last_link_copy_src, nullptr);
  EXPECT_EQ(g_mock_state.last_link_copy_dst, nullptr);

  req = nullptr;
  EXPECT_GE(H5VLlink_move(nullptr, &loc, pt_file, &loc, H5VL_PASSTHRU,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, &req),
            0);
  consume_req_free(req);
  EXPECT_EQ(g_mock_state.last_link_move_src, nullptr);
  EXPECT_NE(g_mock_state.last_link_move_dst, nullptr);

  req = nullptr;
  EXPECT_GE(H5VLlink_move(pt_file, &loc, nullptr, &loc, H5VL_PASSTHRU,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_NE(g_mock_state.last_link_move_src, nullptr);
  EXPECT_EQ(g_mock_state.last_link_move_dst, nullptr);

  H5VL_link_get_args_t lget{};
  lget.op_type = H5VL_LINK_GET_INFO;
  req = nullptr;
  EXPECT_GE(
      H5VLlink_get(pt_file, &loc, H5VL_PASSTHRU, &lget, H5P_DEFAULT, &req), 0);
  consume_req_free(req);

  H5VL_link_specific_args_t lspec{};
  lspec.op_type = H5VL_LINK_EXISTS;
  lspec.args.exists.exists = &exists;
  req = nullptr;
  EXPECT_GE(
      H5VLlink_specific(pt_file, &loc, H5VL_PASSTHRU, &lspec, H5P_DEFAULT,
                        &req),
      0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLlink_optional(pt_file, &loc, H5VL_PASSTHRU, &opt_args,
                              H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  // Objects with req != nullptr.
  H5I_type_t opened_type = H5I_BADID;
  req = nullptr;
  void* pt_obj = H5VLobject_open(pt_file, &loc, H5VL_PASSTHRU, &opened_type,
                                 H5P_DEFAULT, &req);
  ASSERT_NE(pt_obj, nullptr);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLobject_copy(pt_file, &loc, "src", pt_file2, &loc, "dst",
                            H5VL_PASSTHRU, H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  H5VL_object_get_args_t oget{};
  H5O_type_t otype = H5O_TYPE_UNKNOWN;
  oget.op_type = H5VL_OBJECT_GET_TYPE;
  oget.args.get_type.obj_type = &otype;
  req = nullptr;
  EXPECT_GE(
      H5VLobject_get(pt_obj, &loc, H5VL_PASSTHRU, &oget, H5P_DEFAULT, &req), 0);
  consume_req_free(req);

  H5VL_object_specific_args_t ospec{};
  ospec.op_type = H5VL_OBJECT_FLUSH;
  req = nullptr;
  EXPECT_GE(H5VLobject_specific(pt_obj, &loc, H5VL_PASSTHRU, &ospec,
                                H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  req = nullptr;
  EXPECT_GE(H5VLobject_optional(pt_obj, &loc, H5VL_PASSTHRU, &opt_args,
                                H5P_DEFAULT, &req),
            0);
  consume_req_free(req);

  EXPECT_GE(H5VLgroup_close(pt_obj, H5VL_PASSTHRU, H5P_DEFAULT, nullptr), 0);

  // Blob operations.
  std::array<uint8_t, 8> blob_id{};
  std::array<uint8_t, 4> blob_in{1, 2, 3, 4};
  std::array<uint8_t, 4> blob_out{};
  EXPECT_GE(H5VLblob_put(pt_file, H5VL_PASSTHRU, blob_in.data(), blob_in.size(),
                         blob_id.data(), nullptr),
            0);
  EXPECT_EQ(blob_id[0], 0xAB);
  EXPECT_GE(H5VLblob_get(pt_file, H5VL_PASSTHRU, blob_id.data(),
                         blob_out.data(), blob_out.size(), nullptr),
            0);
  EXPECT_EQ(blob_out[0], 0x5A);

  H5VL_blob_specific_args_t bspec{};
  bool is_null = false;
  bspec.op_type = H5VL_BLOB_ISNULL;
  bspec.args.is_null.isnull = &is_null;
  EXPECT_GE(H5VLblob_specific(pt_file, H5VL_PASSTHRU, blob_id.data(), &bspec),
            0);
  EXPECT_GE(
      H5VLblob_optional(pt_file, H5VL_PASSTHRU, blob_id.data(), &opt_args), 0);

  // Token operations.
  H5O_token_t tok1{};
  H5O_token_t tok2{};
  tok2.__data[0] = 0x01;
  int tcmp = 0;
  EXPECT_GE(H5VLtoken_cmp(pt_file, H5VL_PASSTHRU, &tok1, &tok2, &tcmp), 0);
  EXPECT_NE(tcmp, 0);

  char* mock_tok_str = nullptr;
  ASSERT_GE(H5VLtoken_to_str(pt_file, H5I_GROUP, H5VL_PASSTHRU, &tok1,
                             &mock_tok_str),
            0);
  ASSERT_NE(mock_tok_str, nullptr);
  absl::Cleanup free_mock_tok = [mock_tok_str] { H5free_memory(mock_tok_str); };
  EXPECT_STREQ(mock_tok_str, "tok0");

  H5O_token_t tok_parsed{};
  EXPECT_GE(H5VLtoken_from_str(pt_file, H5I_GROUP, H5VL_PASSTHRU, mock_tok_str,
                               &tok_parsed),
            0);
  EXPECT_EQ(tok_parsed.__data[0], 0x42);

  // Top-level generic optional (pass req=nullptr per H5VL_pass_through_optional).
  EXPECT_GE(
      H5VLoptional(pt_file, H5VL_PASSTHRU, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_EQ(g_mock_state.generic_optional_calls, 1);

  // Negative under==NULL paths for create/open callbacks over MockVol.
  g_mock_state.emit_req = false;
  g_mock_state.fail_ops = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VLattr_create(pt_file, &loc, H5VL_PASSTHRU, "a",
                              H5T_NATIVE_INT32, H5S_ALL, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLattr_open(pt_file, &loc, H5VL_PASSTHRU, "a", H5P_DEFAULT,
                            H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLdataset_create(pt_file, &loc, H5VL_PASSTHRU, "d", H5P_DEFAULT,
                                 H5T_NATIVE_INT32, H5S_ALL, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLdataset_open(pt_file, &loc, H5VL_PASSTHRU, "d", H5P_DEFAULT,
                               H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLdatatype_commit(pt_file, &loc, H5VL_PASSTHRU, "t",
                                  H5T_NATIVE_INT32, H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLdatatype_open(pt_file, &loc, H5VL_PASSTHRU, "t", H5P_DEFAULT,
                                H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLgroup_create(pt_file, &loc, H5VL_PASSTHRU, "g", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLgroup_open(pt_file, &loc, H5VL_PASSTHRU, "g", H5P_DEFAULT,
                             H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLobject_open(pt_file, &loc, H5VL_PASSTHRU, &opened_type,
                              H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLfile_create("mfile", H5F_ACC_TRUNC, H5P_DEFAULT,
                              pt_fapl.get(), H5P_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VLfile_open("mfile", H5F_ACC_RDWR, pt_fapl.get(), H5P_DEFAULT,
                            nullptr),
              nullptr);
  }
  g_mock_state.fail_ops = false;

  // Close files with async req on one close and nullptr on the other.
  g_mock_state.emit_req = true;
  req = nullptr;
  EXPECT_GE(H5VLfile_close(pt_file, H5VL_PASSTHRU, H5P_DEFAULT, &req), 0);
  consume_req_free(req);
  EXPECT_GE(H5VLfile_close(pt_file2, H5VL_PASSTHRU, H5P_DEFAULT, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
