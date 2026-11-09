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
// Tests group hierarchy path utilities, name caching and address lookup,
// and open-ID path updates on move, delete, mount, and unmount
// (third_party/hdf5/src/H5Gname.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gname.c

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from internal
// cache/header structs pulled in through `H5private.h` and `*pkg.h`.
#ifndef NDEBUG
#define H5GNAME_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Gpkg.h"
#pragma pop_macro("try")
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5RSprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GNAME_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GNAME_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

// RAII guard for an HDF5 internal API context and optional VOL wrapper.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t vol_id = H5I_INVALID_HID) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (vol_id != H5I_INVALID_HID) {
        H5VL_object_t* vol_obj = H5VL_vol_object(vol_id);
        if (vol_obj != nullptr && H5VL_set_vol_wrapper(vol_obj) >= 0) {
          vol_wrapped_ = true;
        }
      }
    }
  }

  ~ScopedApiContext() {
    if (vol_wrapped_) {
      H5VL_reset_vol_wrapper();
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
  bool vol_wrapped_ = false;
};

std::string GetIdName(hid_t id) {
  const ssize_t len = H5Iget_name(id, nullptr, 0);
  if (len <= 0) {
    return "";
  }
  std::string out(static_cast<size_t>(len) + 1, '\0');
  const ssize_t rc = H5Iget_name(id, out.data(), out.size());
  if (rc <= 0) {
    return "";
  }
  out.resize(static_cast<size_t>(len));
  return out;
}

TEST(H5GnameTest, ComponentNormalizeAndBuildFullpath) {
  // 1. H5G__component skips leading slashes and measures the first component.
  size_t comp_len = 999;
  const char* comp = H5G__component("///alpha/beta", &comp_len);
  EXPECT_EQ(comp_len, 5U);
  EXPECT_STREQ(comp, "alpha/beta");

  comp = H5G__component("plain", &comp_len);
  EXPECT_EQ(comp_len, 5U);
  EXPECT_STREQ(comp, "plain");

  comp = H5G__component("///", &comp_len);
  EXPECT_EQ(comp_len, 0U);
  EXPECT_STREQ(comp, "");

  EXPECT_STREQ(H5G__component("//tail", nullptr), "tail");

  // 2. H5G_normalize collapses duplicate slashes and strips trailing slashes
  // except on "/".
  struct NormCase {
    const char* input;
    std::string_view expected;
  };
  constexpr NormCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "/", .expected = "/"},
      {.input = "///", .expected = "/"},
      {.input = "/a//b///c/", .expected = "/a/b/c"},
      {.input = "rel/path/", .expected = "rel/path"},
      {.input = "single", .expected = "single"},
  };
  for (const auto& tc : kCases) {
    char* norm = H5G_normalize(tc.input);
    ASSERT_NE(norm, nullptr);
    absl::Cleanup free_norm = [norm] { H5MM_xfree(norm); };
    EXPECT_EQ(std::string_view(norm), tc.expected);
  }

  // 3. H5G_build_fullpath_refstr_str inserts '/' only when prefix lacks one.
  H5RS_str_t* root_r = H5RS_create("/");
  ASSERT_NE(root_r, nullptr);
  absl::Cleanup decr_root = [root_r] { H5RS_decr(root_r); };

  H5RS_str_t* child_from_root = H5G_build_fullpath_refstr_str(root_r, "grp");
  ASSERT_NE(child_from_root, nullptr);
  absl::Cleanup decr_c1 = [child_from_root] { H5RS_decr(child_from_root); };
  EXPECT_STREQ(H5RS_get_str(child_from_root), "/grp");

  H5RS_str_t* child_nested =
      H5G_build_fullpath_refstr_str(child_from_root, "leaf");
  ASSERT_NE(child_nested, nullptr);
  absl::Cleanup decr_c2 = [child_nested] { H5RS_decr(child_nested); };
  EXPECT_STREQ(H5RS_get_str(child_nested), "/grp/leaf");
}

TEST(H5GnameTest, NameInitSetCopyResetAndFreeManagePaths) {
  H5G_name_t src{};
  H5G_name_reset(&src);
  ASSERT_GE(H5G__name_init(&src, "/base"), 0);
  absl::Cleanup free_src = [&src] { H5G_name_free(&src); };
  EXPECT_STREQ(H5RS_get_str(src.full_path_r), "/base");
  EXPECT_STREQ(H5RS_get_str(src.user_path_r), "/base");
  EXPECT_EQ(src.obj_hidden, 0U);

  // Deep copy duplicates ref-counted strings; shallow copy transfers them.
  H5G_name_t deep{};
  H5G_name_reset(&deep);
  absl::Cleanup free_deep = [&deep] { H5G_name_free(&deep); };
  ASSERT_GE(H5G_name_copy(&deep, &src, H5_COPY_DEEP), 0);
  EXPECT_STREQ(H5RS_get_str(deep.full_path_r), "/base");

  H5G_name_t shallow{};
  H5G_name_reset(&shallow);
  absl::Cleanup free_shallow = [&shallow] { H5G_name_free(&shallow); };
  ASSERT_GE(H5G_name_copy(&shallow, &deep, H5_COPY_SHALLOW), 0);
  EXPECT_EQ(deep.full_path_r, nullptr);
  EXPECT_EQ(deep.user_path_r, nullptr);
  EXPECT_STREQ(H5RS_get_str(shallow.full_path_r), "/base");

  // H5G_name_set replaces existing paths on dst and handles missing user/full.
  H5G_name_t child{};
  H5G_name_reset(&child);
  absl::Cleanup free_child = [&child] { H5G_name_free(&child); };
  ASSERT_GE(H5G_name_set(&shallow, &child, "sub"), 0);
  EXPECT_STREQ(H5RS_get_str(child.full_path_r), "/base/sub");
  EXPECT_STREQ(H5RS_get_str(child.user_path_r), "/base/sub");

  H5G_name_t full_only{};
  H5G_name_reset(&full_only);
  full_only.full_path_r = H5RS_create("/only_full");
  absl::Cleanup free_full_only = [&full_only] { H5G_name_free(&full_only); };
  ASSERT_GE(H5G_name_set(&full_only, &child, "next"), 0);
  EXPECT_STREQ(H5RS_get_str(child.full_path_r), "/only_full/next");
  EXPECT_EQ(child.user_path_r, nullptr);

  H5G_name_t empty_loc{};
  H5G_name_reset(&empty_loc);
  ASSERT_GE(H5G_name_set(&empty_loc, &child, "ignored"), 0);
  EXPECT_EQ(child.full_path_r, nullptr);
  EXPECT_EQ(child.user_path_r, nullptr);
}

TEST(H5GnameTest, GetNameCoversCachedUncachedRootAndUnlinkedObjects) {
  ScopedFile file = CreateInMemoryFile("gname_get.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup g1(
      H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(
      H5Gcreate2(file.get(), "/g1/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ASSERT_TRUE(g2.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/g1/g2/dataset_long",
                                H5T_NATIVE_INT32, space.get(), H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Lcreate_soft("/g1/g2/dataset_long", file.get(), "/g1/soft_link",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 1. Cached path lookup, truncation, and internal cached flag.
  char short_buf[6] = {};
  EXPECT_EQ(H5Iget_name(dset.get(), short_buf, sizeof(short_buf)),
            static_cast<ssize_t>(std::strlen("/g1/g2/dataset_long")));
  EXPECT_STREQ(short_buf, "/g1/g");

  H5G_loc_t dset_loc{};
  ASSERT_GE(H5G_loc(dset.get(), &dset_loc), 0);
  bool cached = false;
  size_t name_len = 0;
  char full_buf[32] = {};
  {
    ScopedApiContext ctx(dset.get());
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5G_get_name(&dset_loc, full_buf, sizeof(full_buf), &name_len,
                           &cached),
              0);
    EXPECT_TRUE(cached);
    EXPECT_STREQ(full_buf, "/g1/g2/dataset_long");
    EXPECT_EQ(name_len, std::strlen("/g1/g2/dataset_long"));
  }

  // 2. Token-opened object has no cached user_path_r -> traverses via
  // H5G_get_name_by_addr (including root group and soft-link skip).
  H5O_info2_t root_info{};
  ASSERT_GE(H5Oget_info_by_name3(file.get(), "/", &root_info, H5O_INFO_BASIC,
                                 H5P_DEFAULT),
            0);
  ScopedObject root_by_tok(H5Oopen_by_token(file.get(), root_info.token));
  ASSERT_TRUE(root_by_tok.valid());
  EXPECT_EQ(GetIdName(root_by_tok.get()), "/");

  H5O_info2_t dset_info{};
  ASSERT_GE(H5Oget_info(dset.get(), &dset_info, H5O_INFO_BASIC), 0);
  ScopedObject dset_by_tok(H5Oopen_by_token(file.get(), dset_info.token));
  ASSERT_TRUE(dset_by_tok.valid());
  EXPECT_EQ(GetIdName(dset_by_tok.get()), "/g1/g2/dataset_long");

  // Truncated buffer >= 2 bytes on uncached H5G_get_name_by_addr.
  char tok_trunc[5] = {};
  EXPECT_EQ(H5Iget_name(dset_by_tok.get(), tok_trunc, sizeof(tok_trunc)),
            static_cast<ssize_t>(std::strlen("/g1/g2/dataset_long")));
  EXPECT_STREQ(tok_trunc, "/g1/");

  H5G_loc_t tok_loc{};
  ASSERT_GE(H5G_loc(dset_by_tok.get(), &tok_loc), 0);
  {
    ScopedApiContext ctx(dset_by_tok.get());
    ASSERT_TRUE(ctx.valid());
    cached = true;
    ASSERT_GE(H5G_get_name(&tok_loc, nullptr, 0, &name_len, &cached), 0);
    EXPECT_FALSE(cached);
    EXPECT_EQ(name_len, std::strlen("/g1/g2/dataset_long"));
  }

  // 3. Unlinking the open dataset clears its cached paths; H5G_get_name_by_addr
  // then finds no link and reports length 0.
  ASSERT_GE(H5Ldelete(file.get(), "/g1/g2/dataset_long", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Iget_name(dset.get(), full_buf, sizeof(full_buf)), 0);
}

TEST(H5GnameTest, MoveUpdatesOpenGroupsDatasetsTypesAndRelativePaths) {
  ScopedFile file = CreateInMemoryFile("gname_move_main.h5");
  ScopedFile other_file = CreateInMemoryFile("gname_move_other.h5");
  ASSERT_TRUE(file.valid());
  ASSERT_TRUE(other_file.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());

  // Other-file dataset remains open to cover !H5F_SAME_SHARED in callback.
  ScopedDataset other_ds(H5Dcreate2(other_file.get(), "/other_ds",
                                    H5T_NATIVE_INT32, space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(other_ds.valid());

  // Transient datatype remains open to cover !H5T_is_named in callback.
  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient_dt.valid());

  ScopedGroup src_grp(
      H5Gcreate2(file.get(), "/src", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup sub_grp(H5Gcreate2(file.get(), "/src/sub", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset sub_ds(H5Dcreate2(file.get(), "/src/sub/ds", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(src_grp.valid());
  ASSERT_TRUE(sub_grp.valid());
  ASSERT_TRUE(sub_ds.valid());
  ASSERT_TRUE(named_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/src/sub/dt", named_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Token-opened object (full_path_r == NULL) exercises early return in cb.
  H5O_info2_t ds_info{};
  ASSERT_GE(H5Oget_info(sub_ds.get(), &ds_info, H5O_INFO_BASIC), 0);
  ScopedObject tok_ds(H5Oopen_by_token(file.get(), ds_info.token));
  ASSERT_TRUE(tok_ds.valid());

  // Object opened through a short soft link triggers full_suffix_len >= path_len
  // in H5G__name_move_path when the underlying long group moves.
  ASSERT_GE(H5Lcreate_soft("/src/sub", file.get(), "/s", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ScopedGroup via_soft(H5Gopen2(file.get(), "/s", H5P_DEFAULT));
  ASSERT_TRUE(via_soft.valid());

  // Open an object under a sibling group whose component length differs and
  // whose prefix matches in length but differs in characters ("/src2", "/srr").
  ScopedGroup sibling_diff_len(H5Gcreate2(file.get(), "/src2", H5P_DEFAULT,
                                          H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup sibling_same_len(H5Gcreate2(file.get(), "/srr", H5P_DEFAULT,
                                          H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sibling_diff_len.valid());
  ASSERT_TRUE(sibling_same_len.valid());

  // 1. Rename group "/src" -> "/dst_group": updates open H5I_GROUP IDs
  // (src_grp, sub_grp) while H5G_name_replace leaves H5I_DATASET and
  // H5I_DATATYPE IDs (sub_ds, named_dt) unchanged because the moved hard link's
  // target is a group. Siblings, transient_dt, and other_ds stay untouched.
  ASSERT_GE(H5Lmove(file.get(), "/src", file.get(), "/dst_group", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_EQ(GetIdName(src_grp.get()), "/dst_group");
  EXPECT_EQ(GetIdName(sub_grp.get()), "/dst_group/sub");
  EXPECT_EQ(GetIdName(sub_ds.get()), "/src/sub/ds");
  EXPECT_EQ(GetIdName(named_dt.get()), "/src/sub/dt");
  EXPECT_EQ(GetIdName(sibling_diff_len.get()), "/src2");
  EXPECT_EQ(GetIdName(sibling_same_len.get()), "/srr");
  EXPECT_EQ(GetIdName(other_ds.get()), "/other_ds");

  // 2. Rename dataset and named datatype directly to exercise H5I_DATASET and
  // H5I_DATATYPE (plus non-named datatype skip) in H5G__name_replace_cb with
  // full_suffix_len == 0.
  ScopedDataset dst_ds(
      H5Dopen2(file.get(), "/dst_group/sub/ds", H5P_DEFAULT));
  ScopedType dst_dt(H5Topen2(file.get(), "/dst_group/sub/dt", H5P_DEFAULT));
  ASSERT_TRUE(dst_ds.valid());
  ASSERT_TRUE(dst_dt.valid());
  ASSERT_GE(H5Lmove(file.get(), "/dst_group/sub/ds", file.get(),
                    "/dst_group/sub/ds2", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetIdName(dst_ds.get()), "/dst_group/sub/ds2");
  ASSERT_GE(H5Lmove(file.get(), "/dst_group/sub/dt", file.get(),
                    "/dst_group/sub/dt2", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetIdName(dst_dt.get()), "/dst_group/sub/dt2");
}

TEST(H5GnameTest, MountAndUnmountHideParentPathsAndUpdateChildPaths) {
  ScopedFile gp = CreateInMemoryFile("gname_gp.h5");
  ScopedFile parent = CreateInMemoryFile("gname_parent.h5");
  ScopedFile child = CreateInMemoryFile("gname_child.h5");
  ASSERT_TRUE(gp.valid());
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());

  // Grandparent groups/datasets: /mnt_p/hidden_sub/hidden_ds and /plain.
  ScopedGroup gp_mnt(
      H5Gcreate2(gp.get(), "/mnt_p", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup gp_hidden_sub(H5Gcreate2(gp.get(), "/mnt_p/hidden_sub",
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset gp_hidden_ds(H5Dcreate2(gp.get(), "/mnt_p/hidden_sub/hidden_ds",
                                        H5T_NATIVE_INT32, space.get(),
                                        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup gp_plain(
      H5Gcreate2(gp.get(), "/plain", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(gp_mnt.valid());
  ASSERT_TRUE(gp_hidden_sub.valid());
  ASSERT_TRUE(gp_hidden_ds.valid());
  ASSERT_TRUE(gp_plain.valid());

  // Parent groups/datasets: /mnt_c/p_hidden and /p_other.
  ScopedGroup p_mnt(
      H5Gcreate2(parent.get(), "/mnt_c", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset p_hidden(H5Dcreate2(parent.get(), "/mnt_c/p_hidden",
                                    H5T_NATIVE_INT32, space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset p_other(H5Dcreate2(parent.get(), "/p_other", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(p_mnt.valid());
  ASSERT_TRUE(p_hidden.valid());
  ASSERT_TRUE(p_other.valid());

  // Child dataset and nested subgroup: /c_before (opened pre-mount) and
  // /c_dir/c_deep.
  ScopedDataset c_before(H5Dcreate2(child.get(), "/c_before", H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ScopedGroup c_dir(
      H5Gcreate2(child.get(), "/c_dir", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup c_deep(H5Gcreate2(child.get(), "/c_dir/c_deep", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(c_before.valid());
  ASSERT_TRUE(c_dir.valid());
  ASSERT_TRUE(c_deep.valid());
  c_deep.reset();
  c_dir.reset();

  // 1. Mount parent on gp:/mnt_p; gp_hidden_ds is hidden, p_other updates.
  ASSERT_GE(H5Fmount(gp.get(), "/mnt_p", parent.get(), H5P_DEFAULT), 0);
  EXPECT_EQ(H5Iget_name(gp_hidden_ds.get(), nullptr, 0), 0);
  EXPECT_EQ(GetIdName(gp_mnt.get()), "/mnt_p");
  EXPECT_EQ(GetIdName(gp_plain.get()), "/plain");
  EXPECT_EQ(GetIdName(p_other.get()), "/p_other");

  // 2. Mount child on gp:/mnt_p/mnt_c (2-level mount chain). H5G_NAME_MOUNT
  // updates c_before's canonical full_path_r while preserving user_path_r.
  ASSERT_GE(H5Fmount(gp.get(), "/mnt_p/mnt_c", child.get(), H5P_DEFAULT), 0);
  EXPECT_EQ(H5Iget_name(p_hidden.get(), nullptr, 0), 0);
  EXPECT_EQ(GetIdName(c_before.get()), "/c_before");
  {
    H5G_loc_t c_loc{};
    ASSERT_GE(H5G_loc(c_before.get(), &c_loc), 0);
    ASSERT_NE(c_loc.path->full_path_r, nullptr);
    EXPECT_STREQ(H5RS_get_str(c_loc.path->full_path_r),
                 "/mnt_p/mnt_c/c_before");
  }

  // Open subgroup c_deep through grandparent so user_path_r includes mount
  // prefix, then move /mnt_p/mnt_c/c_dir -> /mnt_p/mnt_c/c_dir2
  // (path_prefix2_len > 0).
  ScopedGroup c_deep_via_gp(
      H5Gopen2(gp.get(), "/mnt_p/mnt_c/c_dir/c_deep", H5P_DEFAULT));
  ASSERT_TRUE(c_deep_via_gp.valid());
  ASSERT_GE(H5Lmove(gp.get(), "/mnt_p/mnt_c/c_dir", gp.get(),
                    "/mnt_p/mnt_c/c_dir2", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetIdName(c_deep_via_gp.get()), "/mnt_p/mnt_c/c_dir2/c_deep");

  // Trigger another H5Ldelete on gp to hit same-file/mount traversal checks.
  ScopedDataset gp_temp(H5Dcreate2(gp.get(), "/plain/temp", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(gp_temp.valid());
  ASSERT_GE(H5Ldelete(gp.get(), "/plain/temp", H5P_DEFAULT), 0);

  // 3. Unmount child: c_deep_via_gp user_path_r was longer than unmounted
  // full_path_r so user_path_r is cleared; c_before retains its user_path_r;
  // p_hidden is un-hidden.
  ASSERT_GE(H5Funmount(gp.get(), "/mnt_p/mnt_c"), 0);
  EXPECT_EQ(GetIdName(c_deep_via_gp.get()), "/c_dir2/c_deep");
  EXPECT_EQ(GetIdName(c_before.get()), "/c_before");
  EXPECT_EQ(GetIdName(p_hidden.get()), "/mnt_c/p_hidden");

  // 4. Unmount parent: gp_hidden_ds is un-hidden and restores its name.
  ASSERT_GE(H5Funmount(gp.get(), "/mnt_p"), 0);
  EXPECT_EQ(GetIdName(gp_hidden_ds.get()), "/mnt_p/hidden_sub/hidden_ds");

  // 5. Mount child onto parent BEFORE mounting parent onto gp, then mount
  // parent onto gp: exercises intermediate mount chain walk in
  // H5G__name_replace_cb (obj_in_child = true via H5F_PARENT loop) and
  // cross-file rejection in H5G__get_name_by_addr_cb for shadowed /mnt_c.
  H5O_info2_t pmnt_info{};
  ASSERT_GE(H5Oget_info(p_mnt.get(), &pmnt_info, H5O_INFO_BASIC), 0);
  ScopedObject tok_pmnt(H5Oopen_by_token(parent.get(), pmnt_info.token));
  ASSERT_TRUE(tok_pmnt.valid());

  ASSERT_GE(H5Fmount(parent.get(), "/mnt_c", child.get(), H5P_DEFAULT), 0);
  ASSERT_GE(H5Fmount(gp.get(), "/mnt_p", parent.get(), H5P_DEFAULT), 0);
  EXPECT_EQ(H5Iget_name(tok_pmnt.get(), nullptr, 0), 0);
  {
    H5G_loc_t c_loc{};
    ASSERT_GE(H5G_loc(c_before.get(), &c_loc), 0);
    ASSERT_NE(c_loc.path->full_path_r, nullptr);
    EXPECT_STREQ(H5RS_get_str(c_loc.path->full_path_r),
                 "/mnt_p/mnt_c/c_before");
  }
  ASSERT_GE(H5Funmount(gp.get(), "/mnt_p"), 0);
  ASSERT_GE(H5Funmount(parent.get(), "/mnt_c"), 0);
}

TEST(H5GnameTest, NameReplaceHandlesSoftExternalAnonAndInvalidArgs) {
  ScopedFile file = CreateInMemoryFile("gname_replace_edge.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/target", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // 1. Deleting soft and external links exercises their link-type switch arms.
  ASSERT_GE(H5Lcreate_soft("/target", file.get(), "/soft_del", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(file.get(), "/soft_del", H5P_DEFAULT), 0);

  ASSERT_GE(H5Lcreate_external("ext_file.h5", "/ext_obj", file.get(),
                               "/ext_del", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(file.get(), "/ext_del", H5P_DEFAULT), 0);

  // 2. Deleting inside an unlinked group passes NULL src_full_path_r.
  ScopedGroup unlinked_grp(H5Gcreate2(file.get(), "/temp_grp", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(unlinked_grp.valid());
  ASSERT_GE(H5Lcreate_hard(file.get(), "/target", unlinked_grp.get(), "child",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(file.get(), "/temp_grp", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(unlinked_grp.get(), "child", H5P_DEFAULT), 0);

  // 3. Direct H5G_name_replace error branches for unknown link type and op.
  auto* h5f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(h5f, nullptr);
  H5RS_str_t* path_r = H5RS_create("/target");
  ASSERT_NE(path_r, nullptr);
  absl::Cleanup decr_path = [path_r] { H5RS_decr(path_r); };

  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());

    H5O_link_t bad_lnk{};
    bad_lnk.type = H5L_TYPE_ERROR;
    EXPECT_LT(H5G_name_replace(&bad_lnk, H5G_NAME_DELETE, h5f, path_r, nullptr,
                               nullptr),
              0);

    EXPECT_LT(H5G_name_replace(nullptr, static_cast<H5G_names_op_t>(99), h5f,
                               path_r, nullptr, nullptr),
              0);
  }
}

}  // namespace
}  // namespace hdf5
