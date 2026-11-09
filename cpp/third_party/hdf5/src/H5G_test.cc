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
// Tests public group lifecycle, metadata inspection, flush/refresh, and
// asynchronous H5G*_async APIs (third_party/hdf5/src/H5G.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5G.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

constexpr H5VL_class_value_t kMockVolValue = 311;
constexpr char kMockVolName[] = "h5g_async_mock_vol";

struct MockRequest {
  H5VL_request_status_t wait_status = H5VL_REQUEST_STATUS_SUCCEED;
};

struct MockVolTracker {
  bool fail_group_create = false;
  bool fail_group_open = false;
  bool fail_group_get = false;
  bool fail_group_specific = false;
  bool fail_group_close = false;
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
  std::string name;
};

MockVolTracker* g_mock_tracker = nullptr;

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
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
                      const char* name, hid_t /*lcpl_id*/, hid_t /*gcpl_id*/,
                      hid_t /*gapl_id*/, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_create) {
    return nullptr;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return new MockGroup{g_mock_tracker, name != nullptr ? name : "<anon>"};
}

void* MockGroupOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                    const char* name, hid_t /*gapl_id*/, hid_t /*dxpl_id*/,
                    void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_open) {
    return nullptr;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return new MockGroup{g_mock_tracker, name != nullptr ? name : ""};
}

herr_t MockGroupGet(void* /*obj*/, H5VL_group_get_args_t* args,
                    hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_get) {
    return -1;
  }
  if (args->op_type == H5VL_GROUP_GET_INFO && args->args.get_info.ginfo) {
    H5G_info_t* info = args->args.get_info.ginfo;
    info->storage_type = H5G_STORAGE_TYPE_COMPACT;
    info->nlinks = 3;
    info->max_corder = 3;
    info->mounted = false;
  } else if (args->op_type == H5VL_GROUP_GET_GCPL) {
    args->args.get_gcpl.gcpl_id = H5Pcreate(H5P_GROUP_CREATE);
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockGroupSpecific(void* /*obj*/, H5VL_group_specific_args_t* /*args*/,
                         hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_specific) {
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  return 0;
}

herr_t MockGroupClose(void* grp, hid_t /*dxpl_id*/, void** req) {
  if (g_mock_tracker != nullptr && g_mock_tracker->fail_group_close) {
    // Only fail on demand when testing error path; caller frees later.
    return -1;
  }
  if (req != nullptr && g_mock_tracker != nullptr) {
    *req = g_mock_tracker->MakeRequest();
  }
  delete static_cast<MockGroup*>(grp);
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

class ScopedMockGroupVol {
 public:
  explicit ScopedMockGroupVol(MockVolTracker* tracker) {
    g_mock_tracker = tracker;
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = kMockVolValue;
    cls.name = kMockVolName;
    cls.cap_flags = H5VL_CAP_FLAG_ASYNC;
    cls.file_cls.create = MockFileCreate;
    cls.file_cls.close = MockFileClose;
    cls.group_cls.create = MockGroupCreate;
    cls.group_cls.open = MockGroupOpen;
    cls.group_cls.get = MockGroupGet;
    cls.group_cls.specific = MockGroupSpecific;
    cls.group_cls.close = MockGroupClose;
    cls.introspect_cls.opt_query = MockIntrospectOptQuery;
    cls.request_cls.wait = MockRequestWait;
    cls.request_cls.free = MockRequestFree;
    vol_id_ = H5VLregister_connector(&cls, H5P_DEFAULT);
  }

  ~ScopedMockGroupVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_mock_tracker = nullptr;
  }

  ScopedMockGroupVol(const ScopedMockGroupVol&) = delete;
  ScopedMockGroupVol& operator=(const ScopedMockGroupVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

TEST(H5GTest, CreateOpenAnonInfoAndGetCreatePlist) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(H5Fcreate("h5g_basic.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  // 1. Create group with custom LCPL (intermediate groups) and GCPL.
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 4, 2), 0);

  ScopedGroup nested(H5Gcreate2(file.get(), "/parent/child", lcpl.get(),
                                gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(nested.valid());

  ScopedGroup child_sub(H5Gcreate2(nested.get(), "sub_b", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_sub.valid());
  child_sub.reset();

  ScopedGroup child_first(H5Gcreate2(nested.get(), "sub_a", H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_first.valid());
  child_first.reset();

  // 2. Verify H5Gget_create_plist returns copied GCPL settings.
  ScopedPlist fetched_gcpl(H5Gget_create_plist(nested.get()));
  ASSERT_TRUE(fetched_gcpl.valid());
  unsigned corder_flags = 0;
  ASSERT_GE(H5Pget_link_creation_order(fetched_gcpl.get(), &corder_flags), 0);
  EXPECT_EQ(corder_flags, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ASSERT_GE(
      H5Pget_link_phase_change(fetched_gcpl.get(), &max_compact, &min_dense),
      0);
  EXPECT_EQ(max_compact, 4u);
  EXPECT_EQ(min_dense, 2u);

  // 3. Anonymous group created with explicit GCPL & GAPL, then linked into tree.
  ScopedPlist gapl(H5Pcreate(H5P_GROUP_ACCESS));
  ASSERT_TRUE(gapl.valid());
  ScopedGroup anon(H5Gcreate_anon(file.get(), gcpl.get(), gapl.get()));
  ASSERT_TRUE(anon.valid());
  ASSERT_GE(H5Olink(anon.get(), nested.get(), "linked_anon", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  anon.reset();

  // Unlinked anonymous group is safely dropped on close.
  ScopedGroup anon_unlinked(
      H5Gcreate_anon(file.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_unlinked.valid());
  anon_unlinked.reset();

  // 4. Open existing group and query H5Gget_info / by_name / by_idx.
  ScopedGroup reopened(H5Gopen2(file.get(), "/parent/child", gapl.get()));
  ASSERT_TRUE(reopened.valid());

  H5G_info_t info{};
  ASSERT_GE(H5Gget_info(file.get(), &info), 0);
  EXPECT_EQ(info.nlinks, 1u);

  ASSERT_GE(H5Gget_info(reopened.get(), &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(info.nlinks, 3u);
  EXPECT_EQ(info.max_corder, 3);
  EXPECT_FALSE(info.mounted);

  H5G_info_t by_name_info{};
  ASSERT_GE(H5Gget_info_by_name(file.get(), "/parent/child", &by_name_info,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(by_name_info.nlinks, 3u);

  H5G_info_t by_idx_info{};
  ASSERT_GE(H5Gget_info_by_idx(nested.get(), ".", H5_INDEX_CRT_ORDER,
                               H5_ITER_INC, 0, &by_idx_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(by_idx_info.nlinks, 0u);

  // 5. Flush and refresh group metadata.
  EXPECT_GE(H5Gflush(reopened.get()), 0);
  EXPECT_GE(H5Grefresh(reopened.get()), 0);

  // 6. Explicit H5Gclose check.
  EXPECT_GE(H5Gclose(reopened.release()), 0);
}

TEST(H5GTest, SynchronousArgumentAndStateErrorsFailCleanly) {
  ScopedFile file = CreateInMemoryFile("h5g_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());

  ScopedErrorSilence silence;

  // H5Gcreate2 validation errors.
  EXPECT_EQ(H5Gcreate2(file.get(), nullptr, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(H5I_INVALID_HID, "/bad_loc", H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "/bad_gapl", H5P_DEFAULT, H5P_DEFAULT,
                       gcpl.get()),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "/bad_lcpl", fapl.get(), H5P_DEFAULT,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "/bad_gcpl", H5P_DEFAULT, fapl.get(),
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate2(file.get(), "/missing_parent/c", H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Gcreate_anon validation errors.
  EXPECT_EQ(H5Gcreate_anon(file.get(), fapl.get(), H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate_anon(file.get(), H5P_DEFAULT, gcpl.get()),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate_anon(H5I_INVALID_HID, H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate_anon(space.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Gcreate_anon on read-only file fails inside VOL group_create.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ssize_t img_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(img_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            img_size);
  grp.reset();
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("h5g_errors_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  EXPECT_EQ(H5Gcreate_anon(ro_file.get(), H5P_DEFAULT, H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Gopen2 validation errors.
  EXPECT_EQ(H5Gopen2(ro_file.get(), nullptr, H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen2(ro_file.get(), "", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen2(H5I_INVALID_HID, "/g", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen2(ro_file.get(), "/g", gcpl.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen2(ro_file.get(), "/does_not_exist", H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Gget_create_plist / H5Gget_info* / H5Gclose / H5Gflush / H5Grefresh errors.
  EXPECT_EQ(H5Gget_create_plist(ro_file.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Gget_create_plist(H5I_INVALID_HID), H5I_INVALID_HID);

  H5G_info_t info{};
  EXPECT_LT(H5Gget_info(space.get(), &info), 0);
  EXPECT_LT(H5Gget_info(ro_file.get(), nullptr), 0);
  EXPECT_LT(H5Gget_info(H5I_INVALID_HID, &info), 0);

  EXPECT_LT(H5Gget_info_by_name(ro_file.get(), "/g", nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Gget_info_by_name(H5I_INVALID_HID, "/g", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Gget_info_by_name(ro_file.get(), "/g", &info, gcpl.get()), 0);
  EXPECT_LT(H5Gget_info_by_name(ro_file.get(), "/nope", &info, H5P_DEFAULT), 0);

  EXPECT_LT(H5Gget_info_by_idx(ro_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Gget_info_by_idx(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Gget_info_by_idx(ro_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, &info, gcpl.get()),
            0);
  EXPECT_LT(H5Gget_info_by_idx(ro_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               99, &info, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Gclose(ro_file.get()), 0);
  EXPECT_LT(H5Gclose(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Gflush(ro_file.get()), 0);
  EXPECT_LT(H5Gflush(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Grefresh(ro_file.get()), 0);
  EXPECT_LT(H5Grefresh(H5I_INVALID_HID), 0);
}

TEST(H5GTest, NativeVolAsyncApisWithAndWithoutEventSet) {
  ScopedFile file = CreateInMemoryFile("h5g_native_async.h5");
  ASSERT_TRUE(file.valid());
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. With H5ES_NONE.
  ScopedGroup g1(H5Gcreate_async(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT, H5ES_NONE));
  ASSERT_TRUE(g1.valid());
  ASSERT_GE(H5Gclose_async(g1.release(), H5ES_NONE), 0);

  ScopedGroup g1_reopen(H5Gopen_async(file.get(), "/g1", H5P_DEFAULT,
                                      H5ES_NONE));
  ASSERT_TRUE(g1_reopen.valid());

  H5G_info_t info{};
  EXPECT_GE(H5Gget_info_async(file.get(), &info, H5ES_NONE), 0);
  EXPECT_GE(
      H5Gget_info_by_name_async(file.get(), "/g1", &info, H5P_DEFAULT,
                                H5ES_NONE),
      0);
  EXPECT_GE(H5Gget_info_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     0, &info, H5P_DEFAULT, H5ES_NONE),
            0);
  ASSERT_GE(H5Gclose_async(g1_reopen.release(), H5ES_NONE), 0);

  // 2. With valid event set on native VOL (tokens stay NULL).
  ScopedGroup g2(H5Gcreate_async(file.get(), "/g2", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT, es.get()));
  ASSERT_TRUE(g2.valid());
  EXPECT_GE(H5Gget_info_async(g2.get(), &info, es.get()), 0);
  EXPECT_GE(
      H5Gget_info_by_name_async(file.get(), "/g2", &info, H5P_DEFAULT, es.get()),
      0);
  EXPECT_GE(H5Gget_info_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     1, &info, H5P_DEFAULT, es.get()),
            0);
  EXPECT_GE(H5Gclose_async(g2.release(), es.get()), 0);

  ScopedGroup g2_reopen(
      H5Gopen_async(file.get(), "/g2", H5P_DEFAULT, es.get()));
  ASSERT_TRUE(g2_reopen.valid());
  EXPECT_GE(H5Gclose_async(g2_reopen.release(), es.get()), 0);

  // 3. Error propagation in async wrappers.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Gcreate_async(file.get(), nullptr, H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, es.get()),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen_async(file.get(), "/missing", H5P_DEFAULT, es.get()),
            H5I_INVALID_HID);
  EXPECT_LT(H5Gget_info_async(file.get(), nullptr, es.get()), 0);
  EXPECT_LT(
      H5Gget_info_by_name_async(file.get(), "/missing", &info, H5P_DEFAULT,
                                es.get()),
      0);
  EXPECT_LT(H5Gget_info_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     99, &info, H5P_DEFAULT, es.get()),
            0);
  EXPECT_LT(H5Gclose_async(file.get(), es.get()), 0);
}

TEST(H5GTest, MockAsyncVolInsertsTokensAndHandlesInsertFailures) {
  MockVolTracker tracker;
  ScopedMockGroupVol vol(&tracker);
  ASSERT_NE(vol.id(), H5I_INVALID_HID);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), vol.id(), nullptr), 0);

  ScopedFile file(H5Fcreate("mock_async.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. Happy path: every async operation produces a token inserted into es.
  ScopedGroup g_created(H5Gcreate_async(file.get(), "/async_grp", H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT, es.get()));
  ASSERT_TRUE(g_created.valid());

  ScopedGroup g_opened(
      H5Gopen_async(file.get(), "/async_grp", H5P_DEFAULT, es.get()));
  ASSERT_TRUE(g_opened.valid());

  H5G_info_t info{};
  EXPECT_GE(H5Gget_info_async(g_created.get(), &info, es.get()), 0);
  EXPECT_EQ(info.nlinks, 3u);

  info = {};
  EXPECT_GE(H5Gget_info_by_name_async(file.get(), "/async_grp", &info,
                                      H5P_DEFAULT, es.get()),
            0);
  EXPECT_EQ(info.nlinks, 3u);

  info = {};
  EXPECT_GE(H5Gget_info_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     0, &info, H5P_DEFAULT, es.get()),
            0);
  EXPECT_EQ(info.nlinks, 3u);

  EXPECT_GE(H5Gclose_async(g_opened.release(), es.get()), 0);
  EXPECT_GE(H5Gclose_async(g_created.release(), es.get()), 0);

  size_t count = 0;
  ASSERT_GE(H5ESget_count(es.get(), &count), 0);
  EXPECT_EQ(count, 7u);

  size_t in_progress = 0;
  bool op_failed = false;
  ASSERT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &in_progress, &op_failed), 0);
  EXPECT_EQ(in_progress, 0u);
  EXPECT_FALSE(op_failed);
  EXPECT_TRUE(tracker.live_requests.empty());

  // 2. Sync get_create_plist, flush, and refresh against mock VOL (both ok and
  //    VOL-failure branches).
  ScopedGroup g_sync(
      H5Gcreate2(file.get(), "/sync_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_sync.valid());
  ScopedPlist gcpl(H5Gget_create_plist(g_sync.get()));
  EXPECT_TRUE(gcpl.valid());
  EXPECT_GE(H5Gflush(g_sync.get()), 0);
  EXPECT_GE(H5Grefresh(g_sync.get()), 0);

  {
    ScopedErrorSilence silence;
    tracker.fail_group_get = true;
    EXPECT_EQ(H5Gget_create_plist(g_sync.get()), H5I_INVALID_HID);
    tracker.fail_group_get = false;

    tracker.fail_group_specific = true;
    EXPECT_LT(H5Gflush(g_sync.get()), 0);
    EXPECT_LT(H5Grefresh(g_sync.get()), 0);
    tracker.fail_group_specific = false;
  }

  // 3. Invalid es_id (fapl is != H5ES_NONE) triggers H5ES_insert failure after
  //    token creation in all 6 *_async functions.
  ScopedErrorSilence silence;
  const hid_t bad_es = fapl.get();

  EXPECT_EQ(H5Gcreate_async(file.get(), "/fail_es", H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, bad_es),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen_async(file.get(), "/sync_grp", H5P_DEFAULT, bad_es),
            H5I_INVALID_HID);
  EXPECT_LT(H5Gget_info_async(g_sync.get(), &info, bad_es), 0);
  EXPECT_LT(H5Gget_info_by_name_async(file.get(), "/sync_grp", &info,
                                      H5P_DEFAULT, bad_es),
            0);
  EXPECT_LT(H5Gget_info_by_idx_async(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     0, &info, H5P_DEFAULT, bad_es),
            0);
  EXPECT_LT(H5Gclose_async(g_sync.release(), bad_es), 0);
}

}  // namespace
}  // namespace hdf5
