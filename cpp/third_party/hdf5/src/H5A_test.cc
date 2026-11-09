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
// Unit tests for HDF5 attribute creation, opening, reading/writing, metadata
// queries, renaming, iteration, deletion, existence checks, and asynchronous
// VOL + event-set integration (third_party/hdf5/src/H5A.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5A.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
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
using ::testing::UnorderedElementsAre;

// ---------------------------------------------------------------------------
// Minimal asynchronous pass-through VOL connector over H5VL_NATIVE.
// Emits synthetic request tokens whenever non-NULL request pointers are
// supplied, allowing H5A*_async() tests to exercise non-NULL token insertion
// and failure rollback in H5ES_insert().
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
  bool fail_next_attr_create = false;
  bool fail_next_attr_open = false;
  bool fail_next_attr_get = false;
  bool fail_next_attr_specific = false;
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

void* MockAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                     const char* name, hid_t type_id, hid_t space_id,
                     hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_attr_create) {
    ActiveMockVolState()->fail_next_attr_create = false;
    return nullptr;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLattr_create(o->under_object, loc_params, o->under_vol_id,
                                name, type_id, space_id, acpl_id, aapl_id,
                                dxpl_id, nullptr);
  if (under == nullptr) {
    return nullptr;
  }
  MaybeEmitToken(req);
  return WrapObj(under, o->under_vol_id);
}

void* MockAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                   const char* name, hid_t aapl_id, hid_t dxpl_id, void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_attr_open) {
    ActiveMockVolState()->fail_next_attr_open = false;
    return nullptr;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  void* under = H5VLattr_open(o->under_object, loc_params, o->under_vol_id,
                              name, aapl_id, dxpl_id, nullptr);
  if (under == nullptr) {
    return nullptr;
  }
  MaybeEmitToken(req);
  return WrapObj(under, o->under_vol_id);
}

herr_t MockAttrRead(void* attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                    void** req) {
  auto* o = static_cast<MockVolObj*>(attr);
  herr_t rc = H5VLattr_read(o->under_object, o->under_vol_id, mem_type_id, buf,
                            dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                     hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(attr);
  herr_t rc = H5VLattr_write(o->under_object, o->under_vol_id, mem_type_id, buf,
                             dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockAttrGet(void* obj, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                   void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_attr_get) {
    ActiveMockVolState()->fail_next_attr_get = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  herr_t rc =
      H5VLattr_get(o->under_object, o->under_vol_id, args, dxpl_id, req);
  return rc;
}

herr_t MockAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                        H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                        void** req) {
  if (ActiveMockVolState() != nullptr &&
      ActiveMockVolState()->fail_next_attr_specific) {
    ActiveMockVolState()->fail_next_attr_specific = false;
    return -1;
  }
  auto* o = static_cast<MockVolObj*>(obj);
  herr_t rc = H5VLattr_specific(o->under_object, loc_params, o->under_vol_id,
                                args, dxpl_id, nullptr);
  if (rc >= 0) {
    MaybeEmitToken(req);
  }
  return rc;
}

herr_t MockAttrOptional(void* obj, H5VL_optional_args_t* args, hid_t dxpl_id,
                        void** req) {
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLattr_optional(o->under_object, o->under_vol_id, args, dxpl_id,
                           req);
}

herr_t MockAttrClose(void* attr, hid_t dxpl_id, void** req) {
  auto* o = static_cast<MockVolObj*>(attr);
  herr_t rc =
      H5VLattr_close(o->under_object, o->under_vol_id, dxpl_id, nullptr);
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
  auto* o = static_cast<MockVolObj*>(obj);
  return H5VLintrospect_opt_query(o->under_object, o->under_vol_id, cls,
                                  opt_type, flags);
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

constexpr H5VL_class_value_t kMockVolValue =
    static_cast<H5VL_class_value_t>(517);

const H5VL_class_t kMockVolClass = {
    H5VL_VERSION,
    kMockVolValue,
    "h5a_test_async_passthru",
    0,
    H5VL_CAP_FLAG_ASYNC,
    MockInit,
    MockTerm,
    {/* info_cls */ sizeof(MockVolInfo), MockInfoCopy, MockInfoCmp, MockInfoFree,
     nullptr, nullptr},
    {/* wrap_cls */ MockWrapGetObject, MockWrapGetCtx, MockWrapObj,
     MockUnwrapObj, MockWrapFreeCtx},
    {/* attr_cls */ MockAttrCreate, MockAttrOpen, MockAttrRead, MockAttrWrite,
     MockAttrGet, MockAttrSpecific, MockAttrOptional, MockAttrClose},
    {/* dataset_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr},
    {/* datatype_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {/* file_cls */ MockFileCreate, MockFileOpen, MockFileGet, MockFileSpecific,
     MockFileOptional, MockFileClose},
    {/* group_cls */ MockGroupCreate, MockGroupOpen, MockGroupGet,
     MockGroupSpecific, MockGroupOptional, MockGroupClose},
    {/* link_cls */ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {/* object_cls */ nullptr, nullptr, nullptr, nullptr, nullptr},
    {/* introspect_cls */ MockIntrospectGetConnCls, MockIntrospectGetCapFlags,
     MockIntrospectOptQuery},
    {/* request_cls */ MockRequestWait, nullptr, nullptr, MockRequestSpecific,
     nullptr, MockRequestFree},
    {/* blob_cls */ nullptr, nullptr, nullptr, nullptr},
    {/* token_cls */ nullptr, nullptr, nullptr},
    nullptr,
};

H5VL_class_t MakeMockVolClass() { return kMockVolClass; }

// ---------------------------------------------------------------------------
// Synchronous attribute API tests
// ---------------------------------------------------------------------------

TEST(H5ATest, SyncCreateReadWriteMetadataAcrossObjectTypes) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedSpace vec_space([&]() {
    hsize_t dims[1] = {4};
    return H5Screate_simple(1, dims, nullptr);
  }());
  ASSERT_TRUE(vec_space.valid());
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());

  ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT,
                                vec_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(grp.get(), "dt", committed_dt.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Attribute creation plist with UTF-8 character encoding.
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);

  // Attach on file root, subgroup, dataset, and committed datatype.
  {
    ScopedAttr root_attr(H5Acreate2(file.get(), "root_a", H5T_NATIVE_INT,
                                    scalar.get(), acpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(root_attr.valid());
    int v = 42;
    ASSERT_GE(H5Awrite(root_attr.get(), H5T_NATIVE_INT, &v), 0);

    int out = 0;
    ASSERT_GE(H5Aread(root_attr.get(), H5T_NATIVE_INT, &out), 0);
    EXPECT_EQ(out, 42);

    ScopedSpace sp(H5Aget_space(root_attr.get()));
    ASSERT_TRUE(sp.valid());
    EXPECT_EQ(H5Sget_simple_extent_type(sp.get()), H5S_SCALAR);

    ScopedType tp(H5Aget_type(root_attr.get()));
    ASSERT_TRUE(tp.valid());
    EXPECT_EQ(H5Tget_class(tp.get()), H5T_INTEGER);

    ScopedPlist got_acpl(H5Aget_create_plist(root_attr.get()));
    ASSERT_TRUE(got_acpl.valid());
    H5T_cset_t cset = H5T_CSET_ERROR;
    ASSERT_GE(H5Pget_char_encoding(got_acpl.get(), &cset), 0);
    EXPECT_EQ(cset, H5T_CSET_UTF8);

    H5A_info_t info{};
    ASSERT_GE(H5Aget_info(root_attr.get(), &info), 0);
    EXPECT_EQ(info.cset, H5T_CSET_UTF8);
    EXPECT_EQ(info.data_size, sizeof(int));

    EXPECT_EQ(H5Aget_storage_size(root_attr.get()), sizeof(int));

    // Zero-size query with non-NULL buffer must not overwrite sentinel bytes.
    char sentinel[4] = {'X', 'Y', 'Z', '\0'};
    EXPECT_EQ(H5Aget_name(root_attr.get(), 0, sentinel), 6);
    EXPECT_EQ(sentinel[0], 'X');
    EXPECT_EQ(H5Aget_name(root_attr.get(), 0, nullptr), 6);

    char small_buf[4] = {};
    EXPECT_EQ(H5Aget_name(root_attr.get(), sizeof(small_buf), small_buf), 6);
    EXPECT_STREQ(small_buf, "roo");

    char full_buf[16] = {};
    EXPECT_EQ(H5Aget_name(root_attr.get(), sizeof(full_buf), full_buf), 6);
    EXPECT_STREQ(full_buf, "root_a");
  }

  // Create and query via *_by_name on dataset and committed datatype.
  {
    ScopedAttr dset_attr(H5Acreate_by_name(
        file.get(), "/grp/dset", "vec", H5T_NATIVE_INT, vec_space.get(),
        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset_attr.valid());
    const std::array<int, 4> in = {10, 20, 30, 40};
    ASSERT_GE(H5Awrite(dset_attr.get(), H5T_NATIVE_INT, in.data()), 0);
  }
  {
    ScopedAttr dt_attr(H5Acreate2(committed_dt.get(), "dt_null",
                                  H5T_NATIVE_INT, null_space.get(), H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dt_attr.valid());
    EXPECT_EQ(H5Aget_storage_size(dt_attr.get()), 0);
  }

  EXPECT_EQ(H5Aexists(file.get(), "root_a"), 1);
  EXPECT_EQ(H5Aexists(file.get(), "absent"), 0);
  EXPECT_EQ(
      H5Aexists_by_name(file.get(), "/grp/dset", "vec", H5P_DEFAULT), 1);
  EXPECT_EQ(
      H5Aexists_by_name(file.get(), "/grp/dset", "absent", H5P_DEFAULT), 0);

  // Open by name and read back.
  {
    ScopedAttr reopened_vec(H5Aopen_by_name(
        file.get(), "/grp/dset", "vec", H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(reopened_vec.valid());
    std::array<int, 4> out = {};
    ASSERT_GE(H5Aread(reopened_vec.get(), H5T_NATIVE_INT, out.data()), 0);
    EXPECT_THAT(out, ElementsAre(10, 20, 30, 40));
  }

  // Open/info/name by index on /grp.
  {
    ScopedAttr ga1(H5Acreate2(grp.get(), "beta", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ga1.valid());
    ScopedAttr ga2(H5Acreate2(grp.get(), "alpha", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ga2.valid());
  }
  {
    ScopedAttr opened_idx(H5Aopen_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                         H5_ITER_INC, 0, H5P_DEFAULT,
                                         H5P_DEFAULT));
    ASSERT_TRUE(opened_idx.valid());
    char name[32] = {};
    EXPECT_EQ(H5Aget_name(opened_idx.get(), sizeof(name), name), 5);
    EXPECT_STREQ(name, "alpha");
  }

  char idx_sentinel[2] = {'Q', '\0'};
  EXPECT_EQ(H5Aget_name_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               1, idx_sentinel, 0, H5P_DEFAULT),
            4);
  EXPECT_EQ(idx_sentinel[0], 'Q');

  char idx_name[16] = {};
  EXPECT_EQ(H5Aget_name_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               1, idx_name, sizeof(idx_name), H5P_DEFAULT),
            4);
  EXPECT_STREQ(idx_name, "beta");

  H5A_info_t by_name_info{};
  ASSERT_GE(H5Aget_info_by_name(file.get(), "/grp", "alpha", &by_name_info,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(by_name_info.data_size, sizeof(int));

  H5A_info_t by_idx_info{};
  ASSERT_GE(H5Aget_info_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_DEC,
                               0, &by_idx_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(by_idx_info.data_size, sizeof(int));

  // Rename (including no-op identical rename), iterate, and delete.
  ASSERT_GE(H5Arename(grp.get(), "alpha", "alpha"), 0);
  ASSERT_GE(H5Arename(grp.get(), "alpha", "gamma"), 0);
  ASSERT_GE(H5Arename_by_name(file.get(), "/grp", "beta", "delta", H5P_DEFAULT),
            0);

  // Iterate full, short-circuit positive, resume, and negative error callback.
  std::vector<std::string> visited;
  auto collect_cb = [](hid_t /*loc_id*/, const char* attr_name,
                       const H5A_info_t* /*ainfo*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(attr_name);
    return 0;
  };
  ASSERT_GE(H5Aiterate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited, ElementsAre("delta", "gamma"));

  visited.clear();
  auto stop_cb = [](hid_t /*loc_id*/, const char* attr_name,
                    const H5A_info_t* /*ainfo*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(attr_name);
    return 7;
  };
  hsize_t iter_idx = 0;
  EXPECT_EQ(H5Aiterate_by_name(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               &iter_idx, stop_cb, &visited, H5P_DEFAULT),
            7);
  EXPECT_THAT(visited, ElementsAre("delta"));
  EXPECT_EQ(iter_idx, 1);

  EXPECT_EQ(H5Aiterate_by_name(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                               &iter_idx, collect_cb, &visited, H5P_DEFAULT),
            0);
  EXPECT_THAT(visited, ElementsAre("delta", "gamma"));

  auto err_cb = [](hid_t /*loc_id*/, const char* /*attr_name*/,
                   const H5A_info_t* /*ainfo*/, void* /*op_data*/) -> herr_t {
    return -3;
  };
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Aiterate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                          err_cb, nullptr),
              -3);
  }

  ASSERT_GE(H5Adelete(grp.get(), "gamma"), 0);
  ASSERT_GE(
      H5Adelete_by_name(file.get(), "/grp", "delta", H5P_DEFAULT), 0);
  ASSERT_GE(H5Adelete_by_idx(file.get(), "/grp/dset", H5_INDEX_NAME,
                             H5_ITER_INC, 0, H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Aexists(grp.get(), "gamma"), 0);
  EXPECT_EQ(H5Aexists(grp.get(), "delta"), 0);
  EXPECT_EQ(H5Aexists(dset.get(), "vec"), 0);
}

TEST(H5ATest, ValidationAndOperationFailurePaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedAttr valid_attr(H5Acreate2(grp.get(), "a1", H5T_NATIVE_INT,
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(valid_attr.valid());
  int v = 5;
  ASSERT_GE(H5Awrite(valid_attr.get(), H5T_NATIVE_INT, &v), 0);

  ScopedType str_type(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_type.valid());
  ASSERT_GE(H5Tset_size(str_type.get(), 8), 0);

  ScopedErrorSilence silence;

  // Attribute creation argument validation & execution failures.
  EXPECT_LT(H5Acreate2(valid_attr.get(), "bad", H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), nullptr, H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), "", H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(H5I_INVALID_HID, "x", H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), "dup", H5I_INVALID_HID, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), "dup2", H5T_NATIVE_INT, H5I_INVALID_HID,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), "dup3", H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, grp.get()),
            0);
  EXPECT_LT(H5Acreate2(grp.get(), "a1", H5T_NATIVE_INT, scalar.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // create_by_name error paths.
  EXPECT_LT(H5Acreate_by_name(valid_attr.get(), ".", "x", H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), ".", nullptr, H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), ".", "", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), nullptr, "x", H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), "", "x", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(H5I_INVALID_HID, ".", "x", H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), ".", "x", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, grp.get(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Acreate_by_name(grp.get(), "/no_such_obj", "x", H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT),
            0);

  // open / open_by_name / open_by_idx error paths.
  EXPECT_LT(H5Aopen(valid_attr.get(), "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen(grp.get(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen(grp.get(), "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen(H5I_INVALID_HID, "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen(grp.get(), "a1", grp.get()), 0);
  EXPECT_LT(H5Aopen(grp.get(), "missing", H5P_DEFAULT), 0);

  EXPECT_LT(H5Aopen_by_name(valid_attr.get(), ".", "a1", H5P_DEFAULT,
                            H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_name(grp.get(), ".", nullptr, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_name(grp.get(), ".", "", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen_by_name(grp.get(), nullptr, "a1", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_name(grp.get(), "", "a1", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Aopen_by_name(H5I_INVALID_HID, ".", "a1", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Aopen_by_name(grp.get(), ".", "a1", grp.get(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Aopen_by_name(grp.get(), "/no_such_obj", "a1", H5P_DEFAULT,
                            H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Aopen_by_name(grp.get(), ".", "missing", H5P_DEFAULT, H5P_DEFAULT), 0);

  EXPECT_LT(H5Aopen_by_idx(valid_attr.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                           grp.get(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), "/no_such_obj", H5_INDEX_NAME,
                           H5_ITER_INC, 0, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aopen_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // write / read error paths (bad attr_id, bad type_id, null buf, type error).
  EXPECT_LT(H5Awrite(valid_attr.get(), grp.get(), &v), 0);
  EXPECT_LT(H5Awrite(valid_attr.get(), H5T_NATIVE_INT, nullptr), 0);
  EXPECT_LT(H5Awrite(grp.get(), H5T_NATIVE_INT, &v), 0);
  char sbuf[8] = "abcdefg";
  EXPECT_LT(H5Awrite(valid_attr.get(), str_type.get(), sbuf), 0);

  EXPECT_LT(H5Aread(valid_attr.get(), grp.get(), &v), 0);
  EXPECT_LT(H5Aread(valid_attr.get(), H5T_NATIVE_INT, nullptr), 0);
  EXPECT_LT(H5Aread(grp.get(), H5T_NATIVE_INT, &v), 0);
  EXPECT_LT(H5Aread(valid_attr.get(), str_type.get(), sbuf), 0);

  // Queries & info error paths.
  EXPECT_LT(H5Aget_space(grp.get()), 0);
  EXPECT_LT(H5Aget_type(grp.get()), 0);
  EXPECT_LT(H5Aget_create_plist(grp.get()), 0);
  EXPECT_EQ(H5Aget_storage_size(grp.get()), 0);

  EXPECT_LT(H5Aget_name(grp.get(), sizeof(sbuf), sbuf), 0);
  EXPECT_LT(H5Aget_name(valid_attr.get(), 4, nullptr), 0);

  EXPECT_LT(H5Aget_name_by_idx(valid_attr.get(), ".", H5_INDEX_NAME,
                               H5_ITER_INC, 0, sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                               0, sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                               sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               nullptr, 4, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), ".", H5_INDEX_N, H5_ITER_INC, 0, sbuf,
                               sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_N, 0,
                               sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), "/no_such_obj", H5_INDEX_NAME,
                               H5_ITER_INC, 0, sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               sbuf, sizeof(sbuf), H5P_DEFAULT),
            0);

  H5A_info_t info{};
  EXPECT_LT(H5Aget_info(grp.get(), &info), 0);
  EXPECT_LT(H5Aget_info(valid_attr.get(), nullptr), 0);

  EXPECT_LT(
      H5Aget_info_by_name(valid_attr.get(), ".", "a1", &info, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Aget_info_by_name(grp.get(), nullptr, "a1", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aget_info_by_name(grp.get(), "", "a1", &info, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Aget_info_by_name(grp.get(), ".", nullptr, &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aget_info_by_name(grp.get(), ".", "", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aget_info_by_name(grp.get(), ".", "a1", nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Aget_info_by_name(H5I_INVALID_HID, ".", "a1", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aget_info_by_name(grp.get(), "/no_such_obj", "a1", &info,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Aget_info_by_name(grp.get(), ".", "missing", &info, H5P_DEFAULT), 0);

  EXPECT_LT(H5Aget_info_by_idx(valid_attr.get(), ".", H5_INDEX_NAME,
                               H5_ITER_INC, 0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                               0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                               &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC, 0,
                               &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                               0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), "/no_such_obj", H5_INDEX_NAME,
                               H5_ITER_INC, 0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aget_info_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               &info, H5P_DEFAULT),
            0);

  // rename / rename_by_name error paths.
  EXPECT_LT(H5Arename(valid_attr.get(), "a1", "a2"), 0);
  EXPECT_LT(H5Arename(grp.get(), nullptr, "a2"), 0);
  EXPECT_LT(H5Arename(grp.get(), "", "a2"), 0);
  EXPECT_LT(H5Arename(grp.get(), "a1", nullptr), 0);
  EXPECT_LT(H5Arename(grp.get(), "a1", ""), 0);
  EXPECT_LT(H5Arename(H5I_INVALID_HID, "a1", "a2"), 0);
  EXPECT_LT(H5Arename(grp.get(), "missing", "a2"), 0);

  EXPECT_LT(
      H5Arename_by_name(valid_attr.get(), ".", "a1", "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), ".", nullptr, "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), ".", "", "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), ".", "a1", nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), ".", "a1", "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), nullptr, "a1", "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), "", "a1", "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(H5I_INVALID_HID, ".", "a1", "a2", H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Arename_by_name(grp.get(), "/no_such_obj", "a1", "a2", H5P_DEFAULT), 0);
  EXPECT_LT(H5Arename_by_name(grp.get(), ".", "missing", "a2", H5P_DEFAULT), 0);

  // iterate2 / iterate_by_name error paths.
  auto noop_cb = [](hid_t, const char*, const H5A_info_t*, void*) -> herr_t {
    return 0;
  };
  EXPECT_LT(H5Aiterate2(valid_attr.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        noop_cb, nullptr),
            0);
  EXPECT_LT(H5Aiterate2(grp.get(), H5_INDEX_N, H5_ITER_INC, nullptr, noop_cb,
                        nullptr),
            0);
  EXPECT_LT(H5Aiterate2(grp.get(), H5_INDEX_NAME, H5_ITER_N, nullptr, noop_cb,
                        nullptr),
            0);
  EXPECT_LT(H5Aiterate2(H5I_INVALID_HID, H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        noop_cb, nullptr),
            0);

  EXPECT_LT(H5Aiterate_by_name(valid_attr.get(), ".", H5_INDEX_NAME,
                               H5_ITER_INC, nullptr, noop_cb, nullptr,
                               H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(grp.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                               nullptr, noop_cb, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(grp.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                               nullptr, noop_cb, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(grp.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                               nullptr, noop_cb, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(grp.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                               nullptr, noop_cb, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                               nullptr, noop_cb, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Aiterate_by_name(grp.get(), "/no_such", H5_INDEX_NAME,
                               H5_ITER_INC, nullptr, noop_cb, nullptr,
                               H5P_DEFAULT),
            0);

  // delete / delete_by_name / delete_by_idx / close / exists error paths.
  EXPECT_LT(H5Adelete(valid_attr.get(), "a1"), 0);
  EXPECT_LT(H5Adelete(grp.get(), nullptr), 0);
  EXPECT_LT(H5Adelete(grp.get(), ""), 0);
  EXPECT_LT(H5Adelete(H5I_INVALID_HID, "a1"), 0);
  EXPECT_LT(H5Adelete(grp.get(), "missing"), 0);

  EXPECT_LT(H5Adelete_by_name(valid_attr.get(), ".", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(grp.get(), nullptr, "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(grp.get(), "", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(grp.get(), ".", nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(grp.get(), ".", "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(H5I_INVALID_HID, ".", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Adelete_by_name(grp.get(), "/no_such_obj", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Adelete_by_name(grp.get(), ".", "missing", H5P_DEFAULT), 0);

  EXPECT_LT(H5Adelete_by_idx(valid_attr.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), ".", H5_INDEX_N, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_N, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), "/no_such_obj", H5_INDEX_NAME,
                             H5_ITER_INC, 0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Adelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                             H5P_DEFAULT),
            0);

  EXPECT_LT(H5Aclose(grp.get()), 0);

  EXPECT_LT(H5Aexists(valid_attr.get(), "a1"), 0);
  EXPECT_LT(H5Aexists(grp.get(), nullptr), 0);
  EXPECT_LT(H5Aexists(grp.get(), ""), 0);
  EXPECT_LT(H5Aexists(H5I_INVALID_HID, "a1"), 0);

  EXPECT_LT(H5Aexists_by_name(valid_attr.get(), ".", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(grp.get(), ".", nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(grp.get(), ".", "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(grp.get(), nullptr, "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(grp.get(), "", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(H5I_INVALID_HID, ".", "a1", H5P_DEFAULT), 0);
  EXPECT_LT(H5Aexists_by_name(grp.get(), "/no_such_obj", "a1", H5P_DEFAULT), 0);
}

TEST(H5ATest, AsyncApiWithMockVolAndEventSet) {
  MockVolState state;
  ActiveMockVolState() = &state;
  auto clear_state = absl::MakeCleanup([]() { ActiveMockVolState() = nullptr; });

  H5VL_class_t vol_cls = MakeMockVolClass();
  hid_t vol_id = H5VLregister_connector(&vol_cls, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  auto unregister_vol =
      absl::MakeCleanup([&]() { H5VLunregister_connector(vol_id); });

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  MockVolInfo info{H5VL_NATIVE, nullptr};
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, &info), 0);

  ScopedFile file(
      H5Fcreate("async_attr.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. Exercise all 12 H5A*_async variants with H5ES_NONE (token_ptr == NULL).
  {
    int wval = 11;
    int rval = 0;
    bool exists = false;
    ScopedAttr a_sync(H5Acreate_async(grp.get(), "sync1", H5T_NATIVE_INT,
                                      scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                      H5ES_NONE));
    ASSERT_TRUE(a_sync.valid());
    ASSERT_GE(
        H5Awrite_async(a_sync.get(), H5T_NATIVE_INT, &wval, H5ES_NONE), 0);
    ASSERT_GE(
        H5Aread_async(a_sync.get(), H5T_NATIVE_INT, &rval, H5ES_NONE), 0);
    EXPECT_EQ(rval, 11);
    EXPECT_GE(H5Aclose_async(a_sync.release(), H5ES_NONE), 0);

    ScopedAttr a_bn(H5Acreate_by_name_async(file.get(), "/grp", "sync2",
                                            H5T_NATIVE_INT, scalar.get(),
                                            H5P_DEFAULT, H5P_DEFAULT,
                                            H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(a_bn.valid());
    a_bn.reset();

    ScopedAttr o1(H5Aopen_async(grp.get(), "sync1", H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(o1.valid());
    ScopedAttr o2(H5Aopen_by_name_async(file.get(), "/grp", "sync2",
                                        H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(o2.valid());
    ScopedAttr o3(H5Aopen_by_idx_async(file.get(), "/grp", H5_INDEX_NAME,
                                       H5_ITER_INC, 0, H5P_DEFAULT,
                                       H5P_DEFAULT, H5ES_NONE));
    ASSERT_TRUE(o3.valid());
    o1.reset();
    o2.reset();
    o3.reset();

    ASSERT_GE(H5Aexists_async(grp.get(), "sync1", &exists, H5ES_NONE), 0);
    EXPECT_TRUE(exists);
    ASSERT_GE(H5Aexists_by_name_async(file.get(), "/grp", "sync2", &exists,
                                      H5P_DEFAULT, H5ES_NONE),
              0);
    EXPECT_TRUE(exists);
    ASSERT_GE(H5Arename_async(grp.get(), "sync1", "sync1_r", H5ES_NONE), 0);
    ASSERT_GE(H5Arename_by_name_async(file.get(), "/grp", "sync2", "sync2_r",
                                      H5P_DEFAULT, H5ES_NONE),
              0);
  }

  // 2. Exercise all 12 H5A*_async variants with valid es_id (tokens generated
  //    and inserted via H5ES_insert, then awaited and freed via H5ESwait).
  {
    int w1 = 101;
    int w2 = 202;
    int r1 = 0;
    int r2 = 0;
    bool ex1 = false;
    bool ex2 = false;

    ScopedAttr a1(H5Acreate_async(grp.get(), "async1", H5T_NATIVE_INT,
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  es.get()));
    ASSERT_TRUE(a1.valid());
    ScopedAttr a2(H5Acreate_by_name_async(file.get(), "/grp", "async2",
                                          H5T_NATIVE_INT, scalar.get(),
                                          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                          es.get()));
    ASSERT_TRUE(a2.valid());

    ASSERT_GE(H5Awrite_async(a1.get(), H5T_NATIVE_INT, &w1, es.get()), 0);
    ASSERT_GE(H5Awrite_async(a2.get(), H5T_NATIVE_INT, &w2, es.get()), 0);
    ASSERT_GE(H5Aread_async(a1.get(), H5T_NATIVE_INT, &r1, es.get()), 0);
    ASSERT_GE(H5Aread_async(a2.get(), H5T_NATIVE_INT, &r2, es.get()), 0);
    EXPECT_EQ(r1, 101);
    EXPECT_EQ(r2, 202);

    ASSERT_GE(H5Aclose_async(a1.release(), es.get()), 0);
    ASSERT_GE(H5Aclose_async(a2.release(), es.get()), 0);

    ScopedAttr o1(H5Aopen_async(grp.get(), "async1", H5P_DEFAULT, es.get()));
    ASSERT_TRUE(o1.valid());
    ScopedAttr o2(H5Aopen_by_name_async(file.get(), "/grp", "async2",
                                        H5P_DEFAULT, H5P_DEFAULT, es.get()));
    ASSERT_TRUE(o2.valid());
    ScopedAttr o3(H5Aopen_by_idx_async(file.get(), "/grp", H5_INDEX_NAME,
                                       H5_ITER_INC, 0, H5P_DEFAULT, H5P_DEFAULT,
                                       es.get()));
    ASSERT_TRUE(o3.valid());
    ASSERT_GE(H5Aclose_async(o1.release(), es.get()), 0);
    ASSERT_GE(H5Aclose_async(o2.release(), es.get()), 0);
    ASSERT_GE(H5Aclose_async(o3.release(), es.get()), 0);

    ASSERT_GE(H5Arename_async(grp.get(), "async1", "async1_r", es.get()), 0);
    ASSERT_GE(H5Arename_by_name_async(file.get(), "/grp", "async2", "async2_r",
                                      H5P_DEFAULT, es.get()),
              0);
    ASSERT_GE(H5Aexists_async(grp.get(), "async1_r", &ex1, es.get()), 0);
    ASSERT_GE(H5Aexists_by_name_async(file.get(), "/grp", "async2_r", &ex2,
                                      H5P_DEFAULT, es.get()),
              0);
    EXPECT_TRUE(ex1);
    EXPECT_TRUE(ex2);

    size_t in_progress = 1;
    bool err_occurred = true;
    ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &err_occurred),
              0);
    EXPECT_EQ(in_progress, 0);
    EXPECT_FALSE(err_occurred);
    EXPECT_TRUE(state.live_tokens.empty());
  }

  // 3. Pass invalid non-H5ES_NONE event set IDs and trigger operation/H5ES
  //    failures to cover failure + H5ES_insert rollback branches across all
  //    12 H5A*_async entry points.
  {
    ScopedErrorSilence silence;
    hid_t bad_es = fapl.get();  // Non-zero, non-H5I_EVENTSET hid_t.
    int buf = 7;
    bool ex = false;

    EXPECT_LT(H5Acreate_async(grp.get(), "err_c1", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Acreate_by_name_async(file.get(), "/grp", "err_c2",
                                      H5T_NATIVE_INT, scalar.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Aopen_async(grp.get(), "err_c1", H5P_DEFAULT, bad_es), 0);
    EXPECT_LT(H5Aopen_by_name_async(file.get(), "/grp", "err_c2", H5P_DEFAULT,
                                    H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Aopen_by_idx_async(file.get(), "/grp", H5_INDEX_NAME,
                                   H5_ITER_INC, 0, H5P_DEFAULT, H5P_DEFAULT,
                                   bad_es),
              0);

    ScopedAttr tmp(H5Aopen(grp.get(), "async1_r", H5P_DEFAULT));
    ASSERT_TRUE(tmp.valid());
    EXPECT_LT(H5Awrite_async(tmp.get(), H5T_NATIVE_INT, &buf, bad_es), 0);
    EXPECT_LT(H5Aread_async(tmp.get(), H5T_NATIVE_INT, &buf, bad_es), 0);
    EXPECT_LT(H5Arename_async(grp.get(), "async1_r", "async1_r2", bad_es), 0);
    EXPECT_LT(H5Arename_by_name_async(file.get(), "/grp", "async2_r",
                                      "async2_r2", H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Aexists_async(grp.get(), "async1_r2", &ex, bad_es), 0);
    EXPECT_LT(H5Aexists_by_name_async(file.get(), "/grp", "async2_r2", &ex,
                                      H5P_DEFAULT, bad_es),
              0);
    EXPECT_LT(H5Aclose_async(tmp.release(), bad_es), 0);

    // Underlying call failures with valid event set.
    EXPECT_LT(H5Acreate_async(grp.get(), nullptr, H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Acreate_by_name_async(file.get(), "/grp", nullptr,
                                      H5T_NATIVE_INT, scalar.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Aopen_async(grp.get(), "missing", H5P_DEFAULT, es.get()), 0);
    EXPECT_LT(H5Aopen_by_name_async(file.get(), "/grp", "missing", H5P_DEFAULT,
                                    H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Aopen_by_idx_async(file.get(), "/grp", H5_INDEX_NAME,
                                   H5_ITER_INC, 99, H5P_DEFAULT, H5P_DEFAULT,
                                   es.get()),
              0);
    EXPECT_LT(H5Awrite_async(grp.get(), H5T_NATIVE_INT, &buf, es.get()), 0);
    EXPECT_LT(H5Aread_async(grp.get(), H5T_NATIVE_INT, &buf, es.get()), 0);
    EXPECT_LT(H5Arename_async(grp.get(), "missing", "x", es.get()), 0);
    EXPECT_LT(H5Arename_by_name_async(file.get(), "/grp", "missing", "x",
                                      H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Aexists_async(grp.get(), nullptr, &ex, es.get()), 0);
    EXPECT_LT(H5Aexists_async(grp.get(), "x", nullptr, es.get()), 0);
    EXPECT_LT(H5Aexists_async(H5I_INVALID_HID, "x", &ex, es.get()), 0);
    EXPECT_LT(H5Aexists_by_name_async(file.get(), "/grp", nullptr, &ex,
                                      H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Aexists_by_name_async(file.get(), "/grp", "x", nullptr,
                                      H5P_DEFAULT, es.get()),
              0);
    EXPECT_LT(H5Aexists_by_name_async(file.get(), "/no_such", "x", &ex,
                                      H5P_DEFAULT, es.get()),
              0);
    EXPECT_GE(H5Arename_async(grp.get(), "async1_r2", "async1_r2", es.get()),
              0);
    state.fail_next_attr_specific = true;
    EXPECT_LT(H5Aexists(grp.get(), "async1_r2"), 0);
    EXPECT_LT(H5Aclose_async(grp.get(), es.get()), 0);

    // Verify NULL attribute info/get failures through mock VOL connector.
    ScopedAttr ga(H5Aopen(grp.get(), "async1_r2", H5P_DEFAULT));
    ASSERT_TRUE(ga.valid());
    state.fail_next_attr_get = true;
    EXPECT_LT(H5Aget_space(ga.get()), 0);
    state.fail_next_attr_get = true;
    EXPECT_LT(H5Aget_type(ga.get()), 0);
    state.fail_next_attr_get = true;
    EXPECT_LT(H5Aget_create_plist(ga.get()), 0);
    char nbuf[16] = {};
    state.fail_next_attr_get = true;
    EXPECT_LT(H5Aget_name(ga.get(), sizeof(nbuf), nbuf), 0);
    state.fail_next_attr_get = true;
    EXPECT_EQ(H5Aget_storage_size(ga.get()), 0);
    H5A_info_t ainfo{};
    state.fail_next_attr_get = true;
    EXPECT_LT(H5Aget_info(ga.get(), &ainfo), 0);
  }
}

}  // namespace
}  // namespace hdf5
