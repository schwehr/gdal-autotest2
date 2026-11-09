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
// Tests deprecated HDF5 H5G group APIs (H5Gcreate1, H5Gopen1, H5Glink,
// H5Glink2, H5Gmove, H5Gmove2, H5Gunlink, H5Gget_linkval, H5Gset_comment,
// H5Gget_comment, H5Giterate, H5Gget_num_objs, H5Gget_objinfo,
// H5Gget_objname_by_idx, H5Gget_objtype_by_idx) and H5G_map_obj_type
// (third_party/hdf5/src/H5Gdeprec.c).
//
// Google3 builds :hdf5 with deprecated symbols enabled
// (H5_NO_DEPRECATED_SYMBOLS undefined) and without parallel MPI.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gdeprec.c

#ifndef NDEBUG
#define H5GDEPREC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Ldevelop.h"
}  // extern "C"
#ifdef H5GDEPREC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GDEPREC_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5L_type_t kTestUdLinkId = static_cast<H5L_type_t>(H5L_TYPE_UD_MIN + 11);

hid_t TestUdTraverse(const char* /*link_name*/, hid_t /*cur_group*/,
                     const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                     hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

ssize_t TestUdQuery(const char* /*link_name*/, const void* /*lnkdata*/,
                    size_t lnkdata_size, void* buf, size_t buf_size) {
  if (buf != nullptr && buf_size > 0) {
    static_cast<char*>(buf)[0] = '\0';
  }
  return static_cast<ssize_t>(lnkdata_size);
}

TEST(H5GdeprecTest, MapObjTypeCoversAllEnumValues) {
  struct Case {
    H5O_type_t input;
    H5G_obj_t expected;
  };
  constexpr auto kCases = std::to_array<Case>({
      {.input = H5O_TYPE_GROUP, .expected = H5G_GROUP},
      {.input = H5O_TYPE_DATASET, .expected = H5G_DATASET},
      {.input = H5O_TYPE_NAMED_DATATYPE, .expected = H5G_TYPE},
      {.input = H5O_TYPE_MAP, .expected = H5G_UNKNOWN},
      {.input = H5O_TYPE_UNKNOWN, .expected = H5G_UNKNOWN},
      {.input = H5O_TYPE_NTYPES, .expected = H5G_UNKNOWN},
      {.input = static_cast<H5O_type_t>(5), .expected = H5G_UNKNOWN},
  });

  for (const Case& c : kCases) {
    EXPECT_EQ(H5G_map_obj_type(c.input), c.expected);
  }
}

TEST(H5GdeprecTest, Create1AndOpen1SuccessAndSizeHint) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // size_hint == 0 uses default GCPL.
  ScopedGroup g_default(H5Gcreate1(file.get(), "g_default", 0));
  ASSERT_TRUE(g_default.valid());

  // size_hint > 0 copies the default GCPL and stores lheap_size_hint.
  constexpr size_t kHint = 512;
  ScopedGroup g_hint(H5Gcreate1(file.get(), "g_hint", kHint));
  ASSERT_TRUE(g_hint.valid());
  ScopedPlist gcpl(H5Gget_create_plist(g_hint.get()));
  ASSERT_TRUE(gcpl.valid());
  size_t actual_hint = 0;
  ASSERT_GE(H5Pget_local_heap_size_hint(gcpl.get(), &actual_hint), 0);
  EXPECT_EQ(actual_hint, kHint);

  // Nested group via H5Gcreate1 and reopen via H5Gopen1.
  ScopedGroup sub(H5Gcreate1(g_hint.get(), "sub", 0));
  ASSERT_TRUE(sub.valid());
  sub.reset();
  g_hint.reset();

  ScopedGroup reopened(H5Gopen1(file.get(), "/g_hint/sub"));
  EXPECT_TRUE(reopened.valid());
}

TEST(H5GdeprecTest, Create1AndOpen1RejectInvalidArguments) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup existing(H5Gcreate1(file.get(), "existing", 0));
  ASSERT_TRUE(existing.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Gcreate1(file.get(), nullptr, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate1(file.get(), "", 0), H5I_INVALID_HID);
  EXPECT_EQ(
      H5Gcreate1(file.get(), "huge", static_cast<size_t>(UINT32_MAX) + 1),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate1(H5I_INVALID_HID, "bad_id", 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate1(space.get(), "bad_loc", 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate1(file.get(), "existing", 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Gcreate1(file.get(), "missing_parent/child", 0), H5I_INVALID_HID);

  EXPECT_EQ(H5Gopen1(file.get(), nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen1(file.get(), ""), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen1(H5I_INVALID_HID, "existing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen1(file.get(), "missing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Gopen1(file.get(), "dset"), H5I_INVALID_HID);
}

TEST(H5GdeprecTest, LinkAndLink2HardSoftAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup g1(H5Gcreate1(file.get(), "g1", 0));
  ASSERT_TRUE(g1.valid());
  ScopedGroup g2(H5Gcreate1(file.get(), "g2", 0));
  ASSERT_TRUE(g2.valid());

  // 1. H5Glink hard & soft links.
  ASSERT_GE(H5Glink(file.get(), H5G_LINK_HARD, "g1", "g1_hard"), 0);
  ASSERT_GE(H5Glink(file.get(), H5G_LINK_SOFT, "/g1", "g1_soft"), 0);

  // 2. H5Glink2 hard link across groups, soft link across groups, and SAME_LOC.
  ASSERT_GE(H5Glink2(file.get(), "g1", H5G_LINK_HARD, g2.get(), "hard_to_g1"),
            0);
  ASSERT_GE(H5Glink2(H5I_INVALID_HID, "/g1", H5G_LINK_SOFT, g2.get(),
                     "soft_in_g2"),
            0);
  ASSERT_GE(
      H5Glink2(g2.get(), "/g2", H5G_LINK_SOFT, H5G_SAME_LOC, "soft_same_loc"),
      0);

  // 3. Error paths for H5Glink and H5Glink2.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedErrorSilence silence;

  EXPECT_LT(H5Glink(file.get(), H5G_LINK_HARD, nullptr, "x"), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_HARD, "", "x"), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_HARD, "g1", nullptr), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_HARD, "g1", ""), 0);
  EXPECT_LT(H5Glink(H5I_INVALID_HID, H5G_LINK_HARD, "g1", "x"), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_HARD, "missing", "x"), 0);
  EXPECT_LT(H5Glink(H5I_INVALID_HID, H5G_LINK_SOFT, "/g1", "x"), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_SOFT, "/g1", "g1_soft"), 0);
  EXPECT_LT(H5Glink(file.get(), H5G_LINK_ERROR, "g1", "x"), 0);
  EXPECT_LT(H5Glink(file.get(), H5L_TYPE_EXTERNAL, "g1", "x"), 0);

  EXPECT_LT(H5Glink2(file.get(), nullptr, H5G_LINK_HARD, g2.get(), "x"), 0);
  EXPECT_LT(H5Glink2(file.get(), "", H5G_LINK_HARD, g2.get(), "x"), 0);
  EXPECT_LT(H5Glink2(file.get(), "g1", H5G_LINK_HARD, g2.get(), nullptr), 0);
  EXPECT_LT(H5Glink2(file.get(), "g1", H5G_LINK_HARD, g2.get(), ""), 0);
  EXPECT_LT(
      H5Glink2(H5I_INVALID_HID, "g1", H5G_LINK_HARD, g2.get(), "x"), 0);
  EXPECT_LT(
      H5Glink2(file.get(), "g1", H5G_LINK_HARD, H5I_INVALID_HID, "x"), 0);
  EXPECT_LT(H5Glink2(file.get(), "g1", H5G_LINK_HARD, g2.get(), "hard_to_g1"),
            0);
  EXPECT_LT(
      H5Glink2(file.get(), "/g1", H5G_LINK_SOFT, H5I_INVALID_HID, "x"), 0);
  EXPECT_LT(H5Glink2(file.get(), "/g1", H5G_LINK_SOFT, g2.get(), "soft_in_g2"),
            0);
  EXPECT_LT(H5Glink2(file.get(), "g1", H5G_LINK_ERROR, g2.get(), "x"), 0);
  EXPECT_LT(H5Glink2(space.get(), "g1", H5G_LINK_SOFT, H5G_SAME_LOC, "x"), 0);
}

TEST(H5GdeprecTest, MoveMove2AndUnlink) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup src(H5Gcreate1(file.get(), "src", 0));
  ASSERT_TRUE(src.valid());
  ScopedGroup dst(H5Gcreate1(file.get(), "dst", 0));
  ASSERT_TRUE(dst.valid());
  ScopedGroup target(H5Gcreate1(src.get(), "item", 0));
  ASSERT_TRUE(target.valid());
  target.reset();

  // 1. H5Gmove within same group and H5Gmove2 with SAME_LOC and cross-group.
  ASSERT_GE(H5Gmove(src.get(), "item", "item_moved"), 0);
  ASSERT_GE(H5Gmove2(src.get(), "item_moved", H5G_SAME_LOC, "item_same_dst"),
            0);
  ASSERT_GE(H5Gmove2(H5G_SAME_LOC, "item_same_dst", src.get(), "item_same_src"),
            0);
  ASSERT_GE(H5Gmove2(src.get(), "item_same_src", dst.get(), "item_in_dst"), 0);
  EXPECT_GT(H5Lexists(dst.get(), "item_in_dst", H5P_DEFAULT), 0);

  // 2. H5Gunlink removes the link.
  ASSERT_GE(H5Gunlink(dst.get(), "item_in_dst"), 0);
  EXPECT_EQ(H5Lexists(dst.get(), "item_in_dst", H5P_DEFAULT), 0);

  // 3. Error paths for H5Gmove, H5Gmove2, and H5Gunlink.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedErrorSilence silence;

  EXPECT_LT(H5Gmove(H5I_INVALID_HID, "a", "b"), 0);
  EXPECT_LT(H5Gmove(src.get(), "missing", "b"), 0);

  EXPECT_LT(H5Gmove2(src.get(), nullptr, dst.get(), "b"), 0);
  EXPECT_LT(H5Gmove2(src.get(), "", dst.get(), "b"), 0);
  EXPECT_LT(H5Gmove2(src.get(), "a", dst.get(), nullptr), 0);
  EXPECT_LT(H5Gmove2(src.get(), "a", dst.get(), ""), 0);
  EXPECT_LT(H5Gmove2(H5G_SAME_LOC, "a", H5G_SAME_LOC, "b"), 0);
  EXPECT_LT(H5Gmove2(space.get(), "a", dst.get(), "b"), 0);
  EXPECT_LT(H5Gmove2(src.get(), "a", dset.get(), "b"), 0);
  EXPECT_LT(H5Gmove2(src.get(), "missing", dst.get(), "b"), 0);

  EXPECT_LT(H5Gunlink(dst.get(), nullptr), 0);
  EXPECT_LT(H5Gunlink(dst.get(), ""), 0);
  EXPECT_LT(H5Gunlink(H5I_INVALID_HID, "x"), 0);
  EXPECT_LT(H5Gunlink(dst.get(), "missing"), 0);
}

TEST(H5GdeprecTest, GetLinkvalAndComments) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate1(file.get(), "grp", 0));
  ASSERT_TRUE(grp.valid());

  constexpr char kTarget[] = "/grp";
  ASSERT_GE(H5Glink(file.get(), H5G_LINK_SOFT, kTarget, "slink"), 0);

  // H5Gget_linkval full read, truncated read, and size == 0 check.
  std::array<char, 16> linkbuf{};
  linkbuf.fill('X');
  ASSERT_GE(H5Gget_linkval(file.get(), "slink", linkbuf.size(), linkbuf.data()),
            0);
  EXPECT_STREQ(linkbuf.data(), kTarget);

  std::array<char, 3> trunc{};
  ASSERT_GE(H5Gget_linkval(file.get(), "slink", trunc.size(), trunc.data()), 0);
  EXPECT_EQ(H5Gget_linkval(file.get(), "slink", 0, nullptr), 0);

  // Comments: no comment, set, query bufsize, truncated read, overwrite, clear.
  std::array<char, 16> cbuf{};
  EXPECT_EQ(H5Gget_comment(file.get(), "grp", cbuf.size(), cbuf.data()), 0);

  constexpr char kComment[] = "group_note";
  ASSERT_GE(H5Gset_comment(file.get(), "grp", kComment), 0);
  EXPECT_EQ(H5Gget_comment(file.get(), "grp", 0, nullptr),
            static_cast<int>(sizeof(kComment) - 1));
  EXPECT_EQ(H5Gget_comment(file.get(), ".", 0, nullptr), 0);

  cbuf.fill('Z');
  EXPECT_EQ(H5Gget_comment(grp.get(), ".", cbuf.size(), cbuf.data()),
            static_cast<int>(sizeof(kComment) - 1));
  EXPECT_STREQ(cbuf.data(), kComment);

  std::array<char, 4> small_cbuf{};
  EXPECT_EQ(H5Gget_comment(file.get(), "grp", small_cbuf.size(),
                           small_cbuf.data()),
            static_cast<int>(sizeof(kComment) - 1));

  // Clearing comment via nullptr removes comment.
  ASSERT_GE(H5Gset_comment(file.get(), "grp", nullptr), 0);
  EXPECT_EQ(H5Gget_comment(file.get(), "grp", cbuf.size(), cbuf.data()), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Gget_linkval(file.get(), nullptr, linkbuf.size(), linkbuf.data()),
            0);
  EXPECT_LT(H5Gget_linkval(file.get(), "", linkbuf.size(), linkbuf.data()), 0);
  EXPECT_LT(H5Gget_linkval(H5I_INVALID_HID, "slink", linkbuf.size(),
                           linkbuf.data()),
            0);
  EXPECT_LT(H5Gget_linkval(file.get(), "grp", linkbuf.size(), linkbuf.data()),
            0);

  EXPECT_LT(H5Gset_comment(file.get(), nullptr, "c"), 0);
  EXPECT_LT(H5Gset_comment(file.get(), "", "c"), 0);
  EXPECT_LT(H5Gset_comment(H5I_INVALID_HID, "grp", "c"), 0);
  EXPECT_LT(H5Gset_comment(file.get(), "missing", "c"), 0);

  EXPECT_LT(H5Gget_comment(file.get(), nullptr, cbuf.size(), cbuf.data()), 0);
  EXPECT_LT(H5Gget_comment(file.get(), "", cbuf.size(), cbuf.data()), 0);
  EXPECT_LT(H5Gget_comment(file.get(), "grp", 4, nullptr), 0);
  EXPECT_LT(H5Gget_comment(H5I_INVALID_HID, "grp", cbuf.size(), cbuf.data()),
            0);
  EXPECT_LT(H5Gget_comment(file.get(), "missing", cbuf.size(), cbuf.data()), 0);
}

TEST(H5GdeprecTest, IterateNumObjsByIdxAndObjinfo) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "a_dtype", committed_dt.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedDataset dset(H5Dcreate2(file.get(), "b_dset", H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedGroup grp(H5Gcreate1(file.get(), "c_grp", 0));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(H5Glink(file.get(), H5G_LINK_SOFT, "/c_grp", "d_slink"), 0);
  ASSERT_GE(H5Glink(file.get(), H5G_LINK_SOFT, "/no_such", "e_dangling"), 0);

  // Register a temporary UD link class so H5G_UDLINK branch is covered.
  const H5L_class_t ud_cls = {
      H5L_LINK_CLASS_T_VERS,
      kTestUdLinkId,
      "test_ud_link",
      nullptr,
      nullptr,
      nullptr,
      TestUdTraverse,
      nullptr,
      TestUdQuery,
  };
  ASSERT_GE(H5Lregister(&ud_cls), 0);
  absl::Cleanup unregister_ud = [] { H5Lunregister(kTestUdLinkId); };
  constexpr char kUdPayload[] = "ud_payload";
  ASSERT_GE(H5Lcreate_ud(file.get(), "f_udlink", kTestUdLinkId, kUdPayload,
                         sizeof(kUdPayload), H5P_DEFAULT, H5P_DEFAULT),
            0);

  // H5Gget_num_objs on root and empty subgroup.
  hsize_t root_objs = 0;
  ASSERT_GE(H5Gget_num_objs(file.get(), &root_objs), 0);
  EXPECT_EQ(root_objs, 6u);
  hsize_t grp_objs = 99;
  ASSERT_GE(H5Gget_num_objs(grp.get(), &grp_objs), 0);
  EXPECT_EQ(grp_objs, 0u);

  // H5Giterate: idx_p == nullptr full pass.
  std::vector<std::string> visited;
  auto collect_all = [](hid_t /*g*/, const char* name, void* data) -> herr_t {
    static_cast<std::vector<std::string>*>(data)->emplace_back(name);
    return 0;
  };
  EXPECT_EQ(H5Giterate(file.get(), "/", nullptr, collect_all, &visited), 0);
  EXPECT_THAT(visited, ElementsAre("a_dtype", "b_dset", "c_grp", "d_slink",
                                   "e_dangling", "f_udlink"));

  // H5Giterate: early stop > 0 and resume via *idx_p.
  visited.clear();
  auto stop_after_two = [](hid_t /*g*/, const char* name,
                           void* data) -> herr_t {
    auto* out = static_cast<std::vector<std::string>*>(data);
    out->emplace_back(name);
    return out->size() == 2 ? 7 : 0;
  };
  int idx = 0;
  EXPECT_EQ(H5Giterate(file.get(), ".", &idx, stop_after_two, &visited), 7);
  EXPECT_THAT(visited, ElementsAre("a_dtype", "b_dset"));
  EXPECT_EQ(idx, 2);

  visited.clear();
  EXPECT_EQ(H5Giterate(file.get(), ".", &idx, collect_all, &visited), 0);
  EXPECT_THAT(visited, ElementsAre("c_grp", "d_slink", "e_dangling",
                                   "f_udlink"));

  // H5Giterate: callback returning < 0.
  auto fail_cb = [](hid_t /*g*/, const char* /*name*/, void* /*d*/) -> herr_t {
    return -1;
  };

  // H5Gget_objname_by_idx checks.
  EXPECT_EQ(H5Gget_objname_by_idx(file.get(), 1, nullptr, 0), 6);
  std::array<char, 8> untouched;
  untouched.fill('Q');
  EXPECT_EQ(H5Gget_objname_by_idx(file.get(), 1, untouched.data(), 0), 6);
  EXPECT_EQ(untouched[0], 'Q');

  std::array<char, 4> short_name{};
  EXPECT_EQ(H5Gget_objname_by_idx(file.get(), 1, short_name.data(),
                                  short_name.size()),
            6);
  std::array<char, 16> full_name{};
  EXPECT_EQ(H5Gget_objname_by_idx(file.get(), 1, full_name.data(),
                                  full_name.size()),
            6);
  EXPECT_STREQ(full_name.data(), "b_dset");

  // H5Gget_objtype_by_idx on committed datatype, dataset, group.
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 0), H5G_TYPE);
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 1), H5G_DATASET);
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 2), H5G_GROUP);

  // H5Gget_objinfo with statbuf == nullptr, groups, dsets, types, soft links,
  // trailing slashes/dots trimming, and UD links.
  EXPECT_GE(H5Gget_objinfo(file.get(), "c_grp", true, nullptr), 0);

  H5G_stat_t st{};
  ASSERT_GE(H5Gget_objinfo(file.get(), "a_dtype", true, &st), 0);
  EXPECT_EQ(st.type, H5G_TYPE);
  EXPECT_EQ(st.nlink, 1u);

  ASSERT_GE(H5Gget_objinfo(file.get(), "b_dset", false, &st), 0);
  EXPECT_EQ(st.type, H5G_DATASET);

  ASSERT_GE(H5Gget_objinfo(file.get(), "d_slink", true, &st), 0);
  EXPECT_EQ(st.type, H5G_GROUP);

  ASSERT_GE(H5Gget_objinfo(file.get(), "d_slink/.", false, &st), 0);
  EXPECT_EQ(st.type, H5G_LINK);
  EXPECT_EQ(st.linklen, sizeof("/c_grp"));

  // Name consisting solely of '.' and '/' results in name_len == 0 after trim.
  ASSERT_GE(H5Gget_objinfo(grp.get(), "./.", false, &st), 0);
  EXPECT_EQ(st.type, H5G_GROUP);

  ASSERT_GE(H5Gget_objinfo(file.get(), "f_udlink", false, &st), 0);
  EXPECT_EQ(st.type, H5G_UDLINK);
  EXPECT_EQ(st.linklen, sizeof(kUdPayload));

  // Negative checks.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Giterate(file.get(), ".", &idx, fail_cb, nullptr), 0);
  EXPECT_LT(H5Giterate(file.get(), nullptr, nullptr, collect_all, &visited), 0);
  EXPECT_LT(H5Giterate(file.get(), "", nullptr, collect_all, &visited), 0);
  int neg_idx = -1;
  EXPECT_LT(H5Giterate(file.get(), ".", &neg_idx, collect_all, &visited), 0);
  EXPECT_LT(H5Giterate(file.get(), ".", nullptr, nullptr, nullptr), 0);
  EXPECT_LT(H5Giterate(H5I_INVALID_HID, ".", nullptr, collect_all, &visited),
            0);
  EXPECT_LT(H5Giterate(file.get(), "missing", nullptr, collect_all, &visited),
            0);

  EXPECT_LT(H5Gget_num_objs(space.get(), &root_objs), 0);
  EXPECT_LT(H5Gget_num_objs(file.get(), nullptr), 0);

  EXPECT_LT(H5Gget_objinfo(file.get(), nullptr, false, &st), 0);
  EXPECT_LT(H5Gget_objinfo(file.get(), "", false, &st), 0);
  EXPECT_LT(H5Gget_objinfo(H5I_INVALID_HID, "c_grp", false, &st), 0);
  EXPECT_LT(H5Gget_objinfo(file.get(), "missing", false, &st), 0);
  EXPECT_LT(H5Gget_objinfo(file.get(), "e_dangling", true, &st), 0);

  EXPECT_LT(H5Gget_objname_by_idx(H5I_INVALID_HID, 0, full_name.data(),
                                  full_name.size()),
            0);
  EXPECT_LT(
      H5Gget_objname_by_idx(file.get(), 99, full_name.data(), full_name.size()),
      0);

  EXPECT_EQ(H5Gget_objtype_by_idx(H5I_INVALID_HID, 0), H5G_UNKNOWN);
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 99), H5G_UNKNOWN);
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 3), H5G_GROUP);
  EXPECT_EQ(H5Gget_objtype_by_idx(file.get(), 4), H5G_UNKNOWN);
}

}  // namespace
}  // namespace hdf5
