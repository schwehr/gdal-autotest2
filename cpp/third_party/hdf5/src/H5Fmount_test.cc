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
// Tests file mounting, unmounting, mount-table binary search & growth,
// hierarchy traversal, shared-file mount cleanup, object counting, and mount
// hierarchy flushing (third_party/hdf5/src/H5Fmount.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fmount.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FMOUNT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5F_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5FMOUNT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FMOUNT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FmountTest, MultiLevelMountTraverseFlushAndUnmount) {
  // 1. Create grandparent, parent, and child in-memory files with distinct
  // datasets so traversal across two mount hops reaches child data.
  ScopedFile grandparent = CreateInMemoryFile("mnt_grandparent.h5");
  ScopedFile parent = CreateInMemoryFile("mnt_parent.h5");
  ScopedFile child = CreateInMemoryFile("mnt_child.h5");
  ASSERT_TRUE(grandparent.valid());
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  ScopedGroup gp_mnt(
      H5Gcreate2(grandparent.get(), "/mnt_p", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup gp_plain(
      H5Gcreate2(grandparent.get(), "/plain", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup p_mnt(
      H5Gcreate2(parent.get(), "/mnt_c", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(gp_mnt.valid());
  ASSERT_TRUE(gp_plain.valid());
  ASSERT_TRUE(p_mnt.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  // 2. Mount leaf_root onto child's "." (exercises multi-iteration do..while in
  // H5F_traverse_mount), mount parent onto grandparent:/mnt_p, and child onto
  // /mnt_p/mnt_c (relative to parent, since parent is already mounted!).
  ScopedFile leaf_root = CreateInMemoryFile("mnt_leaf_root.h5");
  ASSERT_TRUE(leaf_root.valid());
  ScopedDataset ds(H5Dcreate2(leaf_root.get(), "/leaf_val", H5T_NATIVE_INT32,
                              space.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
  const int32_t expected = 4242;
  ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &expected),
            0);
  ds.reset();
  // Reset creation group handles cleanly before mounting; leaf_root mounts onto
  // child's "." before child mounts onto parent so H5F_traverse_mount runs >1
  // loop iteration.
  gp_mnt.reset();
  gp_plain.reset();
  p_mnt.reset();

  ASSERT_GE(H5Fmount(child.get(), ".", leaf_root.get(), H5P_DEFAULT), 0);

  auto* h5_child =
      static_cast<H5F_t*>(H5VL_object_verify(child.get(), H5I_FILE));
  auto* h5_parent =
      static_cast<H5F_t*>(H5VL_object_verify(parent.get(), H5I_FILE));
  auto* h5_leaf =
      static_cast<H5F_t*>(H5VL_object_verify(leaf_root.get(), H5I_FILE));
  ASSERT_NE(h5_child, nullptr);
  ASSERT_NE(h5_parent, nullptr);
  ASSERT_NE(h5_leaf, nullptr);
  EXPECT_FALSE(H5F_is_mount(h5_child));
  EXPECT_FALSE(H5F_is_mount(h5_parent));
  EXPECT_TRUE(H5F_is_mount(h5_leaf));

  // Mount parent onto grandparent and child onto grandparent:/mnt_p/mnt_c.
  ASSERT_GE(H5Fmount(grandparent.get(), "/mnt_p", parent.get(), H5P_DEFAULT), 0);
  ASSERT_GE(
      H5Fmount(grandparent.get(), "/mnt_p/mnt_c", child.get(), H5P_DEFAULT), 0);
  EXPECT_TRUE(H5F_is_mount(h5_parent));
  EXPECT_TRUE(H5F_is_mount(h5_child));

  // 3. Traverse through grandparent -> parent -> child -> leaf_root (".").
  ScopedDataset traversed(
      H5Dopen2(grandparent.get(), "/mnt_p/mnt_c/leaf_val", H5P_DEFAULT));
  ASSERT_TRUE(traversed.valid());
  int32_t actual = 0;
  ASSERT_GE(H5Dread(traversed.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &actual),
            0);
  EXPECT_EQ(actual, expected);

  // Accessing a non-mounted group on grandparent exercises traversal miss while
  // parent's mount table is non-empty.
  ScopedGroup opened_plain(H5Gopen2(grandparent.get(), "/plain", H5P_DEFAULT));
  ASSERT_TRUE(opened_plain.valid());

  // 4. Global flush on the deepest leaf walks up to grandparent and recurses
  // down across all mounts.
  EXPECT_GE(H5Fflush(leaf_root.get(), H5F_SCOPE_GLOBAL), 0);
  EXPECT_GE(H5Fflush(grandparent.get(), H5F_SCOPE_GLOBAL), 0);

  traversed.reset();
  opened_plain.reset();

  // 5. Unmount leaf_root (topmost at /mnt_p/mnt_c), then child, then parent.
  ASSERT_GE(H5Funmount(grandparent.get(), "/mnt_p/mnt_c"), 0);
  EXPECT_FALSE(H5F_is_mount(h5_leaf));
  ASSERT_GE(H5Funmount(grandparent.get(), "/mnt_p/mnt_c"), 0);
  EXPECT_FALSE(H5F_is_mount(h5_child));
  ASSERT_GE(H5Funmount(grandparent.get(), "/mnt_p"), 0);
  EXPECT_FALSE(H5F_is_mount(h5_parent));
}

TEST(H5FmountTest, MountAndUnmountErrorConditions) {
  ScopedFile parent = CreateInMemoryFile("mnt_err_parent.h5");
  ScopedFile child1 = CreateInMemoryFile("mnt_err_c1.h5");
  ScopedFile child2 = CreateInMemoryFile("mnt_err_c2.h5");
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child1.valid());
  ASSERT_TRUE(child2.valid());

  ScopedGroup g1(
      H5Gcreate2(parent.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(
      H5Gcreate2(parent.get(), "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup plain(
      H5Gcreate2(parent.get(), "/plain", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup c1_sub(
      H5Gcreate2(child1.get(), "/in_c1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ASSERT_TRUE(g2.valid());
  ASSERT_TRUE(plain.valid());
  ASSERT_TRUE(c1_sub.valid());
  c1_sub.reset();

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(parent.get(), "/not_a_group", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();

  // Keep g1 open before mounting so mounting on g1_pre/'.' reaches
  // H5G_MOUNTED(mount_point) check after H5Fmount.
  {
    ScopedErrorSilence silence;
    // Nonexistent mount point.
    EXPECT_LT(H5Fmount(parent.get(), "/does_not_exist", child1.get(), H5P_DEFAULT), 0);
    // Dataset (not a group) as mount point.
    EXPECT_LT(H5Fmount(parent.get(), "/not_a_group", child1.get(), H5P_DEFAULT), 0);
    // Direct self-cycle.
    EXPECT_LT(H5Fmount(parent.get(), "/g1", parent.get(), H5P_DEFAULT), 0);
    // Unmounting a non-existent group or a group with no mount fails.
    EXPECT_LT(H5Funmount(parent.get(), "/missing_mnt"), 0);
    EXPECT_LT(H5Funmount(parent.get(), "/plain"), 0);
  }

  ASSERT_GE(H5Fmount(parent.get(), "/g1", child1.get(), H5P_DEFAULT), 0);

  {
    ScopedErrorSilence silence;
    // Child already mounted elsewhere.
    EXPECT_LT(H5Fmount(parent.get(), "/g2", child1.get(), H5P_DEFAULT), 0);
    // Mounting parent into child1 would introduce a cycle.
    EXPECT_LT(H5Fmount(child1.get(), "/in_c1", parent.get(), H5P_DEFAULT), 0);
    // Pre-opened mount-point group g1 already has H5G_MOUNTED == true.
    EXPECT_LT(H5Fmount(g1.get(), ".", child2.get(), H5P_DEFAULT), 0);
    // Unmounting a plain group when mount table is non-empty fails.
    EXPECT_LT(H5Funmount(parent.get(), "/plain"), 0);
  }

  // Mismatched file close degree (STRONG vs WEAK) is rejected.
  ScopedPlist strong_fapl = MakeCoreFapl();
  ASSERT_TRUE(strong_fapl.valid());
  ASSERT_GE(H5Pset_fclose_degree(strong_fapl.get(), H5F_CLOSE_STRONG), 0);
  ScopedFile strong_child(H5Fcreate("mnt_err_strong.h5", H5F_ACC_TRUNC,
                                    H5P_DEFAULT, strong_fapl.get()));
  ASSERT_TRUE(strong_child.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fmount(parent.get(), "/g2", strong_child.get(), H5P_DEFAULT), 0);
  }

  plain.reset();
  g2.reset();
  g1.reset();
  ASSERT_GE(H5Funmount(parent.get(), "/g1"), 0);
}

TEST(H5FmountTest, ExternalLinkAsMountPointIsRejected) {
  TempDir dir("mnt_extlink_dir");
  const std::string ext_path = dir.child("ext_target.h5").string();
  const std::string main_path = dir.child("ext_main.h5").string();

  ScopedFile ext_file(
      H5Fcreate(ext_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_file.valid());
  ScopedGroup ext_grp(
      H5Gcreate2(ext_file.get(), "/ext_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_grp.valid());
  ext_grp.reset();
  ext_file.reset();

  ScopedFile main_file(
      H5Fcreate(main_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(main_file.valid());
  ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/ext_grp", main_file.get(),
                               "/ext_link", H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedPlist default_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(default_fapl.valid());
  ScopedFile child(H5Fcreate(dir.child("ext_child.h5").c_str(), H5F_ACC_TRUNC,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Fmount(main_file.get(), "/ext_link", child.get(), H5P_DEFAULT), 0);
}

TEST(H5FmountTest, TableGrowthReallocationBinarySearchAndPreOpenedUnmount) {
  ScopedFile parent = CreateInMemoryFile("mnt_grow_parent.h5");
  ASSERT_TRUE(parent.valid());

  // Create /before first, then 20 groups with /between in the middle so binary
  // search misses exercise both `< 0` and `> 0` branches as well as mount
  // table growth 0 -> 16 -> 32.
  ScopedGroup before_unmounted(
      H5Gcreate2(parent.get(), "/before", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(before_unmounted.valid());
  before_unmounted.reset();

  constexpr int kNumMounts = 20;
  std::vector<ScopedGroup> pre_opened;
  pre_opened.reserve(kNumMounts);
  ScopedGroup between_unmounted;
  for (int i = 0; i < kNumMounts; ++i) {
    if (i == 10) {
      between_unmounted = ScopedGroup(H5Gcreate2(
          parent.get(), "/between", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(between_unmounted.valid());
      between_unmounted.reset();
    }
    const std::string name = "/g_" + std::to_string(i);
    pre_opened.emplace_back(
        H5Gcreate2(parent.get(), name.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(pre_opened.back().valid());
  }

  // Mount in interleaved order so insertions hit front, middle, and back of
  // the sorted mount table.
  std::vector<int> order{10, 0, 19, 5, 15, 2, 8, 12, 17, 1,
                         3, 4, 6, 7, 9, 11, 13, 14, 16, 18};
  std::vector<ScopedFile> children;
  children.reserve(kNumMounts);
  for (int i = 0; i < kNumMounts; ++i) {
    const std::string fname = "mnt_grow_child_" + std::to_string(i) + ".h5";
    children.push_back(CreateInMemoryFile(fname.c_str()));
    ASSERT_TRUE(children.back().valid());
  }
  for (int idx : order) {
    const std::string name = "/g_" + std::to_string(idx);
    ASSERT_GE(H5Fmount(parent.get(), name.c_str(), children[idx].get(), H5P_DEFAULT), 0);
  }

  // Test pre-opened group unmount across left, middle, and right table entries
  // plus unmounted-group error search on a populated table.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Funmount(parent.get(), "/before"), 0);
    EXPECT_LT(H5Funmount(parent.get(), "/between"), 0);
  }
  auto* h5_c0 = static_cast<H5F_t*>(H5VL_object_verify(children[0].get(), H5I_FILE));
  auto* h5_c1 = static_cast<H5F_t*>(H5VL_object_verify(children[1].get(), H5I_FILE));
  auto* h5_c10 = static_cast<H5F_t*>(H5VL_object_verify(children[10].get(), H5I_FILE));
  auto* h5_c19 = static_cast<H5F_t*>(H5VL_object_verify(children[19].get(), H5I_FILE));
  ASSERT_NE(h5_c0, nullptr);
  ASSERT_NE(h5_c1, nullptr);
  ASSERT_NE(h5_c10, nullptr);
  ASSERT_NE(h5_c19, nullptr);
  EXPECT_TRUE(H5F_is_mount(h5_c0));
  EXPECT_TRUE(H5F_is_mount(h5_c1));

  ASSERT_GE(H5Funmount(pre_opened[0].get(), "."), 0);
  ASSERT_GE(H5Funmount(pre_opened[19].get(), "."), 0);
  ASSERT_GE(H5Funmount(parent.get(), "/g_10"), 0);
  EXPECT_FALSE(H5F_is_mount(h5_c0));
  EXPECT_FALSE(H5F_is_mount(h5_c19));
  EXPECT_FALSE(H5F_is_mount(h5_c10));
  EXPECT_TRUE(H5F_is_mount(h5_c1));

  pre_opened.clear();

  // Close all child file handles first (WEAK close defers tearing down the 17
  // mounted children while `parent` remains open), then close `parent` so
  // H5F__close_mounts loops backwards and unmounts + closes all 17 cleanly.
  auto* h5_parent =
      static_cast<H5F_t*>(H5VL_object_verify(parent.get(), H5I_FILE));
  ASSERT_NE(h5_parent, nullptr);
  EXPECT_EQ(h5_parent->nmounts, 17U);
  children.clear();
  EXPECT_EQ(h5_parent->nmounts, 17U);
  parent.reset();
}

TEST(H5FmountTest, ReopenSharedMountTableCountIdsAndCloseOrder) {
  // 1. Create a file with two handles via H5Freopen sharing one H5F_shared_t.
  ScopedFile p1 = CreateInMemoryFile("mnt_shared_p.h5");
  ASSERT_TRUE(p1.valid());
  ScopedFile p2(H5Freopen(p1.get()));
  ASSERT_TRUE(p2.valid());

  ScopedFile c1 = CreateInMemoryFile("mnt_shared_c1.h5");
  ScopedFile c2 = CreateInMemoryFile("mnt_shared_c2.h5");
  ASSERT_TRUE(c1.valid());
  ASSERT_TRUE(c2.valid());

  ScopedGroup m1(
      H5Gcreate2(p1.get(), "/m1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup m2(
      H5Gcreate2(p2.get(), "/m2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(m1.valid());
  ASSERT_TRUE(m2.valid());

  ScopedGroup c2_grp(
      H5Gcreate2(c2.get(), "/in_c2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(c2_grp.valid());

  ASSERT_GE(H5Fmount(p1.get(), "/m1", c1.get(), H5P_DEFAULT), 0);
  ASSERT_GE(H5Fmount(p2.get(), "/m2", c2.get(), H5P_DEFAULT), 0);

  // Unmount c2 via pre-opened group m2, then re-mount on p2.
  ASSERT_GE(H5Funmount(m2.get(), "."), 0);
  ASSERT_GE(H5Fmount(p2.get(), "/m2", c2.get(), H5P_DEFAULT), 0);

  // Close m1 and c1, then close p1 while p2/c2/m2/c2_grp remain open:
  // H5F__mount_count_ids_recurse(p1) sees 0 open IDs on p1/c1 (skipping c2 since
  // c2->parent == p2 != p1), so p1 enters H5F__close_mounts(p1), skipping c2
  // (`child[u].file->parent != p1`) and closing c1.
  m1.reset();
  c1.reset();
  p1.reset();

  // Closing c2 while m2 and c2_grp remain open exercises
  // H5G_get_shared_count(m2) > 1 in H5F__mount_count_ids_recurse; then closing
  // p2, m2, c2_grp completes teardown from the child's delayed close.
  c2.reset();
  p2.reset();
  m2.reset();
  c2_grp.reset();
}

TEST(H5FmountTest, SemiCloseDegreeChecksMountHierarchyOpenObjects) {
  ScopedPlist semi_fapl = MakeCoreFapl();
  ASSERT_TRUE(semi_fapl.valid());
  ASSERT_GE(H5Pset_fclose_degree(semi_fapl.get(), H5F_CLOSE_SEMI), 0);

  ScopedFile parent(H5Fcreate("mnt_semi_p.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              semi_fapl.get()));
  ScopedFile child(H5Fcreate("mnt_semi_c.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             semi_fapl.get()));
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  ScopedGroup mp(
      H5Gcreate2(parent.get(), "/mp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mp.valid());
  ASSERT_GE(H5Fmount(parent.get(), "/mp", child.get(), H5P_DEFAULT), 0);

  // Keep mp open while child handle closes; H5F__close(parent) with SEMI fails
  // because mp contributes an open object in H5F__mount_count_ids_recurse.
  child.reset();
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fclose(parent.get()), 0);
  }
  mp.reset();
  EXPECT_GE(H5Fclose(parent.release()), 0);
}

}  // namespace
}  // namespace hdf5
