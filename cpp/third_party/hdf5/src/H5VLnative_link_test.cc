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
// Tests native VOL connector link callbacks
// (third_party/hdf5/src/H5VLnative_link.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_link.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
}  // extern "C"

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

constexpr H5L_type_t kCustomUdLinkId = static_cast<H5L_type_t>(201);

herr_t DummyUdCreate(const char* /*link_name*/, hid_t /*loc_group*/,
                     const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                     hid_t /*lcpl_id*/) {
  return 0;
}

hid_t DummyUdTraverse(const char* /*link_name*/, hid_t /*cur_group*/,
                      const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                      hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

ssize_t DummyUdQuery(const char* /*link_name*/, const void* lnkdata,
                     size_t lnkdata_size, void* buf, size_t buf_size) {
  if (buf != nullptr && buf_size >= lnkdata_size && lnkdata != nullptr) {
    std::memcpy(buf, lnkdata, lnkdata_size);
  }
  return static_cast<ssize_t>(lnkdata_size);
}

herr_t CollectLinksCb(hid_t /*group*/, const char* name,
                      const H5L_info2_t* /*info*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(name);
  return 0;
}

herr_t FailLinkIterCb(hid_t /*group*/, const char* /*name*/,
                      const H5L_info2_t* /*info*/, void* /*op_data*/) {
  return -1;
}

TEST(H5VLnativeLinkTest, CreateHardSoftExternalAndUserDefinedLinks) {
  H5L_class_t ud_cls{};
  ud_cls.version = H5L_LINK_CLASS_T_VERS;
  ud_cls.id = kCustomUdLinkId;
  ud_cls.comment = "test_ud_link";
  ud_cls.create_func = DummyUdCreate;
  ud_cls.trav_func = DummyUdTraverse;
  ud_cls.query_func = DummyUdQuery;
  ASSERT_GE(H5Lregister(&ud_cls), 0);
  absl::Cleanup unregister_ud = [] {
    if (H5Lis_registered(kCustomUdLinkId) > 0) {
      H5Lunregister(kCustomUdLinkId);
    }
  };

  ScopedFile file1 = CreateInMemoryFile("links1.h5");
  ASSERT_TRUE(file1.valid());
  ScopedFile file2 = CreateInMemoryFile("links2.h5");
  ASSERT_TRUE(file2.valid());

  ScopedGroup g1(
      H5Gcreate2(file1.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ScopedGroup target(
      H5Gcreate2(g1.get(), "target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target.valid());
  target.reset();

  // Hard link with both loc IDs, H5L_SAME_LOC as cur, and H5L_SAME_LOC as new.
  ASSERT_GE(H5Lcreate_hard(g1.get(), "target", file1.get(), "/hard_from_g1",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(H5L_SAME_LOC, "/g1/target", file1.get(),
                           "/hard_same_cur", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(file1.get(), "/g1/target", H5L_SAME_LOC,
                           "/hard_same_new", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // H5Olink creates hard link via loc_params->type != H5VL_OBJECT_BY_NAME.
  ScopedGroup anon(H5Gcreate_anon(file1.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon.valid());
  ASSERT_GE(
      H5Olink(anon.get(), file1.get(), "/linked_anon", H5P_DEFAULT, H5P_DEFAULT),
      0);

  // Soft, external, and custom UD links.
  ASSERT_GE(H5Lcreate_soft("/g1/target", file1.get(), "/soft_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_external("ext.h5", "/ext_obj", file1.get(), "/ext_link",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);
  const char ud_payload[] = "ud_data";
  ASSERT_GE(H5Lcreate_ud(file1.get(), "/ud_link", kCustomUdLinkId, ud_payload,
                         sizeof(ud_payload), H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Negative creation paths: cross-file hard link, missing target, duplicates.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lcreate_hard(file1.get(), "/g1/target", file2.get(),
                             "/bad_xfile", H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lcreate_hard(file1.get(), "/missing_target", file1.get(),
                             "/hard_fail", H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Olink(anon.get(), file1.get(), "/linked_anon", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lcreate_soft("/g1/target", file1.get(), "/soft_link",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lcreate_ud(file1.get(), "/ud_link", kCustomUdLinkId, ud_payload,
                           sizeof(ud_payload), H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5VLnativeLinkTest, CopyMoveGetAndDeleteLinks) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup sub(
      H5Gcreate2(file.get(), "/sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  ScopedGroup leaf(
      H5Gcreate2(sub.get(), "leaf", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(leaf.valid());
  leaf.reset();

  // Copy and move (including deprecated H5Gmove which passes dst_obj=NULL).
  ASSERT_GE(H5Lcopy(file.get(), "/sub/leaf", file.get(), "/leaf_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lmove(file.get(), "/leaf_copy", sub.get(), "leaf_moved",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Gmove(file.get(), "/sub/leaf_moved", "/leaf_gmoved"), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lcopy(file.get(), "/no_such_link", file.get(), "/copy_fail",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lmove(file.get(), "/no_such_link", file.get(), "/move_fail",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  ASSERT_GE(H5Lcreate_soft("/sub/leaf", file.get(), "/soft_ref", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // H5VL_LINK_GET_INFO by name & by idx.
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(file.get(), "/soft_ref", &linfo, H5P_DEFAULT), 0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  H5L_info2_t linfo_idx{};
  ASSERT_GE(H5Lget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &linfo_idx, H5P_DEFAULT),
            0);

  // H5VL_LINK_GET_NAME by idx.
  char name_buf[64]{};
  ssize_t name_len = H5Lget_name_by_idx(file.get(), "/", H5_INDEX_NAME,
                                        H5_ITER_INC, 0, name_buf,
                                        sizeof(name_buf), H5P_DEFAULT);
  EXPECT_GT(name_len, 0);
  EXPECT_STREQ(name_buf, "leaf_gmoved");

  // H5VL_LINK_GET_VAL by name & by idx.
  char val_buf[64]{};
  ASSERT_GE(
      H5Lget_val(file.get(), "/soft_ref", val_buf, sizeof(val_buf), H5P_DEFAULT),
      0);
  EXPECT_STREQ(val_buf, "/sub/leaf");

  char val_idx_buf[64]{};
  ASSERT_GE(H5Lget_val_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 1,
                              val_idx_buf, sizeof(val_idx_buf), H5P_DEFAULT),
            0);
  EXPECT_STREQ(val_idx_buf, "/sub/leaf");

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lget_info2(file.get(), "/missing", &linfo, H5P_DEFAULT), 0);
    EXPECT_LT(H5Lget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                  99, &linfo_idx, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_name_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                 99, name_buf, sizeof(name_buf), H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_val(file.get(), "/leaf_gmoved", val_buf, sizeof(val_buf),
                         H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_val_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 99,
                                val_idx_buf, sizeof(val_idx_buf), H5P_DEFAULT),
              0);
  }

  // H5VL_LINK_EXISTS, H5VL_LINK_ITER (iterate/visit, both self and by_name),
  // and H5VL_LINK_DELETE (by name and by idx).
  EXPECT_GT(H5Lexists(file.get(), "/soft_ref", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(file.get(), "/does_not_exist", H5P_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lexists(file.get(), "/no_such_parent/child", H5P_DEFAULT), 0);
  }

  std::vector<std::string> visited;
  ASSERT_GE(
      H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, CollectLinksCb, &visited),
      0);
  EXPECT_THAT(visited,
              ::testing::IsSupersetOf({"leaf_gmoved", "soft_ref", "sub", "sub/leaf"}));

  visited.clear();
  ASSERT_GE(H5Lvisit_by_name2(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                              CollectLinksCb, &visited, H5P_DEFAULT),
            0);
  EXPECT_THAT(visited, ::testing::ElementsAre("leaf"));

  visited.clear();
  hsize_t iter_idx = 0;
  ASSERT_GE(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_INC, &iter_idx,
                        CollectLinksCb, &visited),
            0);
  EXPECT_THAT(visited, ::testing::ElementsAre("leaf_gmoved", "soft_ref", "sub"));

  visited.clear();
  hsize_t sub_idx = 0;
  ASSERT_GE(H5Literate_by_name2(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                                &sub_idx, CollectLinksCb, &visited, H5P_DEFAULT),
            0);
  EXPECT_THAT(visited, ::testing::ElementsAre("leaf"));

  {
    ScopedErrorSilence silence;
    hsize_t err_idx = 0;
    EXPECT_LT(H5Lvisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, FailLinkIterCb,
                        nullptr),
              0);
    EXPECT_LT(H5Literate2(file.get(), H5_INDEX_NAME, H5_ITER_INC, &err_idx,
                          FailLinkIterCb, nullptr),
              0);
    EXPECT_LT(H5Lvisit_by_name2(file.get(), "/missing", H5_INDEX_NAME,
                                H5_ITER_INC, CollectLinksCb, &visited,
                                H5P_DEFAULT),
              0);
    err_idx = 0;
    EXPECT_LT(H5Literate_by_name2(file.get(), "/missing", H5_INDEX_NAME,
                                  H5_ITER_INC, &err_idx, CollectLinksCb, &visited,
                                  H5P_DEFAULT),
              0);
  }

  ASSERT_GE(H5Ldelete(file.get(), "/soft_ref", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ldelete(file.get(), "/soft_ref", H5P_DEFAULT), 0);
    EXPECT_LT(H5Ldelete_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 99,
                               H5P_DEFAULT),
              0);
  }
}

TEST(H5VLnativeLinkTest, DirectCallbacksCoverEdgeAndErrorBranches) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup g1(
      H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());

  void* file_obj = H5VL_object(file.get());
  ASSERT_NE(file_obj, nullptr);

  ScopedApiContext cx;

  H5VL_loc_params_t valid_loc{};
  valid_loc.type = H5VL_OBJECT_BY_NAME;
  valid_loc.obj_type = H5I_FILE;
  valid_loc.loc_data.loc_by_name.name = "/g1";
  valid_loc.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;

  H5VL_loc_params_t dst_copy_loc{};
  dst_copy_loc.type = H5VL_OBJECT_BY_NAME;
  dst_copy_loc.obj_type = H5I_FILE;
  dst_copy_loc.loc_data.loc_by_name.name = "/direct_copy";
  dst_copy_loc.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;

  // Direct link_copy with src_obj == NULL and dst_obj == NULL.
  EXPECT_GE(H5VL__native_link_copy(nullptr, &valid_loc, file_obj, &dst_copy_loc,
                                   H5P_LINK_CREATE_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT, nullptr),
            0);
  H5VL_loc_params_t dst_copy_loc2 = dst_copy_loc;
  dst_copy_loc2.loc_data.loc_by_name.name = "/direct_copy2";
  EXPECT_GE(H5VL__native_link_copy(file_obj, &valid_loc, nullptr, &dst_copy_loc2,
                                   H5P_LINK_CREATE_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT, nullptr),
            0);

  // Direct link_move with src_obj == NULL.
  H5VL_loc_params_t src_move_loc = valid_loc;
  src_move_loc.loc_data.loc_by_name.name = "/direct_copy2";
  H5VL_loc_params_t dst_move_loc = valid_loc;
  dst_move_loc.loc_data.loc_by_name.name = "/direct_moved";
  EXPECT_GE(H5VL__native_link_move(nullptr, &src_move_loc, file_obj,
                                   &dst_move_loc, H5P_LINK_CREATE_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT, nullptr),
            0);

  ScopedErrorSilence silence;

  H5VL_loc_params_t bad_loc{};
  bad_loc.type = H5VL_OBJECT_BY_NAME;
  bad_loc.obj_type = H5I_DATASPACE;
  bad_loc.loc_data.loc_by_name.name = "/g1";
  bad_loc.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;

  // H5VL__native_link_create error branches.
  H5VL_link_create_args_t create_args{};
  create_args.op_type = H5VL_LINK_CREATE_HARD;
  create_args.args.hard.curr_obj = file_obj;
  create_args.args.hard.curr_loc_params = bad_loc;
  EXPECT_LT(H5VL__native_link_create(&create_args, file_obj, &valid_loc,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                     nullptr),
            0);

  create_args.args.hard.curr_loc_params = valid_loc;
  EXPECT_LT(H5VL__native_link_create(&create_args, file_obj, &bad_loc,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                     nullptr),
            0);

  create_args.op_type = H5VL_LINK_CREATE_SOFT;
  create_args.args.soft.target = "/g1";
  EXPECT_LT(H5VL__native_link_create(&create_args, file_obj, &bad_loc,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                     nullptr),
            0);

  create_args.op_type = H5VL_LINK_CREATE_UD;
  create_args.args.ud.type = kCustomUdLinkId;
  EXPECT_LT(H5VL__native_link_create(&create_args, file_obj, &bad_loc,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                     nullptr),
            0);

  // H5VL__native_link_copy / move bad obj_type.
  EXPECT_LT(H5VL__native_link_copy(file_obj, &bad_loc, file_obj, &valid_loc,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                   nullptr),
            0);
  EXPECT_LT(H5VL__native_link_copy(file_obj, &valid_loc, file_obj, &bad_loc,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                   nullptr),
            0);
  EXPECT_LT(H5VL__native_link_move(file_obj, &bad_loc, file_obj, &valid_loc,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                   nullptr),
            0);
  EXPECT_LT(H5VL__native_link_move(file_obj, &valid_loc, file_obj, &bad_loc,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                   nullptr),
            0);

  // H5VL__native_link_get bad obj_type, unknown loc_params.type, invalid op.
  H5L_info2_t linfo{};
  H5VL_link_get_args_t lget_args{};
  lget_args.op_type = H5VL_LINK_GET_INFO;
  lget_args.args.get_info.linfo = &linfo;
  EXPECT_LT(H5VL__native_link_get(file_obj, &bad_loc, &lget_args, H5P_DEFAULT,
                                  nullptr),
            0);

  H5VL_loc_params_t self_loc{};
  self_loc.type = H5VL_OBJECT_BY_SELF;
  self_loc.obj_type = H5I_FILE;
  EXPECT_LT(H5VL__native_link_get(file_obj, &self_loc, &lget_args, H5P_DEFAULT,
                                  nullptr),
            0);

  char val_buf[16]{};
  lget_args.op_type = H5VL_LINK_GET_VAL;
  lget_args.args.get_val.buf = val_buf;
  lget_args.args.get_val.buf_size = sizeof(val_buf);
  EXPECT_LT(H5VL__native_link_get(file_obj, &self_loc, &lget_args, H5P_DEFAULT,
                                  nullptr),
            0);

  // H5VL__native_link_specific error branches.
  bool exists = false;
  H5VL_link_specific_args_t spec_args{};
  spec_args.op_type = H5VL_LINK_EXISTS;
  spec_args.args.exists.exists = &exists;
  EXPECT_LT(H5VL__native_link_specific(file_obj, &bad_loc, &spec_args,
                                       H5P_DEFAULT, nullptr),
            0);

  spec_args.op_type = H5VL_LINK_ITER;
  spec_args.args.iterate.op = CollectLinksCb;
  EXPECT_LT(H5VL__native_link_specific(file_obj, &bad_loc, &spec_args,
                                       H5P_DEFAULT, nullptr),
            0);

  H5VL_loc_params_t idx_loc{};
  idx_loc.type = H5VL_OBJECT_BY_IDX;
  idx_loc.obj_type = H5I_FILE;
  EXPECT_LT(H5VL__native_link_specific(file_obj, &idx_loc, &spec_args,
                                       H5P_DEFAULT, nullptr),
            0);

  spec_args.op_type = H5VL_LINK_DELETE;
  EXPECT_LT(H5VL__native_link_specific(file_obj, &bad_loc, &spec_args,
                                       H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL__native_link_specific(file_obj, &self_loc, &spec_args,
                                       H5P_DEFAULT, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
