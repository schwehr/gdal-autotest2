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
// Tests native VOL connector group callbacks
// (third_party/hdf5/src/H5VLnative_group.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_group.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
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

herr_t CollectGroupMembersCb(hid_t /*group*/, const char* name, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(name);
  return 0;
}

herr_t StopEarlyGroupCb(hid_t /*group*/, const char* /*name*/, void* op_data) {
  int* count = static_cast<int*>(op_data);
  ++(*count);
  return 7;
}

herr_t FailGroupCb(hid_t /*group*/, const char* /*name*/, void* /*op_data*/) {
  return -1;
}

herr_t FailingFlushCb(hid_t /*obj_id*/, void* /*udata*/) { return -1; }

TEST(H5VLnativeGroupTest, CreateOpenAndGetGroupPropertiesAndInfo) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup g1(H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, gcpl.get(),
                            H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());

  ScopedGroup sub1(
      H5Gcreate2(g1.get(), "sub1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub1.valid());
  ScopedGroup sub2(
      H5Gcreate2(g1.get(), "sub2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub2.valid());

  // Duplicate group creation fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
        0);
  }

  // Anonymous group creation + linking + closing without linking.
  ScopedGroup anon(H5Gcreate_anon(file.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon.valid());
  ASSERT_GE(
      H5Olink(anon.get(), file.get(), "/anon_linked", H5P_DEFAULT, H5P_DEFAULT),
      0);

  ScopedGroup anon_unlinked(
      H5Gcreate_anon(file.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_unlinked.valid());
  anon_unlinked.reset();

  // Open existing group and fail on missing group.
  ScopedGroup reopened(H5Gopen2(file.get(), "/anon_linked", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Gopen2(file.get(), "/missing_group", H5P_DEFAULT), 0);
  }

  // H5VL_GROUP_GET_GCPL.
  ScopedPlist retrieved_gcpl(H5Gget_create_plist(g1.get()));
  ASSERT_TRUE(retrieved_gcpl.valid());
  unsigned crt_order_flags = 0;
  ASSERT_GE(
      H5Pget_link_creation_order(retrieved_gcpl.get(), &crt_order_flags), 0);
  EXPECT_EQ(crt_order_flags, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);

  // H5VL_GROUP_GET_INFO by self, by name, and by idx.
  H5G_info_t info_self{};
  ASSERT_GE(H5Gget_info(g1.get(), &info_self), 0);
  EXPECT_EQ(info_self.nlinks, 2);

  H5G_info_t info_by_name{};
  ASSERT_GE(
      H5Gget_info_by_name(file.get(), "/g1", &info_by_name, H5P_DEFAULT), 0);
  EXPECT_EQ(info_by_name.nlinks, 2);

  H5G_info_t info_by_idx{};
  ASSERT_GE(H5Gget_info_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 1,
                               &info_by_idx, H5P_DEFAULT),
            0);
  EXPECT_EQ(info_by_idx.nlinks, 2);

  {
    ScopedErrorSilence silence;
    H5G_info_t dummy_info{};
    EXPECT_LT(
        H5Gget_info_by_name(file.get(), "/missing", &dummy_info, H5P_DEFAULT),
        0);
    EXPECT_LT(H5Gget_info_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                 99, &dummy_info, H5P_DEFAULT),
              0);
  }
}

TEST(H5VLnativeGroupTest, ReadOnlyAnonGroupCreateAndRefreshBehavior) {
  TempFile tmp("ro_group_create.h5");
  {
    ScopedFile f(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/ro_g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  ScopedFile ro(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro.valid());

  ScopedGroup ro_g(H5Gopen2(ro.get(), "/ro_g", H5P_DEFAULT));
  ASSERT_TRUE(ro_g.valid());
  ASSERT_GE(H5Grefresh(ro_g.get()), 0);

  void* ro_g_obj = H5VL_object(ro_g.get());
  ASSERT_NE(ro_g_obj, nullptr);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Gcreate_anon(ro.get(), H5P_DEFAULT, H5P_DEFAULT), 0);

  ScopedApiContext cx;
  H5VL_group_specific_args_t spec_args{};
  spec_args.op_type = H5VL_GROUP_REFRESH;
  spec_args.args.refresh.grp_id = H5I_INVALID_HID;
  EXPECT_LT(
      H5VL__native_group_specific(ro_g_obj, &spec_args, H5P_DEFAULT, nullptr),
      0);
}

TEST(H5VLnativeGroupTest, MountUnmountFlushAndRefresh) {
  ScopedFile parent = CreateInMemoryFile("parent.h5");
  ASSERT_TRUE(parent.valid());
  ScopedFile child = CreateInMemoryFile("child.h5");
  ASSERT_TRUE(child.valid());

  ScopedGroup mnt_pt(
      H5Gcreate2(parent.get(), "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt_pt.valid());
  ScopedGroup child_sub(H5Gcreate2(child.get(), "/inside", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_sub.valid());
  child_sub.reset();

  ASSERT_GE(H5Fmount(parent.get(), "/mnt", child.get(), H5P_DEFAULT), 0);
  ScopedGroup mounted_inside(
      H5Gopen2(parent.get(), "/mnt/inside", H5P_DEFAULT));
  ASSERT_TRUE(mounted_inside.valid());
  mounted_inside.reset();

  ASSERT_GE(H5Funmount(parent.get(), "/mnt"), 0);

  // Mount and unmount error paths.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Fmount(parent.get(), "/nonexistent", child.get(), H5P_DEFAULT), 0);
    EXPECT_LT(H5Funmount(parent.get(), "/mnt"), 0);
    EXPECT_LT(H5Funmount(parent.get(), "/nonexistent"), 0);
  }

  // Group flush and refresh.
  ASSERT_GE(H5Gflush(mnt_pt.get()), 0);
  ASSERT_GE(H5Grefresh(mnt_pt.get()), 0);

  // Flush failure via failing object flush callback.
  ScopedPlist fail_fapl = MakeCoreFapl();
  ASSERT_TRUE(fail_fapl.valid());
  ASSERT_GE(H5Pset_object_flush_cb(fail_fapl.get(), FailingFlushCb, nullptr),
            0);
  ScopedFile fail_file(
      H5Fcreate("flush_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fail_fapl.get()));
  ASSERT_TRUE(fail_file.valid());
  ScopedGroup fg(H5Gcreate2(fail_file.get(), "/fg", H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT));
  ASSERT_TRUE(fg.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Gflush(fg.get()), 0);
  }
}

TEST(H5VLnativeGroupTest, DeprecatedIterateAndGetObjinfo) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup a(
      H5Gcreate2(file.get(), "/alpha", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a.valid());
  ScopedGroup b(
      H5Gcreate2(file.get(), "/beta", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(b.valid());
  ASSERT_GE(H5Lcreate_soft("/alpha", file.get(), "/soft_to_alpha", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  std::vector<std::string> names;
  int idx = 0;
  ASSERT_GE(H5Giterate(file.get(), "/", &idx, CollectGroupMembersCb, &names), 0);
  EXPECT_THAT(names, ::testing::ElementsAre("alpha", "beta", "soft_to_alpha"));
  EXPECT_EQ(idx, 3);

  int early_count = 0;
  int start_idx = 0;
  EXPECT_EQ(
      H5Giterate(file.get(), "/", &start_idx, StopEarlyGroupCb, &early_count),
      7);
  EXPECT_EQ(early_count, 1);

  {
    ScopedErrorSilence silence;
    int err_idx = 0;
    EXPECT_LT(H5Giterate(file.get(), "/", &err_idx, FailGroupCb, nullptr), 0);
    EXPECT_LT(H5Giterate(file.get(), "/no_such_grp", &err_idx,
                         CollectGroupMembersCb, &names),
              0);
  }

  H5G_stat_t sb{};
  ASSERT_GE(H5Gget_objinfo(file.get(), "/soft_to_alpha", false, &sb), 0);
  EXPECT_EQ(sb.type, H5G_LINK);

  sb = {};
  ASSERT_GE(H5Gget_objinfo(file.get(), "/soft_to_alpha", true, &sb), 0);
  EXPECT_EQ(sb.type, H5G_GROUP);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Gget_objinfo(file.get(), "/missing", false, &sb), 0);
  }
}

TEST(H5VLnativeGroupTest, DirectCallbacksExerciseInternalErrorBranches) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  void* file_obj = H5VL_object(file.get());
  void* grp_obj = H5VL_object(grp.get());
  ASSERT_NE(file_obj, nullptr);
  ASSERT_NE(grp_obj, nullptr);

  ScopedErrorSilence silence;
  ScopedApiContext cx;

  H5VL_loc_params_t bad_loc{};
  bad_loc.type = H5VL_OBJECT_BY_SELF;
  bad_loc.obj_type = H5I_DATASPACE;

  // Invalid obj_type in create/open.
  EXPECT_EQ(H5VL__native_group_create(file_obj, &bad_loc, "bad", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT,
                                      nullptr),
            nullptr);
  EXPECT_EQ(H5VL__native_group_open(file_obj, &bad_loc, "bad", H5P_DEFAULT,
                                    H5P_DEFAULT, nullptr),
            nullptr);

  // Invalid obj_type and loc_params.type in group_get.
  H5G_info_t ginfo{};
  H5VL_group_get_args_t get_args{};
  get_args.op_type = H5VL_GROUP_GET_INFO;
  get_args.args.get_info.loc_params = bad_loc;
  get_args.args.get_info.ginfo = &ginfo;
  EXPECT_LT(
      H5VL__native_group_get(file_obj, &get_args, H5P_DEFAULT, nullptr), 0);

  H5O_token_t dummy_token{};
  get_args.args.get_info.loc_params.obj_type = H5I_FILE;
  get_args.args.get_info.loc_params.type = H5VL_OBJECT_BY_TOKEN;
  get_args.args.get_info.loc_params.loc_data.loc_by_token.token = &dummy_token;
  EXPECT_LT(
      H5VL__native_group_get(file_obj, &get_args, H5P_DEFAULT, nullptr), 0);

  // Optional bad obj_type and invalid op_type.
  H5VL_native_group_optional_args_t opt_data{};
  opt_data.iterate_old.loc_params = bad_loc;
  opt_data.iterate_old.loc_params.type = H5VL_OBJECT_BY_NAME;
  opt_data.iterate_old.loc_params.loc_data.loc_by_name.name = ".";
  opt_data.iterate_old.loc_params.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;
  hsize_t last_obj = 0;
  opt_data.iterate_old.last_obj = &last_obj;
  opt_data.iterate_old.op = CollectGroupMembersCb;
  H5VL_optional_args_t opt_args{};
  opt_args.op_type = H5VL_NATIVE_GROUP_ITERATE_OLD;
  opt_args.args = &opt_data;
  EXPECT_LT(
      H5VL__native_group_optional(file_obj, &opt_args, H5P_DEFAULT, nullptr),
      0);

  H5G_stat_t statbuf{};
  opt_data.get_objinfo.loc_params = bad_loc;
  opt_data.get_objinfo.loc_params.type = H5VL_OBJECT_BY_NAME;
  opt_data.get_objinfo.loc_params.loc_data.loc_by_name.name = "/g";
  opt_data.get_objinfo.loc_params.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;
  opt_data.get_objinfo.follow_link = false;
  opt_data.get_objinfo.statbuf = &statbuf;
  opt_args.op_type = H5VL_NATIVE_GROUP_GET_OBJINFO;
  EXPECT_LT(
      H5VL__native_group_optional(file_obj, &opt_args, H5P_DEFAULT, nullptr),
      0);

  opt_args.op_type = 999;
  EXPECT_LT(
      H5VL__native_group_optional(file_obj, &opt_args, H5P_DEFAULT, nullptr),
      0);
}

}  // namespace
}  // namespace hdf5
