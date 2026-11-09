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
// Tests public and internal object header APIs: opening (by name/index/token),
// copying, flushing, refreshing, hard-linking, refcount adjustment, existence
// queries, data-model and native object info queries, comments, recursive
// visitation, closing, metadata cache corking, token serialization/comparison,
// and asynchronous H5O*_async APIs (third_party/hdf5/src/H5O.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5O.c

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5O_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
}  // extern "C"
#ifdef H5O_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

constexpr H5VL_class_value_t kMockVolValue = 323;
constexpr char kMockVolName[] = "h5o_async_mock_vol";

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

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct MockRequest {
  H5VL_request_status_t wait_status = H5VL_REQUEST_STATUS_SUCCEED;
};

struct MockGroup {
  int id = 1;
};

struct MockFile {
  int id = 0;
};

struct MockVolTracker {
  bool fail_object_open = false;
  bool bad_open_type = false;
  bool fail_object_copy = false;
  bool fail_object_get = false;
  bool fail_object_specific = false;
  bool fail_object_optional = false;
  bool fail_link_create = false;
  bool fail_group_close_once = false;
  bool fail_token_cmp = false;
  bool fail_token_to_str = false;
  bool fail_token_from_str = false;
  MockGroup bad_open_dummy{};
  std::vector<MockRequest*> live_requests;

  ~MockVolTracker() {
    for (MockRequest* req : live_requests) {
      delete req;
    }
  }

  void* MakeRequest() {
    auto* req = new MockRequest{};
    live_requests.push_back(req);
    return req;
  }

  void FreeRequest(void* req) {
    auto* typed = static_cast<MockRequest*>(req);
    std::erase(live_requests, typed);
    delete typed;
  }
};

MockVolTracker* g_mock_tracker = nullptr;
H5VL_class_t g_mock_vol_cls{};

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  return new MockFile{};
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  delete static_cast<MockFile*>(file);
  return 0;
}

void* MockGroupCreate(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                      const char* /*name*/, hid_t /*lcpl_id*/,
                      hid_t /*gcpl_id*/, hid_t /*gapl_id*/, hid_t /*dxpl_id*/,
                      void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  return new MockGroup{};
}

herr_t MockGroupClose(void* grp, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_close_once) {
    g_mock_tracker->fail_group_close_once = false;
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  delete static_cast<MockGroup*>(grp);
  return 0;
}

void* MockObjectOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5I_type_t* opened_type, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_object_open) {
    return nullptr;
  }
  if (g_mock_tracker != nullptr && g_mock_tracker->bad_open_type) {
    *opened_type = H5I_BADID;
    return &g_mock_tracker->bad_open_dummy;
  }
  *opened_type = H5I_GROUP;
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return new MockGroup{};
}

herr_t MockObjectCopy(void* /*src_obj*/,
                      const H5VL_loc_params_t* /*loc_params1*/,
                      const char* /*src_name*/, void* /*dst_obj*/,
                      const H5VL_loc_params_t* /*loc_params2*/,
                      const char* /*dst_name*/, hid_t /*ocpypl_id*/,
                      hid_t /*lcpl_id*/, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_object_copy) {
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockObjectGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                     H5VL_object_get_args_t* args, hid_t /*dxpl_id*/,
                     void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_object_get) {
    return -1;
  }
  if (args->op_type == H5VL_OBJECT_GET_INFO && args->args.get_info.oinfo) {
    H5O_info2_t* oinfo = args->args.get_info.oinfo;
    *oinfo = {};
    oinfo->type = H5O_TYPE_GROUP;
    oinfo->rc = 1;
    oinfo->token.__data[0] = 0x5A;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockObjectSpecific(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_object_specific_args_t* args, hid_t /*dxpl_id*/,
                          void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_object_specific) {
    return -1;
  }
  if (args->op_type == H5VL_OBJECT_EXISTS &&
      args->args.exists.exists != nullptr) {
    *args->args.exists.exists = true;
  } else if (args->op_type == H5VL_OBJECT_VISIT &&
             args->args.visit.op != nullptr) {
    H5O_info2_t info{};
    info.type = H5O_TYPE_GROUP;
    info.token.__data[0] = 1;
    return args->args.visit.op(0, ".", &info, args->args.visit.op_data);
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockObjectOptional(void* /*obj*/,
                          const H5VL_loc_params_t* /*loc_params*/,
                          H5VL_optional_args_t* /*args*/, hid_t /*dxpl_id*/,
                          void** /*req*/) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_object_optional) {
    return -1;
  }
  return -1;
}

herr_t MockLinkCreate(H5VL_link_create_args_t* /*args*/, void* /*obj*/,
                      const H5VL_loc_params_t* /*loc_params*/,
                      hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                      void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_link_create) {
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockIntrospectGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                const H5VL_class_t** conn_cls) {
  *conn_cls = &g_mock_vol_cls;
  return 0;
}

herr_t MockIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/,
                              int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

herr_t MockRequestWait(void* req, uint64_t /*timeout*/,
                       H5VL_request_status_t* status) {
  *status = static_cast<MockRequest*>(req)->wait_status;
  return 0;
}

herr_t MockRequestFree(void* req) {
  if (g_mock_tracker != nullptr) {
    g_mock_tracker->FreeRequest(req);
  } else {
    delete static_cast<MockRequest*>(req);
  }
  return 0;
}

herr_t MockTokenCmp(void* /*obj*/, const H5O_token_t* token1,
                    const H5O_token_t* token2, int* cmp_value) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_token_cmp) {
    return -1;
  }
  *cmp_value = std::memcmp(token1, token2, sizeof(H5O_token_t));
  return 0;
}

herr_t MockTokenToStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                      const H5O_token_t* /*token*/, char** token_str) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_token_to_str) {
    return -1;
  }
  constexpr char kMockTok[] = "mock_tok";
  *token_str = static_cast<char*>(H5allocate_memory(sizeof(kMockTok), false));
  if (*token_str == nullptr) {
    return -1;
  }
  std::memcpy(*token_str, kMockTok, sizeof(kMockTok));
  return 0;
}

herr_t MockTokenFromStr(void* /*obj*/, H5I_type_t /*obj_type*/,
                        const char* token_str, H5O_token_t* token) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_token_from_str) {
    return -1;
  }
  *token = {};
  if (std::strcmp(token_str, "mock_tok") != 0) {
    return -1;
  }
  token->__data[0] = 0x5A;
  return 0;
}

class ScopedMockObjectVol {
 public:
  explicit ScopedMockObjectVol(MockVolTracker* tracker) {
    g_mock_tracker = tracker;
    g_mock_vol_cls = {};
    g_mock_vol_cls.version = H5VL_VERSION;
    g_mock_vol_cls.value = kMockVolValue;
    g_mock_vol_cls.name = kMockVolName;
    g_mock_vol_cls.cap_flags = H5VL_CAP_FLAG_ASYNC;
    g_mock_vol_cls.file_cls.create = MockFileCreate;
    g_mock_vol_cls.file_cls.close = MockFileClose;
    g_mock_vol_cls.group_cls.create = MockGroupCreate;
    g_mock_vol_cls.group_cls.close = MockGroupClose;
    g_mock_vol_cls.link_cls.create = MockLinkCreate;
    g_mock_vol_cls.object_cls.open = MockObjectOpen;
    g_mock_vol_cls.object_cls.copy = MockObjectCopy;
    g_mock_vol_cls.object_cls.get = MockObjectGet;
    g_mock_vol_cls.object_cls.specific = MockObjectSpecific;
    g_mock_vol_cls.object_cls.optional = MockObjectOptional;
    g_mock_vol_cls.introspect_cls.get_conn_cls = MockIntrospectGetConnCls;
    g_mock_vol_cls.introspect_cls.opt_query = MockIntrospectOptQuery;
    g_mock_vol_cls.request_cls.wait = MockRequestWait;
    g_mock_vol_cls.request_cls.free = MockRequestFree;
    g_mock_vol_cls.token_cls.cmp = MockTokenCmp;
    g_mock_vol_cls.token_cls.to_str = MockTokenToStr;
    g_mock_vol_cls.token_cls.from_str = MockTokenFromStr;
    vol_id_ = H5VLregister_connector(&g_mock_vol_cls, H5P_DEFAULT);
  }

  ~ScopedMockObjectVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_mock_tracker = nullptr;
  }

  ScopedMockObjectVol(const ScopedMockObjectVol&) = delete;
  ScopedMockObjectVol& operator=(const ScopedMockObjectVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

struct VisitState {
  std::vector<std::string> names;
  std::vector<H5O_type_t> types;
  int stop_after = -1;
  herr_t stop_code = 1;
  bool fail = false;
};

herr_t VisitCallback(hid_t /*obj*/, const char* name, const H5O_info2_t* info,
                     void* op_data) {
  auto* state = static_cast<VisitState*>(op_data);
  if (state->fail) {
    return -1;
  }
  state->names.push_back(name != nullptr ? name : "");
  state->types.push_back(info->type);
  if (state->stop_after > 0 &&
      static_cast<int>(state->names.size()) >= state->stop_after) {
    return state->stop_code;
  }
  return 0;
}

TEST(H5OTest, OpenByNameIdxTokenAndClose) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5o_open.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset_b(H5Dcreate2(grp.get(), "b_dset", H5T_NATIVE_INT32,
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(dset_b.valid());
  dset_b.reset();

  ScopedType committed_a(H5Tcopy(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(committed_a.valid());
  ASSERT_GE(H5Tcommit2(grp.get(), "a_dtype", committed_a.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  committed_a.reset();

  ScopedGroup sub_c(
      H5Gcreate2(grp.get(), "c_sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub_c.valid());
  sub_c.reset();

  ASSERT_GE(H5Lcreate_soft("/grp/b_dset", grp.get(), "d_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // H5Oopen & H5Oclose across group, dataset, committed datatype, and softlink.
  hid_t raw_g = H5Oopen(file.get(), "/grp", H5P_DEFAULT);
  ASSERT_GE(raw_g, 0);
  EXPECT_EQ(H5Iget_type(raw_g), H5I_GROUP);
  EXPECT_GE(H5Oclose(raw_g), 0);

  hid_t raw_d = H5Oopen(grp.get(), "b_dset", H5P_DEFAULT);
  ASSERT_GE(raw_d, 0);
  EXPECT_EQ(H5Iget_type(raw_d), H5I_DATASET);
  EXPECT_GE(H5Oclose(raw_d), 0);

  hid_t raw_t = H5Oopen(grp.get(), "a_dtype", H5P_DEFAULT);
  ASSERT_GE(raw_t, 0);
  EXPECT_EQ(H5Iget_type(raw_t), H5I_DATATYPE);
  EXPECT_GE(H5Oclose(raw_t), 0);

  hid_t raw_soft = H5Oopen(grp.get(), "d_soft", H5P_DEFAULT);
  ASSERT_GE(raw_soft, 0);
  EXPECT_EQ(H5Iget_type(raw_soft), H5I_DATASET);
  EXPECT_GE(H5Oclose(raw_soft), 0);

  // H5Oopen_by_idx across NAME and CRT_ORDER (INC & DEC).
  ScopedObject idx0_name(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                        H5_ITER_INC, 0, H5P_DEFAULT));
  ASSERT_TRUE(idx0_name.valid());
  EXPECT_EQ(H5Iget_type(idx0_name.get()), H5I_DATATYPE);

  ScopedObject idx0_crt(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_CRT_ORDER,
                                       H5_ITER_INC, 0, H5P_DEFAULT));
  ASSERT_TRUE(idx0_crt.valid());
  EXPECT_EQ(H5Iget_type(idx0_crt.get()), H5I_DATASET);

  ScopedObject idx_last_crt(H5Oopen_by_idx(
      file.get(), "/grp", H5_INDEX_CRT_ORDER, H5_ITER_DEC, 1, H5P_DEFAULT));
  ASSERT_TRUE(idx_last_crt.valid());
  EXPECT_EQ(H5Iget_type(idx_last_crt.get()), H5I_GROUP);

  // H5Oopen_by_token round-trip and H5Otoken_* comparisons/serialization.
  H5O_info2_t d_info{};
  ASSERT_GE(
      H5Oget_info_by_name3(grp.get(), "b_dset", &d_info, H5O_INFO_ALL,
                           H5P_DEFAULT),
      0);
  H5O_info2_t t_info{};
  ASSERT_GE(
      H5Oget_info_by_name3(grp.get(), "a_dtype", &t_info, H5O_INFO_ALL,
                           H5P_DEFAULT),
      0);

  ScopedObject tok_obj(H5Oopen_by_token(file.get(), d_info.token));
  ASSERT_TRUE(tok_obj.valid());
  EXPECT_EQ(H5Iget_type(tok_obj.get()), H5I_DATASET);
  H5O_info2_t reopened_info{};
  ASSERT_GE(H5Oget_info3(tok_obj.get(), &reopened_info, H5O_INFO_BASIC), 0);

  int cmp = 99;
  ASSERT_GE(H5Otoken_cmp(file.get(), &d_info.token, &reopened_info.token, &cmp),
            0);
  EXPECT_EQ(cmp, 0);

  int cmp_diff1 = 0;
  int cmp_diff2 = 0;
  ASSERT_GE(H5Otoken_cmp(file.get(), &d_info.token, &t_info.token, &cmp_diff1),
            0);
  ASSERT_GE(H5Otoken_cmp(file.get(), &t_info.token, &d_info.token, &cmp_diff2),
            0);
  EXPECT_NE(cmp_diff1, 0);
  EXPECT_EQ(cmp_diff1 < 0, cmp_diff2 > 0);

  char* tok_str = nullptr;
  ASSERT_GE(H5Otoken_to_str(file.get(), &d_info.token, &tok_str), 0);
  ASSERT_NE(tok_str, nullptr);
  absl::Cleanup free_tok_str = [&] { H5free_memory(tok_str); };
  H5O_token_t parsed_token{};
  ASSERT_GE(H5Otoken_from_str(file.get(), tok_str, &parsed_token), 0);
  ASSERT_GE(H5Otoken_cmp(file.get(), &d_info.token, &parsed_token, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  // H5Oexists_by_name on hard link, valid soft link, and dangling/missing links.
  ASSERT_GE(H5Lcreate_soft("/grp/missing_leaf", grp.get(), "d_dangling_leaf",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/no/such/target", grp.get(), "d_dangling_parent",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Oexists_by_name(grp.get(), "b_dset", H5P_DEFAULT), 0);
  EXPECT_GT(H5Oexists_by_name(grp.get(), "d_soft", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Oexists_by_name(grp.get(), "d_dangling_leaf", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Oexists_by_name(file.get(), "/no_such_link", H5P_DEFAULT), 0);

  // Negative error paths for open/close/token/exists.
  ScopedAttr attr(H5Acreate2(grp.get(), "attr", H5T_NATIVE_INT32, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Oopen(file.get(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Oopen(file.get(), "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oopen(scalar.get(), "/grp", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oopen(file.get(), "/grp", fapl.get()), 0);
  EXPECT_LT(H5Oopen(file.get(), "/missing", H5P_DEFAULT), 0);

  EXPECT_LT(H5Oopen_by_idx(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_UNKNOWN, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_N, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                           0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_N, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC, 99,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_idx(scalar.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oopen_by_token(file.get(), H5O_TOKEN_UNDEF), 0);
  EXPECT_LT(H5Oopen_by_token(scalar.get(), d_info.token), 0);
  EXPECT_LT(H5Oopen_by_token(H5I_INVALID_HID, d_info.token), 0);

  // Reject non-closeable ID types via H5O__close_check_type.
  EXPECT_LT(H5Oclose(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Oclose(file.get()), 0);
  EXPECT_LT(H5Oclose(scalar.get()), 0);
  EXPECT_LT(H5Oclose(attr.get()), 0);
  EXPECT_LT(H5Oclose(fapl.get()), 0);
  EXPECT_LT(H5Oclose(es.get()), 0);

  EXPECT_LT(H5Oexists_by_name(file.get(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Oexists_by_name(file.get(), "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oexists_by_name(file.get(), "/grp", fapl.get()), 0);
  EXPECT_LT(H5Oexists_by_name(scalar.get(), "/grp", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oexists_by_name(grp.get(), "d_dangling_parent", H5P_DEFAULT), 0);

  EXPECT_LT(H5Otoken_cmp(scalar.get(), &d_info.token, &d_info.token, &cmp), 0);
  EXPECT_LT(H5Otoken_cmp(file.get(), &d_info.token, &d_info.token, nullptr), 0);
  EXPECT_LT(H5Otoken_to_str(scalar.get(), &d_info.token, &tok_str), 0);
  EXPECT_LT(H5Otoken_to_str(file.get(), nullptr, &tok_str), 0);
  EXPECT_LT(H5Otoken_to_str(file.get(), &d_info.token, nullptr), 0);
  EXPECT_LT(H5Otoken_from_str(scalar.get(), tok_str, &parsed_token), 0);
  EXPECT_LT(H5Otoken_from_str(file.get(), nullptr, &parsed_token), 0);
  EXPECT_LT(H5Otoken_from_str(file.get(), tok_str, nullptr), 0);
}

TEST(H5OTest, CopyLinkRefcountFlushAndRefresh) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile src_file(
      H5Fcreate("h5o_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file(
      H5Fcreate("h5o_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(dst_file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedGroup src_tree(H5Gcreate2(src_file.get(), "/tree", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_tree.valid());
  ScopedGroup src_child(H5Gcreate2(src_tree.get(), "child", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_child.valid());

  ScopedDataset src_dset(H5Dcreate2(src_tree.get(), "d", H5T_NATIVE_INT32,
                                    scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(src_dset.valid());
  const int32_t v = 123;
  ASSERT_GE(H5Dwrite(src_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &v),
            0);
  ScopedAttr src_attr(H5Acreate2(src_dset.get(), "tag", H5T_NATIVE_INT32,
                                 scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_attr.valid());
  ASSERT_GE(H5Awrite(src_attr.get(), H5T_NATIVE_INT32, &v), 0);
  src_attr.reset();

  // Flush and refresh on writable file objects.
  ASSERT_GE(H5Oflush(src_dset.get()), 0);
  ASSERT_GE(H5Orefresh(src_dset.get()), 0);
  ASSERT_GE(H5Oflush(src_tree.get()), 0);

  ScopedType src_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(src_dt.valid());
  ASSERT_GE(H5Tcommit2(src_tree.get(), "dt", src_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Oflush(src_dt.get()), 0);
  src_dt.reset();
  src_dset.reset();
  src_child.reset();
  ASSERT_GE(H5Lcreate_soft("/tree/d", src_tree.get(), "soft_d", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  src_tree.reset();

  // Copy with custom lcpl intermediate groups, shallow hierarchy, expand soft,
  // and without attrs.
  ScopedPlist lcpl_interm(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_interm.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl_interm.get(), 1), 0);

  ASSERT_GE(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "/nested/full",
                    H5P_DEFAULT, lcpl_interm.get()),
            0);
  EXPECT_GT(H5Lexists(dst_file.get(), "/nested/full/child", H5P_DEFAULT), 0);

  ScopedPlist ocpy_shallow(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy_shallow.valid());
  ASSERT_GE(H5Pset_copy_object(
                ocpy_shallow.get(),
                H5O_COPY_SHALLOW_HIERARCHY_FLAG |
                    H5O_COPY_EXPAND_SOFT_LINK_FLAG |
                    H5O_COPY_WITHOUT_ATTR_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "/shallow",
                    ocpy_shallow.get(), H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst_file.get(), "/shallow/d", H5P_DEFAULT), 0);

  // H5Olink with anonymous group, dataset, datatype + additional hard link.
  ScopedGroup anon_g(H5Gcreate_anon(dst_file.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_g.valid());
  ASSERT_GE(
      H5Olink(anon_g.get(), dst_file.get(), "/extra/anon_grp",
              lcpl_interm.get(), H5P_DEFAULT),
      0);
  ASSERT_GE(
      H5Olink(anon_g.get(), dst_file.get(), "/anon_grp_alias", H5P_DEFAULT,
              H5P_DEFAULT),
      0);

  ScopedDataset anon_d(H5Dcreate_anon(dst_file.get(), H5T_NATIVE_INT32,
                                      scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_d.valid());
  ASSERT_GE(H5Olink(anon_d.get(), dst_file.get(), "/anon_dset", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  // Refcount adjustment.
  H5O_info2_t info{};
  ASSERT_GE(H5Oget_info3(anon_d.get(), &info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(info.rc, 1u);
  ASSERT_GE(H5Oincr_refcount(anon_d.get()), 0);
  ASSERT_GE(H5Oget_info3(anon_d.get(), &info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(info.rc, 2u);
  ASSERT_GE(H5Odecr_refcount(anon_d.get()), 0);
  ASSERT_GE(H5Oget_info3(anon_d.get(), &info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(info.rc, 1u);
  anon_d.reset();
  anon_g.reset();

  // Open src_file image read-only to exercise full H5Orefresh path on RDONLY
  // and read-only failures on incr/decr_refcount & set_comment.
  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t img_sz = H5Fget_file_image(src_file.get(), nullptr, 0);
  ASSERT_GT(img_sz, 0);
  std::vector<uint8_t> img(static_cast<size_t>(img_sz));
  ASSERT_EQ(H5Fget_file_image(src_file.get(), img.data(), img.size()), img_sz);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), img.data(), img.size()), 0);
  ScopedFile ro_file(
      H5Fopen("h5o_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "/tree/d", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset.valid());
  ScopedType ro_dt(H5Topen2(ro_file.get(), "/tree/dt", H5P_DEFAULT));
  ASSERT_TRUE(ro_dt.valid());
  ASSERT_GE(H5Orefresh(ro_dset.get()), 0);
  ASSERT_GE(H5Orefresh(ro_dt.get()), 0);

  // Argument errors for copy/link/flush/refresh/refcount.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Ocopy(src_file.get(), nullptr, dst_file.get(), "/x", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "", dst_file.get(), "/x", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", dst_file.get(), nullptr,
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "/x", H5P_DEFAULT,
                    fapl.get()),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "/x", fapl.get(),
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(scalar.get(), "/tree", dst_file.get(), "/x", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", scalar.get(), "/x", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ocopy(src_file.get(), "/tree", dst_file.get(), "/shallow",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Olink(ro_dset.get(), H5L_SAME_LOC, "/bad", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Olink(ro_dset.get(), ro_file.get(), nullptr, H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Olink(ro_dset.get(), ro_file.get(), "", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Olink(ro_dset.get(), ro_file.get(), "/bad", fapl.get(),
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Olink(ro_dset.get(), ro_file.get(), "/bad", H5P_DEFAULT,
                    fapl.get()),
            0);
  EXPECT_LT(H5Olink(scalar.get(), dst_file.get(), "/bad", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Olink(ro_dset.get(), scalar.get(), "/bad", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Olink(ro_dset.get(), ro_file.get(), "/tree/d", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oflush(scalar.get()), 0);
  EXPECT_LT(H5Oflush(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Orefresh(scalar.get()), 0);
  EXPECT_LT(H5Orefresh(H5I_INVALID_HID), 0);

  EXPECT_LT(H5Oincr_refcount(scalar.get()), 0);
  EXPECT_LT(H5Oincr_refcount(ro_dset.get()), 0);
  EXPECT_LT(H5Odecr_refcount(scalar.get()), 0);
  EXPECT_LT(H5Odecr_refcount(ro_dset.get()), 0);
}

TEST(H5OTest, InfoNativeInfoCommentsVisitAndCorking) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5o_info_visit.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "d0", H5T_NATIVE_INT32, scalar.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedAttr attr(H5Acreate2(dset.get(), "a0", H5T_NATIVE_INT32, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  attr.reset();

  // Comments via object ID and name (initial empty, set, truncate, clear).
  char buf[32] = "sentinel";
  EXPECT_EQ(H5Oget_comment(dset.get(), buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "");

  ASSERT_GE(H5Oset_comment(dset.get(), "hello comment"), 0);
  EXPECT_EQ(H5Oget_comment(dset.get(), nullptr, 0), 13);
  char short_buf[6]{};
  EXPECT_EQ(H5Oget_comment(dset.get(), short_buf, sizeof(short_buf)), 13);
  EXPECT_EQ(std::string(short_buf, sizeof(short_buf)), "hello ");
  EXPECT_EQ(H5Oget_comment(dset.get(), buf, sizeof(buf)), 13);
  EXPECT_STREQ(buf, "hello comment");

  ASSERT_GE(
      H5Oset_comment_by_name(grp.get(), "d0", "updated", H5P_DEFAULT), 0);
  EXPECT_EQ(
      H5Oget_comment_by_name(grp.get(), "d0", buf, sizeof(buf), H5P_DEFAULT),
      7);
  EXPECT_STREQ(buf, "updated");

  ASSERT_GE(H5Oset_comment_by_name(grp.get(), "d0", nullptr, H5P_DEFAULT), 0);
  EXPECT_EQ(
      H5Oget_comment_by_name(grp.get(), "d0", buf, sizeof(buf), H5P_DEFAULT),
      0);

  // H5Oget_info3 & H5Oget_native_info variants.
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(dset.get(), &oinfo, H5O_INFO_ALL), 0);
  EXPECT_EQ(oinfo.type, H5O_TYPE_DATASET);
  EXPECT_EQ(oinfo.num_attrs, 1u);
  ASSERT_GE(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_BASIC,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(oinfo.type, H5O_TYPE_DATASET);

  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(dset.get(), &ninfo, H5O_NATIVE_INFO_ALL), 0);
  EXPECT_GT(ninfo.hdr.space.total, 0u);
  ASSERT_GE(H5Oget_native_info_by_name(grp.get(), "d0", &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_META_SIZE, H5P_DEFAULT),
            0);

  // H5Ovisit3 & H5Ovisit_by_name3 (full, early stop, operator error).
  VisitState visit_all;
  EXPECT_EQ(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &visit_all, H5O_INFO_ALL),
            0);
  EXPECT_THAT(visit_all.names, UnorderedElementsAre(".", "grp", "grp/d0"));

  VisitState visit_by_name;
  EXPECT_EQ(
      H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_CRT_ORDER, H5_ITER_DEC,
                        VisitCallback, &visit_by_name, H5O_INFO_BASIC,
                        H5P_DEFAULT),
      0);
  EXPECT_THAT(visit_by_name.names, ElementsAre(".", "d0"));

  VisitState visit_stop;
  visit_stop.stop_after = 1;
  visit_stop.stop_code = 42;
  EXPECT_EQ(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_NATIVE, VisitCallback,
                      &visit_stop, H5O_INFO_BASIC),
            42);

  // Metadata cache corking (public + internal APIs).
  bool corked = true;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(dset.get(), &corked), 0);
  EXPECT_FALSE(corked);
  ASSERT_GE(H5Odisable_mdc_flushes(dset.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(dset.get(), &corked), 0);
  EXPECT_TRUE(corked);
  ASSERT_GE(H5Oenable_mdc_flushes(dset.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(dset.get(), &corked), 0);
  EXPECT_FALSE(corked);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t* oloc = H5O_get_loc(dset.get());
    ASSERT_NE(oloc, nullptr);
    ASSERT_GE(H5O__disable_mdc_flushes(oloc), 0);
    bool internal_corked = false;
    ASSERT_GE(H5O__are_mdc_flushes_disabled(oloc, &internal_corked), 0);
    EXPECT_TRUE(internal_corked);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__disable_mdc_flushes(oloc), 0);
    }
    ASSERT_GE(H5O__enable_mdc_flushes(oloc), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O__enable_mdc_flushes(oloc), 0);
    }
  }

  // Invalid argument & error paths.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Oget_info3(dset.get(), nullptr, H5O_INFO_ALL), 0);
  EXPECT_LT(H5Oget_info3(dset.get(), &oinfo, 0x8000u), 0);
  EXPECT_LT(H5Oget_info3(scalar.get(), &oinfo, H5O_INFO_BASIC), 0);

  EXPECT_LT(H5Oget_info_by_name3(grp.get(), nullptr, &oinfo, H5O_INFO_BASIC,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_name3(grp.get(), "d0", nullptr, H5O_INFO_BASIC,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Oget_info_by_name3(grp.get(), "d0", &oinfo, 0x8000u, H5P_DEFAULT), 0);
  EXPECT_LT(H5Oget_info_by_name3(grp.get(), "missing", &oinfo, H5O_INFO_BASIC,
                                 H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oget_info_by_idx3(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, &oinfo, H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_UNKNOWN,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_BASIC,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_NAME,
                                H5_ITER_UNKNOWN, 0, &oinfo, H5O_INFO_BASIC,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                                0, nullptr, H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                                0, &oinfo, 0x8000u, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                                0, &oinfo, H5O_INFO_BASIC, fapl.get()),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(scalar.get(), "/grp", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_BASIC,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                                99, &oinfo, H5O_INFO_BASIC, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oget_native_info(dset.get(), nullptr, H5O_NATIVE_INFO_ALL), 0);
  EXPECT_LT(H5Oget_native_info(dset.get(), &ninfo, 0x8000u), 0);
  EXPECT_LT(H5Oget_native_info(scalar.get(), &ninfo, H5O_NATIVE_INFO_HDR), 0);

  EXPECT_LT(H5Oget_native_info_by_name(grp.get(), nullptr, &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_name(grp.get(), "", &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_name(grp.get(), "d0", nullptr,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_name(grp.get(), "d0", &ninfo, 0x8000u,
                                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_name(grp.get(), "d0", &ninfo,
                                       H5O_NATIVE_INFO_HDR, fapl.get()),
            0);
  EXPECT_LT(H5Oget_native_info_by_name(scalar.get(), "d0", &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), nullptr, H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_UNKNOWN,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_UNKNOWN, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, nullptr,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo, 0x8000u,
                                      H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, fapl.get()),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(scalar.get(), "/grp", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oset_comment(scalar.get(), "c"), 0);
  EXPECT_LT(H5Oset_comment_by_name(grp.get(), nullptr, "c", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oset_comment_by_name(grp.get(), "", "c", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oset_comment_by_name(grp.get(), "d0", "c", fapl.get()), 0);
  EXPECT_LT(H5Oset_comment_by_name(scalar.get(), "d0", "c", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oset_comment_by_name(grp.get(), "missing", "c", H5P_DEFAULT), 0);

  EXPECT_LT(H5Oget_comment(scalar.get(), buf, sizeof(buf)), 0);
  EXPECT_LT(
      H5Oget_comment_by_name(grp.get(), nullptr, buf, sizeof(buf), H5P_DEFAULT),
      0);
  EXPECT_LT(H5Oget_comment_by_name(grp.get(), "", buf, sizeof(buf), H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Oget_comment_by_name(grp.get(), "d0", buf, sizeof(buf), fapl.get()), 0);
  EXPECT_LT(
      H5Oget_comment_by_name(scalar.get(), "d0", buf, sizeof(buf), H5P_DEFAULT),
      0);
  EXPECT_LT(H5Oget_comment_by_name(grp.get(), "missing", buf, sizeof(buf),
                                   H5P_DEFAULT),
            0);

  VisitState visit_err;
  visit_err.fail = true;
  EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &visit_err, H5O_INFO_BASIC),
            0);
  EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, VisitCallback,
                      &visit_all, H5O_INFO_BASIC),
            0);
  EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, VisitCallback,
                      &visit_all, H5O_INFO_BASIC),
            0);
  EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                      &visit_all, H5O_INFO_BASIC),
            0);
  EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &visit_all, 0x8000u),
            0);
  EXPECT_LT(H5Ovisit3(scalar.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &visit_all, H5O_INFO_BASIC),
            0);

  EXPECT_LT(H5Ovisit_by_name3(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &visit_all, H5O_INFO_BASIC,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &visit_all, H5O_INFO_BASIC,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_UNKNOWN, H5_ITER_INC,
                              VisitCallback, &visit_all, H5O_INFO_BASIC,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_NAME,
                              H5_ITER_UNKNOWN, VisitCallback, &visit_all,
                              H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                              nullptr, &visit_all, H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &visit_all, 0x8000u, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &visit_all, H5O_INFO_BASIC,
                              fapl.get()),
            0);
  EXPECT_LT(H5Ovisit_by_name3(scalar.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &visit_all, H5O_INFO_BASIC,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name3(file.get(), "/missing", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &visit_all,
                              H5O_INFO_BASIC, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Odisable_mdc_flushes(scalar.get()), 0);
  EXPECT_LT(H5Oenable_mdc_flushes(scalar.get()), 0);
  EXPECT_LT(H5Oare_mdc_flushes_disabled(dset.get(), nullptr), 0);
  EXPECT_LT(H5Oare_mdc_flushes_disabled(scalar.get(), &corked), 0);
}

TEST(H5OTest, AsyncApisAndMockVolErrorPropagation) {
  // 1. Synchronous fallback via H5ES_NONE on native Core VFD.
  ScopedFile native_file = CreateInMemoryFile("h5o_async_native.h5");
  ASSERT_TRUE(native_file.valid());
  ScopedGroup native_grp(H5Gcreate2(native_file.get(), "/g", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(native_grp.valid());
  native_grp.reset();

  hid_t native_open =
      H5Oopen_async(native_file.get(), "/g", H5P_DEFAULT, H5ES_NONE);
  ASSERT_GE(native_open, 0);
  ASSERT_GE(H5Oflush_async(native_open, H5ES_NONE), 0);
  ASSERT_GE(H5Orefresh_async(native_open, H5ES_NONE), 0);
  H5O_info2_t native_info{};
  ASSERT_GE(H5Oget_info_by_name_async(native_file.get(), "/g", &native_info,
                                      H5O_INFO_BASIC, H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(
      H5Ocopy_async(native_file.get(), "/g", native_file.get(), "/g_copy",
                    H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
      0);
  ASSERT_GE(H5Oclose_async(native_open, H5ES_NONE), 0);

  hid_t native_idx = H5Oopen_by_idx_async(native_file.get(), "/", H5_INDEX_NAME,
                                          H5_ITER_INC, 0, H5P_DEFAULT,
                                          H5ES_NONE);
  ASSERT_GE(native_idx, 0);
  ASSERT_GE(H5Oclose_async(native_idx, H5ES_NONE), 0);

  // 2. Real async requests via Mock Async VOL connector & ScopedEventSet.
  MockVolTracker tracker;
  ScopedMockObjectVol mock_vol(&tracker);
  ASSERT_GE(mock_vol.id(), 0);

  ScopedPlist mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(mock_fapl.valid());
  ASSERT_GE(H5Pset_vol(mock_fapl.get(), mock_vol.id(), nullptr), 0);
  ScopedFile mock_file(H5Fcreate("mock_h5o.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                 mock_fapl.get()));
  ASSERT_TRUE(mock_file.valid());
  ScopedGroup mock_grp(H5Gcreate2(mock_file.get(), "/g", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mock_grp.valid());

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  hid_t async_o = H5Oopen_async(mock_file.get(), "/g", H5P_DEFAULT, es.get());
  ASSERT_GE(async_o, 0);
  hid_t async_idx = H5Oopen_by_idx_async(
      mock_file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0, H5P_DEFAULT,
      es.get());
  ASSERT_GE(async_idx, 0);
  H5O_info2_t async_info{};
  ASSERT_GE(H5Oget_info_by_name_async(mock_file.get(), "/g", &async_info,
                                      H5O_INFO_ALL, H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Ocopy_async(mock_file.get(), "/g", mock_file.get(), "/g2",
                          H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Oflush_async(async_o, es.get()), 0);
  ASSERT_GE(H5Orefresh_async(async_o, es.get()), 0);
  ASSERT_GE(H5Oclose_async(async_o, es.get()), 0);
  ASSERT_GE(H5Oclose_async(async_idx, es.get()), 0);

  size_t num_in_progress = 1;
  bool op_failed = true;
  ASSERT_GE(H5ESwait(es.get(), 0, &num_in_progress, &op_failed), 0);
  EXPECT_EQ(num_in_progress, 0u);
  EXPECT_FALSE(op_failed);

  // 3. Error & cancellation paths through Mock VOL and invalid es_id.
  ScopedPlist non_es_plist(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(non_es_plist.valid());
  ScopedType uncommitted_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(uncommitted_dt.valid());

  ScopedErrorSilence silence;

  // Non-eventset es_id causes H5ES_insert failure with token != nullptr.
  EXPECT_LT(
      H5Oopen_async(mock_file.get(), "/g", H5P_DEFAULT, non_es_plist.get()), 0);
  EXPECT_LT(H5Oopen_by_idx_async(mock_file.get(), "/", H5_INDEX_NAME,
                                 H5_ITER_INC, 0, H5P_DEFAULT,
                                 non_es_plist.get()),
            0);
  EXPECT_LT(H5Ocopy_async(mock_file.get(), "/g", mock_file.get(), "/g2",
                          H5P_DEFAULT, H5P_DEFAULT, non_es_plist.get()),
            0);
  EXPECT_LT(H5Oflush_async(mock_grp.get(), non_es_plist.get()), 0);
  EXPECT_LT(H5Orefresh_async(mock_grp.get(), non_es_plist.get()), 0);
  EXPECT_LT(H5Oget_info_by_name_async(mock_file.get(), "/g", &async_info,
                                      H5O_INFO_ALL, H5P_DEFAULT,
                                      non_es_plist.get()),
            0);

  hid_t to_close_fail_es =
      H5Oopen_async(mock_file.get(), "/g", H5P_DEFAULT, es.get());
  ASSERT_GE(to_close_fail_es, 0);
  ASSERT_GE(H5ESwait(es.get(), 0, &num_in_progress, &op_failed), 0);
  EXPECT_LT(H5Oclose_async(to_close_fail_es, non_es_plist.get()), 0);

  // Uncommitted datatype in H5Oclose_async with valid es_id triggers
  // H5VL_vol_object == NULL branch.
  EXPECT_LT(H5Oclose_async(uncommitted_dt.get(), es.get()), 0);
  EXPECT_LT(H5Oclose_async(H5I_INVALID_HID, es.get()), 0);

  // Simulated VOL failures.
  tracker.fail_object_open = true;
  EXPECT_LT(H5Oopen_async(mock_file.get(), "/g", H5P_DEFAULT, es.get()), 0);
  EXPECT_LT(H5Oopen_by_idx_async(mock_file.get(), "/", H5_INDEX_NAME,
                                 H5_ITER_INC, 0, H5P_DEFAULT, es.get()),
            0);
  H5O_token_t dummy_tok{};
  dummy_tok.__data[0] = 0x5A;
  EXPECT_LT(H5Oopen_by_token(mock_file.get(), dummy_tok), 0);
  tracker.fail_object_open = false;

  tracker.bad_open_type = true;
  EXPECT_LT(H5Oopen(mock_file.get(), "/g", H5P_DEFAULT), 0);
  EXPECT_LT(H5Oopen_by_idx(mock_file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oopen_by_token(mock_file.get(), dummy_tok), 0);
  tracker.bad_open_type = false;

  tracker.fail_object_copy = true;
  EXPECT_LT(H5Ocopy_async(mock_file.get(), "/g", mock_file.get(), "/g2",
                          H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);
  tracker.fail_object_copy = false;

  tracker.fail_object_get = true;
  EXPECT_LT(H5Oget_info_by_name_async(mock_file.get(), "/g", &async_info,
                                      H5O_INFO_ALL, H5P_DEFAULT, es.get()),
            0);
  tracker.fail_object_get = false;

  tracker.fail_object_specific = true;
  EXPECT_LT(H5Oflush_async(mock_grp.get(), es.get()), 0);
  EXPECT_LT(H5Orefresh_async(mock_grp.get(), es.get()), 0);
  tracker.fail_object_specific = false;

  // Fail group close once for H5Oclose and H5Oclose_async, then close cleanly.
  hid_t close_fail_sync = H5Oopen(mock_file.get(), "/g", H5P_DEFAULT);
  ASSERT_GE(close_fail_sync, 0);
  tracker.fail_group_close_once = true;
  EXPECT_LT(H5Oclose(close_fail_sync), 0);
  ASSERT_GE(H5Oclose(close_fail_sync), 0);

  hid_t close_fail_async =
      H5Oopen_async(mock_file.get(), "/g", H5P_DEFAULT, es.get());
  ASSERT_GE(close_fail_async, 0);
  ASSERT_GE(H5ESwait(es.get(), 0, &num_in_progress, &op_failed), 0);
  tracker.fail_group_close_once = true;
  EXPECT_LT(H5Oclose_async(close_fail_async, H5ES_NONE), 0);
  ASSERT_GE(H5Oclose_async(close_fail_async, H5ES_NONE), 0);

  // Cross-VOL mismatch on H5Olink, plus optional/token failures on mock VOL.
  EXPECT_LT(H5Olink(mock_grp.get(), native_file.get(), "/cross", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  tracker.fail_link_create = true;
  EXPECT_LT(H5Olink(mock_grp.get(), mock_file.get(), "/fail_link", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  tracker.fail_link_create = false;

  H5O_native_info_t mock_ninfo{};
  EXPECT_LT(
      H5Oget_native_info(mock_grp.get(), &mock_ninfo, H5O_NATIVE_INFO_ALL), 0);
  EXPECT_LT(H5Oget_native_info_by_name(mock_file.get(), "/g", &mock_ninfo,
                                       H5O_NATIVE_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_native_info_by_idx(mock_file.get(), "/", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &mock_ninfo,
                                      H5O_NATIVE_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Odisable_mdc_flushes(mock_grp.get()), 0);
  EXPECT_LT(H5Oenable_mdc_flushes(mock_grp.get()), 0);
  bool mock_corked = false;
  EXPECT_LT(H5Oare_mdc_flushes_disabled(mock_grp.get(), &mock_corked), 0);

  tracker.fail_token_cmp = true;
  int cmp = 0;
  EXPECT_LT(H5Otoken_cmp(mock_file.get(), &dummy_tok, &dummy_tok, &cmp), 0);
  tracker.fail_token_cmp = false;

  tracker.fail_token_to_str = true;
  char* bad_str = nullptr;
  EXPECT_LT(H5Otoken_to_str(mock_file.get(), &dummy_tok, &bad_str), 0);
  tracker.fail_token_to_str = false;

  tracker.fail_token_from_str = true;
  EXPECT_LT(H5Otoken_from_str(mock_file.get(), "mock_tok", &dummy_tok), 0);
  tracker.fail_token_from_str = false;

  ASSERT_GE(H5ESwait(es.get(), 0, &num_in_progress, &op_failed), 0);
}

}  // namespace
}  // namespace hdf5
