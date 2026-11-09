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
// Tests native VOL connector object callbacks
// (third_party/hdf5/src/H5VLnative_object.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_object.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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

herr_t CollectObjectsCb(hid_t /*obj*/, const char* name,
                        const H5O_info2_t* /*info*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(name);
  return 0;
}

herr_t FailObjectsCb(hid_t /*obj*/, const char* /*name*/,
                     const H5O_info2_t* /*info*/, void* /*op_data*/) {
  return -1;
}

herr_t FailingObjectFlushCb(hid_t /*obj_id*/, void* /*udata*/) { return -1; }

TEST(H5VLnativeObjectTest, OpenCopyAndGetObjectMetadataAndReferences) {
  ScopedFile file = CreateInMemoryFile("obj1.h5");
  ASSERT_TRUE(file.valid());
  ScopedFile file2 = CreateInMemoryFile("obj2.h5");
  ASSERT_TRUE(file2.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();

  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dt", committed_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  committed_dt.reset();

  // Open by name, by index, and by token.
  ScopedObject open_by_name(H5Oopen(file.get(), "/grp/dset", H5P_DEFAULT));
  ASSERT_TRUE(open_by_name.valid());

  ScopedObject open_by_idx(H5Oopen_by_idx(
      file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC, 0, H5P_DEFAULT));
  ASSERT_TRUE(open_by_idx.valid());

  H5O_info2_t dset_info{};
  ASSERT_GE(H5Oget_info3(open_by_name.get(), &dset_info, H5O_INFO_ALL), 0);
  EXPECT_EQ(dset_info.type, H5O_TYPE_DATASET);

  ScopedObject open_by_token(H5Oopen_by_token(file.get(), dset_info.token));
  ASSERT_TRUE(open_by_token.valid());

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Oopen(file.get(), "/missing", H5P_DEFAULT), 0);
    EXPECT_LT(H5Oopen_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 99,
                             H5P_DEFAULT),
              0);
    H5O_token_t bad_token{};
    ASSERT_GE(H5VLnative_addr_to_token(file.get(), 99999999ULL, &bad_token), 0);
    EXPECT_LT(H5Oopen_by_token(file.get(), bad_token), 0);
  }

  // H5Ocopy across files and negative cases.
  ASSERT_GE(H5Ocopy(file.get(), "/grp", file2.get(), "/grp_copy", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(file.get(), "/missing", file2.get(), "/fail_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // H5VL_OBJECT_GET_FILE via H5Iget_file_id.
  ScopedFile got_file(H5Iget_file_id(open_by_name.get()));
  ASSERT_TRUE(got_file.valid());

  // H5VL_OBJECT_GET_NAME via H5VL_OBJECT_BY_SELF (H5Iget_name).
  char self_name[64]{};
  ssize_t self_len = H5Iget_name(open_by_name.get(), self_name, sizeof(self_name));
  EXPECT_GT(self_len, 0);
  EXPECT_STREQ(self_name, "/grp/dset");

  // H5VL_OBJECT_LOOKUP + H5VL_OBJECT_GET_TYPE + H5VL_OBJECT_GET_NAME(BY_TOKEN)
  // via HDF5 references.
  H5R_ref_t ref_grp{};
  ASSERT_GE(H5Rcreate_object(file.get(), "/grp", H5P_DEFAULT, &ref_grp), 0);
  absl::Cleanup destroy_ref_grp = [&ref_grp] { H5Rdestroy(&ref_grp); };

  H5O_type_t ref_type = H5O_TYPE_UNKNOWN;
  ASSERT_GE(H5Rget_obj_type3(&ref_grp, H5P_DEFAULT, &ref_type), 0);
  EXPECT_EQ(ref_type, H5O_TYPE_GROUP);

  char ref_name[64]{};
  ssize_t ref_name_len =
      H5Rget_obj_name(&ref_grp, H5P_DEFAULT, ref_name, sizeof(ref_name));
  EXPECT_GT(ref_name_len, 0);
  EXPECT_STREQ(ref_name, "/grp");

  // H5VL_OBJECT_GET_INFO by name and by index (+ errors).
  H5O_info2_t info_by_name{};
  ASSERT_GE(H5Oget_info_by_name3(file.get(), "/dt", &info_by_name,
                                 H5O_INFO_BASIC, H5P_DEFAULT),
            0);
  EXPECT_EQ(info_by_name.type, H5O_TYPE_NAMED_DATATYPE);

  H5O_info2_t info_by_idx{};
  ASSERT_GE(H5Oget_info_by_idx3(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &info_by_idx, H5O_INFO_ALL, H5P_DEFAULT),
            0);

  {
    ScopedErrorSilence silence;
    H5R_ref_t bad_ref{};
    EXPECT_LT(H5Rcreate_object(file.get(), "/missing", H5P_DEFAULT, &bad_ref),
              0);
    EXPECT_LT(H5Oget_info_by_name3(file.get(), "/missing", &info_by_name,
                                   H5O_INFO_BASIC, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Oget_info_by_idx3(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                  99, &info_by_idx, H5O_INFO_ALL, H5P_DEFAULT),
              0);
  }

  // Delete an unreferenced object and query its reference type -> 0 == rc.
  ScopedGroup temp_grp(
      H5Gcreate2(file.get(), "/ephemeral", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(temp_grp.valid());
  temp_grp.reset();

  H5R_ref_t eph_ref{};
  ASSERT_GE(H5Rcreate_object(file.get(), "/ephemeral", H5P_DEFAULT, &eph_ref),
            0);
  absl::Cleanup destroy_eph = [&eph_ref] { H5Rdestroy(&eph_ref); };
  ASSERT_GE(H5Ldelete(file.get(), "/ephemeral", H5P_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    H5O_type_t deleted_type = H5O_TYPE_UNKNOWN;
    EXPECT_LT(H5Rget_obj_type3(&eph_ref, H5P_DEFAULT, &deleted_type), 0);
  }
}

TEST(H5VLnativeObjectTest, SpecificOperationsRefCountExistsVisitFlushRefresh) {
  TempFile ro_tmp("obj_ro.h5");
  {
    ScopedFile f(
        H5Fcreate(ro_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }
  {
    ScopedFile ro(H5Fopen(ro_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro.valid());
    ScopedGroup ro_g(H5Gopen2(ro.get(), "/g", H5P_DEFAULT));
    ASSERT_TRUE(ro_g.valid());
    ASSERT_GE(H5Orefresh(ro_g.get()), 0);

    void* ro_g_obj = H5VL_object(ro_g.get());
    ASSERT_NE(ro_g_obj, nullptr);

    ScopedErrorSilence silence;
    EXPECT_LT(H5Oincr_refcount(ro_g.get()), 0);

    ScopedApiContext cx;
    H5VL_loc_params_t ro_self{};
    ro_self.type = H5VL_OBJECT_BY_SELF;
    ro_self.obj_type = H5I_GROUP;
    H5VL_object_specific_args_t spec_args{};
    spec_args.op_type = H5VL_OBJECT_REFRESH;
    spec_args.args.refresh.obj_id = H5I_INVALID_HID;
    EXPECT_LT(H5VL__native_object_specific(ro_g_obj, &ro_self, &spec_args,
                                           H5P_DEFAULT, nullptr),
              0);
  }

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup sub(
      H5Gcreate2(grp.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());

  // Increment then decrement refcount cleanly.
  ASSERT_GE(H5Oincr_refcount(grp.get()), 0);
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_BASIC), 0);
  EXPECT_EQ(oinfo.rc, 2U);
  ASSERT_GE(H5Odecr_refcount(grp.get()), 0);
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_BASIC), 0);
  EXPECT_EQ(oinfo.rc, 1U);

  // H5Oexists_by_name.
  ASSERT_GE(H5Lcreate_soft("/no_target", file.get(), "/dangling", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_GT(H5Oexists_by_name(file.get(), "/grp", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Oexists_by_name(file.get(), "/dangling", H5P_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Oexists_by_name(file.get(), "/no_such_grp/child", H5P_DEFAULT),
              0);
  }

  // H5Ovisit3 and H5Ovisit_by_name3 (+ errors).
  std::vector<std::string> visited;
  ASSERT_GE(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, CollectObjectsCb,
                      &visited, H5O_INFO_BASIC),
            0);
  EXPECT_THAT(visited, ::testing::IsSupersetOf({".", "grp", "grp/sub"}));

  visited.clear();
  ASSERT_GE(H5Ovisit_by_name3(file.get(), "/grp", H5_INDEX_NAME, H5_ITER_INC,
                              CollectObjectsCb, &visited, H5O_INFO_BASIC,
                              H5P_DEFAULT),
            0);
  EXPECT_THAT(visited, ::testing::ElementsAre(".", "sub"));

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC, FailObjectsCb,
                        nullptr, H5O_INFO_BASIC),
              0);
    EXPECT_LT(H5Ovisit_by_name3(file.get(), "/missing", H5_INDEX_NAME,
                                H5_ITER_INC, CollectObjectsCb, &visited,
                                H5O_INFO_BASIC, H5P_DEFAULT),
              0);
  }

  // H5Oflush and H5Orefresh.
  ASSERT_GE(H5Oflush(grp.get()), 0);
  ASSERT_GE(H5Orefresh(grp.get()), 0);

  // Failing object flush callback exercises H5VL_OBJECT_FLUSH failure.
  ScopedPlist fail_fapl = MakeCoreFapl();
  ASSERT_TRUE(fail_fapl.valid());
  ASSERT_GE(
      H5Pset_object_flush_cb(fail_fapl.get(), FailingObjectFlushCb, nullptr),
      0);
  ScopedFile fail_file(H5Fcreate("oflush_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fail_fapl.get()));
  ASSERT_TRUE(fail_file.valid());
  ScopedGroup fg(H5Gcreate2(fail_file.get(), "/fg", H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT));
  ASSERT_TRUE(fg.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Oflush(fg.get()), 0);
  }
}

TEST(H5VLnativeObjectTest, OptionalCommentsMdcFlushesAndNativeInfo) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Set/get comment by self and by name (+ errors).
  ASSERT_GE(H5Oset_comment(grp.get(), "self comment"), 0);
  char cbuf[64]{};
  ASSERT_GT(H5Oget_comment(grp.get(), cbuf, sizeof(cbuf)), 0);
  EXPECT_STREQ(cbuf, "self comment");

  ASSERT_GE(H5Oset_comment_by_name(file.get(), "/grp", "name comment",
                                   H5P_DEFAULT),
            0);
  std::memset(cbuf, 0, sizeof(cbuf));
  ASSERT_GT(H5Oget_comment_by_name(file.get(), "/grp", cbuf, sizeof(cbuf),
                                   H5P_DEFAULT),
            0);
  EXPECT_STREQ(cbuf, "name comment");

  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Oset_comment_by_name(file.get(), "/missing", "bad", H5P_DEFAULT), 0);
    EXPECT_LT(H5Oget_comment_by_name(file.get(), "/missing", cbuf, sizeof(cbuf),
                                     H5P_DEFAULT),
              0);
  }

  // MDC corking (disable / status / enable + error cases).
  bool disabled = true;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);

  ASSERT_GE(H5Odisable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_TRUE(disabled);

  {
    ScopedErrorSilence silence;
    // Double-disable returns error.
    EXPECT_LT(H5Odisable_mdc_flushes(grp.get()), 0);
  }

  ASSERT_GE(H5Oenable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);

  {
    ScopedErrorSilence silence;
    // Enable when not corked returns error.
    EXPECT_LT(H5Oenable_mdc_flushes(grp.get()), 0);
  }

  // H5Oget_native_info(_by_name|_by_idx) (+ errors).
  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(grp.get(), &ninfo, H5O_NATIVE_INFO_ALL), 0);
  EXPECT_GE(ninfo.hdr.nmesgs, 1U);

  ninfo = {};
  ASSERT_GE(H5Oget_native_info_by_name(file.get(), "/grp", &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_GE(ninfo.hdr.nmesgs, 1U);

  ninfo = {};
  ASSERT_GE(H5Oget_native_info_by_idx(file.get(), "/", H5_INDEX_NAME,
                                      H5_ITER_INC, 0, &ninfo,
                                      H5O_NATIVE_INFO_ALL, H5P_DEFAULT),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Oget_native_info_by_name(file.get(), "/missing", &ninfo,
                                         H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Oget_native_info_by_idx(file.get(), "/", H5_INDEX_NAME,
                                        H5_ITER_INC, 99, &ninfo,
                                        H5O_NATIVE_INFO_ALL, H5P_DEFAULT),
              0);
  }
}

TEST(H5VLnativeObjectTest, DirectCallbacksExerciseUnsupportedAndErrorBranches) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  void* file_obj = H5VL_object(file.get());
  ASSERT_NE(file_obj, nullptr);

  ScopedErrorSilence silence;

  H5VL_loc_params_t bad_loc{};
  bad_loc.type = H5VL_OBJECT_BY_SELF;
  bad_loc.obj_type = H5I_DATASPACE;

  H5VL_loc_params_t self_loc{};
  self_loc.type = H5VL_OBJECT_BY_SELF;
  self_loc.obj_type = H5I_FILE;

  H5VL_loc_params_t name_loc{};
  name_loc.type = H5VL_OBJECT_BY_NAME;
  name_loc.obj_type = H5I_FILE;
  name_loc.loc_data.loc_by_name.name = "/g";
  name_loc.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;

  H5VL_loc_params_t idx_loc{};
  idx_loc.type = H5VL_OBJECT_BY_IDX;
  idx_loc.obj_type = H5I_FILE;

  H5O_token_t dummy_token{};
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), 99999999ULL, &dummy_token), 0);
  H5VL_loc_params_t token_loc{};
  token_loc.type = H5VL_OBJECT_BY_TOKEN;
  token_loc.obj_type = H5I_FILE;
  token_loc.loc_data.loc_by_token.token = &dummy_token;

  ScopedApiContext cx;

  // object_open: bad obj_type and unsupported H5VL_OBJECT_BY_SELF.
  H5I_type_t opened_type = H5I_BADID;
  EXPECT_EQ(H5VL__native_object_open(file_obj, &bad_loc, &opened_type,
                                     H5P_DEFAULT, nullptr),
            nullptr);
  EXPECT_EQ(H5VL__native_object_open(file_obj, &self_loc, &opened_type,
                                     H5P_DEFAULT, nullptr),
            nullptr);

  // object_copy: bad obj_type on src and dst.
  EXPECT_LT(H5VL__native_object_copy(file_obj, &bad_loc, "/g", file_obj,
                                     &name_loc, "/g2", H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL__native_object_copy(file_obj, &name_loc, "/g", file_obj,
                                     &bad_loc, "/g2", H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT, nullptr),
            0);

  // object_get error branches.
  H5VL_object_get_args_t get_args{};
  void* got_file_ptr = nullptr;
  get_args.op_type = H5VL_OBJECT_GET_FILE;
  get_args.args.get_file.file = &got_file_ptr;
  EXPECT_LT(
      H5VL__native_object_get(file_obj, &bad_loc, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VL__native_object_get(file_obj, &name_loc, &get_args, H5P_DEFAULT,
                                    nullptr),
            0);

  char nbuf[32]{};
  size_t nlen = 0;
  get_args.op_type = H5VL_OBJECT_GET_NAME;
  get_args.args.get_name.buf = nbuf;
  get_args.args.get_name.buf_size = sizeof(nbuf);
  get_args.args.get_name.name_len = &nlen;
  EXPECT_LT(
      H5VL__native_object_get(file_obj, &idx_loc, &get_args, H5P_DEFAULT, nullptr),
      0);
  EXPECT_LT(H5VL__native_object_get(file_obj, &token_loc, &get_args,
                                    H5P_DEFAULT, nullptr),
            0);

  H5O_type_t otype = H5O_TYPE_UNKNOWN;
  get_args.op_type = H5VL_OBJECT_GET_TYPE;
  get_args.args.get_type.obj_type = &otype;
  EXPECT_LT(
      H5VL__native_object_get(file_obj, &self_loc, &get_args, H5P_DEFAULT, nullptr),
      0);

  H5O_info2_t oinfo{};
  get_args.op_type = H5VL_OBJECT_GET_INFO;
  get_args.args.get_info.fields = H5O_INFO_BASIC;
  get_args.args.get_info.oinfo = &oinfo;
  EXPECT_LT(H5VL__native_object_get(file_obj, &token_loc, &get_args,
                                    H5P_DEFAULT, nullptr),
            0);

  // object_specific error branches.
  H5VL_object_specific_args_t spec_args{};
  bool exists = false;
  spec_args.op_type = H5VL_OBJECT_EXISTS;
  spec_args.args.exists.exists = &exists;
  EXPECT_LT(H5VL__native_object_specific(file_obj, &bad_loc, &spec_args,
                                         H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL__native_object_specific(file_obj, &idx_loc, &spec_args,
                                         H5P_DEFAULT, nullptr),
            0);

  H5O_token_t lookup_tok{};
  spec_args.op_type = H5VL_OBJECT_LOOKUP;
  spec_args.args.lookup.token_ptr = &lookup_tok;
  EXPECT_LT(H5VL__native_object_specific(file_obj, &idx_loc, &spec_args,
                                         H5P_DEFAULT, nullptr),
            0);

  spec_args.op_type = H5VL_OBJECT_VISIT;
  spec_args.args.visit.idx_type = H5_INDEX_NAME;
  spec_args.args.visit.order = H5_ITER_INC;
  spec_args.args.visit.fields = H5O_INFO_BASIC;
  spec_args.args.visit.op = CollectObjectsCb;
  EXPECT_LT(H5VL__native_object_specific(file_obj, &idx_loc, &spec_args,
                                         H5P_DEFAULT, nullptr),
            0);

  // object_optional error branches.
  H5VL_native_object_optional_args_t opt_data{};
  H5VL_optional_args_t opt_args{};
  size_t clen = 0;
  opt_data.get_comment.buf = nbuf;
  opt_data.get_comment.buf_size = sizeof(nbuf);
  opt_data.get_comment.comment_len = &clen;
  opt_args.op_type = H5VL_NATIVE_OBJECT_GET_COMMENT;
  opt_args.args = &opt_data;
  EXPECT_LT(H5VL__native_object_optional(file_obj, &bad_loc, &opt_args,
                                         H5P_DEFAULT, nullptr),
            0);
  EXPECT_LT(H5VL__native_object_optional(file_obj, &idx_loc, &opt_args,
                                         H5P_DEFAULT, nullptr),
            0);

  opt_args.op_type = H5VL_NATIVE_OBJECT_SET_COMMENT;
  opt_data.set_comment.comment = "ignored";
  EXPECT_LT(H5VL__native_object_optional(file_obj, &idx_loc, &opt_args,
                                         H5P_DEFAULT, nullptr),
            0);

  H5O_native_info_t ninfo{};
  opt_args.op_type = H5VL_NATIVE_OBJECT_GET_NATIVE_INFO;
  opt_data.get_native_info.fields = H5O_NATIVE_INFO_ALL;
  opt_data.get_native_info.ninfo = &ninfo;
  EXPECT_LT(H5VL__native_object_optional(file_obj, &token_loc, &opt_args,
                                         H5P_DEFAULT, nullptr),
            0);

  opt_args.op_type = 999;
  EXPECT_LT(H5VL__native_object_optional(file_obj, &self_loc, &opt_args,
                                         H5P_DEFAULT, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
