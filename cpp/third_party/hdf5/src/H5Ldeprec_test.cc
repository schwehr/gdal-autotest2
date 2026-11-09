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
// Tests deprecated HDF5 1.8/1.10 link APIs (H5Literate1,
// H5Literate_by_name1, H5Lget_info1, H5Lget_info_by_idx1, H5Lvisit1,
// H5Lvisit_by_name1) in third_party/hdf5/src/H5Ldeprec.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ldeprec.c

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

constexpr H5L_type_t kCustomUdLinkType = static_cast<H5L_type_t>(181);
constexpr H5VL_class_value_t kNonNativeVolValue = 321;
constexpr char kNonNativeVolName[] = "h5ldeprec_nonnative_mock_vol";

hid_t DummyUdTraverse(const char* /*link_name*/, hid_t /*cur_group*/,
                      const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                      hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

ssize_t DummyUdQuery(const char* /*link_name*/, const void* /*lnkdata*/,
                     size_t lnkdata_size, void* buf, size_t buf_size) {
  if (buf != nullptr && buf_size > 0 && lnkdata_size > 0) {
    std::memset(buf, 0, buf_size < lnkdata_size ? buf_size : lnkdata_size);
  }
  return static_cast<ssize_t>(lnkdata_size);
}

struct MockNonNativeFile {
  int dummy = 1;
};

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  return new MockNonNativeFile{};
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  delete static_cast<MockNonNativeFile*>(file);
  return 0;
}

H5VL_class_t g_non_native_cls{};
bool g_fail_get_conn_cls = false;

herr_t MockGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                      const H5VL_class_t** conn_cls) {
  if (g_fail_get_conn_cls) {
    return -1;
  }
  *conn_cls = &g_non_native_cls;
  return 0;
}

herr_t MockOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/, int /*opt_type*/,
                    uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

class ScopedNonNativeVol {
 public:
  ScopedNonNativeVol() {
    g_fail_get_conn_cls = false;
    g_non_native_cls = {};
    g_non_native_cls.version = H5VL_VERSION;
    g_non_native_cls.value = kNonNativeVolValue;
    g_non_native_cls.name = kNonNativeVolName;
    g_non_native_cls.file_cls.create = MockFileCreate;
    g_non_native_cls.file_cls.close = MockFileClose;
    g_non_native_cls.introspect_cls.get_conn_cls = MockGetConnCls;
    g_non_native_cls.introspect_cls.opt_query = MockOptQuery;
    vol_id_ = H5VLregister_connector(&g_non_native_cls, H5P_DEFAULT);
  }

  ~ScopedNonNativeVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_fail_get_conn_cls = false;
  }

  ScopedNonNativeVol(const ScopedNonNativeVol&) = delete;
  ScopedNonNativeVol& operator=(const ScopedNonNativeVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

struct IterRecord {
  std::string name;
  H5L_type_t type;
  bool corder_valid;
  int64_t corder;
  H5T_cset_t cset;
  haddr_t address;
  size_t val_size;
};

struct IterContext {
  std::vector<IterRecord> records;
  int stop_after = -1;
  herr_t stop_code = 1;
  bool return_error = false;
};

herr_t RecordLinkCb(hid_t /*group*/, const char* name, const H5L_info1_t* info,
                    void* op_data) {
  auto* ctx = static_cast<IterContext*>(op_data);
  if (ctx->return_error) {
    return -1;
  }
  IterRecord rec{};
  rec.name = name ? name : "";
  if (info != nullptr) {
    rec.type = info->type;
    rec.corder_valid = info->corder_valid;
    rec.corder = info->corder;
    rec.cset = info->cset;
    if (info->type == H5L_TYPE_HARD) {
      rec.address = info->u.address;
    } else {
      rec.val_size = info->u.val_size;
    }
  }
  ctx->records.push_back(rec);
  if (ctx->stop_after > 0 &&
      static_cast<int>(ctx->records.size()) >= ctx->stop_after) {
    return ctx->stop_code;
  }
  return 0;
}

TEST(H5LdeprecTest, GetInfo1AndByIdx1CoverHardSoftExternalAndUdLinks) {
  H5L_class_t ud_cls{};
  ud_cls.version = H5L_LINK_CLASS_T_VERS;
  ud_cls.id = kCustomUdLinkType;
  ud_cls.comment = "deprec_test_ud";
  ud_cls.trav_func = DummyUdTraverse;
  ud_cls.query_func = DummyUdQuery;
  ASSERT_GE(H5Lregister(&ud_cls), 0);
  absl::Cleanup unreg_ud = [] { (void)H5Lunregister(kCustomUdLinkType); };

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5ldeprec_info.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/tracked", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedGroup sub(
      H5Gcreate2(grp.get(), "z_hard", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());

  const char* kSoftTarget = "/tracked/z_hard";
  ASSERT_GE(H5Lcreate_soft(kSoftTarget, grp.get(), "a_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_external("other.h5", "/obj", grp.get(), "m_ext",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
  const char kUdData[] = "hello";
  ASSERT_GE(H5Lcreate_ud(grp.get(), "u_ud", kCustomUdLinkType, kUdData,
                         sizeof(kUdData), H5P_DEFAULT, H5P_DEFAULT),
            0);

  // H5Lget_info1: hard link, soft link, external link, UD link, and NULL linfo.
  H5L_info1_t hard_info{};
  ASSERT_GE(H5Lget_info1(grp.get(), "z_hard", &hard_info, H5P_DEFAULT), 0);
  EXPECT_EQ(hard_info.type, H5L_TYPE_HARD);
  EXPECT_TRUE(hard_info.corder_valid);
  EXPECT_EQ(hard_info.corder, 0);
  EXPECT_NE(hard_info.u.address, HADDR_UNDEF);

  H5L_info1_t soft_info{};
  ASSERT_GE(
      H5Lget_info1(file.get(), "/tracked/a_soft", &soft_info, H5P_DEFAULT), 0);
  EXPECT_EQ(soft_info.type, H5L_TYPE_SOFT);
  EXPECT_EQ(soft_info.u.val_size, std::strlen(kSoftTarget) + 1);

  H5L_info1_t ext_info{};
  ASSERT_GE(H5Lget_info1(grp.get(), "m_ext", &ext_info, H5P_DEFAULT), 0);
  EXPECT_EQ(ext_info.type, H5L_TYPE_EXTERNAL);
  EXPECT_GT(ext_info.u.val_size, 0u);

  H5L_info1_t ud_info{};
  ASSERT_GE(H5Lget_info1(grp.get(), "u_ud", &ud_info, H5P_DEFAULT), 0);
  EXPECT_EQ(ud_info.type, kCustomUdLinkType);
  EXPECT_EQ(ud_info.u.val_size, sizeof(kUdData));

  EXPECT_GE(H5Lget_info1(grp.get(), "z_hard", nullptr, H5P_DEFAULT), 0);

  // H5Lget_info_by_idx1: name & creation order, INC & DEC, hard/soft/NULL.
  H5L_info1_t by_idx{};
  ASSERT_GE(H5Lget_info_by_idx1(file.get(), "/tracked", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &by_idx, H5P_DEFAULT),
            0);
  EXPECT_EQ(by_idx.type, H5L_TYPE_SOFT);

  ASSERT_GE(H5Lget_info_by_idx1(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                                0, &by_idx, H5P_DEFAULT),
            0);
  EXPECT_EQ(by_idx.type, H5L_TYPE_HARD);
  EXPECT_EQ(by_idx.u.address, hard_info.u.address);

  ASSERT_GE(H5Lget_info_by_idx1(grp.get(), ".", H5_INDEX_NAME, H5_ITER_DEC, 0,
                                &by_idx, H5P_DEFAULT),
            0);
  EXPECT_EQ(by_idx.type, H5L_TYPE_HARD);

  EXPECT_GE(H5Lget_info_by_idx1(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                nullptr, H5P_DEFAULT),
            0);
}

TEST(H5LdeprecTest, Iterate1ByName1Visit1AndVisitByName1) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("h5ldeprec_iter.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup root_sub(
      H5Gcreate2(file.get(), "/sub", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(root_sub.valid());
  ScopedGroup nested(H5Gcreate2(root_sub.get(), "c_nested", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(nested.valid());
  ASSERT_GE(H5Lcreate_soft("c_nested", root_sub.get(), "a_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/sub/c_nested", nested.get(), "in_nested_soft",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(nested.get(), ".", root_sub.get(), "b_hard",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 1. H5Literate1 with file ID and group ID, full & resume via idx_p.
  IterContext file_ctx;
  ASSERT_EQ(H5Literate1(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        RecordLinkCb, &file_ctx),
            0);
  ASSERT_EQ(file_ctx.records.size(), 1u);
  EXPECT_EQ(file_ctx.records[0].name, "sub");
  EXPECT_EQ(file_ctx.records[0].type, H5L_TYPE_HARD);

  IterContext stop_ctx;
  stop_ctx.stop_after = 2;
  stop_ctx.stop_code = 7;
  hsize_t resume_idx = 0;
  EXPECT_EQ(H5Literate1(root_sub.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC,
                        &resume_idx, RecordLinkCb, &stop_ctx),
            7);
  EXPECT_EQ(resume_idx, 2u);
  ASSERT_EQ(stop_ctx.records.size(), 2u);
  EXPECT_EQ(stop_ctx.records[0].name, "c_nested");
  EXPECT_EQ(stop_ctx.records[1].name, "a_soft");

  IterContext rest_ctx;
  ASSERT_EQ(H5Literate1(root_sub.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC,
                        &resume_idx, RecordLinkCb, &rest_ctx),
            0);
  ASSERT_EQ(rest_ctx.records.size(), 1u);
  EXPECT_EQ(rest_ctx.records[0].name, "b_hard");

  // 2. H5Literate_by_name1 by name descending.
  IterContext by_name_ctx;
  ASSERT_EQ(
      H5Literate_by_name1(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_DEC,
                          nullptr, RecordLinkCb, &by_name_ctx, H5P_DEFAULT),
      0);
  ASSERT_EQ(by_name_ctx.records.size(), 3u);
  EXPECT_THAT((std::vector<std::string>{by_name_ctx.records[0].name,
                                        by_name_ctx.records[1].name,
                                        by_name_ctx.records[2].name}),
              ElementsAre("c_nested", "b_hard", "a_soft"));

  // 3. H5Lvisit1 recursively traverses links including soft link in subgroup.
  IterContext visit_ctx;
  ASSERT_EQ(H5Lvisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb,
                      &visit_ctx),
            0);
  EXPECT_GE(visit_ctx.records.size(), 5u);

  // Early short-circuit in H5Lvisit1.
  IterContext visit_stop;
  visit_stop.stop_after = 2;
  visit_stop.stop_code = 13;
  EXPECT_EQ(H5Lvisit1(root_sub.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb,
                      &visit_stop),
            13);
  EXPECT_EQ(visit_stop.records.size(), 2u);

  // 4. H5Lvisit_by_name1.
  IterContext visit_name_ctx;
  ASSERT_EQ(H5Lvisit_by_name1(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &visit_name_ctx, H5P_DEFAULT),
            0);
  EXPECT_GE(visit_name_ctx.records.size(), 4u);

  // 5. Negative callback returns < 0 propagate through VOL.
  IterContext err_ctx;
  err_ctx.return_error = true;
  ScopedErrorSilence silence;
  EXPECT_LT(H5Literate1(root_sub.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        RecordLinkCb, &err_ctx),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, RecordLinkCb, &err_ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit1(root_sub.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb,
                      &err_ctx),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &err_ctx, H5P_DEFAULT),
            0);
}

TEST(H5LdeprecTest, ArgumentValidationAndNonNativeVolErrors) {
  ScopedFile file = CreateInMemoryFile("h5ldeprec_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedNonNativeVol mock_vol;
  ASSERT_NE(mock_vol.id(), H5I_INVALID_HID);
  ScopedPlist mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(mock_fapl.valid());
  ASSERT_GE(H5Pset_vol(mock_fapl.get(), mock_vol.id(), nullptr), 0);
  ScopedFile mock_file(
      H5Fcreate("non_native.h5", H5F_ACC_TRUNC, H5P_DEFAULT, mock_fapl.get()));
  ASSERT_TRUE(mock_file.valid());

  H5L_info1_t info{};
  IterContext ctx;
  ScopedErrorSilence silence;

  // H5Literate1 argument errors.
  EXPECT_LT(H5Literate1(space.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(H5Literate1(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(H5Literate1(file.get(), H5_INDEX_N, H5_ITER_INC, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(H5Literate1(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(H5Literate1(file.get(), H5_INDEX_NAME, H5_ITER_N, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(H5Literate1(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        nullptr, &ctx),
            0);

  // H5Literate_by_name1 argument & lookup errors.
  EXPECT_LT(H5Literate_by_name1(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, nullptr, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, RecordLinkCb, &ctx, fapl.get()),
            0);
  EXPECT_LT(H5Literate_by_name1(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Literate_by_name1(file.get(), "/missing", H5_INDEX_NAME, H5_ITER_INC,
                          nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
      0);

  // H5Lget_info1 argument & lookup errors.
  EXPECT_LT(H5Lget_info1(file.get(), nullptr, &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info1(file.get(), "", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info1(file.get(), "missing", &info, fapl.get()), 0);
  EXPECT_LT(H5Lget_info1(space.get(), "missing", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info1(file.get(), "missing", &info, H5P_DEFAULT), 0);

  // H5Lget_info_by_idx1 argument & lookup errors.
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, fapl.get()),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                                &info, H5P_DEFAULT),
            0);

  // H5Lvisit1 argument errors.
  EXPECT_LT(
      H5Lvisit1(space.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb, &ctx),
      0);
  EXPECT_LT(
      H5Lvisit1(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, RecordLinkCb, &ctx),
      0);
  EXPECT_LT(
      H5Lvisit1(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, RecordLinkCb, &ctx),
      0);
  EXPECT_LT(H5Lvisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr, &ctx),
            0);

  // H5Lvisit_by_name1 argument & lookup errors.
  EXPECT_LT(H5Lvisit_by_name1(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              nullptr, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, fapl.get()),
            0);
  EXPECT_LT(H5Lvisit_by_name1(space.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit_by_name1(file.get(), "/missing", H5_INDEX_NAME,
                              H5_ITER_INC, RecordLinkCb, &ctx, H5P_DEFAULT),
            0);

  // Non-native VOL rejection across all 6 deprecated H5L functions.
  EXPECT_LT(H5Literate1(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(
      H5Literate_by_name1(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                          nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lget_info1(mock_file.get(), "x", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info_by_idx1(mock_file.get(), ".", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit1(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb,
                      &ctx),
            0);
  EXPECT_LT(H5Lvisit_by_name1(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);

  // H5VL_object_is_native failure branch when get_conn_cls fails.
  g_fail_get_conn_cls = true;
  EXPECT_LT(H5Literate1(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        RecordLinkCb, &ctx),
            0);
  EXPECT_LT(
      H5Literate_by_name1(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                          nullptr, RecordLinkCb, &ctx, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Lget_info1(mock_file.get(), "x", &info, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info_by_idx1(mock_file.get(), ".", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &info, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lvisit1(mock_file.get(), H5_INDEX_NAME, H5_ITER_INC, RecordLinkCb,
                      &ctx),
            0);
  EXPECT_LT(H5Lvisit_by_name1(mock_file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              RecordLinkCb, &ctx, H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
