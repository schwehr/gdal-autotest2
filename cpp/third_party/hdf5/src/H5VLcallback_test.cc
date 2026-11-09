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
// Tests Virtual Object Layer (VOL) callback dispatch wrappers across public
// pass-through APIs, internal wrappers, optional operations, and connector
// info/wrap management (third_party/hdf5/src/H5VLcallback.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLcallback.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ScopedVol = ScopedH5Id<H5VLclose>;

constexpr H5VL_class_value_t kFullMockVolValue = 501;
constexpr H5VL_class_value_t kEmptyMockVolValue = 502;
constexpr H5VL_class_value_t kPlainInfoVolValue = 503;
constexpr char kFullMockVolName[] = "google_test_full_mock_vol";
constexpr char kEmptyMockVolName[] = "google_test_empty_mock_vol";
constexpr char kPlainInfoVolName[] = "google_test_plain_info_mock_vol";
constexpr uint64_t kMockCapFlags =
    H5VL_CAP_FLAG_ATTR_BASIC | H5VL_CAP_FLAG_DATASET_BASIC |
    H5VL_CAP_FLAG_FILE_BASIC | H5VL_CAP_FLAG_GROUP_BASIC | H5VL_CAP_FLAG_ASYNC;

struct MockInfo {
  int magic = 42;
};

struct PlainInfo {
  int a = 1;
  int b = 2;
};

struct MockWrapCtx {
  int ctx_id = 77;
};

struct MockWrappedObj {
  void* inner = nullptr;
  H5I_type_t obj_type = H5I_BADID;
};

struct MockObject {
  void* under = nullptr;
  int tag = 0;
};

struct MockState {
  bool fail_init = false;
  bool fail_term = false;
  bool fail_info_copy = false;
  bool fail_info_cmp = false;
  bool fail_info_free = false;
  bool fail_info_to_str = false;
  bool fail_info_from_str = false;
  bool fail_get_object = false;
  bool fail_get_wrap_ctx = false;
  bool fail_wrap_object = false;
  bool fail_unwrap_object = false;
  bool fail_free_wrap_ctx = false;
  bool fail_callbacks = false;
  bool forward_to_native = false;
  bool set_async_req = false;
  herr_t positive_iter_return = 0;
  int dummy_req = 999;
  int dummy_ret = 123;
  int callback_calls = 0;
  int req_wait_calls = 0;
  int req_free_calls = 0;

  void Reset() { *this = MockState{}; }
};

MockState g_state;
MockObject g_dummy_obj{nullptr, 1};

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  bool pushed() const { return pushed_; }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

ScopedVol GetNativeVol() {
  return ScopedVol(H5VLget_connector_id_by_value(H5_VOL_NATIVE));
}

herr_t MockInit(hid_t /*vipl_id*/) {
  ++g_state.callback_calls;
  return g_state.fail_init ? -1 : 0;
}

herr_t MockTerm() {
  ++g_state.callback_calls;
  return g_state.fail_term ? -1 : 0;
}

void* MockInfoCopy(const void* info) {
  ++g_state.callback_calls;
  if (g_state.fail_info_copy || info == nullptr) return nullptr;
  auto* copy = new MockInfo(*static_cast<const MockInfo*>(info));
  return copy;
}

herr_t MockInfoCmp(int* cmp_value, const void* info1, const void* info2) {
  ++g_state.callback_calls;
  if (g_state.fail_info_cmp) return -1;
  const int v1 = static_cast<const MockInfo*>(info1)->magic;
  const int v2 = static_cast<const MockInfo*>(info2)->magic;
  *cmp_value = (v1 < v2) ? -1 : (v1 > v2 ? 1 : 0);
  return 0;
}

herr_t MockInfoFree(void* info) {
  ++g_state.callback_calls;
  if (g_state.fail_info_free) return -1;
  delete static_cast<MockInfo*>(info);
  return 0;
}

herr_t MockInfoToStr(const void* info, char** str) {
  ++g_state.callback_calls;
  if (g_state.fail_info_to_str) return -1;
  const int magic = static_cast<const MockInfo*>(info)->magic;
  const std::string text = std::to_string(magic);
  char* buf = static_cast<char*>(H5allocate_memory(text.size() + 1, false));
  if (buf == nullptr) return -1;
  std::memcpy(buf, text.c_str(), text.size() + 1);
  *str = buf;
  return 0;
}

herr_t MockInfoFromStr(const char* str, void** info) {
  ++g_state.callback_calls;
  if (g_state.fail_info_from_str) return -1;
  auto* out = new MockInfo();
  out->magic = std::atoi(str);
  *info = out;
  return 0;
}

void* MockWrapGetObject(const void* obj) {
  ++g_state.callback_calls;
  if (g_state.fail_get_object) return nullptr;
  return const_cast<void*>(obj);
}

herr_t MockGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  ++g_state.callback_calls;
  if (g_state.fail_get_wrap_ctx) return -1;
  *wrap_ctx = new MockWrapCtx();
  return 0;
}

void* MockWrapObject(void* obj, H5I_type_t obj_type, void* /*wrap_ctx*/) {
  ++g_state.callback_calls;
  if (g_state.fail_wrap_object) return nullptr;
  return new MockWrappedObj{obj, obj_type};
}

void* MockUnwrapObject(void* obj) {
  ++g_state.callback_calls;
  if (g_state.fail_unwrap_object) return nullptr;
  auto* wrapped = static_cast<MockWrappedObj*>(obj);
  void* inner = wrapped->inner;
  delete wrapped;
  return inner;
}

herr_t MockFreeWrapCtx(void* wrap_ctx) {
  ++g_state.callback_calls;
  if (g_state.fail_free_wrap_ctx) return -1;
  delete static_cast<MockWrapCtx*>(wrap_ctx);
  return 0;
}

// Attribute callbacks.
void* MockAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                     const char* attr_name, hid_t type_id, hid_t space_id,
                     hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.forward_to_native && obj != nullptr) {
    auto* parent = static_cast<MockObject*>(obj);
    if (parent->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      void* under = H5VLattr_create(parent->under, loc_params, native_vol.get(),
                                    attr_name, type_id, space_id, acpl_id,
                                    aapl_id, dxpl_id, req);
      return under ? new MockObject{under, 2} : nullptr;
    }
  }
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return &g_state.dummy_ret;
}

void* MockAttrOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                   const char* /*name*/, hid_t /*aapl_id*/, hid_t /*dxpl_id*/,
                   void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return &g_state.dummy_ret;
}

herr_t MockAttrRead(void* /*attr*/, hid_t /*mem_type_id*/, void* /*buf*/,
                    hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockAttrWrite(void* /*attr*/, hid_t /*mem_type_id*/, const void* /*buf*/,
                     hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockAttrGet(void* /*obj*/, H5VL_attr_get_args_t* /*args*/,
                   hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockAttrSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_attr_specific_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** /*req*/) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  return g_state.positive_iter_return;
}

herr_t MockAttrOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                        hid_t /*dxpl_id*/, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return g_state.positive_iter_return;
}

herr_t MockAttrClose(void* attr, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && attr != &g_dummy_obj &&
      attr != &g_state.dummy_ret && attr != nullptr) {
    auto* m = static_cast<MockObject*>(attr);
    herr_t rc = 0;
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      rc = H5VLattr_close(m->under, native_vol.get(), dxpl_id, req);
    }
    delete m;
    return rc;
  }
  return 0;
}

// Dataset callbacks.
void* MockDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                        const char* name, hid_t lcpl_id, hid_t type_id,
                        hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                        hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.forward_to_native && obj != nullptr) {
    auto* parent = static_cast<MockObject*>(obj);
    if (parent->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      void* under = H5VLdataset_create(parent->under, loc_params,
                                       native_vol.get(), name, lcpl_id, type_id,
                                       space_id, dcpl_id, dapl_id, dxpl_id,
                                       req);
      return under ? new MockObject{under, 3} : nullptr;
    }
  }
  return &g_state.dummy_ret;
}

void* MockDatasetOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                      const char* /*name*/, hid_t /*dapl_id*/,
                      hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? nullptr : &g_state.dummy_ret;
}

herr_t MockDatasetRead(size_t /*count*/, void* /*dset*/[],
                       hid_t /*mem_type_id*/[], hid_t /*mem_space_id*/[],
                       hid_t /*file_space_id*/[], hid_t /*dxpl_id*/,
                       void* /*buf*/[], void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockDatasetWrite(size_t /*count*/, void* /*dset*/[],
                        hid_t /*mem_type_id*/[], hid_t /*mem_space_id*/[],
                        hid_t /*file_space_id*/[], hid_t /*dxpl_id*/,
                        const void* /*buf*/[], void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockDatasetGet(void* /*obj*/, H5VL_dataset_get_args_t* /*args*/,
                      hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockDatasetSpecific(void* /*obj*/,
                           H5VL_dataset_specific_args_t* /*args*/,
                           hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockDatasetOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                           hid_t /*dxpl_id*/, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return 0;
}

herr_t MockDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && dset != &g_dummy_obj &&
      dset != &g_state.dummy_ret && dset != nullptr) {
    auto* m = static_cast<MockObject*>(dset);
    herr_t rc = 0;
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      rc = H5VLdataset_close(m->under, native_vol.get(), dxpl_id, req);
    }
    delete m;
    return rc;
  }
  return 0;
}

// Datatype callbacks.
void* MockDatatypeCommit(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t type_id, hid_t lcpl_id,
                         hid_t tcpl_id, hid_t tapl_id, hid_t dxpl_id,
                         void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.forward_to_native && obj != nullptr) {
    auto* parent = static_cast<MockObject*>(obj);
    if (parent->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      void* under = H5VLdatatype_commit(parent->under, loc_params,
                                        native_vol.get(), name, type_id,
                                        lcpl_id, tcpl_id, tapl_id, dxpl_id,
                                        req);
      return under ? new MockObject{under, 4} : nullptr;
    }
  }
  return &g_state.dummy_ret;
}

void* MockDatatypeOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                       const char* /*name*/, hid_t /*tapl_id*/,
                       hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? nullptr : &g_state.dummy_ret;
}

herr_t MockDatatypeGet(void* obj, H5VL_datatype_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && obj != &g_dummy_obj &&
      obj != &g_state.dummy_ret && obj != nullptr) {
    auto* m = static_cast<MockObject*>(obj);
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      return H5VLdatatype_get(m->under, native_vol.get(), args, dxpl_id, req);
    }
  }
  return 0;
}

herr_t MockDatatypeSpecific(void* /*obj*/,
                            H5VL_datatype_specific_args_t* /*args*/,
                            hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockDatatypeOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                            hid_t /*dxpl_id*/, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return 0;
}

herr_t MockDatatypeClose(void* dt, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && dt != &g_dummy_obj &&
      dt != &g_state.dummy_ret && dt != nullptr) {
    auto* m = static_cast<MockObject*>(dt);
    herr_t rc = 0;
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      rc = H5VLdatatype_close(m->under, native_vol.get(), dxpl_id, req);
    }
    delete m;
    return rc;
  }
  return 0;
}

// File callbacks.
void* MockFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                     hid_t fapl_id, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.forward_to_native) {
    ScopedVol native_vol = GetNativeVol();
    ScopedPlist under_fapl(H5Pcopy(fapl_id));
    if (!under_fapl.valid()) return nullptr;
    if (H5Pset_vol(under_fapl.get(), native_vol.get(), nullptr) < 0) {
      return nullptr;
    }
    void* under = H5VLfile_create(name, flags, fcpl_id, under_fapl.get(),
                                  dxpl_id, req);
    return under ? new MockObject{under, 1} : nullptr;
  }
  return &g_state.dummy_ret;
}

void* MockFileOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl_id*/,
                   hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? nullptr : &g_state.dummy_ret;
}

herr_t MockFileGet(void* obj, H5VL_file_get_args_t* args, hid_t dxpl_id,
                   void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && obj != &g_dummy_obj &&
      obj != &g_state.dummy_ret && obj != nullptr) {
    auto* m = static_cast<MockObject*>(obj);
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      return H5VLfile_get(m->under, native_vol.get(), args, dxpl_id, req);
    }
  }
  return 0;
}

herr_t MockFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                        hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && obj != &g_dummy_obj &&
      obj != &g_state.dummy_ret && obj != nullptr) {
    auto* m = static_cast<MockObject*>(obj);
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      return H5VLfile_specific(m->under, native_vol.get(), args, dxpl_id, req);
    }
  }
  return 0;
}

herr_t MockFileOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                        hid_t /*dxpl_id*/, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return 0;
}

herr_t MockFileClose(void* file, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && file != &g_dummy_obj &&
      file != &g_state.dummy_ret && file != nullptr) {
    auto* m = static_cast<MockObject*>(file);
    herr_t rc = 0;
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      rc = H5VLfile_close(m->under, native_vol.get(), dxpl_id, req);
    }
    delete m;
    return rc;
  }
  return 0;
}

// Group callbacks.
void* MockGroupCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* name, hid_t lcpl_id, hid_t gcpl_id,
                      hid_t gapl_id, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (g_state.forward_to_native && obj != nullptr) {
    auto* parent = static_cast<MockObject*>(obj);
    if (parent->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      void* under =
          H5VLgroup_create(parent->under, loc_params, native_vol.get(), name,
                           lcpl_id, gcpl_id, gapl_id, dxpl_id, req);
      return under ? new MockObject{under, 5} : nullptr;
    }
  }
  return &g_state.dummy_ret;
}

void* MockGroupOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                    const char* /*name*/, hid_t /*gapl_id*/, hid_t /*dxpl_id*/,
                    void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? nullptr : &g_state.dummy_ret;
}

herr_t MockGroupGet(void* /*obj*/, H5VL_group_get_args_t* /*args*/,
                    hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockGroupSpecific(void* /*obj*/, H5VL_group_specific_args_t* /*args*/,
                         hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockGroupOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                         hid_t /*dxpl_id*/, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return g_state.positive_iter_return;
}

herr_t MockGroupClose(void* grp, hid_t dxpl_id, void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.forward_to_native && grp != &g_dummy_obj &&
      grp != &g_state.dummy_ret && grp != nullptr) {
    auto* m = static_cast<MockObject*>(grp);
    herr_t rc = 0;
    if (m->under != nullptr) {
      ScopedVol native_vol = GetNativeVol();
      rc = H5VLgroup_close(m->under, native_vol.get(), dxpl_id, req);
    }
    delete m;
    return rc;
  }
  return 0;
}

// Link callbacks.
herr_t MockLinkCreate(H5VL_link_create_args_t* /*args*/, void* /*obj*/,
                      const H5VL_loc_params_t* /*loc_params*/,
                      hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                      void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockLinkCopy(void* /*src_obj*/,
                    const H5VL_loc_params_t* /*loc_params1*/, void* /*dst_obj*/,
                    const H5VL_loc_params_t* /*loc_params2*/, hid_t /*lcpl_id*/,
                    hid_t /*lapl_id*/, hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockLinkMove(void* /*src_obj*/,
                    const H5VL_loc_params_t* /*loc_params1*/, void* /*dst_obj*/,
                    const H5VL_loc_params_t* /*loc_params2*/, hid_t /*lcpl_id*/,
                    hid_t /*lapl_id*/, hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockLinkGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                   H5VL_link_get_args_t* /*args*/, hid_t /*dxpl_id*/,
                   void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockLinkSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_link_specific_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** /*req*/) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  return g_state.positive_iter_return;
}

herr_t MockLinkOptional(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_optional_args_t* /*args*/, hid_t /*dxpl_id*/,
                        void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return 0;
}

// Object callbacks.
void* MockObjectOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5I_type_t* opened_type, hid_t /*dxpl_id*/,
                     void** /*req*/) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return nullptr;
  if (opened_type != nullptr) *opened_type = H5I_GROUP;
  return &g_state.dummy_ret;
}

herr_t MockObjectCopy(void* /*src_obj*/,
                      const H5VL_loc_params_t* /*src_loc_params*/,
                      const char* /*src_name*/, void* /*dst_obj*/,
                      const H5VL_loc_params_t* /*dst_loc_params*/,
                      const char* /*dst_name*/, hid_t /*ocpypl_id*/,
                      hid_t /*lcpl_id*/, hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockObjectGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5VL_object_get_args_t* /*args*/, hid_t /*dxpl_id*/,
                     void** /*req*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockObjectSpecific(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_object_specific_args_t* /*args*/,
                          hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  return g_state.positive_iter_return;
}

herr_t MockObjectOptional(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_optional_args_t* /*args*/, hid_t /*dxpl_id*/,
                          void** req) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (g_state.set_async_req && req != nullptr) *req = &g_state.dummy_req;
  return 0;
}

// Introspection callbacks.
herr_t MockIntrospectGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                const H5VL_class_t** conn_cls) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  *conn_cls = nullptr;
  return 0;
}

herr_t MockIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  *cap_flags = kMockCapFlags;
  return 0;
}

herr_t MockIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/,
                              int /*opt_type*/, uint64_t* flags) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (flags != nullptr) *flags = H5VL_OPT_QUERY_SUPPORTED;
  return 0;
}

// Request callbacks.
herr_t MockRequestWait(void* /*req*/, uint64_t /*timeout*/,
                       H5VL_request_status_t* status) {
  ++g_state.callback_calls;
  ++g_state.req_wait_calls;
  if (g_state.fail_callbacks) return -1;
  *status = H5VL_REQUEST_STATUS_SUCCEED;
  return 0;
}

herr_t MockRequestNotify(void* /*req*/, H5VL_request_notify_t cb, void* ctx) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (cb != nullptr) cb(ctx, H5VL_REQUEST_STATUS_SUCCEED);
  return 0;
}

herr_t MockRequestCancel(void* /*req*/, H5VL_request_status_t* status) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  if (status != nullptr) *status = H5VL_REQUEST_STATUS_CANCELED;
  return 0;
}

herr_t MockRequestSpecific(void* /*req*/,
                           H5VL_request_specific_args_t* /*args*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockRequestOptional(void* /*req*/, H5VL_optional_args_t* /*args*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockRequestFree(void* /*req*/) {
  ++g_state.callback_calls;
  ++g_state.req_free_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

// Blob callbacks.
herr_t MockBlobPut(void* /*obj*/, const void* /*buf*/, size_t /*size*/,
                   void* /*blob_id*/, void* /*ctx*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockBlobGet(void* /*obj*/, const void* /*blob_id*/, void* /*buf*/,
                   size_t /*size*/, void* /*ctx*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockBlobSpecific(void* /*obj*/, void* /*blob_id*/,
                        H5VL_blob_specific_args_t* /*args*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

herr_t MockBlobOptional(void* /*obj*/, void* /*blob_id*/,
                        H5VL_optional_args_t* /*args*/) {
  ++g_state.callback_calls;
  return g_state.fail_callbacks ? -1 : 0;
}

// Token callbacks.
herr_t MockTokenCmp(void* /*obj*/, const H5O_token_t* token1,
                    const H5O_token_t* token2, int* cmp_value) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  *cmp_value = std::memcmp(token1, token2, sizeof(H5O_token_t));
  return 0;
}

herr_t MockTokenToStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                      const H5O_token_t* /*token*/, char** token_str) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  constexpr char kTok[] = "mock_tok";
  char* buf = static_cast<char*>(H5allocate_memory(sizeof(kTok), false));
  if (buf == nullptr) return -1;
  std::memcpy(buf, kTok, sizeof(kTok));
  *token_str = buf;
  return 0;
}

herr_t MockTokenFromStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                        const char* /*token_str*/, H5O_token_t* token) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  *token = H5O_TOKEN_UNDEF;
  token->__data[0] = 0x55;
  return 0;
}

herr_t MockTopLevelOptional(void* /*obj*/, H5VL_optional_args_t* /*args*/,
                            hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_state.callback_calls;
  if (g_state.fail_callbacks) return -1;
  return g_state.positive_iter_return;
}

H5VL_class_t MakeFullMockClass() {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = kFullMockVolValue;
  cls.name = kFullMockVolName;
  cls.conn_version = 1;
  cls.cap_flags = kMockCapFlags;
  cls.initialize = MockInit;
  cls.terminate = MockTerm;

  cls.info_cls.size = sizeof(MockInfo);
  cls.info_cls.copy = MockInfoCopy;
  cls.info_cls.cmp = MockInfoCmp;
  cls.info_cls.free = MockInfoFree;
  cls.info_cls.to_str = MockInfoToStr;
  cls.info_cls.from_str = MockInfoFromStr;

  cls.wrap_cls.get_object = MockWrapGetObject;
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

  cls.optional = MockTopLevelOptional;
  return cls;
}

H5VL_class_t MakeEmptyMockClass() {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = kEmptyMockVolValue;
  cls.name = kEmptyMockVolName;
  cls.conn_version = 2;
  cls.cap_flags = H5VL_CAP_FLAG_NONE;
  return cls;
}

H5VL_class_t MakePlainInfoMockClass() {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = kPlainInfoVolValue;
  cls.name = kPlainInfoVolName;
  cls.conn_version = 3;
  cls.cap_flags = H5VL_CAP_FLAG_THREADSAFE;
  cls.info_cls.size = sizeof(PlainInfo);
  return cls;
}

class H5VLCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(H5open(), 0);
    g_state.Reset();
    full_cls_ = MakeFullMockClass();
    empty_cls_ = MakeEmptyMockClass();
    plain_cls_ = MakePlainInfoMockClass();

    full_vol_.reset(H5VLregister_connector(&full_cls_, H5P_DEFAULT));
    ASSERT_TRUE(full_vol_.valid());
    empty_vol_.reset(H5VLregister_connector(&empty_cls_, H5P_DEFAULT));
    ASSERT_TRUE(empty_vol_.valid());
    plain_vol_.reset(H5VLregister_connector(&plain_cls_, H5P_DEFAULT));
    ASSERT_TRUE(plain_vol_.valid());

    full_conn_ =
        static_cast<H5VL_connector_t*>(H5I_object_verify(full_vol_, H5I_VOL));
    empty_conn_ =
        static_cast<H5VL_connector_t*>(H5I_object_verify(empty_vol_, H5I_VOL));
    ASSERT_NE(full_conn_, nullptr);
    ASSERT_NE(empty_conn_, nullptr);
  }

  void TearDown() override {
    g_state.Reset();
    if (full_vol_.valid()) {
      H5VLunregister_connector(full_vol_.get());
      full_vol_.reset();
    }
    if (empty_vol_.valid()) {
      H5VLunregister_connector(empty_vol_.get());
      empty_vol_.reset();
    }
    if (plain_vol_.valid()) {
      H5VLunregister_connector(plain_vol_.get());
      plain_vol_.reset();
    }
  }

  H5VL_object_t MakeFullVolObj() {
    H5VL_object_t obj{};
    obj.data = &g_dummy_obj;
    obj.connector = full_conn_;
    obj.rc = 1;
    return obj;
  }

  H5VL_object_t MakeEmptyVolObj() {
    H5VL_object_t obj{};
    obj.data = &g_dummy_obj;
    obj.connector = empty_conn_;
    obj.rc = 1;
    return obj;
  }

  H5VL_class_t full_cls_{};
  H5VL_class_t empty_cls_{};
  H5VL_class_t plain_cls_{};
  ScopedVol full_vol_;
  ScopedVol empty_vol_;
  ScopedVol plain_vol_;
  H5VL_connector_t* full_conn_ = nullptr;
  H5VL_connector_t* empty_conn_ = nullptr;
};

TEST_F(H5VLCallbackTest, LifecycleInitializeTerminateCapsAndValue) {
  EXPECT_GE(H5VLinitialize(full_vol_, H5P_DEFAULT), 0);
  EXPECT_GE(H5VLterminate(full_vol_), 0);
  EXPECT_GE(H5VLinitialize(empty_vol_, H5P_DEFAULT), 0);
  EXPECT_GE(H5VLterminate(empty_vol_), 0);

  uint64_t cap_flags = 0;
  ASSERT_GE(H5VLget_cap_flags(full_vol_, &cap_flags), 0);
  EXPECT_EQ(cap_flags, kMockCapFlags);
  EXPECT_GE(H5VLget_cap_flags(full_vol_, nullptr), 0);

  H5VL_class_value_t conn_val = H5_VOL_INVALID;
  ASSERT_GE(H5VLget_value(full_vol_, &conn_val), 0);
  EXPECT_EQ(conn_val, kFullMockVolValue);
  EXPECT_GE(H5VLget_value(full_vol_, nullptr), 0);

  ScopedErrorSilence silence;
  g_state.fail_init = true;
  EXPECT_LT(H5VLinitialize(full_vol_, H5P_DEFAULT), 0);
  g_state.fail_init = false;

  g_state.fail_term = true;
  EXPECT_LT(H5VLterminate(full_vol_), 0);
  g_state.fail_term = false;

  EXPECT_LT(H5VLinitialize(H5I_INVALID_HID, H5P_DEFAULT), 0);
  EXPECT_LT(H5VLterminate(H5I_INVALID_HID), 0);
  EXPECT_LT(H5VLget_cap_flags(H5I_INVALID_HID, &cap_flags), 0);
  EXPECT_LT(H5VLget_value(H5I_INVALID_HID, &conn_val), 0);
}

TEST_F(H5VLCallbackTest, ConnectorInfoCopyCmpFreeAndStringConversions) {
  // Custom callbacks.
  MockInfo src_info{123};
  void* copied_info = nullptr;
  ASSERT_GE(H5VLcopy_connector_info(full_vol_, &copied_info, &src_info), 0);
  ASSERT_NE(copied_info, nullptr);
  EXPECT_EQ(static_cast<MockInfo*>(copied_info)->magic, 123);

  int cmp_val = 99;
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, full_vol_, &src_info, copied_info),
            0);
  EXPECT_EQ(cmp_val, 0);
  MockInfo higher_info{200};
  ASSERT_GE(
      H5VLcmp_connector_info(&cmp_val, full_vol_, &src_info, &higher_info), 0);
  EXPECT_LT(cmp_val, 0);
  EXPECT_GE(H5VLcmp_connector_info(nullptr, full_vol_, &src_info, copied_info),
            0);

  // NULL pointer combinations for comparison.
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, full_vol_, nullptr, copied_info),
            0);
  EXPECT_EQ(cmp_val, -1);
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, full_vol_, copied_info, nullptr),
            0);
  EXPECT_EQ(cmp_val, 1);
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, full_vol_, nullptr, nullptr), 0);
  EXPECT_EQ(cmp_val, 0);

  // NULL src_info copy & NULL info free.
  void* null_copy = &src_info;
  ASSERT_GE(H5VLcopy_connector_info(full_vol_, &null_copy, nullptr), 0);
  EXPECT_EQ(null_copy, nullptr);
  EXPECT_GE(H5VLfree_connector_info(full_vol_, nullptr), 0);

  // String serialization and deserialization.
  char* info_str = nullptr;
  ASSERT_GE(H5VLconnector_info_to_str(&src_info, full_vol_, &info_str), 0);
  ASSERT_NE(info_str, nullptr);
  EXPECT_STREQ(info_str, "123");
  H5free_memory(info_str);
  info_str = nullptr;

  ASSERT_GE(H5VLconnector_info_to_str(nullptr, H5I_INVALID_HID, &info_str), 0);
  EXPECT_EQ(info_str, nullptr);

  void* parsed_info = nullptr;
  ASSERT_GE(H5VLconnector_str_to_info("789", full_vol_, &parsed_info), 0);
  ASSERT_NE(parsed_info, nullptr);
  EXPECT_EQ(static_cast<MockInfo*>(parsed_info)->magic, 789);
  EXPECT_GE(H5VLfree_connector_info(full_vol_, parsed_info), 0);

  void* null_parsed = &src_info;
  ASSERT_GE(H5VLconnector_str_to_info(nullptr, full_vol_, &null_parsed), 0);
  EXPECT_EQ(null_parsed, nullptr);

  // Fallback memcpy / memcmp / H5MM_xfree_const when callbacks are NULL and
  // size > 0.
  PlainInfo p1{10, 20};
  PlainInfo p2{10, 30};
  void* plain_copy = nullptr;
  ASSERT_GE(H5VLcopy_connector_info(plain_vol_, &plain_copy, &p1), 0);
  ASSERT_NE(plain_copy, nullptr);
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, plain_vol_, &p1, plain_copy), 0);
  EXPECT_EQ(cmp_val, 0);
  ASSERT_GE(H5VLcmp_connector_info(&cmp_val, plain_vol_, &p1, &p2), 0);
  EXPECT_NE(cmp_val, 0);
  EXPECT_GE(H5VLfree_connector_info(plain_vol_, plain_copy), 0);

  // Missing to_str / from_str callbacks return nullptr outputs cleanly.
  EXPECT_GE(H5VLconnector_info_to_str(&p1, plain_vol_, &info_str), 0);
  EXPECT_EQ(info_str, nullptr);
  EXPECT_GE(H5VLconnector_str_to_info("abc", plain_vol_, &parsed_info), 0);
  EXPECT_EQ(parsed_info, nullptr);

  // Error paths.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLcopy_connector_info(H5I_INVALID_HID, &null_copy, &src_info),
              0);
    EXPECT_LT(H5VLcmp_connector_info(&cmp_val, H5I_INVALID_HID, &p1, &p2), 0);
    EXPECT_LT(H5VLfree_connector_info(H5I_INVALID_HID, copied_info), 0);
    EXPECT_LT(H5VLconnector_info_to_str(&src_info, H5I_INVALID_HID, &info_str),
              0);
    EXPECT_LT(H5VLconnector_str_to_info("1", H5I_INVALID_HID, &parsed_info), 0);

    // size == 0 and copy == nullptr -> UNSUPPORTED.
    EXPECT_LT(H5VLcopy_connector_info(empty_vol_, &null_copy, &src_info), 0);

    g_state.fail_info_copy = true;
    EXPECT_LT(H5VLcopy_connector_info(full_vol_, &null_copy, &src_info), 0);
    g_state.fail_info_copy = false;

    g_state.fail_info_cmp = true;
    EXPECT_LT(
        H5VL_cmp_connector_info(full_conn_, &cmp_val, &src_info, copied_info),
        0);
    // H5VLcmp_connector_info does not check H5VL_cmp_connector_info's return.
    EXPECT_GE(
        H5VLcmp_connector_info(&cmp_val, full_vol_, &src_info, copied_info), 0);
    g_state.fail_info_cmp = false;

    g_state.fail_info_to_str = true;
    EXPECT_LT(H5VLconnector_info_to_str(&src_info, full_vol_, &info_str), 0);
    g_state.fail_info_to_str = false;

    g_state.fail_info_from_str = true;
    EXPECT_LT(H5VLconnector_str_to_info("1", full_vol_, &parsed_info), 0);
    g_state.fail_info_from_str = false;

    g_state.fail_info_free = true;
    EXPECT_LT(H5VLfree_connector_info(full_vol_, copied_info), 0);
    g_state.fail_info_free = false;
  }

  EXPECT_GE(H5VLfree_connector_info(full_vol_, copied_info), 0);
}

TEST_F(H5VLCallbackTest, ObjectWrapUnwrapAndGetWrapCtx) {
  int raw_obj = 55;
  EXPECT_EQ(H5VLget_object(&raw_obj, full_vol_), &raw_obj);
  EXPECT_EQ(H5VLget_object(&raw_obj, empty_vol_), &raw_obj);

  void* wrap_ctx = nullptr;
  ASSERT_GE(H5VLget_wrap_ctx(&raw_obj, empty_vol_, &wrap_ctx), 0);
  EXPECT_EQ(wrap_ctx, nullptr);

  // wrap_object/unwrap_object with NULL wrap_ctx or missing wrap_object.
  EXPECT_EQ(H5VLwrap_object(&raw_obj, H5I_DATASET, full_vol_, nullptr),
            &raw_obj);
  EXPECT_EQ(H5VLunwrap_object(&raw_obj, empty_vol_), &raw_obj);

  // Full wrap -> unwrap round trip.
  ASSERT_GE(H5VLget_wrap_ctx(&raw_obj, full_vol_, &wrap_ctx), 0);
  ASSERT_NE(wrap_ctx, nullptr);
  void* wrapped = H5VLwrap_object(&raw_obj, H5I_DATASET, full_vol_, wrap_ctx);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(H5VLunwrap_object(wrapped, full_vol_), &raw_obj);
  EXPECT_GE(H5VLfree_wrap_ctx(wrap_ctx, full_vol_), 0);
  EXPECT_GE(H5VLfree_wrap_ctx(nullptr, full_vol_), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5VLget_object(nullptr, full_vol_), nullptr);
  EXPECT_EQ(H5VLget_object(&raw_obj, H5I_INVALID_HID), nullptr);
  EXPECT_LT(H5VLget_wrap_ctx(&raw_obj, H5I_INVALID_HID, &wrap_ctx), 0);
  EXPECT_EQ(H5VLwrap_object(nullptr, H5I_DATASET, full_vol_, nullptr), nullptr);
  EXPECT_EQ(H5VLwrap_object(&raw_obj, H5I_DATASET, H5I_INVALID_HID, nullptr),
            nullptr);
  EXPECT_EQ(H5VLunwrap_object(nullptr, full_vol_), nullptr);
  EXPECT_EQ(H5VLunwrap_object(&raw_obj, H5I_INVALID_HID), nullptr);
  EXPECT_LT(H5VLfree_wrap_ctx(nullptr, H5I_INVALID_HID), 0);

  g_state.fail_get_wrap_ctx = true;
  EXPECT_LT(H5VLget_wrap_ctx(&raw_obj, full_vol_, &wrap_ctx), 0);
  g_state.fail_get_wrap_ctx = false;

  ASSERT_GE(H5VLget_wrap_ctx(&raw_obj, full_vol_, &wrap_ctx), 0);
  g_state.fail_wrap_object = true;
  EXPECT_EQ(H5VLwrap_object(&raw_obj, H5I_DATASET, full_vol_, wrap_ctx),
            nullptr);
  g_state.fail_wrap_object = false;

  g_state.fail_unwrap_object = true;
  EXPECT_EQ(H5VLunwrap_object(&raw_obj, full_vol_), nullptr);
  g_state.fail_unwrap_object = false;

  g_state.fail_free_wrap_ctx = true;
  EXPECT_LT(H5VLfree_wrap_ctx(wrap_ctx, full_vol_), 0);
  g_state.fail_free_wrap_ctx = false;
  EXPECT_GE(H5VLfree_wrap_ctx(wrap_ctx, full_vol_), 0);
}

TEST_F(H5VLCallbackTest, AttributePassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5VL_attr_get_args_t get_args{};
  H5VL_attr_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};
  int buf = 0;

  // Layer 3 public passthrough wrappers: positive.
  void* attr =
      H5VLattr_create(&g_dummy_obj, &loc, full_vol_, "a", H5T_NATIVE_INT,
                      H5S_ALL, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr);
  EXPECT_NE(attr, nullptr);
  void* opened = H5VLattr_open(&g_dummy_obj, &loc, full_vol_, "a", H5P_DEFAULT,
                               H5P_DEFAULT, nullptr);
  EXPECT_NE(opened, nullptr);
  EXPECT_GE(
      H5VLattr_read(attr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(H5VLattr_write(attr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT,
                           nullptr),
            0);
  EXPECT_GE(H5VLattr_get(attr, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(
      H5VLattr_specific(attr, &loc, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      1);
  EXPECT_EQ(
      H5VLattr_optional(attr, full_vol_, &opt_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VLattr_close(attr, full_vol_, H5P_DEFAULT, nullptr), 0);

  // Layer 2 internal wrappers inside an active API context.
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();
  H5VL_object_t empty_obj = MakeEmptyVolObj();

  EXPECT_NE(H5VL_attr_create(&full_obj, &loc, "a", H5T_NATIVE_INT, H5S_ALL,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_NE(H5VL_attr_open(&full_obj, &loc, "a", H5P_DEFAULT, H5P_DEFAULT,
                           nullptr),
            nullptr);
  EXPECT_GE(H5VL_attr_read(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(
      H5VL_attr_write(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(H5VL_attr_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(H5VL_attr_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
            1);
  EXPECT_EQ(H5VL_attr_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VL_attr_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  // Invalid args / bad connector ID on Layer 3.
  EXPECT_EQ(H5VLattr_create(nullptr, &loc, full_vol_, "a", H5T_NATIVE_INT,
                            H5S_ALL, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_EQ(H5VLattr_create(&g_dummy_obj, &loc, H5I_INVALID_HID, "a",
                            H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLattr_open(nullptr, &loc, full_vol_, "a", H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLattr_open(&g_dummy_obj, &loc, H5I_INVALID_HID, "a", H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(
      H5VLattr_read(nullptr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_read(attr, H5I_INVALID_HID, H5T_NATIVE_INT, &buf,
                          H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLattr_write(nullptr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT,
                           nullptr),
            0);
  EXPECT_LT(H5VLattr_write(attr, H5I_INVALID_HID, H5T_NATIVE_INT, &buf,
                           H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLattr_get(nullptr, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLattr_get(attr, H5I_INVALID_HID, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLattr_get(attr, full_vol_, nullptr, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLattr_specific(nullptr, &loc, full_vol_, &spec_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(H5VLattr_specific(attr, &loc, H5I_INVALID_HID, &spec_args,
                              H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLattr_optional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_optional(attr, H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(H5VLattr_close(nullptr, full_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLattr_close(attr, H5I_INVALID_HID, H5P_DEFAULT, nullptr), 0);

  // Missing callbacks (empty connector) on Layer 2 & 3.
  EXPECT_EQ(H5VLattr_create(&g_dummy_obj, &loc, empty_vol_, "a", H5T_NATIVE_INT,
                            H5S_ALL, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_EQ(H5VLattr_open(&g_dummy_obj, &loc, empty_vol_, "a", H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(
      H5VLattr_read(attr, empty_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_write(attr, empty_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT,
                           nullptr),
            0);
  EXPECT_LT(H5VLattr_get(attr, empty_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLattr_specific(attr, &loc, empty_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_optional(attr, empty_vol_, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLattr_close(attr, empty_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_attr_read(&empty_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT,
                           nullptr),
            0);

  // Failing callbacks on both Layer 2 and Layer 3.
  g_state.fail_callbacks = true;
  EXPECT_EQ(H5VL_attr_create(&full_obj, &loc, "a", H5T_NATIVE_INT, H5S_ALL,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_attr_open(&full_obj, &loc, "a", H5P_DEFAULT, H5P_DEFAULT,
                           nullptr),
            nullptr);
  EXPECT_LT(H5VL_attr_read(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_attr_write(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VL_attr_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_attr_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_attr_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_attr_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  EXPECT_EQ(H5VLattr_create(&g_dummy_obj, &loc, full_vol_, "a", H5T_NATIVE_INT,
                            H5S_ALL, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_EQ(H5VLattr_open(&g_dummy_obj, &loc, full_vol_, "a", H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(
      H5VLattr_read(attr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_write(attr, full_vol_, H5T_NATIVE_INT, &buf, H5P_DEFAULT,
                           nullptr),
            0);
  EXPECT_LT(H5VLattr_get(attr, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLattr_specific(attr, &loc, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLattr_optional(attr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLattr_close(attr, full_vol_, H5P_DEFAULT, nullptr), 0);
  g_state.fail_callbacks = false;

  // Fail H5VL_set_vol_wrapper inside Layer 2 attr ops.
  g_state.fail_get_wrap_ctx = true;
  EXPECT_EQ(H5VL_attr_create(&full_obj, &loc, "a", H5T_NATIVE_INT, H5S_ALL,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_attr_open(&full_obj, &loc, "a", H5P_DEFAULT, H5P_DEFAULT,
                           nullptr),
            nullptr);
  EXPECT_LT(H5VL_attr_read(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_attr_write(&full_obj, H5T_NATIVE_INT, &buf, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VL_attr_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_attr_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_attr_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_attr_close(&full_obj, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, DatasetPassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5VL_dataset_get_args_t get_args{};
  H5VL_dataset_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};
  int data1 = 1;
  int data2 = 2;
  void* dsets[2] = {&g_dummy_obj, &g_dummy_obj};
  void* bad_dsets[2] = {&g_dummy_obj, nullptr};
  hid_t mtypes[2] = {H5T_NATIVE_INT, H5T_NATIVE_INT};
  hid_t spaces[2] = {H5S_ALL, H5S_ALL};
  void* rbufs[2] = {&data1, &data2};
  const void* wbufs[2] = {&data1, &data2};

  void* dset = H5VLdataset_create(&g_dummy_obj, &loc, full_vol_, "d",
                                  H5P_DEFAULT, H5T_NATIVE_INT, H5S_ALL,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                  nullptr);
  EXPECT_NE(dset, nullptr);
  EXPECT_NE(H5VLdataset_open(&g_dummy_obj, &loc, full_vol_, "d", H5P_DEFAULT,
                             H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_GE(H5VLdataset_read(2, dsets, full_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_GE(H5VLdataset_write(2, dsets, full_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_GE(H5VLdataset_get(dset, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(
      H5VLdataset_specific(dset, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(
      H5VLdataset_optional(dset, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(H5VLdataset_close(dset, full_vol_, H5P_DEFAULT, nullptr), 0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();

  EXPECT_NE(H5VL_dataset_create(&full_obj, &loc, "d", H5P_DEFAULT,
                                H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_NE(H5VL_dataset_open(&full_obj, &loc, "d", H5P_DEFAULT, H5P_DEFAULT,
                              nullptr),
            nullptr);
  EXPECT_GE(H5VL_dataset_read(2, dsets, full_conn_, mtypes, spaces, spaces,
                              H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_GE(H5VL_dataset_write(2, dsets, full_conn_, mtypes, spaces, spaces,
                               H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_GE(H5VL_dataset_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VL_dataset_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_dataset_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_dataset_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  // Layer 3 arg validation.
  EXPECT_EQ(H5VLdataset_create(nullptr, &loc, full_vol_, "d", H5P_DEFAULT,
                               H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdataset_create(&g_dummy_obj, &loc, H5I_INVALID_HID, "d",
                               H5P_DEFAULT, H5T_NATIVE_INT, H5S_ALL,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdataset_open(nullptr, &loc, full_vol_, "d", H5P_DEFAULT,
                             H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdataset_open(&g_dummy_obj, &loc, H5I_INVALID_HID, "d",
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLdataset_read(1, nullptr, full_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(2, bad_dsets, full_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(1, dsets, full_vol_, nullptr, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(1, dsets, full_vol_, mtypes, nullptr, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(1, dsets, full_vol_, mtypes, spaces, nullptr,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(1, dsets, full_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, nullptr, nullptr),
            0);
  EXPECT_LT(H5VLdataset_read(1, dsets, H5I_INVALID_HID, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, nullptr, full_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(2, bad_dsets, full_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, full_vol_, nullptr, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, full_vol_, mtypes, nullptr, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, full_vol_, mtypes, spaces, nullptr,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, full_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, nullptr, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, H5I_INVALID_HID, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_get(nullptr, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLdataset_get(dset, H5I_INVALID_HID, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLdataset_specific(nullptr, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdataset_specific(dset, H5I_INVALID_HID, &spec_args, H5P_DEFAULT,
                                 nullptr),
            0);
  EXPECT_LT(
      H5VLdataset_optional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdataset_optional(dset, H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                 nullptr),
            0);
  EXPECT_LT(H5VLdataset_close(nullptr, full_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLdataset_close(dset, H5I_INVALID_HID, H5P_DEFAULT, nullptr), 0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_EQ(H5VLdataset_create(&g_dummy_obj, &loc, empty_vol_, "d", H5P_DEFAULT,
                               H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdataset_open(&g_dummy_obj, &loc, empty_vol_, "d", H5P_DEFAULT,
                             H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLdataset_read(1, dsets, empty_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, empty_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_get(dset, empty_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLdataset_specific(dset, empty_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLdataset_optional(dset, empty_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdataset_close(dset, empty_vol_, H5P_DEFAULT, nullptr), 0);

  // Failing callbacks on Layer 2 & 3.
  g_state.fail_callbacks = true;
  EXPECT_EQ(H5VL_dataset_create(&full_obj, &loc, "d", H5P_DEFAULT,
                                H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_dataset_open(&full_obj, &loc, "d", H5P_DEFAULT, H5P_DEFAULT,
                              nullptr),
            nullptr);
  EXPECT_LT(H5VL_dataset_read(1, dsets, full_conn_, mtypes, spaces, spaces,
                              H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_write(1, dsets, full_conn_, mtypes, spaces, spaces,
                               H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_dataset_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  EXPECT_EQ(H5VLdataset_create(&g_dummy_obj, &loc, full_vol_, "d", H5P_DEFAULT,
                               H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdataset_open(&g_dummy_obj, &loc, full_vol_, "d", H5P_DEFAULT,
                             H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLdataset_read(1, dsets, full_vol_, mtypes, spaces, spaces,
                             H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_write(1, dsets, full_vol_, mtypes, spaces, spaces,
                              H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VLdataset_get(dset, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLdataset_specific(dset, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLdataset_optional(dset, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdataset_close(dset, full_vol_, H5P_DEFAULT, nullptr), 0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_EQ(H5VL_dataset_create(&full_obj, &loc, "d", H5P_DEFAULT,
                                H5T_NATIVE_INT, H5S_ALL, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_dataset_open(&full_obj, &loc, "d", H5P_DEFAULT, H5P_DEFAULT,
                              nullptr),
            nullptr);
  EXPECT_LT(H5VL_dataset_read(1, dsets, full_conn_, mtypes, spaces, spaces,
                              H5P_DEFAULT, rbufs, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_write(1, dsets, full_conn_, mtypes, spaces, spaces,
                               H5P_DEFAULT, wbufs, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_dataset_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_dataset_close(&full_obj, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, DatatypePassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5VL_datatype_get_args_t get_args{};
  H5VL_datatype_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};

  void* dt =
      H5VLdatatype_commit(&g_dummy_obj, &loc, full_vol_, "t", H5T_NATIVE_INT,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                          nullptr);
  EXPECT_NE(dt, nullptr);
  EXPECT_NE(H5VLdatatype_open(&g_dummy_obj, &loc, full_vol_, "t", H5P_DEFAULT,
                              H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_GE(H5VLdatatype_get(dt, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(
      H5VLdatatype_specific(dt, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(
      H5VLdatatype_optional(dt, full_vol_, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VLdatatype_close(dt, full_vol_, H5P_DEFAULT, nullptr), 0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();
  H5VL_object_t* out_obj_ptr = nullptr;

  EXPECT_NE(H5VL_datatype_commit(&full_obj, &loc, "t", H5T_NATIVE_INT,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_NE(H5VL_datatype_open(&full_obj, &loc, "t", H5P_DEFAULT, H5P_DEFAULT,
                               nullptr),
            nullptr);
  EXPECT_GE(H5VL_datatype_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VL_datatype_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_datatype_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_datatype_optional_op(&full_obj, &opt_args, H5P_DEFAULT, nullptr,
                                      &out_obj_ptr),
            0);
  EXPECT_EQ(out_obj_ptr, &full_obj);
  EXPECT_GE(H5VL_datatype_optional_op(&full_obj, &opt_args, H5P_DEFAULT, nullptr,
                                      nullptr),
            0);
  EXPECT_GE(H5VL_datatype_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5VLdatatype_commit(nullptr, &loc, full_vol_, "t", H5T_NATIVE_INT,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdatatype_commit(&g_dummy_obj, &loc, H5I_INVALID_HID, "t",
                                H5T_NATIVE_INT, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdatatype_open(nullptr, &loc, full_vol_, "t", H5P_DEFAULT,
                              H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdatatype_open(&g_dummy_obj, &loc, H5I_INVALID_HID, "t",
                              H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(
      H5VLdatatype_get(nullptr, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLdatatype_get(dt, H5I_INVALID_HID, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdatatype_specific(nullptr, full_vol_, &spec_args, H5P_DEFAULT,
                                  nullptr),
            0);
  EXPECT_LT(H5VLdatatype_specific(dt, H5I_INVALID_HID, &spec_args, H5P_DEFAULT,
                                  nullptr),
            0);
  EXPECT_LT(
      H5VLdatatype_optional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLdatatype_optional(dt, H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                  nullptr),
            0);
  EXPECT_LT(H5VLdatatype_close(nullptr, full_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLdatatype_close(dt, H5I_INVALID_HID, H5P_DEFAULT, nullptr), 0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_EQ(H5VLdatatype_commit(&g_dummy_obj, &loc, empty_vol_, "t",
                                H5T_NATIVE_INT, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdatatype_open(&g_dummy_obj, &loc, empty_vol_, "t", H5P_DEFAULT,
                              H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLdatatype_get(dt, empty_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLdatatype_specific(dt, empty_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLdatatype_optional(dt, empty_vol_, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLdatatype_close(dt, empty_vol_, H5P_DEFAULT, nullptr), 0);

  // Failing callbacks on Layer 2 & 3.
  g_state.fail_callbacks = true;
  EXPECT_EQ(H5VL_datatype_commit(&full_obj, &loc, "t", H5T_NATIVE_INT,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_datatype_open(&full_obj, &loc, "t", H5P_DEFAULT, H5P_DEFAULT,
                               nullptr),
            nullptr);
  EXPECT_LT(H5VL_datatype_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_datatype_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_datatype_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_datatype_optional_op(&full_obj, &opt_args, H5P_DEFAULT, nullptr,
                                &out_obj_ptr),
      0);
  EXPECT_LT(H5VL_datatype_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  EXPECT_EQ(H5VLdatatype_commit(&g_dummy_obj, &loc, full_vol_, "t",
                                H5T_NATIVE_INT, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLdatatype_open(&g_dummy_obj, &loc, full_vol_, "t", H5P_DEFAULT,
                              H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLdatatype_get(dt, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLdatatype_specific(dt, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLdatatype_optional(dt, full_vol_, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLdatatype_close(dt, full_vol_, H5P_DEFAULT, nullptr), 0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_EQ(H5VL_datatype_commit(&full_obj, &loc, "t", H5T_NATIVE_INT,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_datatype_open(&full_obj, &loc, "t", H5P_DEFAULT, H5P_DEFAULT,
                               nullptr),
            nullptr);
  EXPECT_LT(H5VL_datatype_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_datatype_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_datatype_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_datatype_optional_op(&full_obj, &opt_args, H5P_DEFAULT, nullptr,
                                nullptr),
      0);
  EXPECT_LT(H5VL_datatype_close(&full_obj, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, FilePassthruAndInternalWrappers) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  MockInfo info{9};
  ASSERT_GE(H5Pset_vol(fapl.get(), full_vol_, &info), 0);

  ScopedPlist empty_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(empty_fapl.valid());
  ASSERT_GE(H5Pset_vol(empty_fapl.get(), empty_vol_, nullptr), 0);

  ScopedPlist non_fapl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(non_fapl.valid());

  H5VL_file_get_args_t get_args{};
  H5VL_file_specific_args_t flush_args{};
  flush_args.op_type = H5VL_FILE_FLUSH;
  flush_args.args.flush.obj_type = H5I_FILE;
  flush_args.args.flush.scope = H5F_SCOPE_LOCAL;
  H5VL_optional_args_t opt_args{};

  void* file_obj = H5VLfile_create("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                   fapl.get(), H5P_DEFAULT, nullptr);
  EXPECT_NE(file_obj, nullptr);
  void* opened_file =
      H5VLfile_open("mock.h5", H5F_ACC_RDONLY, fapl.get(), H5P_DEFAULT, nullptr);
  EXPECT_NE(opened_file, nullptr);
  EXPECT_GE(H5VLfile_get(file_obj, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(
      H5VLfile_specific(file_obj, full_vol_, &flush_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(H5VLfile_specific(nullptr, full_vol_, &flush_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_GE(
      H5VLfile_optional(file_obj, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_GE(H5VLfile_close(file_obj, full_vol_, H5P_DEFAULT, nullptr), 0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();

  EXPECT_NE(H5VL_file_create(full_conn_, "mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get(), H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_NE(H5VL_file_open(full_conn_, "mock.h5", H5F_ACC_RDONLY, fapl.get(),
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_GE(H5VL_file_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VL_file_specific(&full_obj, &flush_args, H5P_DEFAULT, nullptr),
            0);

  // Special IS_ACCESSIBLE / DELETE paths with vol_obj == nullptr.
  bool accessible = false;
  H5VL_file_specific_args_t acc_args{};
  acc_args.op_type = H5VL_FILE_IS_ACCESSIBLE;
  acc_args.args.is_accessible.filename = "mock.h5";
  acc_args.args.is_accessible.fapl_id = fapl.get();
  acc_args.args.is_accessible.accessible = &accessible;
  EXPECT_GE(H5VL_file_specific(nullptr, &acc_args, H5P_DEFAULT, nullptr), 0);

  H5VL_file_specific_args_t del_args{};
  del_args.op_type = H5VL_FILE_DELETE;
  del_args.args.del.filename = "mock.h5";
  del_args.args.del.fapl_id = fapl.get();
  EXPECT_GE(H5VL_file_specific(nullptr, &del_args, H5P_DEFAULT, nullptr), 0);

  EXPECT_GE(H5VL_file_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VL_file_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  // Invalid FAPL / connector checks.
  EXPECT_EQ(H5VLfile_create("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            H5I_INVALID_HID, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLfile_create("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            non_fapl.get(), H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLfile_open("mock.h5", H5F_ACC_RDONLY, H5I_INVALID_HID,
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLfile_open("mock.h5", H5F_ACC_RDONLY, non_fapl.get(),
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLfile_get(nullptr, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLfile_get(file_obj, H5I_INVALID_HID, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLfile_specific(file_obj, H5I_INVALID_HID, &flush_args,
                              H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLfile_optional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLfile_optional(file_obj, H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(H5VLfile_close(nullptr, full_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLfile_close(file_obj, H5I_INVALID_HID, H5P_DEFAULT, nullptr), 0);

  // Invalid FAPL in H5VL_file_specific(IS_ACCESSIBLE / DELETE).
  acc_args.args.is_accessible.fapl_id = H5I_INVALID_HID;
  EXPECT_LT(H5VL_file_specific(nullptr, &acc_args, H5P_DEFAULT, nullptr), 0);
  acc_args.args.is_accessible.fapl_id = non_fapl.get();
  EXPECT_LT(H5VL_file_specific(nullptr, &acc_args, H5P_DEFAULT, nullptr), 0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_EQ(H5VLfile_create("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            empty_fapl.get(), H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLfile_open("mock.h5", H5F_ACC_RDONLY, empty_fapl.get(),
                          H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLfile_get(file_obj, empty_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLfile_specific(file_obj, empty_vol_, &flush_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(
      H5VLfile_optional(file_obj, empty_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLfile_close(file_obj, empty_vol_, H5P_DEFAULT, nullptr), 0);

  // Failing callbacks on Layer 2 & 3 + default VOL H5VL_file_open fallback.
  g_state.fail_callbacks = true;
  EXPECT_EQ(H5VL_file_create(full_conn_, "mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get(), H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_file_open(full_conn_, "mock.h5", H5F_ACC_RDONLY, fapl.get(),
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VL_file_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_file_specific(&full_obj, &flush_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_file_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_file_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  EXPECT_EQ(H5VLfile_create("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get(),
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLfile_open("mock.h5", H5F_ACC_RDONLY, fapl.get(), H5P_DEFAULT,
                          nullptr),
            nullptr);
  EXPECT_LT(H5VLfile_get(file_obj, full_vol_, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLfile_specific(file_obj, full_vol_, &flush_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLfile_optional(file_obj, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLfile_close(file_obj, full_vol_, H5P_DEFAULT, nullptr), 0);
  g_state.fail_callbacks = false;

  // Default VOL open failure invokes H5PL_iterate fallback cleanly.
  {
    TempFile missing("missing_default_vol_open.h5");
    ScopedVol native_vol = GetNativeVol();
    ASSERT_TRUE(native_vol.valid());
    auto* native_conn =
        static_cast<H5VL_connector_t*>(H5I_object_verify(native_vol, H5I_VOL));
    ASSERT_NE(native_conn, nullptr);
    EXPECT_EQ(H5VL_file_open(native_conn, missing.c_str(), H5F_ACC_RDONLY,
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
              nullptr);
  }

  g_state.fail_get_wrap_ctx = true;
  EXPECT_LT(H5VL_file_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_file_specific(&full_obj, &flush_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_file_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_file_close(&full_obj, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, GroupPassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5VL_group_get_args_t get_args{};
  H5VL_group_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};

  void* grp =
      H5VLgroup_create(&g_dummy_obj, &loc, full_vol_, "g", H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr);
  EXPECT_NE(grp, nullptr);
  EXPECT_NE(H5VLgroup_open(&g_dummy_obj, &loc, full_vol_, "g", H5P_DEFAULT,
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_GE(H5VLgroup_get(grp, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(
      H5VLgroup_specific(grp, full_vol_, &spec_args, H5P_DEFAULT, nullptr), 0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(
      H5VLgroup_optional(grp, full_vol_, &opt_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VLgroup_close(grp, full_vol_, H5P_DEFAULT, nullptr), 0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();

  EXPECT_NE(H5VL_group_create(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_NE(H5VL_group_open(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_GE(H5VL_group_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_GE(H5VL_group_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(H5VL_group_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VL_group_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5VLgroup_create(nullptr, &loc, full_vol_, "g", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLgroup_create(&g_dummy_obj, &loc, H5I_INVALID_HID, "g",
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                             nullptr),
            nullptr);
  EXPECT_EQ(H5VLgroup_open(nullptr, &loc, full_vol_, "g", H5P_DEFAULT,
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLgroup_open(&g_dummy_obj, &loc, H5I_INVALID_HID, "g",
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(
      H5VLgroup_get(nullptr, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_get(grp, H5I_INVALID_HID, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_specific(nullptr, full_vol_, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLgroup_specific(grp, H5I_INVALID_HID, &spec_args, H5P_DEFAULT,
                               nullptr),
            0);
  EXPECT_LT(
      H5VLgroup_optional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLgroup_optional(grp, H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                               nullptr),
            0);
  EXPECT_LT(H5VLgroup_close(nullptr, full_vol_, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLgroup_close(grp, H5I_INVALID_HID, H5P_DEFAULT, nullptr), 0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_EQ(H5VLgroup_create(&g_dummy_obj, &loc, empty_vol_, "g", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLgroup_open(&g_dummy_obj, &loc, empty_vol_, "g", H5P_DEFAULT,
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLgroup_get(grp, empty_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_specific(grp, empty_vol_, &spec_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_optional(grp, empty_vol_, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLgroup_close(grp, empty_vol_, H5P_DEFAULT, nullptr), 0);

  // Failing callbacks.
  g_state.fail_callbacks = true;
  EXPECT_EQ(H5VL_group_create(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_group_open(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_LT(H5VL_group_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_group_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_group_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_group_close(&full_obj, H5P_DEFAULT, nullptr), 0);

  EXPECT_EQ(H5VLgroup_create(&g_dummy_obj, &loc, full_vol_, "g", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VLgroup_open(&g_dummy_obj, &loc, full_vol_, "g", H5P_DEFAULT,
                           H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLgroup_get(grp, full_vol_, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_specific(grp, full_vol_, &spec_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLgroup_optional(grp, full_vol_, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VLgroup_close(grp, full_vol_, H5P_DEFAULT, nullptr), 0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_EQ(H5VL_group_create(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL_group_open(&full_obj, &loc, "g", H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_LT(H5VL_group_get(&full_obj, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_group_specific(&full_obj, &spec_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_group_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5VL_group_close(&full_obj, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, LinkPassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5VL_link_create_args_t create_args{};
  create_args.op_type = H5VL_LINK_CREATE_SOFT;
  create_args.args.soft.target = "target";
  H5VL_link_get_args_t get_args{};
  H5VL_link_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};

  EXPECT_GE(H5VLlink_create(&create_args, &g_dummy_obj, &loc, full_vol_,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VLlink_create(&create_args, nullptr, &loc, full_vol_, H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VLlink_copy(&g_dummy_obj, &loc, &g_dummy_obj, &loc, full_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VLlink_copy(nullptr, &loc, nullptr, &loc, full_vol_, H5P_DEFAULT,
                          H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VLlink_move(&g_dummy_obj, &loc, &g_dummy_obj, &loc, full_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VLlink_move(nullptr, &loc, nullptr, &loc, full_vol_, H5P_DEFAULT,
                          H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(
      H5VLlink_get(&g_dummy_obj, &loc, full_vol_, &get_args, H5P_DEFAULT, nullptr),
      0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(H5VLlink_specific(&g_dummy_obj, &loc, full_vol_, &spec_args,
                              H5P_DEFAULT, nullptr),
            1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VLlink_optional(&g_dummy_obj, &loc, full_vol_, &opt_args,
                              H5P_DEFAULT, nullptr),
            0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();
  H5VL_object_t null_data_obj{};
  null_data_obj.data = nullptr;
  null_data_obj.connector = full_conn_;
  null_data_obj.rc = 1;

  EXPECT_GE(H5VL_link_create(&create_args, &full_obj, &loc, H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  H5VL_link_create_args_t hard_args{};
  hard_args.op_type = H5VL_LINK_CREATE_HARD;
  hard_args.args.hard.curr_obj = &g_dummy_obj;
  EXPECT_GE(H5VL_link_create(&hard_args, &null_data_obj, &loc, H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);

  EXPECT_GE(H5VL_link_copy(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_link_copy(&full_obj, &loc, nullptr, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_link_copy(&null_data_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);

  EXPECT_GE(H5VL_link_move(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_link_move(&full_obj, &loc, nullptr, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);

  EXPECT_GE(
      H5VL_link_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr), 0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(
      H5VL_link_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(
      H5VL_link_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5VLlink_create(&create_args, &g_dummy_obj, &loc, H5I_INVALID_HID,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_copy(&g_dummy_obj, &loc, &g_dummy_obj, &loc,
                          H5I_INVALID_HID, H5P_DEFAULT, H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_move(&g_dummy_obj, &loc, &g_dummy_obj, &loc,
                          H5I_INVALID_HID, H5P_DEFAULT, H5P_DEFAULT,
                          H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLlink_get(nullptr, &loc, full_vol_, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLlink_get(&g_dummy_obj, &loc, H5I_INVALID_HID, &get_args,
                         H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_specific(nullptr, &loc, full_vol_, &spec_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(H5VLlink_specific(&g_dummy_obj, &loc, H5I_INVALID_HID, &spec_args,
                              H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_optional(nullptr, &loc, full_vol_, &opt_args, H5P_DEFAULT,
                              nullptr),
            0);
  EXPECT_LT(H5VLlink_optional(&g_dummy_obj, &loc, H5I_INVALID_HID, &opt_args,
                              H5P_DEFAULT, nullptr),
            0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_LT(H5VLlink_create(&create_args, &g_dummy_obj, &loc, empty_vol_,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_copy(&g_dummy_obj, &loc, &g_dummy_obj, &loc, empty_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_move(&g_dummy_obj, &loc, &g_dummy_obj, &loc, empty_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_get(&g_dummy_obj, &loc, empty_vol_, &get_args, H5P_DEFAULT,
                         nullptr),
            0);
  EXPECT_LT(H5VLlink_specific(&g_dummy_obj, &loc, empty_vol_, &spec_args,
                              H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_optional(&g_dummy_obj, &loc, empty_vol_, &opt_args,
                              H5P_DEFAULT, nullptr),
            0);

  // Failing callbacks.
  g_state.fail_callbacks = true;
  EXPECT_LT(H5VL_link_create(&create_args, &full_obj, &loc, H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_link_copy(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_link_move(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_link_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VL_link_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VL_link_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr), 0);

  EXPECT_LT(H5VLlink_create(&create_args, &g_dummy_obj, &loc, full_vol_,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_copy(&g_dummy_obj, &loc, &g_dummy_obj, &loc, full_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_move(&g_dummy_obj, &loc, &g_dummy_obj, &loc, full_vol_,
                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLlink_get(&g_dummy_obj, &loc, full_vol_, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLlink_specific(&g_dummy_obj, &loc, full_vol_, &spec_args,
                              H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLlink_optional(&g_dummy_obj, &loc, full_vol_, &opt_args,
                              H5P_DEFAULT, nullptr),
            0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_LT(H5VL_link_create(&create_args, &full_obj, &loc, H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_link_copy(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_link_move(&full_obj, &loc, &full_obj, &loc, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_link_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VL_link_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VL_link_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, ObjectPassthruAndInternalWrappers) {
  H5VL_loc_params_t loc{};
  loc.obj_type = H5I_GROUP;
  loc.type = H5VL_OBJECT_BY_SELF;
  H5I_type_t opened_type = H5I_BADID;
  H5VL_object_get_args_t get_args{};
  H5VL_object_specific_args_t spec_args{};
  H5VL_optional_args_t opt_args{};

  EXPECT_NE(H5VLobject_open(&g_dummy_obj, &loc, full_vol_, &opened_type,
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(opened_type, H5I_GROUP);
  EXPECT_GE(H5VLobject_copy(&g_dummy_obj, &loc, "a", &g_dummy_obj, &loc, "b",
                            full_vol_, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            0);
  EXPECT_GE(H5VLobject_get(&g_dummy_obj, &loc, full_vol_, &get_args,
                           H5P_DEFAULT, nullptr),
            0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(H5VLobject_specific(&g_dummy_obj, &loc, full_vol_, &spec_args,
                                H5P_DEFAULT, nullptr),
            1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(H5VLobject_optional(&g_dummy_obj, &loc, full_vol_, &opt_args,
                                H5P_DEFAULT, nullptr),
            0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();
  H5VL_object_t empty_obj = MakeEmptyVolObj();

  EXPECT_NE(H5VL_object_open(&full_obj, &loc, &opened_type, H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_GE(H5VL_object_copy(&full_obj, &loc, "a", &full_obj, &loc, "b",
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_GE(H5VL_object_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr),
            0);
  g_state.positive_iter_return = 1;
  EXPECT_EQ(
      H5VL_object_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
      1);
  g_state.positive_iter_return = 0;
  EXPECT_GE(
      H5VL_object_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr),
      0);

  ScopedErrorSilence silence;
  // Cross-connector H5VL_object_copy must fail.
  EXPECT_LT(H5VL_object_copy(&full_obj, &loc, "a", &empty_obj, &loc, "b",
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);

  // Layer 3 arg checks.
  EXPECT_EQ(H5VLobject_open(nullptr, &loc, full_vol_, &opened_type, H5P_DEFAULT,
                            nullptr),
            nullptr);
  EXPECT_EQ(H5VLobject_open(&g_dummy_obj, &loc, H5I_INVALID_HID, &opened_type,
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLobject_copy(nullptr, &loc, "a", &g_dummy_obj, &loc, "b",
                            full_vol_, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            0);
  EXPECT_LT(H5VLobject_copy(&g_dummy_obj, &loc, "a", nullptr, &loc, "b",
                            full_vol_, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            0);
  EXPECT_LT(H5VLobject_copy(&g_dummy_obj, &loc, "a", &g_dummy_obj, &loc, "b",
                            H5I_INVALID_HID, H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLobject_get(nullptr, &loc, full_vol_, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VLobject_get(&g_dummy_obj, &loc, H5I_INVALID_HID, &get_args,
                           H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_specific(nullptr, &loc, full_vol_, &spec_args,
                                H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_specific(&g_dummy_obj, &loc, H5I_INVALID_HID, &spec_args,
                                H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_optional(nullptr, &loc, full_vol_, &opt_args,
                                H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_optional(&g_dummy_obj, &loc, H5I_INVALID_HID, &opt_args,
                                H5P_DEFAULT, nullptr),
            0);

  // Empty connector -> UNSUPPORTED.
  EXPECT_EQ(H5VLobject_open(&g_dummy_obj, &loc, empty_vol_, &opened_type,
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLobject_copy(&g_dummy_obj, &loc, "a", &g_dummy_obj, &loc, "b",
                            empty_vol_, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            0);
  EXPECT_LT(H5VLobject_get(&g_dummy_obj, &loc, empty_vol_, &get_args,
                           H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_specific(&g_dummy_obj, &loc, empty_vol_, &spec_args,
                                H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_optional(&g_dummy_obj, &loc, empty_vol_, &opt_args,
                                H5P_DEFAULT, nullptr),
            0);

  // Failing callbacks.
  g_state.fail_callbacks = true;
  EXPECT_EQ(
      H5VL_object_open(&full_obj, &loc, &opened_type, H5P_DEFAULT, nullptr),
      nullptr);
  EXPECT_LT(H5VL_object_copy(&full_obj, &loc, "a", &full_obj, &loc, "b",
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_object_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_object_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VL_object_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr),
      0);

  EXPECT_EQ(H5VLobject_open(&g_dummy_obj, &loc, full_vol_, &opened_type,
                            H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_LT(H5VLobject_copy(&g_dummy_obj, &loc, "a", &g_dummy_obj, &loc, "b",
                            full_vol_, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                            nullptr),
            0);
  EXPECT_LT(H5VLobject_get(&g_dummy_obj, &loc, full_vol_, &get_args,
                           H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_specific(&g_dummy_obj, &loc, full_vol_, &spec_args,
                                H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VLobject_optional(&g_dummy_obj, &loc, full_vol_, &opt_args,
                                H5P_DEFAULT, nullptr),
            0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_EQ(
      H5VL_object_open(&full_obj, &loc, &opened_type, H5P_DEFAULT, nullptr),
      nullptr);
  EXPECT_LT(H5VL_object_copy(&full_obj, &loc, "a", &full_obj, &loc, "b",
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL_object_get(&full_obj, &loc, &get_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VL_object_specific(&full_obj, &loc, &spec_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VL_object_optional(&full_obj, &loc, &opt_args, H5P_DEFAULT, nullptr),
      0);
}

TEST_F(H5VLCallbackTest, IntrospectRequestBlobTokenAndOptional) {
  const H5VL_class_t* queried_cls = nullptr;
  uint64_t cap_flags = 0;
  uint64_t opt_flags = 0;

  // Introspect public passthrough.
  EXPECT_GE(H5VLintrospect_get_conn_cls(&g_dummy_obj, full_vol_,
                                        H5VL_GET_CONN_LVL_CURR, &queried_cls),
            0);
  EXPECT_GE(H5VLintrospect_get_cap_flags(nullptr, full_vol_, &cap_flags), 0);
  EXPECT_EQ(cap_flags, kMockCapFlags);
  EXPECT_GE(H5VLintrospect_opt_query(&g_dummy_obj, full_vol_,
                                     H5VL_SUBCLS_DATASET, 1025, &opt_flags),
            0);
  EXPECT_EQ(opt_flags, H5VL_OPT_QUERY_SUPPORTED);
  EXPECT_GE(H5VLintrospect_opt_query(nullptr, full_vol_, H5VL_SUBCLS_NONE, 0,
                                     &opt_flags),
            0);

  // Request public passthrough + H5VLrequest_optional_op.
  int req_tok = 7;
  H5VL_request_status_t status = H5VL_REQUEST_STATUS_IN_PROGRESS;
  H5VL_request_specific_args_t req_spec{};
  H5VL_optional_args_t opt_args{};
  bool notified = false;
  auto notify_cb = [](void* ctx, H5VL_request_status_t st) -> herr_t {
    if (st == H5VL_REQUEST_STATUS_SUCCEED) {
      *static_cast<bool*>(ctx) = true;
    }
    return 0;
  };
  EXPECT_GE(H5VLrequest_wait(&req_tok, full_vol_, 0, &status), 0);
  EXPECT_EQ(status, H5VL_REQUEST_STATUS_SUCCEED);
  EXPECT_GE(H5VLrequest_notify(&req_tok, full_vol_, notify_cb, &notified), 0);
  EXPECT_TRUE(notified);
  EXPECT_GE(H5VLrequest_cancel(&req_tok, full_vol_, &status), 0);
  EXPECT_GE(H5VLrequest_specific(&req_tok, full_vol_, &req_spec), 0);
  EXPECT_GE(H5VLrequest_optional(&req_tok, full_vol_, &opt_args), 0);
  EXPECT_GE(H5VLrequest_optional_op(&req_tok, full_vol_, &opt_args), 0);
  EXPECT_GE(H5VLrequest_free(&req_tok, full_vol_), 0);

  // Blob public passthrough.
  uint8_t blob_buf[8] = {1, 2, 3, 4};
  uint8_t blob_id[4] = {9, 8, 7, 6};
  H5VL_blob_specific_args_t blob_spec{};
  EXPECT_GE(H5VLblob_put(&g_dummy_obj, full_vol_, blob_buf, sizeof(blob_buf),
                         blob_id, nullptr),
            0);
  EXPECT_GE(H5VLblob_get(&g_dummy_obj, full_vol_, blob_id, blob_buf,
                         sizeof(blob_buf), nullptr),
            0);
  EXPECT_GE(H5VLblob_specific(&g_dummy_obj, full_vol_, blob_id, &blob_spec), 0);
  EXPECT_GE(H5VLblob_optional(&g_dummy_obj, full_vol_, blob_id, &opt_args), 0);

  // Token public passthrough + empty fallback.
  H5O_token_t t1 = H5O_TOKEN_UNDEF;
  H5O_token_t t2 = H5O_TOKEN_UNDEF;
  t1.__data[0] = 1;
  t2.__data[0] = 2;
  int cmp_val = 0;
  char* tok_str = nullptr;
  H5O_token_t parsed_tok{};
  EXPECT_GE(H5VLtoken_cmp(&g_dummy_obj, full_vol_, &t1, &t2, &cmp_val), 0);
  EXPECT_LT(cmp_val, 0);
  EXPECT_GE(H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, full_vol_, &t1, &tok_str),
            0);
  ASSERT_NE(tok_str, nullptr);
  H5free_memory(tok_str);
  tok_str = nullptr;
  EXPECT_GE(
      H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, full_vol_, "abc", &parsed_tok),
      0);

  // Token NULL & empty fallback paths.
  EXPECT_GE(H5VLtoken_cmp(&g_dummy_obj, empty_vol_, nullptr, &t2, &cmp_val), 0);
  EXPECT_EQ(cmp_val, -1);
  EXPECT_GE(H5VLtoken_cmp(&g_dummy_obj, empty_vol_, &t1, nullptr, &cmp_val), 0);
  EXPECT_EQ(cmp_val, 1);
  EXPECT_GE(H5VLtoken_cmp(&g_dummy_obj, empty_vol_, nullptr, nullptr, &cmp_val),
            0);
  EXPECT_EQ(cmp_val, 0);
  EXPECT_GE(H5VLtoken_cmp(&g_dummy_obj, empty_vol_, &t1, &t2, &cmp_val), 0);
  EXPECT_LT(cmp_val, 0);
  EXPECT_GE(
      H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, empty_vol_, &t1, &tok_str), 0);
  EXPECT_EQ(tok_str, nullptr);
  EXPECT_GE(H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, empty_vol_, "abc",
                               &parsed_tok),
            0);

  // Top-level H5VLoptional.
  g_state.positive_iter_return = 1;
  EXPECT_EQ(
      H5VLoptional(&g_dummy_obj, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      1);
  g_state.positive_iter_return = 0;

  // Layer 2 internal wrappers for introspect, request, blob, token, optional.
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5VL_object_t full_obj = MakeFullVolObj();

  EXPECT_GE(H5VL_introspect_get_conn_cls(&full_obj, H5VL_GET_CONN_LVL_CURR,
                                         &queried_cls),
            0);
  EXPECT_GE(H5VL_introspect_get_cap_flags(nullptr, &full_cls_, &cap_flags), 0);
  EXPECT_GE(H5VL_introspect_opt_query(&full_obj, H5VL_SUBCLS_FILE, 1025,
                                      &opt_flags),
            0);

  EXPECT_GE(H5VL_request_wait(&full_obj, 0, &status), 0);
  EXPECT_GE(H5VL_request_notify(&full_obj, notify_cb, &notified), 0);
  EXPECT_GE(H5VL_request_cancel(&full_obj, &status), 0);
  EXPECT_GE(H5VL_request_specific(&full_obj, &req_spec), 0);
  EXPECT_GE(H5VL_request_optional(&full_obj, &opt_args), 0);
  EXPECT_GE(H5VL_request_free(&full_obj), 0);

  EXPECT_GE(
      H5VL_blob_put(&full_obj, blob_buf, sizeof(blob_buf), blob_id, nullptr),
      0);
  EXPECT_GE(
      H5VL_blob_get(&full_obj, blob_id, blob_buf, sizeof(blob_buf), nullptr),
      0);
  EXPECT_GE(H5VL_blob_specific(&full_obj, blob_id, &blob_spec), 0);
  EXPECT_GE(H5VL_blob_optional(&full_obj, blob_id, &opt_args), 0);

  EXPECT_GE(H5VL_token_cmp(&full_obj, &t1, &t2, &cmp_val), 0);
  EXPECT_GE(H5VL_token_to_str(&full_obj, H5I_GROUP, &t1, &tok_str), 0);
  H5free_memory(tok_str);
  tok_str = nullptr;
  EXPECT_GE(H5VL_token_from_str(&full_obj, H5I_GROUP, "abc", &parsed_tok), 0);

  g_state.positive_iter_return = 1;
  EXPECT_EQ(H5VL_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 1);
  g_state.positive_iter_return = 0;

  ScopedErrorSilence silence;
  // Layer 3 arg checks & empty/failing paths.
  EXPECT_LT(H5VLintrospect_get_conn_cls(nullptr, full_vol_,
                                        H5VL_GET_CONN_LVL_CURR, &queried_cls),
            0);
  EXPECT_LT(H5VLintrospect_get_conn_cls(&g_dummy_obj, full_vol_,
                                        H5VL_GET_CONN_LVL_CURR, nullptr),
            0);
  EXPECT_LT(H5VLintrospect_get_conn_cls(&g_dummy_obj, H5I_INVALID_HID,
                                        H5VL_GET_CONN_LVL_CURR, &queried_cls),
            0);
  EXPECT_LT(H5VLintrospect_get_conn_cls(&g_dummy_obj, empty_vol_,
                                        H5VL_GET_CONN_LVL_CURR, &queried_cls),
            0);
  EXPECT_LT(H5VLintrospect_get_cap_flags(nullptr, full_vol_, nullptr), 0);
  EXPECT_LT(H5VLintrospect_get_cap_flags(nullptr, H5I_INVALID_HID, &cap_flags),
            0);
  EXPECT_LT(H5VLintrospect_get_cap_flags(nullptr, empty_vol_, &cap_flags), 0);
  EXPECT_LT(H5VLintrospect_opt_query(&g_dummy_obj, H5I_INVALID_HID,
                                     H5VL_SUBCLS_NONE, 0, &opt_flags),
            0);
  EXPECT_LT(H5VLintrospect_opt_query(&g_dummy_obj, empty_vol_, H5VL_SUBCLS_NONE,
                                     0, &opt_flags),
            0);

  EXPECT_LT(H5VLrequest_wait(&req_tok, H5I_INVALID_HID, 0, &status), 0);
  EXPECT_LT(H5VLrequest_wait(&req_tok, empty_vol_, 0, &status), 0);
  EXPECT_LT(H5VLrequest_notify(&req_tok, H5I_INVALID_HID, notify_cb, &notified),
            0);
  EXPECT_LT(H5VLrequest_notify(&req_tok, empty_vol_, notify_cb, &notified), 0);
  EXPECT_LT(H5VLrequest_cancel(&req_tok, H5I_INVALID_HID, &status), 0);
  EXPECT_LT(H5VLrequest_cancel(&req_tok, empty_vol_, &status), 0);
  EXPECT_LT(H5VLrequest_specific(&req_tok, H5I_INVALID_HID, &req_spec), 0);
  EXPECT_LT(H5VLrequest_specific(&req_tok, empty_vol_, &req_spec), 0);
  EXPECT_LT(H5VLrequest_optional(&req_tok, H5I_INVALID_HID, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional(&req_tok, empty_vol_, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional_op(nullptr, full_vol_, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional_op(&req_tok, full_vol_, nullptr), 0);
  EXPECT_LT(H5VLrequest_optional_op(&req_tok, H5I_INVALID_HID, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional_op(&req_tok, empty_vol_, &opt_args), 0);
  EXPECT_LT(H5VLrequest_free(&req_tok, H5I_INVALID_HID), 0);
  EXPECT_LT(H5VLrequest_free(&req_tok, empty_vol_), 0);

  EXPECT_LT(H5VLblob_put(nullptr, full_vol_, blob_buf, sizeof(blob_buf),
                         blob_id, nullptr),
            0);
  EXPECT_LT(H5VLblob_put(&g_dummy_obj, H5I_INVALID_HID, blob_buf,
                         sizeof(blob_buf), blob_id, nullptr),
            0);
  EXPECT_LT(H5VLblob_put(&g_dummy_obj, empty_vol_, blob_buf, sizeof(blob_buf),
                         blob_id, nullptr),
            0);
  EXPECT_LT(H5VLblob_get(nullptr, full_vol_, blob_id, blob_buf,
                         sizeof(blob_buf), nullptr),
            0);
  EXPECT_LT(H5VLblob_get(&g_dummy_obj, H5I_INVALID_HID, blob_id, blob_buf,
                         sizeof(blob_buf), nullptr),
            0);
  EXPECT_LT(H5VLblob_get(&g_dummy_obj, empty_vol_, blob_id, blob_buf,
                         sizeof(blob_buf), nullptr),
            0);
  EXPECT_LT(H5VLblob_specific(nullptr, full_vol_, blob_id, &blob_spec), 0);
  EXPECT_LT(H5VLblob_specific(&g_dummy_obj, H5I_INVALID_HID, blob_id,
                              &blob_spec),
            0);
  EXPECT_LT(
      H5VLblob_specific(&g_dummy_obj, empty_vol_, blob_id, &blob_spec), 0);
  EXPECT_LT(H5VLblob_optional(nullptr, full_vol_, blob_id, &opt_args), 0);
  EXPECT_LT(
      H5VLblob_optional(&g_dummy_obj, H5I_INVALID_HID, blob_id, &opt_args), 0);
  EXPECT_LT(H5VLblob_optional(&g_dummy_obj, empty_vol_, blob_id, &opt_args), 0);

  EXPECT_LT(H5VLtoken_cmp(nullptr, full_vol_, &t1, &t2, &cmp_val), 0);
  EXPECT_LT(H5VLtoken_cmp(&g_dummy_obj, H5I_INVALID_HID, &t1, &t2, &cmp_val),
            0);
  EXPECT_LT(H5VLtoken_cmp(&g_dummy_obj, full_vol_, &t1, &t2, nullptr), 0);
  EXPECT_LT(H5VLtoken_to_str(nullptr, H5I_GROUP, full_vol_, &t1, &tok_str), 0);
  EXPECT_LT(
      H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, H5I_INVALID_HID, &t1, &tok_str),
      0);
  EXPECT_LT(
      H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, full_vol_, nullptr, &tok_str),
      0);
  EXPECT_LT(H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, full_vol_, &t1, nullptr),
            0);
  EXPECT_LT(
      H5VLtoken_from_str(nullptr, H5I_GROUP, full_vol_, "abc", &parsed_tok), 0);
  EXPECT_LT(H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, H5I_INVALID_HID, "abc",
                               &parsed_tok),
            0);
  EXPECT_LT(
      H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, full_vol_, "abc", nullptr),
      0);
  EXPECT_LT(
      H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, full_vol_, nullptr,
                         &parsed_tok),
      0);

  EXPECT_LT(H5VLoptional(nullptr, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(
      H5VLoptional(&g_dummy_obj, H5I_INVALID_HID, &opt_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(
      H5VLoptional(&g_dummy_obj, empty_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);

  // Failing callbacks on Layer 2 & 3.
  g_state.fail_callbacks = true;
  EXPECT_LT(H5VL_introspect_get_conn_cls(&full_obj, H5VL_GET_CONN_LVL_CURR,
                                         &queried_cls),
            0);
  EXPECT_LT(H5VLintrospect_get_conn_cls(&g_dummy_obj, full_vol_,
                                        H5VL_GET_CONN_LVL_CURR, &queried_cls),
            0);
  EXPECT_LT(H5VLintrospect_get_cap_flags(nullptr, full_vol_, &cap_flags), 0);
  EXPECT_LT(H5VL_introspect_opt_query(&full_obj, H5VL_SUBCLS_FILE, 1025,
                                      &opt_flags),
            0);
  EXPECT_LT(H5VLintrospect_opt_query(&g_dummy_obj, full_vol_, H5VL_SUBCLS_FILE,
                                     1025, &opt_flags),
            0);

  EXPECT_LT(H5VL_request_wait(&full_obj, 0, &status), 0);
  EXPECT_LT(H5VLrequest_wait(&req_tok, full_vol_, 0, &status), 0);
  EXPECT_LT(H5VL_request_notify(&full_obj, notify_cb, &notified), 0);
  EXPECT_LT(H5VLrequest_notify(&req_tok, full_vol_, notify_cb, &notified), 0);
  EXPECT_LT(H5VL_request_cancel(&full_obj, &status), 0);
  EXPECT_LT(H5VLrequest_cancel(&req_tok, full_vol_, &status), 0);
  EXPECT_LT(H5VL_request_specific(&full_obj, &req_spec), 0);
  EXPECT_LT(H5VLrequest_specific(&req_tok, full_vol_, &req_spec), 0);
  EXPECT_LT(H5VL_request_optional(&full_obj, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional(&req_tok, full_vol_, &opt_args), 0);
  EXPECT_LT(H5VLrequest_optional_op(&req_tok, full_vol_, &opt_args), 0);
  EXPECT_LT(H5VL_request_free(&full_obj), 0);
  EXPECT_LT(H5VLrequest_free(&req_tok, full_vol_), 0);

  EXPECT_LT(
      H5VL_blob_put(&full_obj, blob_buf, sizeof(blob_buf), blob_id, nullptr),
      0);
  EXPECT_LT(H5VLblob_put(&g_dummy_obj, full_vol_, blob_buf, sizeof(blob_buf),
                         blob_id, nullptr),
            0);
  EXPECT_LT(
      H5VL_blob_get(&full_obj, blob_id, blob_buf, sizeof(blob_buf), nullptr),
      0);
  EXPECT_LT(H5VLblob_get(&g_dummy_obj, full_vol_, blob_id, blob_buf,
                         sizeof(blob_buf), nullptr),
            0);
  EXPECT_LT(H5VL_blob_specific(&full_obj, blob_id, &blob_spec), 0);
  EXPECT_LT(
      H5VLblob_specific(&g_dummy_obj, full_vol_, blob_id, &blob_spec), 0);
  EXPECT_LT(H5VL_blob_optional(&full_obj, blob_id, &opt_args), 0);
  EXPECT_LT(H5VLblob_optional(&g_dummy_obj, full_vol_, blob_id, &opt_args), 0);

  EXPECT_LT(H5VL_token_cmp(&full_obj, &t1, &t2, &cmp_val), 0);
  EXPECT_LT(H5VLtoken_cmp(&g_dummy_obj, full_vol_, &t1, &t2, &cmp_val), 0);
  EXPECT_LT(H5VL_token_to_str(&full_obj, H5I_GROUP, &t1, &tok_str), 0);
  EXPECT_LT(H5VLtoken_to_str(&g_dummy_obj, H5I_GROUP, full_vol_, &t1, &tok_str),
            0);
  EXPECT_LT(H5VL_token_from_str(&full_obj, H5I_GROUP, "abc", &parsed_tok), 0);
  EXPECT_LT(
      H5VLtoken_from_str(&g_dummy_obj, H5I_GROUP, full_vol_, "abc", &parsed_tok),
      0);

  EXPECT_LT(H5VL_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(
      H5VLoptional(&g_dummy_obj, full_vol_, &opt_args, H5P_DEFAULT, nullptr),
      0);
  g_state.fail_callbacks = false;

  g_state.fail_get_wrap_ctx = true;
  EXPECT_LT(H5VL_introspect_get_conn_cls(&full_obj, H5VL_GET_CONN_LVL_CURR,
                                         &queried_cls),
            0);
  EXPECT_LT(H5VL_introspect_opt_query(&full_obj, H5VL_SUBCLS_FILE, 1025,
                                      &opt_flags),
            0);
  EXPECT_LT(H5VL_request_wait(&full_obj, 0, &status), 0);
  EXPECT_LT(H5VL_request_notify(&full_obj, notify_cb, &notified), 0);
  EXPECT_LT(H5VL_request_cancel(&full_obj, &status), 0);
  EXPECT_LT(H5VL_request_specific(&full_obj, &req_spec), 0);
  EXPECT_LT(H5VL_request_optional(&full_obj, &opt_args), 0);
  EXPECT_LT(H5VL_request_free(&full_obj), 0);
  EXPECT_LT(H5VL_optional(&full_obj, &opt_args, H5P_DEFAULT, nullptr), 0);
}

TEST_F(H5VLCallbackTest, PublicOptionalOpsSyncAsyncAndErrorPaths) {
  g_state.forward_to_native = true;
  absl::Cleanup disable_forward = [] {
    g_state.fail_callbacks = false;
    g_state.fail_get_wrap_ctx = false;
    g_state.set_async_req = false;
    g_state.forward_to_native = false;
  };

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  MockInfo info{11};
  ASSERT_GE(H5Pset_vol(fapl.get(), full_vol_, &info), 0);

  ScopedFile file(
      H5Fcreate("opt_op_mem.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(grp.get(), "dtype", dtype.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);

  H5VL_optional_args_t opt_args{};
  opt_args.op_type = H5VL_RESERVED_NATIVE_OPTIONAL + 1;

  // Synchronous calls (H5ES_NONE).
  EXPECT_GE(H5VLattr_optional_op(attr.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
            0);
  EXPECT_GE(
      H5VLdataset_optional_op(dset.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
      0);
  EXPECT_GE(
      H5VLdatatype_optional_op(dtype.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
      0);
  EXPECT_GE(H5VLfile_optional_op(file.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
            0);
  EXPECT_GE(H5VLgroup_optional_op(grp.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
            0);
  EXPECT_GE(H5VLlink_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                 H5P_DEFAULT, H5ES_NONE),
            0);
  EXPECT_GE(H5VLobject_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                   H5P_DEFAULT, H5ES_NONE),
            0);

  // Asynchronous calls creating request tokens inserted into an EventSet.
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  g_state.set_async_req = true;
  EXPECT_GE(H5VLattr_optional_op(attr.get(), &opt_args, H5P_DEFAULT, es.get()),
            0);
  EXPECT_GE(
      H5VLdataset_optional_op(dset.get(), &opt_args, H5P_DEFAULT, es.get()), 0);
  EXPECT_GE(
      H5VLdatatype_optional_op(dtype.get(), &opt_args, H5P_DEFAULT, es.get()),
      0);
  EXPECT_GE(H5VLfile_optional_op(file.get(), &opt_args, H5P_DEFAULT, es.get()),
            0);
  EXPECT_GE(H5VLgroup_optional_op(grp.get(), &opt_args, H5P_DEFAULT, es.get()),
            0);
  EXPECT_GE(H5VLlink_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                 H5P_DEFAULT, es.get()),
            0);
  EXPECT_GE(H5VLobject_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                   H5P_DEFAULT, es.get()),
            0);

  size_t num_in_progress = 0;
  bool op_failed = true;
  EXPECT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &num_in_progress, &op_failed),
            0);
  EXPECT_EQ(num_in_progress, 0);
  EXPECT_FALSE(op_failed);
  EXPECT_EQ(g_state.req_wait_calls, 7);
  es.reset();
  EXPECT_EQ(g_state.req_free_calls, 7);

  // EventSet insert failure paths when es_id != H5ES_NONE is invalid and
  // token != nullptr.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VLattr_optional_op(attr.get(), &opt_args, H5P_DEFAULT, H5I_INVALID_HID),
        0);
    EXPECT_LT(H5VLdataset_optional_op(dset.get(), &opt_args, H5P_DEFAULT,
                                      H5I_INVALID_HID),
              0);
    EXPECT_LT(H5VLdatatype_optional_op(dtype.get(), &opt_args, H5P_DEFAULT,
                                       H5I_INVALID_HID),
              0);
    EXPECT_LT(
        H5VLfile_optional_op(file.get(), &opt_args, H5P_DEFAULT, H5I_INVALID_HID),
        0);
    EXPECT_LT(
        H5VLgroup_optional_op(grp.get(), &opt_args, H5P_DEFAULT, H5I_INVALID_HID),
        0);
    EXPECT_LT(H5VLlink_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                   H5P_DEFAULT, H5I_INVALID_HID),
              0);
    EXPECT_LT(H5VLobject_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                     H5P_DEFAULT, H5I_INVALID_HID),
              0);
    g_state.set_async_req = false;

    // Callback failures & invalid IDs.
    g_state.fail_callbacks = true;
    EXPECT_LT(
        H5VLattr_optional_op(attr.get(), &opt_args, H5P_DEFAULT, H5ES_NONE), 0);
    EXPECT_LT(
        H5VLdataset_optional_op(dset.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
        0);
    EXPECT_LT(
        H5VLdatatype_optional_op(dtype.get(), &opt_args, H5P_DEFAULT, H5ES_NONE),
        0);
    EXPECT_LT(
        H5VLfile_optional_op(file.get(), &opt_args, H5P_DEFAULT, H5ES_NONE), 0);
    EXPECT_LT(
        H5VLgroup_optional_op(grp.get(), &opt_args, H5P_DEFAULT, H5ES_NONE), 0);
    EXPECT_LT(H5VLlink_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                   H5P_DEFAULT, H5ES_NONE),
              0);
    EXPECT_LT(H5VLobject_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                     H5P_DEFAULT, H5ES_NONE),
              0);
    g_state.fail_callbacks = false;

    EXPECT_LT(H5VLattr_optional_op(H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                   H5ES_NONE),
              0);
    EXPECT_LT(H5VLdataset_optional_op(H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                      H5ES_NONE),
              0);
    EXPECT_LT(H5VLdatatype_optional_op(H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                       H5ES_NONE),
              0);
    EXPECT_LT(H5VLdatatype_optional_op(H5T_NATIVE_INT, &opt_args, H5P_DEFAULT,
                                       H5ES_NONE),
              0);
    EXPECT_LT(H5VLfile_optional_op(H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                   H5ES_NONE),
              0);
    EXPECT_LT(H5VLgroup_optional_op(H5I_INVALID_HID, &opt_args, H5P_DEFAULT,
                                    H5ES_NONE),
              0);
    EXPECT_LT(H5VLlink_optional_op(H5I_INVALID_HID, "dset", H5P_DEFAULT,
                                   &opt_args, H5P_DEFAULT, H5ES_NONE),
              0);
    EXPECT_LT(H5VLobject_optional_op(H5I_INVALID_HID, "dset", H5P_DEFAULT,
                                     &opt_args, H5P_DEFAULT, H5ES_NONE),
              0);

    g_state.fail_get_wrap_ctx = true;
    EXPECT_LT(
        H5VLattr_optional_op(attr.get(), &opt_args, H5P_DEFAULT, H5ES_NONE), 0);
    EXPECT_LT(H5VLlink_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                   H5P_DEFAULT, H5ES_NONE),
              0);
    EXPECT_LT(H5VLobject_optional_op(grp.get(), "dset", H5P_DEFAULT, &opt_args,
                                     H5P_DEFAULT, H5ES_NONE),
              0);
    g_state.fail_get_wrap_ctx = false;
  }

  // Ensure handles close cleanly with forwarding intact.
  dtype.reset();
  attr.reset();
  dset.reset();
  grp.reset();
  file.reset();
}

}  // namespace
}  // namespace hdf5
