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
// Tests public link management, creation, inspection, iteration, visitation,
// user-defined link registration, external-link unpacking, and asynchronous
// H5L*_async APIs (third_party/hdf5/src/H5L.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5L.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5L_type_t kCustomUdId1 = static_cast<H5L_type_t>(182);
constexpr H5L_type_t kCustomUdId0 = static_cast<H5L_type_t>(183);
constexpr H5VL_class_value_t kMockVolValue = 322;
constexpr char kMockVolName[] = "h5l_async_mock_vol";

// Custom UD link tracker for testing H5Lregister / H5Lcreate_ud / move / copy.
struct UdTracker {
  int creates = 0;
  int moves = 0;
  int copies = 0;
  int deletes = 0;
  int traversals = 0;
};

UdTracker* g_ud_tracker = nullptr;

herr_t UdCreateCb(const char* /*link_name*/, hid_t /*loc_group*/,
                  const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                  hid_t /*lcpl_id*/) {
  if (g_ud_tracker) ++g_ud_tracker->creates;
  return 0;
}

herr_t UdMoveCb(const char* /*new_name*/, hid_t /*new_loc*/,
                const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  if (g_ud_tracker) ++g_ud_tracker->moves;
  return 0;
}

herr_t UdCopyCb(const char* /*new_name*/, hid_t /*new_loc*/,
                const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  if (g_ud_tracker) ++g_ud_tracker->copies;
  return 0;
}

hid_t UdTraverseCb(const char* /*link_name*/, hid_t cur_group,
                   const void* lnkdata, size_t lnkdata_size, hid_t lapl_id,
                   hid_t /*dxpl_id*/) {
  if (g_ud_tracker) ++g_ud_tracker->traversals;
  if (lnkdata != nullptr && lnkdata_size > 0) {
    return H5Oopen(cur_group, static_cast<const char*>(lnkdata), lapl_id);
  }
  return H5I_INVALID_HID;
}

hid_t UdTraverse0Cb(const char* /*link_name*/, hid_t cur_group,
                    const void* lnkdata, size_t lnkdata_size, hid_t lapl_id) {
  if (g_ud_tracker) ++g_ud_tracker->traversals;
  if (lnkdata != nullptr && lnkdata_size > 0) {
    return H5Oopen(cur_group, static_cast<const char*>(lnkdata), lapl_id);
  }
  return H5I_INVALID_HID;
}

herr_t UdDeleteCb(const char* /*link_name*/, hid_t /*file*/,
                  const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  if (g_ud_tracker) ++g_ud_tracker->deletes;
  return 0;
}

ssize_t UdQueryCb(const char* /*link_name*/, const void* lnkdata,
                  size_t lnkdata_size, void* buf, size_t buf_size) {
  if (buf != nullptr && buf_size > 0 && lnkdata != nullptr &&
      lnkdata_size > 0) {
    const size_t n = std::min(buf_size, lnkdata_size);
    std::memcpy(buf, lnkdata, n);
  }
  return static_cast<ssize_t>(lnkdata_size);
}

// Mock async VOL connector covering async tokens, ES insert failures,
// mismatched VOL connectors, and VOL-layer failure propagation.
struct MockRequest {
  H5VL_request_status_t wait_status = H5VL_REQUEST_STATUS_SUCCEED;
};

struct MockVolTracker {
  bool fail_link_create = false;
  bool fail_link_copy = false;
  bool fail_link_move = false;
  bool fail_link_get = false;
  bool fail_link_specific = false;
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

struct MockFile {
  MockVolTracker* tracker = nullptr;
};

struct MockGroup {
  MockVolTracker* tracker = nullptr;
};

MockVolTracker* g_mock_tracker = nullptr;
H5VL_class_t g_mock_vol_cls{};

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  return new MockFile{g_mock_tracker};
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
  return new MockGroup{g_mock_tracker};
}

herr_t MockGroupClose(void* grp, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  delete static_cast<MockGroup*>(grp);
  return 0;
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

herr_t MockLinkCopy(void* /*src_obj*/, const H5VL_loc_params_t* /*loc_params1*/,
                    void* /*dst_obj*/, const H5VL_loc_params_t* /*loc_params2*/,
                    hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_link_copy) {
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockLinkMove(void* /*src_obj*/, const H5VL_loc_params_t* /*loc_params1*/,
                    void* /*dst_obj*/, const H5VL_loc_params_t* /*loc_params2*/,
                    hid_t /*lcpl_id*/, hid_t /*lapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_link_move) {
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockLinkGet(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                   H5VL_link_get_args_t* args, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_link_get) {
    return -1;
  }
  if (args->op_type == H5VL_LINK_GET_INFO && args->args.get_info.linfo) {
    H5L_info2_t* linfo = args->args.get_info.linfo;
    *linfo = {};
    linfo->type = H5L_TYPE_SOFT;
    linfo->cset = H5T_CSET_ASCII;
    linfo->u.val_size = 5;
  } else if (args->op_type == H5VL_LINK_GET_NAME) {
    const std::string kName = "mock_link";
    if (args->args.get_name.name_len != nullptr) {
      *args->args.get_name.name_len = kName.size();
    }
    if (args->args.get_name.name != nullptr &&
        args->args.get_name.name_size > 0) {
      const size_t copy_len =
          std::min(kName.size(), args->args.get_name.name_size - 1);
      std::memcpy(args->args.get_name.name, kName.data(), copy_len);
      args->args.get_name.name[copy_len] = '\0';
    }
  } else if (args->op_type == H5VL_LINK_GET_VAL) {
    if (args->args.get_val.buf != nullptr && args->args.get_val.buf_size > 0) {
      const char kVal[] = "mock";
      const size_t copy_len =
          std::min(sizeof(kVal), args->args.get_val.buf_size);
      std::memcpy(args->args.get_val.buf, kVal, copy_len);
    }
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockLinkSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                        H5VL_link_specific_args_t* args, hid_t /*dxpl_id*/,
                        void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_link_specific) {
    return -1;
  }
  if (args->op_type == H5VL_LINK_EXISTS &&
      args->args.exists.exists != nullptr) {
    *args->args.exists.exists = true;
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

class ScopedMockLinkVol {
 public:
  explicit ScopedMockLinkVol(MockVolTracker* tracker) {
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
    g_mock_vol_cls.link_cls.copy = MockLinkCopy;
    g_mock_vol_cls.link_cls.move = MockLinkMove;
    g_mock_vol_cls.link_cls.get = MockLinkGet;
    g_mock_vol_cls.link_cls.specific = MockLinkSpecific;
    g_mock_vol_cls.introspect_cls.get_conn_cls = MockIntrospectGetConnCls;
    g_mock_vol_cls.introspect_cls.opt_query = MockIntrospectOptQuery;
    g_mock_vol_cls.request_cls.wait = MockRequestWait;
    g_mock_vol_cls.request_cls.free = MockRequestFree;
    vol_id_ = H5VLregister_connector(&g_mock_vol_cls, H5P_DEFAULT);
  }

  ~ScopedMockLinkVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_mock_tracker = nullptr;
  }

  ScopedMockLinkVol(const ScopedMockLinkVol&) = delete;
  ScopedMockLinkVol& operator=(const ScopedMockLinkVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

struct Iter2State {
  std::vector<std::string> names;
  int stop_after = -1;
  herr_t stop_code = 1;
  bool fail = false;
};

herr_t Iter2Callback(hid_t /*group*/, const char* name,
                     const H5L_info2_t* /*info*/, void* op_data) {
  auto* st = static_cast<Iter2State*>(op_data);
  if (st->fail) {
    return -1;
  }
  st->names.push_back(name ? name : "");
  if (st->stop_after > 0 &&
      static_cast<int>(st->names.size()) >= st->stop_after) {
    return st->stop_code;
  }
  return 0;
}

TEST(H5LTest, CreateMoveCopyDeleteAndQueryLinks) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5l_crud.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup src_grp(
      H5Gcreate2(file.get(), "/src", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(src_grp.valid());
  ScopedGroup dst_grp(
      H5Gcreate2(file.get(), "/dst", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dst_grp.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(src_grp.get(), "d0", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  // 1. H5Lcreate_hard: explicit locs and H5L_SAME_LOC on src or dst.
  ASSERT_GE(H5Lcreate_hard(src_grp.get(), "d0", dst_grp.get(), "d_hard1",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(H5L_SAME_LOC, "d_hard1", dst_grp.get(), "d_hard2",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(src_grp.get(), "d0", H5L_SAME_LOC, "nested/d_hard3",
                           lcpl.get(), H5P_DEFAULT),
            0);

  // 2. H5Lcreate_soft & H5Lget_val / H5Lget_val_by_idx.
  ASSERT_GE(H5Lcreate_soft("/src/d0", dst_grp.get(), "d_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst_grp.get(), "d_soft", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(dst_grp.get(), "does_not_exist", H5P_DEFAULT), 0);

  H5L_info2_t soft_info{};
  ASSERT_GE(H5Lget_info2(dst_grp.get(), "d_soft", &soft_info, H5P_DEFAULT), 0);
  EXPECT_EQ(soft_info.type, H5L_TYPE_SOFT);
  EXPECT_EQ(soft_info.u.val_size, std::strlen("/src/d0") + 1);

  std::string soft_target(soft_info.u.val_size, '\0');
  ASSERT_GE(H5Lget_val(dst_grp.get(), "d_soft", soft_target.data(),
                       soft_target.size(), H5P_DEFAULT),
            0);
  EXPECT_STREQ(soft_target.c_str(), "/src/d0");

  std::string soft_by_idx(soft_info.u.val_size, '\0');
  ASSERT_GE(
      H5Lget_val_by_idx(dst_grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC, 2,
                        soft_by_idx.data(), soft_by_idx.size(), H5P_DEFAULT),
      0);
  EXPECT_STREQ(soft_by_idx.c_str(), "/src/d0");

  // 3. H5Lget_info_by_idx2 & H5Lget_name_by_idx (including size==0 with
  // non-NULL buf).
  H5L_info2_t idx_info{};
  ASSERT_GE(H5Lget_info_by_idx2(file.get(), "/dst", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, 0, &idx_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(idx_info.type, H5L_TYPE_HARD);

  const ssize_t len_null =
      H5Lget_name_by_idx(file.get(), "/dst", H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                         nullptr, 0, H5P_DEFAULT);
  EXPECT_EQ(len_null, 7);  // "d_hard1"

  std::array<char, 4> untouched = {'X', 'Y', 'Z', 'W'};
  const ssize_t len_zero =
      H5Lget_name_by_idx(file.get(), "/dst", H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                         untouched.data(), 0, H5P_DEFAULT);
  EXPECT_EQ(len_zero, 7);
  EXPECT_EQ(untouched, (std::array<char, 4>{'X', 'Y', 'Z', 'W'}));

  std::array<char, 4> truncated{};
  EXPECT_EQ(
      H5Lget_name_by_idx(file.get(), "/dst", H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                         truncated.data(), truncated.size(), H5P_DEFAULT),
      7);
  EXPECT_STREQ(truncated.data(), "d_h");

  std::string full_name(static_cast<size_t>(len_null) + 1, '\0');
  EXPECT_EQ(
      H5Lget_name_by_idx(file.get(), "/dst", H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                         full_name.data(), full_name.size(), H5P_DEFAULT),
      7);
  EXPECT_STREQ(full_name.c_str(), "d_hard1");

  // 4. H5Lcopy and H5Lmove (both directions of H5L_SAME_LOC + distinct locs).
  ASSERT_GE(H5Lcopy(dst_grp.get(), "d_soft", file.get(), "/copied_soft",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcopy(H5L_SAME_LOC, "/copied_soft", file.get(), "/copied_same1",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcopy(file.get(), "/copied_soft", H5L_SAME_LOC, "/copied_same2",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ASSERT_GE(H5Lmove(file.get(), "/copied_same1", dst_grp.get(), "moved_soft",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lmove(H5L_SAME_LOC, "moved_soft", dst_grp.get(), "moved_same1",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lmove(dst_grp.get(), "moved_same1", H5L_SAME_LOC, "moved_same2",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(dst_grp.get(), "moved_same2", H5P_DEFAULT), 0);

  // 5. H5Ldelete and H5Ldelete_by_idx.
  ASSERT_GE(H5Ldelete(file.get(), "/copied_same2", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(file.get(), "/copied_same2", H5P_DEFAULT), 0);

  ASSERT_GE(H5Ldelete_by_idx(dst_grp.get(), ".", H5_INDEX_NAME, H5_ITER_DEC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(dst_grp.get(), "moved_same2", H5P_DEFAULT), 0);
}

TEST(H5LTest, Iterate2AndVisit2SyncAndByName) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5l_iter.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup g(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(g.valid());
  ScopedGroup sub(
      H5Gcreate2(g.get(), "c_sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  ASSERT_GE(
      H5Lcreate_soft("c_sub", g.get(), "a_soft", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/g/c_sub", sub.get(), "inner_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(sub.get(), ".", g.get(), "b_hard", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // H5Literate2 on group & file with stop/resume.
  Iter2State st1;
  st1.stop_after = 2;
  st1.stop_code = 9;
  hsize_t idx = 0;
  EXPECT_EQ(H5Literate2(g.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, &idx,
                        Iter2Callback, &st1),
            9);
  EXPECT_EQ(idx, 2u);
  EXPECT_THAT(st1.names, ElementsAre("c_sub", "a_soft"));

  Iter2State st2;
  ASSERT_EQ(H5Literate2(g.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, &idx,
                        Iter2Callback, &st2),
            0);
  EXPECT_THAT(st2.names, ElementsAre("b_hard"));

  // H5Literate_by_name2.
  Iter2State by_name;
  ASSERT_EQ(H5Literate_by_name2(file.get(), "/g", H5_INDEX_NAME, H5_ITER_DEC,
                                nullptr, Iter2Callback, &by_name, H5P_DEFAULT),
            0);
  EXPECT_THAT(by_name.names, ElementsAre("c_sub", "b_hard", "a_soft"));

  // H5Lvisit2 and H5Lvisit_by_name2.
  Iter2State visit_root;
  ASSERT_EQ(H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, Iter2Callback,
                      &visit_root),
            0);
  EXPECT_GE(visit_root.names.size(), 5u);

  Iter2State visit_by_name;
  ASSERT_EQ(H5Lvisit_by_name2(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &visit_by_name, H5P_DEFAULT),
            0);
  EXPECT_GE(visit_by_name.names.size(), 4u);

  // Negative callback propagation.
  Iter2State fail_st;
  fail_st.fail = true;
  ScopedErrorSilence silence;
  EXPECT_LT(H5Literate2(g.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        Iter2Callback, &fail_st),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, Iter2Callback, &fail_st, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lvisit2(g.get(), H5_INDEX_NAME, H5_ITER_INC, Iter2Callback, &fail_st),
      0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &fail_st, H5P_DEFAULT),
            0);
}

TEST(H5LTest, UserDefinedLinkRegistrationAndElinkValUnpack) {
  UdTracker ud_tracker;
  g_ud_tracker = &ud_tracker;
  absl::Cleanup clear_ud = [] { g_ud_tracker = nullptr; };

  EXPECT_EQ(H5Lis_registered(kCustomUdId1), 0);
  EXPECT_EQ(H5Lis_registered(kCustomUdId0), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);

  // 1. Register v1 UD link class.
  H5L_class_t cls1{};
  cls1.version = H5L_LINK_CLASS_T_VERS;
  cls1.id = kCustomUdId1;
  cls1.comment = "custom_v1";
  cls1.create_func = UdCreateCb;
  cls1.move_func = UdMoveCb;
  cls1.copy_func = UdCopyCb;
  cls1.trav_func = UdTraverseCb;
  cls1.del_func = UdDeleteCb;
  cls1.query_func = UdQueryCb;
  ASSERT_GE(H5Lregister(&cls1), 0);
  absl::Cleanup unreg1 = [] {
    if (H5Lis_registered(kCustomUdId1) > 0) {
      (void)H5Lunregister(kCustomUdId1);
    }
  };
  EXPECT_GT(H5Lis_registered(kCustomUdId1), 0);

  // Re-registering the same ID overrides the existing definition cleanly.
  cls1.comment = "custom_v1_override";
  ASSERT_GE(H5Lregister(&cls1), 0);

  // 2. Register v0 deprecated H5L_class_0_t struct (supported because
  // H5_NO_DEPRECATED_SYMBOLS is not defined in google3).
  H5L_class_0_t cls0{};
  cls0.version = H5L_LINK_CLASS_T_VERS_0;
  cls0.id = kCustomUdId0;
  cls0.comment = "custom_v0";
  cls0.create_func = UdCreateCb;
  cls0.move_func = UdMoveCb;
  cls0.copy_func = UdCopyCb;
  cls0.trav_func = UdTraverse0Cb;
  cls0.del_func = UdDeleteCb;
  cls0.query_func = UdQueryCb;
  ASSERT_GE(H5Lregister(reinterpret_cast<const H5L_class_t*>(&cls0)), 0);
  absl::Cleanup unreg0 = [] {
    if (H5Lis_registered(kCustomUdId0) > 0) {
      (void)H5Lunregister(kCustomUdId0);
    }
  };
  EXPECT_GT(H5Lis_registered(kCustomUdId0), 0);

  ScopedFile file = CreateInMemoryFile("h5l_ud.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup target(
      H5Gcreate2(file.get(), "/target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target.valid());
  target.reset();

  const char kTargetName[] = "/target";
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud1", kCustomUdId1, kTargetName,
                         sizeof(kTargetName), H5P_DEFAULT, H5P_DEFAULT),
            0);
  // Zero-size UD link with NULL udata is permitted.
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud0", kCustomUdId0, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(ud_tracker.creates, 2);

  {
    ScopedObject opened(H5Oopen(file.get(), "/ud1", H5P_DEFAULT));
    EXPECT_TRUE(opened.valid());
  }
  EXPECT_EQ(ud_tracker.traversals, 1);

  std::array<char, 16> ud_val{};
  ASSERT_GE(
      H5Lget_val(file.get(), "/ud1", ud_val.data(), ud_val.size(), H5P_DEFAULT),
      0);
  EXPECT_STREQ(ud_val.data(), "/target");

  ASSERT_GE(H5Lcopy(file.get(), "/ud1", H5L_SAME_LOC, "/ud1_copy", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lmove(file.get(), "/ud1_copy", H5L_SAME_LOC, "/ud1_moved",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(file.get(), "/ud1_moved", H5P_DEFAULT), 0);
  EXPECT_EQ(ud_tracker.copies, 1);
  EXPECT_EQ(ud_tracker.moves, 1);
  EXPECT_EQ(ud_tracker.deletes, 2);

  // Explicit unregister succeeds, and second unregister fails.
  ASSERT_GE(H5Lunregister(kCustomUdId1), 0);
  EXPECT_EQ(H5Lis_registered(kCustomUdId1), 0);

  // 3. H5Lcreate_external + H5Lunpack_elink_val (with & without NULL outputs).
  ASSERT_GE(H5Lcreate_external("f.h5", "///a//b/", file.get(), "/ext_link",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
  H5L_info2_t ext_info{};
  ASSERT_GE(H5Lget_info2(file.get(), "/ext_link", &ext_info, H5P_DEFAULT), 0);
  std::vector<uint8_t> ext_buf(ext_info.u.val_size);
  ASSERT_GE(H5Lget_val(file.get(), "/ext_link", ext_buf.data(), ext_buf.size(),
                       H5P_DEFAULT),
            0);
  unsigned flags = 7;
  const char* fname = nullptr;
  const char* obj_path = nullptr;
  ASSERT_GE(H5Lunpack_elink_val(ext_buf.data(), ext_buf.size(), &flags, &fname,
                                &obj_path),
            0);
  EXPECT_EQ(flags, 0u);
  EXPECT_STREQ(fname, "f.h5");
  EXPECT_STREQ(obj_path, "/a/b");
  EXPECT_GE(H5Lunpack_elink_val(ext_buf.data(), ext_buf.size(), nullptr,
                                nullptr, nullptr),
            0);

  // Argument validation errors for register/unregister/is_registered/unpack.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lregister(nullptr), 0);

  H5L_class_t bad_ver = cls1;
  bad_ver.version = H5L_LINK_CLASS_T_VERS + 1;
  EXPECT_LT(H5Lregister(&bad_ver), 0);

  H5L_class_t bad_id_low = cls1;
  bad_id_low.id = H5L_TYPE_SOFT;
  EXPECT_LT(H5Lregister(&bad_id_low), 0);

  H5L_class_t bad_id_high = cls1;
  bad_id_high.id = static_cast<H5L_type_t>(H5L_TYPE_MAX + 1);
  EXPECT_LT(H5Lregister(&bad_id_high), 0);

  H5L_class_t bad_trav = cls1;
  bad_trav.trav_func = nullptr;
  EXPECT_LT(H5Lregister(&bad_trav), 0);

  EXPECT_LT(H5Lunregister(H5L_TYPE_ERROR), 0);
  EXPECT_LT(H5Lunregister(static_cast<H5L_type_t>(H5L_TYPE_MAX + 1)), 0);
  EXPECT_LT(H5Lunregister(kCustomUdId1), 0);

  EXPECT_LT(H5Lis_registered(H5L_TYPE_ERROR), 0);
  EXPECT_LT(H5Lis_registered(static_cast<H5L_type_t>(H5L_TYPE_MAX + 1)), 0);

  // H5Lunpack_elink_val error checks.
  EXPECT_LT(H5Lunpack_elink_val(nullptr, 8, &flags, &fname, &obj_path), 0);

  constexpr std::array<uint8_t, 5> kBadPackVer = {0x10, 'f', '\0', 'o', '\0'};
  EXPECT_LT(H5Lunpack_elink_val(kBadPackVer.data(), kBadPackVer.size(), &flags,
                                &fname, &obj_path),
            0);

  constexpr std::array<uint8_t, 5> kBadPackFlags = {0x01, 'f', '\0', 'o', '\0'};
  EXPECT_LT(H5Lunpack_elink_val(kBadPackFlags.data(), kBadPackFlags.size(),
                                &flags, &fname, &obj_path),
            0);

  constexpr std::array<uint8_t, 2> kTooShort = {0x00, '\0'};
  EXPECT_LT(H5Lunpack_elink_val(kTooShort.data(), kTooShort.size(), &flags,
                                &fname, &obj_path),
            0);

  constexpr std::array<uint8_t, 5> kNoTrailingNul = {0x00, 'f', '\0', 'o', 'x'};
  EXPECT_LT(H5Lunpack_elink_val(kNoTrailingNul.data(), kNoTrailingNul.size(),
                                &flags, &fname, &obj_path),
            0);

  constexpr std::array<uint8_t, 5> kMissingObjPath = {0x00, 'a', 'b', 'c',
                                                      '\0'};
  EXPECT_LT(H5Lunpack_elink_val(kMissingObjPath.data(), kMissingObjPath.size(),
                                &flags, &fname, &obj_path),
            0);
}

TEST(H5LTest, NativeVolAsyncAndArgumentValidationErrors) {
  ScopedFile file = CreateInMemoryFile("h5l_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // Native VOL *_async APIs with H5ES_NONE and real EventSet (tokens stay
  // NULL).
  ASSERT_GE(H5Lcreate_hard_async(grp.get(), ".", file.get(), "/hard_async1",
                                 H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(
      H5Lcreate_hard_async(file.get(), "/hard_async1", H5L_SAME_LOC,
                           "/hard_async2", H5P_DEFAULT, H5P_DEFAULT, es.get()),
      0);
  ASSERT_GE(H5Lcreate_soft_async("/grp", file.get(), "/soft_async1",
                                 H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(H5Lcreate_soft_async("/grp", file.get(), "/soft_async2",
                                 H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);

  bool exists = false;
  ASSERT_GE(H5Lexists_async(file.get(), "/soft_async1", &exists, H5P_DEFAULT,
                            H5ES_NONE),
            0);
  EXPECT_TRUE(exists);
  exists = false;
  ASSERT_GE(H5Lexists_async(file.get(), "/soft_async2", &exists, H5P_DEFAULT,
                            es.get()),
            0);
  EXPECT_TRUE(exists);

  Iter2State async_it;
  ASSERT_EQ(H5Literate_async(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                             Iter2Callback, &async_it, es.get()),
            0);
  EXPECT_GE(async_it.names.size(), 4u);

  ASSERT_GE(H5Ldelete_async(file.get(), "/soft_async1", H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(H5Ldelete_async(file.get(), "/soft_async2", H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Ldelete_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                   1, H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(H5Ldelete_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                   1, H5P_DEFAULT, es.get()),
            0);

  // Comprehensive argument validation errors.
  ScopedErrorSilence silence;

  // H5Lmove
  EXPECT_LT(
      H5Lmove(H5L_SAME_LOC, "a", H5L_SAME_LOC, "b", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Lmove(file.get(), nullptr, file.get(), "b", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lmove(file.get(), "", file.get(), "b", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lmove(file.get(), "a", file.get(), nullptr, H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lmove(file.get(), "a", file.get(), "", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lmove(space.get(), "a", file.get(), "b", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Lmove(file.get(), "a", space.get(), "b", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Lmove(file.get(), "/grp", file.get(), "/b", fapl.get(), H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Lmove(file.get(), "/grp", file.get(), "/b", H5P_DEFAULT, fapl.get()),
      0);
  EXPECT_LT(H5Lmove(file.get(), "/missing", file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  // H5Lcopy
  EXPECT_LT(
      H5Lcopy(H5L_SAME_LOC, "a", H5L_SAME_LOC, "b", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Lcopy(file.get(), nullptr, file.get(), "b", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcopy(file.get(), "", file.get(), "b", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcopy(file.get(), "a", file.get(), nullptr, H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcopy(file.get(), "a", file.get(), "", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcopy(file.get(), "/grp", file.get(), "/b", fapl.get(), H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Lcopy(space.get(), "a", file.get(), "b", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Lcopy(file.get(), "a", space.get(), "b", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Lcopy(file.get(), "/grp", file.get(), "/b", H5P_DEFAULT, fapl.get()),
      0);
  EXPECT_LT(H5Lcopy(file.get(), "/missing", file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  // H5Lcreate_soft & async
  EXPECT_LT(H5Lcreate_soft("t", H5L_SAME_LOC, "l", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_soft(nullptr, file.get(), "l", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_soft("", file.get(), "l", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lcreate_soft("t", file.get(), "l", fapl.get(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Lcreate_soft("t", file.get(), "l", H5P_DEFAULT, fapl.get()), 0);
  EXPECT_LT(H5Lcreate_soft("t", file.get(), nullptr, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_soft("t", file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_soft_async("t", file.get(), "/grp", H5P_DEFAULT,
                                 H5P_DEFAULT, es.get()),
            0);

  // H5Lcreate_hard & async
  EXPECT_LT(H5Lcreate_hard(H5L_SAME_LOC, "a", H5L_SAME_LOC, "b", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(file.get(), nullptr, file.get(), "b", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_hard(file.get(), "", file.get(), "b", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_hard(file.get(), "a", file.get(), nullptr, H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_hard(file.get(), "a", file.get(), "", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_hard(file.get(), "/grp", file.get(), "b", fapl.get(),
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(file.get(), "/grp", file.get(), "b", H5P_DEFAULT,
                           fapl.get()),
            0);
  EXPECT_LT(H5Lcreate_hard(space.get(), "a", file.get(), "b", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(file.get(), "/grp", space.get(), "b", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(file.get(), "/missing", file.get(), "/b",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard_async(file.get(), "/missing", file.get(), "/b",
                                 H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);

  // H5Lcreate_external
  EXPECT_LT(H5Lcreate_external(nullptr, "o", file.get(), "l", H5P_DEFAULT,
                               H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_external("", "o", file.get(), "l", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_external("f", nullptr, file.get(), "l", H5P_DEFAULT,
                               H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_external("f", "", file.get(), "l", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_external("f", "o", file.get(), nullptr, H5P_DEFAULT,
                               H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_external("f", "o", file.get(), "", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Lcreate_external("f", "o", file.get(), "l", H5P_DEFAULT, fapl.get()),
      0);
  EXPECT_LT(
      H5Lcreate_external("f", "o", space.get(), "l", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_external("f", "o", file.get(), "/grp", H5P_DEFAULT,
                               H5P_DEFAULT),
            0);

  // H5Lcreate_ud
  EXPECT_LT(H5Lcreate_ud(file.get(), nullptr, H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "", H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "u", H5L_TYPE_SOFT, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "u", static_cast<H5L_type_t>(256), nullptr,
                         0, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "u", H5L_TYPE_EXTERNAL, nullptr, 4,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "u", H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, fapl.get()),
            0);
  EXPECT_LT(H5Lcreate_ud(space.get(), "u", H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(file.get(), "/grp", H5L_TYPE_EXTERNAL, "x", 1,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);

  // H5Ldelete & H5Ldelete_by_idx & async
  EXPECT_LT(H5Ldelete(space.get(), "/grp", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete(file.get(), "/missing", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete_async(file.get(), "/missing", H5P_DEFAULT, es.get()), 0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                             H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                   99, H5P_DEFAULT, es.get()),
            0);

  // H5Lget_val & H5Lget_val_by_idx
  std::array<char, 8> buf{};
  EXPECT_LT(
      H5Lget_val(file.get(), nullptr, buf.data(), buf.size(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_val(file.get(), "", buf.data(), buf.size(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_val(file.get(), "/grp", buf.data(), buf.size(), fapl.get()),
            0);
  EXPECT_LT(
      H5Lget_val(space.get(), "/grp", buf.data(), buf.size(), H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_val(file.get(), "/grp", buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              0, buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                              buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC, 0,
                              buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                              0, buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                              buf.data(), buf.size(), fapl.get()),
            0);
  EXPECT_LT(H5Lget_val_by_idx(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                              buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                              buf.data(), buf.size(), H5P_DEFAULT),
            0);

  // H5Lexists & H5Lexists_async
  EXPECT_LT(H5Lexists(file.get(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lexists(file.get(), "", H5P_DEFAULT), 0);
  EXPECT_LT(H5Lexists(file.get(), "/missing_parent/child", H5P_DEFAULT), 0);
  EXPECT_LT(H5Lexists_async(file.get(), "/grp", nullptr, H5P_DEFAULT, es.get()),
            0);
  EXPECT_LT(H5Lexists_async(file.get(), "/missing_parent/child", &exists,
                            H5P_DEFAULT, es.get()),
            0);

  // H5Lget_info2, H5Lget_info_by_idx2, H5Lget_name_by_idx
  H5L_info2_t info{};
  EXPECT_LT(H5Lget_info2(file.get(), nullptr, &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info2(file.get(), "", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info2(file.get(), "/grp", &info, fapl.get()), 0);
  EXPECT_LT(H5Lget_info2(space.get(), "/grp", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info2(file.get(), "/missing", &info, H5P_DEFAULT), 0);

  EXPECT_LT(H5Lget_info_by_idx2(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, fapl.get()),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                                &info, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Lget_name_by_idx(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                               0, buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                               buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                               0, buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                               0, buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               buf.data(), buf.size(), fapl.get()),
            0);
  EXPECT_LT(H5Lget_name_by_idx(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               buf.data(), buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               buf.data(), buf.size(), H5P_DEFAULT),
            0);

  // H5Literate2, H5Literate_async, H5Literate_by_name2, H5Lvisit2,
  // H5Lvisit_by_name2 arg errors.
  Iter2State st;
  EXPECT_LT(H5Literate2(space.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        Iter2Callback, &st),
            0);
  EXPECT_LT(H5Literate2(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, nullptr,
                        Iter2Callback, &st),
            0);
  EXPECT_LT(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, nullptr,
                        Iter2Callback, &st),
            0);
  EXPECT_LT(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        nullptr, &st),
            0);
  EXPECT_LT(H5Literate_async(space.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                             Iter2Callback, &st, es.get()),
            0);

  EXPECT_LT(H5Literate_by_name2(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                nullptr, Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                nullptr, Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, nullptr, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, Iter2Callback, &st, fapl.get()),
            0);
  EXPECT_LT(H5Literate_by_name2(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, Iter2Callback, &st, H5P_DEFAULT),
            0);

  EXPECT_LT(
      H5Lvisit2(space.get(), H5_INDEX_NAME, H5_ITER_INC, Iter2Callback, &st),
      0);
  EXPECT_LT(
      H5Lvisit2(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, Iter2Callback, &st),
      0);
  EXPECT_LT(
      H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, Iter2Callback, &st),
      0);
  EXPECT_LT(H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr, &st), 0);

  EXPECT_LT(H5Lvisit_by_name2(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                              Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                              Iter2Callback, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              nullptr, &st, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &st, fapl.get()),
            0);
  EXPECT_LT(H5Lvisit_by_name2(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &st, H5P_DEFAULT),
            0);
}

TEST(H5LTest, MockAsyncVolTokensInsertFailureAndCrossVolRejection) {
  MockVolTracker tracker;
  ScopedMockLinkVol vol(&tracker);
  ASSERT_NE(vol.id(), H5I_INVALID_HID);

  ScopedPlist mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(mock_fapl.valid());
  ASSERT_GE(H5Pset_vol(mock_fapl.get(), vol.id(), nullptr), 0);

  ScopedFile mock_file(
      H5Fcreate("mock_l.h5", H5F_ACC_TRUNC, H5P_DEFAULT, mock_fapl.get()));
  ASSERT_TRUE(mock_file.valid());
  ScopedGroup mock_grp(H5Gcreate2(mock_file.get(), "/mg", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mock_grp.valid());

  ScopedFile native_file = CreateInMemoryFile("native_l.h5");
  ASSERT_TRUE(native_file.valid());

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. Happy async token creation & event set insertion across all 6 *_async.
  ASSERT_GE(H5Lcreate_soft_async("/t", mock_file.get(), "/ms", H5P_DEFAULT,
                                 H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Lcreate_hard_async(mock_file.get(), "/mg", mock_file.get(),
                                 "/mh1", H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Lcreate_hard_async(H5L_SAME_LOC, "/mg", mock_grp.get(), "mh2",
                                 H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);
  ASSERT_GE(H5Lcreate_hard_async(mock_grp.get(), ".", H5L_SAME_LOC, "mh3",
                                 H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);
  bool exists = false;
  ASSERT_GE(
      H5Lexists_async(mock_file.get(), "/ms", &exists, H5P_DEFAULT, es.get()),
      0);
  EXPECT_TRUE(exists);

  Iter2State iter_state;
  ASSERT_EQ(H5Literate_async(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                             nullptr, Iter2Callback, &iter_state, es.get()),
            0);
  ASSERT_GE(H5Ldelete_async(mock_file.get(), "/ms", H5P_DEFAULT, es.get()), 0);
  ASSERT_GE(H5Ldelete_by_idx_async(mock_file.get(), ".", H5_INDEX_NAME,
                                   H5_ITER_INC, 0, H5P_DEFAULT, es.get()),
            0);

  size_t count = 0;
  ASSERT_GE(H5ESget_count(es.get(), &count), 0);
  EXPECT_EQ(count, 8u);

  size_t in_progress = 0;
  bool op_failed = false;
  ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &op_failed), 0);
  EXPECT_EQ(in_progress, 0u);
  EXPECT_FALSE(op_failed);
  EXPECT_TRUE(tracker.live_requests.empty());

  // 2. Sync calls through mock VOL cover H5VL_link_* paths cleanly.
  EXPECT_GE(H5Lmove(mock_file.get(), "/mh1", mock_file.get(), "/mh1_mv",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lcopy(mock_file.get(), "/mh1_mv", mock_file.get(), "/mh1_cp",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lcreate_external("x.h5", "/o", mock_file.get(), "/me",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lcreate_ud(mock_file.get(), "/mu", H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  H5L_info2_t linfo{};
  EXPECT_GE(H5Lget_info2(mock_file.get(), "/ms", &linfo, H5P_DEFAULT), 0);
  EXPECT_GE(H5Lget_info_by_idx2(mock_file.get(), ".", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &linfo, H5P_DEFAULT),
            0);
  std::array<char, 16> val_buf{};
  EXPECT_GE(H5Lget_val(mock_file.get(), "/ms", val_buf.data(), val_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lget_val_by_idx(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              0, val_buf.data(), val_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_GE(H5Lget_name_by_idx(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, val_buf.data(), val_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_GE(
      H5Literate_by_name2(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                          nullptr, Iter2Callback, &iter_state, H5P_DEFAULT),
      0);
  EXPECT_GE(H5Lvisit2(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      Iter2Callback, &iter_state),
            0);
  EXPECT_GE(H5Lvisit_by_name2(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &iter_state, H5P_DEFAULT),
            0);

  // 3. Mismatched VOL connectors rejected by H5Lmove, H5Lcopy, H5Lcreate_hard.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lmove(mock_file.get(), "/a", native_file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcopy(mock_file.get(), "/a", native_file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(mock_file.get(), "/a", native_file.get(), "/b",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 4. H5ES_insert failures in all 6 *_async routines (non-ES hid_t != NONE).
  const hid_t bad_es = mock_fapl.get();
  EXPECT_LT(H5Lcreate_soft_async("/t", mock_file.get(), "/bad_es_s",
                                 H5P_DEFAULT, H5P_DEFAULT, bad_es),
            0);
  EXPECT_LT(H5Lcreate_hard_async(mock_file.get(), "/mg", mock_file.get(),
                                 "/bad_es_h", H5P_DEFAULT, H5P_DEFAULT, bad_es),
            0);
  EXPECT_LT(H5Ldelete_async(mock_file.get(), "/ms", H5P_DEFAULT, bad_es), 0);
  EXPECT_LT(H5Ldelete_by_idx_async(mock_file.get(), ".", H5_INDEX_NAME,
                                   H5_ITER_INC, 0, H5P_DEFAULT, bad_es),
            0);
  EXPECT_LT(
      H5Lexists_async(mock_file.get(), "/ms", &exists, H5P_DEFAULT, bad_es), 0);
  EXPECT_LT(H5Literate_async(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                             nullptr, Iter2Callback, &iter_state, bad_es),
            0);

  // 5. VOL failure propagation across link callbacks.
  tracker.fail_link_create = true;
  EXPECT_LT(H5Lcreate_external("x.h5", "/o", mock_file.get(), "/fe",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_ud(mock_file.get(), "/fu", H5L_TYPE_EXTERNAL, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Lcreate_soft("/t", mock_file.get(), "/fs", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lcreate_hard(mock_file.get(), "/mg", mock_file.get(), "/fh",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  tracker.fail_link_create = false;

  tracker.fail_link_copy = true;
  EXPECT_LT(H5Lcopy(mock_file.get(), "/a", mock_file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  tracker.fail_link_copy = false;

  tracker.fail_link_move = true;
  EXPECT_LT(H5Lmove(mock_file.get(), "/a", mock_file.get(), "/b", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  tracker.fail_link_move = false;

  tracker.fail_link_get = true;
  EXPECT_LT(H5Lget_val(mock_file.get(), "/ms", val_buf.data(), val_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              0, val_buf.data(), val_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info2(mock_file.get(), "/ms", &linfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info_by_idx2(mock_file.get(), ".", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, val_buf.data(), val_buf.size(), H5P_DEFAULT),
            0);
  tracker.fail_link_get = false;

  tracker.fail_link_specific = true;
  EXPECT_LT(H5Ldelete(mock_file.get(), "/ms", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete_by_idx(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lexists(mock_file.get(), "/ms", H5P_DEFAULT), 0);
  EXPECT_LT(H5Literate2(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        Iter2Callback, &iter_state),
            0);
  EXPECT_LT(
      H5Literate_by_name2(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                          nullptr, Iter2Callback, &iter_state, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lvisit2(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      Iter2Callback, &iter_state),
            0);
  EXPECT_LT(H5Lvisit_by_name2(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              Iter2Callback, &iter_state, H5P_DEFAULT),
            0);
  tracker.fail_link_specific = false;
}

}  // namespace
}  // namespace hdf5
