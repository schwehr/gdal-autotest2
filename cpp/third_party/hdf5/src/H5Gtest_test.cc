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
// Tests internal group testing helpers declared in H5Gpkg.h under H5G_TESTING
// (third_party/hdf5/src/H5Gtest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gtest.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts before pulling
// H5Gpkg.h and enable H5G_TESTING prototypes.
#ifndef NDEBUG
#define H5G_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B_FRIEND
#define H5G_FRIEND
#define H5G_TESTING
#define H5O_FRIEND
#define H5T_FRIEND
#define H5VL_FRIEND
#define H5VL_TESTING
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5Bpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#ifdef H5_HAVE__FLOAT16
typedef uint16_t H5__Float16;
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLpkg.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5G_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5G_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
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
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

ScopedFile CreateFileWithBounds(const char* name, H5F_libver_t low,
                                H5F_libver_t high) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl, low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
}

void AddSoftLinks(hid_t grp, int count, const char* prefix) {
  for (int i = 0; i < count; ++i) {
    const std::string name = std::string(prefix) + std::to_string(i);
    ASSERT_GE(H5Lcreate_soft("/target", grp, name.c_str(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
  }
}

TEST(H5GtestTest, GroupInspectionHelpersCoverOldCompactAndDense) {
  ScopedFile old_file =
      CreateFileWithBounds("old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  // Old-style empty and non-empty groups.
  ScopedGroup old_empty(
      H5Gcreate2(old_file, "/old_empty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup old_full(
      H5Gcreate2(old_file, "/old_full", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(old_empty.valid());
  ASSERT_TRUE(old_full.valid());
  AddSoftLinks(old_full, 2, "s_");

  EXPECT_GT(H5G__is_empty_test(old_empty), 0);
  EXPECT_EQ(H5G__is_empty_test(old_full), 0);
  EXPECT_GT(H5G__has_stab_test(old_empty), 0);
  EXPECT_GT(H5G__has_stab_test(old_full), 0);
  EXPECT_EQ(H5G__has_links_test(old_full, nullptr), 0);
  EXPECT_EQ(H5G__is_new_dense_test(old_full), 0);

  size_t lheap_size = 0;
  EXPECT_GE(H5G__lheap_size_test(old_empty, &lheap_size), 0);
  EXPECT_GT(lheap_size, 0u);

  // Compact empty & non-empty new-format groups.
  ScopedGroup new_empty(
      H5Gcreate2(new_file, "/new_empty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup new_compact(H5Gcreate2(new_file, "/new_compact", H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(new_empty.valid());
  ASSERT_TRUE(new_compact.valid());
  AddSoftLinks(new_compact, 3, "c_");

  EXPECT_GT(H5G__is_empty_test(new_empty), 0);
  EXPECT_EQ(H5G__is_empty_test(new_compact), 0);
  EXPECT_EQ(H5G__has_stab_test(new_empty), 0);
  unsigned nmsgs = 0;
  EXPECT_GT(H5G__has_links_test(new_compact, &nmsgs), 0);
  EXPECT_EQ(nmsgs, 3u);
  EXPECT_GT(H5G__has_links_test(new_compact, nullptr), 0);
  EXPECT_EQ(H5G__is_new_dense_test(new_empty), 0);
  EXPECT_EQ(H5G__is_new_dense_test(new_compact), 0);

  // Do NOT call H5G__new_dense_info_test on old-style groups (upstream leaves
  // linfo uninitialized if no LINFO exists); test on compact new-format group.
  {
    ScopedErrorSilence silence;
    hsize_t name_count = 0;
    hsize_t corder_count = 0;
    EXPECT_LT(H5G__new_dense_info_test(new_empty, &name_count, &corder_count),
              0);
    EXPECT_LT(H5G__lheap_size_test(new_empty, &lheap_size), 0);
  }

  // Dense without creation order index.
  ScopedPlist gcpl_name(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_name.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_name, 2, 1), 0);
  ScopedGroup dense_name(
      H5Gcreate2(new_file, "/dense_name", H5P_DEFAULT, gcpl_name, H5P_DEFAULT));
  ASSERT_TRUE(dense_name.valid());
  AddSoftLinks(dense_name, 4, "dn_");

  EXPECT_EQ(H5G__is_empty_test(dense_name), 0);
  EXPECT_EQ(H5G__has_links_test(dense_name, &nmsgs), 0);
  EXPECT_GT(H5G__is_new_dense_test(dense_name), 0);
  hsize_t name_count = 0;
  hsize_t corder_count = 999;
  ASSERT_GE(H5G__new_dense_info_test(dense_name, &name_count, &corder_count),
            0);
  EXPECT_EQ(name_count, 4u);
  EXPECT_EQ(corder_count, 0u);

  // Dense WITH creation order index.
  ScopedPlist gcpl_corder(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_corder.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_corder, 2, 1), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl_corder, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup dense_corder(H5Gcreate2(new_file, "/dense_corder", H5P_DEFAULT,
                                      gcpl_corder, H5P_DEFAULT));
  ASSERT_TRUE(dense_corder.valid());
  AddSoftLinks(dense_corder, 5, "dc_");

  ASSERT_GE(H5G__new_dense_info_test(dense_corder, &name_count, &corder_count),
            0);
  EXPECT_EQ(name_count, 5u);
  EXPECT_EQ(corder_count, 5u);

  // Invalid IDs fail cleanly across helpers.
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5G__is_empty_test(space), 0);
  EXPECT_LT(H5G__has_links_test(space, &nmsgs), 0);
  EXPECT_LT(H5G__has_stab_test(space), 0);
  EXPECT_LT(H5G__is_new_dense_test(space), 0);
  EXPECT_LT(H5G__new_dense_info_test(space, &name_count, &corder_count), 0);
  EXPECT_LT(H5G__lheap_size_test(space, &lheap_size), 0);
}

TEST(H5GtestTest, UserPathTestCoversObjectsHiddenAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "/my_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "/my_dset", H5T_NATIVE_INT, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file, "/my_type", committed_dt, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  std::array<char, 64> buf{};
  size_t len = 0;
  unsigned hidden = 99;

  // Query path length without copying into buffer, then copy full string.
  ASSERT_GE(H5G__user_path_test(grp, nullptr, &len, &hidden), 0);
  EXPECT_EQ(len, 7u);
  EXPECT_EQ(hidden, 0u);
  ASSERT_GE(H5G__user_path_test(grp, buf.data(), &len, &hidden), 0);
  EXPECT_STREQ(buf.data(), "/my_grp");

  ASSERT_GE(H5G__user_path_test(dset, buf.data(), &len, &hidden), 0);
  EXPECT_STREQ(buf.data(), "/my_dset");

  ASSERT_GE(H5G__user_path_test(committed_dt, buf.data(), &len, &hidden), 0);
  EXPECT_STREQ(buf.data(), "/my_type");

  // Mounting a child file on "/mnt" while "/mnt/sub" is open hides the user
  // path (hidden == 1) until unmounted; unlinking clears user_path_r (len == 0).
  {
    ScopedFile child_f = CreateInMemoryFile("hide_child.h5");
    ASSERT_TRUE(child_f.valid());
    ScopedGroup mnt(
        H5Gcreate2(file, "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(mnt.valid());
    ScopedGroup sub(
        H5Gcreate2(mnt, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
    ASSERT_GE(H5Fmount(file, "/mnt", child_f, H5P_DEFAULT), 0);
    ASSERT_GE(H5G__user_path_test(sub, buf.data(), &len, &hidden), 0);
    EXPECT_EQ(hidden, 1u);
    EXPECT_STREQ(buf.data(), "/mnt/sub");
    ASSERT_GE(H5Funmount(file, "/mnt"), 0);
    ASSERT_GE(H5G__user_path_test(sub, buf.data(), &len, &hidden), 0);
    EXPECT_EQ(hidden, 0u);
  }

  ScopedGroup temp_grp(
      H5Gcreate2(file, "/doomed", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(temp_grp.valid());
  ASSERT_GE(H5Ldelete(file, "/doomed", H5P_DEFAULT), 0);
  ASSERT_GE(H5G__user_path_test(temp_grp, buf.data(), &len, &hidden), 0);
  EXPECT_EQ(len, 0u);
  EXPECT_EQ(hidden, 0u);

  // Anonymous group has no user path -> len == 0, hidden == 0.
  ScopedGroup anon(H5Gcreate_anon(file, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon.valid());
  len = 99;
  hidden = 99;
  ASSERT_GE(H5G__user_path_test(anon, buf.data(), &len, &hidden), 0);
  EXPECT_EQ(len, 0u);
  EXPECT_EQ(hidden, 0u);

  // Error cases: invalid ID, non-committed datatype, swapped non-named inner
  // datatype under a committed datatype ID, and non-data object IDs.
  ScopedErrorSilence silence;
  EXPECT_LT(H5G__user_path_test(H5I_INVALID_HID, buf.data(), &len, &hidden), 0);
  EXPECT_LT(H5G__user_path_test(transient_dt, buf.data(), &len, &hidden), 0);
  {
    auto* comm_dt =
        static_cast<H5T_t*>(H5I_object_verify(committed_dt, H5I_DATATYPE));
    auto* trans_dt =
        static_cast<H5T_t*>(H5I_object_verify(transient_dt, H5I_DATATYPE));
    ASSERT_NE(comm_dt, nullptr);
    ASSERT_NE(trans_dt, nullptr);
    ASSERT_NE(comm_dt->vol_obj, nullptr);
    void* saved_vol_data = comm_dt->vol_obj->data;
    comm_dt->vol_obj->data = trans_dt;
    absl::Cleanup restore_vol_data = [&] {
      comm_dt->vol_obj->data = saved_vol_data;
    };
    EXPECT_LT(H5G__user_path_test(committed_dt, buf.data(), &len, &hidden), 0);
  }
  EXPECT_LT(H5G__user_path_test(file, buf.data(), &len, &hidden), 0);
  EXPECT_LT(H5G__user_path_test(space, buf.data(), &len, &hidden), 0);
}

TEST(H5GtestTest, VerifyCachedStabAndCachedStabsTest) {
  ScopedFile old_file =
      CreateFileWithBounds("stab.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("compact.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  ScopedGroup root(H5Gopen2(old_file, "/", H5P_DEFAULT));
  ASSERT_TRUE(root.valid());
  ScopedGroup sub(
      H5Gcreate2(old_file, "/sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(old_file, "/dset", H5T_NATIVE_INT, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(dt.valid());
  ASSERT_GE(
      H5Tcommit2(old_file, "/dt", dt, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
      0);

  ScopedGroup new_grp(
      H5Gcreate2(new_file, "/new", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(new_grp.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // H5G__verify_cached_stabs_test verifies both symbol-table subgroups (/sub)
  // and non-group entries (/dset, /dt), plus new-format group no-op and bad
  // non-group IDs. Note: soft links have HADDR_UNDEF headers so the helper
  // intentionally only scans symbol-table hard-link entries.
  EXPECT_GE(H5G__verify_cached_stabs_test(root), 0);
  EXPECT_GE(H5G__verify_cached_stabs_test(new_grp), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G__verify_cached_stabs_test(dset), 0);
  }

  // H5G__verify_cached_stab_test on matching entry and error cases.
  auto* sub_g = static_cast<H5G_t*>(H5VL_object_verify(sub, H5I_GROUP));
  auto* new_g = static_cast<H5G_t*>(H5VL_object_verify(new_grp, H5I_GROUP));
  ASSERT_NE(sub_g, nullptr);
  ASSERT_NE(new_g, nullptr);

  H5O_loc_t* sub_oloc = H5G_oloc(sub_g);
  H5O_loc_t* new_oloc = H5G_oloc(new_g);
  H5O_stab_t stab{};
  ASSERT_NE(H5O_msg_read(sub_oloc, H5O_STAB_ID, &stab), nullptr);

  H5G_entry_t ent{};
  ent.type = H5G_CACHED_STAB;
  ent.cache.stab.btree_addr = stab.btree_addr;
  ent.cache.stab.heap_addr = stab.heap_addr;
  EXPECT_GE(H5G__verify_cached_stab_test(sub_oloc, &ent), 0);

  ScopedErrorSilence silence;
  // Not cached.
  H5G_entry_t bad_type = ent;
  bad_type.type = H5G_NOTHING_CACHED;
  EXPECT_LT(H5G__verify_cached_stab_test(sub_oloc, &bad_type), 0);

  // Target object header lacks STAB message.
  EXPECT_LT(H5G__verify_cached_stab_test(new_oloc, &ent), 0);

  // Cached btree/heap mismatch vs STAB message.
  H5G_entry_t mismatch = ent;
  mismatch.cache.stab.btree_addr += 1;
  EXPECT_LT(H5G__verify_cached_stab_test(sub_oloc, &mismatch), 0);

  // Corrupt STAB btree_addr and heap_addr cleanly in a scratch group.
  {
    ScopedGroup scratch(
        H5Gcreate2(old_file, "/scratch", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(scratch.valid());
    auto* sg = static_cast<H5G_t*>(H5VL_object_verify(scratch, H5I_GROUP));
    ASSERT_NE(sg, nullptr);
    H5O_loc_t* s_oloc = H5G_oloc(sg);
    H5O_stab_t s_orig{};
    ASSERT_NE(H5O_msg_read(s_oloc, H5O_STAB_ID, &s_orig), nullptr);

    H5O_stab_t bad_bt = s_orig;
    bad_bt.btree_addr = HADDR_UNDEF;
    H5G_entry_t s_ent{};
    s_ent.type = H5G_CACHED_STAB;
    s_ent.cache.stab.btree_addr = bad_bt.btree_addr;
    s_ent.cache.stab.heap_addr = bad_bt.heap_addr;
    ASSERT_GE(H5O_msg_write(s_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_bt),
              0);
    EXPECT_LT(H5G__verify_cached_stab_test(s_oloc, &s_ent), 0);

    H5O_stab_t bad_hp = s_orig;
    bad_hp.heap_addr = HADDR_UNDEF;
    s_ent.cache.stab.btree_addr = bad_hp.btree_addr;
    s_ent.cache.stab.heap_addr = bad_hp.heap_addr;
    ASSERT_GE(H5O_msg_write(s_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_hp),
              0);
    EXPECT_LT(H5G__verify_cached_stab_test(s_oloc, &s_ent), 0);

    ASSERT_GE(H5O_msg_write(s_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &s_orig),
              0);
  }
}

std::vector<uint8_t> ExtractGtestFileImage(hid_t file_id) {
  ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  ssize_t got = H5Fget_file_image(file_id, image.data(), image.size());
  if (got != size) {
    return {};
  }
  return image;
}

ScopedFile OpenGtestFileImage(const std::vector<uint8_t>& image, unsigned flags,
                              const char* name) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_file_image(fapl, const_cast<uint8_t*>(image.data()),
                        image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen(name, flags, fapl));
}

H5O_mesg_t* FindTestOhMesg(H5O_t* oh, unsigned type_id) {
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type && oh->mesg[i].type->id == type_id) {
      return &oh->mesg[i];
    }
  }
  return nullptr;
}

herr_t DummyMapOptionalCb(void* /*obj*/, H5VL_optional_args_t* args,
                          hid_t /*dxpl_id*/, void** /*req*/) {
  if (args && args->op_type == H5VL_MAP_CLOSE) {
    return 0;
  }
  return -1;
}

TEST(H5GtestTest, InternalErrorPathsAndCacheUnprotectBranches) {
  constexpr haddr_t kBadAddr = 0xdeadbeef00ULL;

  ScopedFile old_file =
      CreateFileWithBounds("gt_old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("gt_new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  ScopedGroup old_empty(
      H5Gcreate2(old_file, "/old_empty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup old_pop(
      H5Gcreate2(old_file, "/old_pop", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(old_empty.valid());
  ASSERT_TRUE(old_pop.valid());
  ScopedGroup old_sub(
      H5Gcreate2(old_pop, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(old_sub.valid());
  {
    ScopedSpace old_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(old_space.valid());
    ScopedDataset old_dset(H5Dcreate2(old_pop, "d", H5T_NATIVE_INT, old_space,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(old_dset.valid());
  }

  ScopedGroup new_empty(
      H5Gcreate2(new_file, "/new_empty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup compact(
      H5Gcreate2(new_file, "/compact", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(new_empty.valid());
  ASSERT_TRUE(compact.valid());
  ASSERT_GE(H5Lcreate_soft("/t", compact, "c0", H5P_DEFAULT, H5P_DEFAULT), 0);

  ScopedPlist gcpl_dense(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_dense.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_dense, 2, 1), 0);
  ScopedGroup dense(
      H5Gcreate2(new_file, "/dense", H5P_DEFAULT, gcpl_dense, H5P_DEFAULT));
  ASSERT_TRUE(dense.valid());
  AddSoftLinks(dense, 4, "d");

  ScopedPlist gcpl_corder(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_corder.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_corder, 2, 1), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl_corder, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup dense_corder(H5Gcreate2(new_file, "/dense_corder", H5P_DEFAULT,
                                      gcpl_corder, H5P_DEFAULT));
  ASSERT_TRUE(dense_corder.valid());
  AddSoftLinks(dense_corder, 4, "dc");

  auto* old_f = static_cast<H5F_t*>(H5VL_object_verify(old_file, H5I_FILE));
  auto* old_empty_g =
      static_cast<H5G_t*>(H5VL_object_verify(old_empty, H5I_GROUP));
  auto* old_pop_g = static_cast<H5G_t*>(H5VL_object_verify(old_pop, H5I_GROUP));
  auto* new_empty_g =
      static_cast<H5G_t*>(H5VL_object_verify(new_empty, H5I_GROUP));
  auto* compact_g = static_cast<H5G_t*>(H5VL_object_verify(compact, H5I_GROUP));
  auto* dense_g = static_cast<H5G_t*>(H5VL_object_verify(dense, H5I_GROUP));
  auto* dense_co_g =
      static_cast<H5G_t*>(H5VL_object_verify(dense_corder, H5I_GROUP));
  ASSERT_NE(old_f, nullptr);
  ASSERT_NE(old_empty_g, nullptr);
  ASSERT_NE(old_pop_g, nullptr);
  ASSERT_NE(new_empty_g, nullptr);
  ASSERT_NE(compact_g, nullptr);
  ASSERT_NE(dense_g, nullptr);
  ASSERT_NE(dense_co_g, nullptr);

  ScopedErrorSilence silence;

  // 1. Corrupt oloc->addr = HADDR_UNDEF triggers H5O_msg_exists failure cleanly
  //    in is_empty, has_links, has_stab, is_new_dense, and verify_cached_stabs.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    const haddr_t saved_addr = new_empty_g->oloc.addr;
    new_empty_g->oloc.addr = HADDR_UNDEF;
    absl::Cleanup restore = [&] { new_empty_g->oloc.addr = saved_addr; };
    EXPECT_LT(H5G__is_empty_test(new_empty), 0);
    unsigned nmsgs = 0;
    EXPECT_LT(H5G__has_links_test(new_empty, &nmsgs), 0);
    EXPECT_LT(H5G__has_stab_test(new_empty), 0);
    EXPECT_LT(H5G__is_new_dense_test(new_empty), 0);
    EXPECT_LT(H5G__verify_cached_stabs_test(new_empty), 0);
  }

  // 2. Conflicting STAB/GINFO messages without disturbing link storage.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_stab_t dummy_stab{};
    dummy_stab.btree_addr = 1000;
    dummy_stab.heap_addr = 2000;

    // 2a. Compact group (has LINK) + temporary STAB -> 114, 234, 301.
    ASSERT_GE(
        H5O_msg_create(&(compact_g->oloc), H5O_STAB_ID, 0, 0, &dummy_stab), 0);
    {
      absl::Cleanup rm = [&] {
        H5O_msg_remove(&(compact_g->oloc), H5O_STAB_ID, 0, false);
      };
      EXPECT_LT(H5G__is_empty_test(compact), 0);
      unsigned nmsgs = 0;
      EXPECT_LT(H5G__has_links_test(compact, &nmsgs), 0);
      EXPECT_LT(H5G__has_stab_test(compact), 0);
    }

    // 2b. Empty new group (has LINFO, no LINK) + temporary STAB -> 129, 354.
    ASSERT_GE(
        H5O_msg_create(&(new_empty_g->oloc), H5O_STAB_ID, 0, 0, &dummy_stab),
        0);
    {
      absl::Cleanup rm = [&] {
        H5O_msg_remove(&(new_empty_g->oloc), H5O_STAB_ID, 0, false);
      };
      EXPECT_LT(H5G__is_empty_test(new_empty), 0);
      EXPECT_EQ(H5G__is_new_dense_test(new_empty), 0);
    }

    // 2c. Old empty group (has STAB) + temporary GINFO -> 163.
    H5O_ginfo_t dummy_ginfo = H5G_CRT_GROUP_INFO_DEF;
    ASSERT_GE(
        H5O_msg_create(&(old_empty_g->oloc), H5O_GINFO_ID, 0, 0, &dummy_ginfo),
        0);
    {
      absl::Cleanup rm = [&] {
        H5O_msg_remove(&(old_empty_g->oloc), H5O_GINFO_ID, 0, false);
      };
      EXPECT_LT(H5G__is_empty_test(old_empty), 0);
    }
  }

  // 3. Mutated LINFO fields on new_empty for 139, 141, 145, 376, 440.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_linfo_t orig_linfo{};
    ASSERT_NE(H5O_msg_read(&(new_empty_g->oloc), H5O_LINFO_ID, &orig_linfo),
              nullptr);
    absl::Cleanup restore = [&] {
      H5O_msg_write(&(new_empty_g->oloc), H5O_LINFO_ID, 0, 0, &orig_linfo);
    };

    H5O_linfo_t linfo = orig_linfo;
    linfo.fheap_addr = HADDR_UNDEF;
    linfo.name_bt2_addr = 1000;
    linfo.corder_bt2_addr = HADDR_UNDEF;
    linfo.nlinks = 0;
    ASSERT_GE(H5O_msg_write(&(new_empty_g->oloc), H5O_LINFO_ID, 0, 0, &linfo),
              0);
    EXPECT_EQ(H5G__is_empty_test(new_empty), 0);

    linfo.name_bt2_addr = HADDR_UNDEF;
    linfo.corder_bt2_addr = 1000;
    ASSERT_GE(H5O_msg_write(&(new_empty_g->oloc), H5O_LINFO_ID, 0, 0, &linfo),
              0);
    EXPECT_EQ(H5G__is_empty_test(new_empty), 0);

    linfo.corder_bt2_addr = HADDR_UNDEF;
    linfo.nlinks = 1;
    ASSERT_GE(H5O_msg_write(&(new_empty_g->oloc), H5O_LINFO_ID, 0, 0, &linfo),
              0);
    EXPECT_EQ(H5G__is_empty_test(new_empty), 0);

    linfo.nlinks = 0;
    linfo.fheap_addr = 1000;
    linfo.name_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5O_msg_write(&(new_empty_g->oloc), H5O_LINFO_ID, 0, 0, &linfo),
              0);
    EXPECT_EQ(H5G__is_new_dense_test(new_empty), 0);
    hsize_t nc = 0;
    hsize_t cc = 0;
    EXPECT_LT(H5G__new_dense_info_test(new_empty, &nc, &cc), 0);
  }

  // 4. Dense B-tree open errors on dense & dense_corder (lines 444, 454).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_linfo_t orig_d{};
    ASSERT_NE(H5O_msg_read(&(dense_g->oloc), H5O_LINFO_ID, &orig_d), nullptr);
    ASSERT_NE(orig_d.nlinks, HSIZET_MAX);
    absl::Cleanup restore_d = [&] {
      H5O_msg_write(&(dense_g->oloc), H5O_LINFO_ID, 0, 0, &orig_d);
    };
    H5O_linfo_t bad_d = orig_d;
    bad_d.name_bt2_addr = kBadAddr;
    ASSERT_GE(H5O_msg_write(&(dense_g->oloc), H5O_LINFO_ID, 0, 0, &bad_d), 0);
    hsize_t nc = 0;
    hsize_t cc = 0;
    EXPECT_LT(H5G__new_dense_info_test(dense, &nc, &cc), 0);

    H5O_linfo_t orig_dc{};
    ASSERT_NE(H5O_msg_read(&(dense_co_g->oloc), H5O_LINFO_ID, &orig_dc),
              nullptr);
    ASSERT_NE(orig_dc.nlinks, HSIZET_MAX);
    ASSERT_TRUE(H5_addr_defined(orig_dc.corder_bt2_addr));
    absl::Cleanup restore_dc = [&] {
      H5O_msg_write(&(dense_co_g->oloc), H5O_LINFO_ID, 0, 0, &orig_dc);
    };
    H5O_linfo_t bad_dc = orig_dc;
    bad_dc.corder_bt2_addr = kBadAddr;
    ASSERT_GE(H5O_msg_write(&(dense_co_g->oloc), H5O_LINFO_ID, 0, 0, &bad_dc),
              0);
    EXPECT_LT(H5G__new_dense_info_test(dense_corder, &nc, &cc), 0);
  }

  // 5. Old empty group stab corruptions -> is_empty_test (171) & lheap_size
  //    (525) & verify_cached_stabs_test (839).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_stab_t orig_stab{};
    ASSERT_NE(H5O_msg_read(&(old_empty_g->oloc), H5O_STAB_ID, &orig_stab),
              nullptr);
    absl::Cleanup restore = [&] {
      H5O_msg_write(&(old_empty_g->oloc), H5O_STAB_ID, 0, H5O_UPDATE_FORCE,
                    &orig_stab);
    };
    H5O_stab_t bad_bt = orig_stab;
    bad_bt.btree_addr = kBadAddr;
    ASSERT_GE(H5O_msg_write(&(old_empty_g->oloc), H5O_STAB_ID, 0,
                            H5O_UPDATE_FORCE, &bad_bt),
              0);
    EXPECT_LT(H5G__is_empty_test(old_empty), 0);
    EXPECT_LT(H5G__verify_cached_stabs_test(old_empty), 0);

    H5O_stab_t bad_hp = orig_stab;
    bad_hp.heap_addr = kBadAddr;
    ASSERT_GE(H5O_msg_write(&(old_empty_g->oloc), H5O_STAB_ID, 0,
                            H5O_UPDATE_FORCE, &bad_hp),
              0);
    size_t lheap_sz = 0;
    EXPECT_LT(H5G__lheap_size_test(old_empty, &lheap_sz), 0);
  }

  // 6. H5G__user_path_test wrong ID / file ID (617) and H5I_MAP VOL (599).
  {
    size_t ulen = 0;
    unsigned hidden = 0;
    std::array<char, 64> buf{};
    EXPECT_LT(H5G__user_path_test(H5I_INVALID_HID, buf.data(), &ulen, &hidden),
              0);
    EXPECT_LT(H5G__user_path_test(old_file, buf.data(), &ulen, &hidden), 0);

    H5VL_class_t map_cls{};
    map_cls.version = H5VL_VERSION;
    map_cls.value = static_cast<H5VL_class_value_t>(512);
    map_cls.name = "h5g_test_map_vol";
    map_cls.optional = DummyMapOptionalCb;
    hid_t vol_id = H5VLregister_connector(&map_cls, H5P_DEFAULT);
    ASSERT_GE(vol_id, 0);
    absl::Cleanup unreg_vol = [&] { H5VLunregister_connector(vol_id); };

    int dummy_map_obj = 1;
    hid_t map_id =
        H5VL__register_using_vol_id_test(H5I_MAP, &dummy_map_obj, vol_id);
    ASSERT_GE(map_id, 0);
    absl::Cleanup close_map = [&] { H5Idec_ref(map_id); };
    EXPECT_LT(H5G__user_path_test(map_id, buf.data(), &ulen, &hidden), 0);
  }

  // 7. Reopened files for raw message decode errors (LINFO in 133/370/434,
  //    STAB in 167/754/834) + H5G_node_t cached stab mismatch errors.
  {
    ASSERT_GE(H5Fflush(new_file, H5F_SCOPE_GLOBAL), 0);
    std::vector<uint8_t> new_img = ExtractGtestFileImage(new_file);
    ASSERT_FALSE(new_img.empty());
    ScopedFile ro_new = OpenGtestFileImage(new_img, H5F_ACC_RDONLY, "ro_n.h5");
    ASSERT_TRUE(ro_new.valid());
    ScopedGroup ro_ne(H5Gopen2(ro_new, "/new_empty", H5P_DEFAULT));
    ASSERT_TRUE(ro_ne.valid());
    auto* ro_ne_g = static_cast<H5G_t*>(H5VL_object_verify(ro_ne, H5I_GROUP));
    ASSERT_NE(ro_ne_g, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_t* oh = H5O_protect(&(ro_ne_g->oloc), H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    H5O_mesg_t* linfo_mesg = FindTestOhMesg(oh, H5O_LINFO_ID);
    ASSERT_NE(linfo_mesg, nullptr);
    ASSERT_NE(linfo_mesg->raw, nullptr);
    void* orig_native = linfo_mesg->native;
    const uint8_t orig_b0 = linfo_mesg->raw[0];
    linfo_mesg->native = nullptr;
    linfo_mesg->raw[0] = 99;
    ASSERT_GE(H5O_unprotect(&(ro_ne_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);

    EXPECT_LT(H5G__is_empty_test(ro_ne), 0);
    EXPECT_LT(H5G__is_new_dense_test(ro_ne), 0);
    hsize_t nc = 0;
    hsize_t cc = 0;
    EXPECT_LT(H5G__new_dense_info_test(ro_ne, &nc, &cc), 0);

    oh = H5O_protect(&(ro_ne_g->oloc), H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    linfo_mesg = FindTestOhMesg(oh, H5O_LINFO_ID);
    ASSERT_NE(linfo_mesg, nullptr);
    linfo_mesg->native = orig_native;
    linfo_mesg->raw[0] = orig_b0;
    ASSERT_GE(H5O_unprotect(&(ro_ne_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);
  }

  {
    ASSERT_GE(H5Fflush(old_file, H5F_SCOPE_GLOBAL), 0);
    std::vector<uint8_t> old_img = ExtractGtestFileImage(old_file);
    ASSERT_FALSE(old_img.empty());
    ScopedFile ro_old = OpenGtestFileImage(old_img, H5F_ACC_RDONLY, "ro_o.h5");
    ASSERT_TRUE(ro_old.valid());
    ScopedGroup ro_oe(H5Gopen2(ro_old, "/old_empty", H5P_DEFAULT));
    ScopedGroup ro_sub(H5Gopen2(ro_old, "/old_pop/sub", H5P_DEFAULT));
    ScopedGroup ro_pop(H5Gopen2(ro_old, "/old_pop", H5P_DEFAULT));
    ASSERT_TRUE(ro_oe.valid());
    ASSERT_TRUE(ro_sub.valid());
    ASSERT_TRUE(ro_pop.valid());
    auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_old, H5I_FILE));
    auto* ro_oe_g = static_cast<H5G_t*>(H5VL_object_verify(ro_oe, H5I_GROUP));
    auto* ro_sub_g = static_cast<H5G_t*>(H5VL_object_verify(ro_sub, H5I_GROUP));
    auto* ro_pop_g = static_cast<H5G_t*>(H5VL_object_verify(ro_pop, H5I_GROUP));
    ASSERT_NE(ro_f, nullptr);
    ASSERT_NE(ro_oe_g, nullptr);
    ASSERT_NE(ro_sub_g, nullptr);
    ASSERT_NE(ro_pop_g, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Corrupt STAB raw_size on /old_empty -> 167, 834.
    {
      H5O_t* oh = H5O_protect(&(ro_oe_g->oloc), H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      H5O_mesg_t* stab_mesg = FindTestOhMesg(oh, H5O_STAB_ID);
      ASSERT_NE(stab_mesg, nullptr);
      void* orig_native = stab_mesg->native;
      const size_t orig_sz = stab_mesg->raw_size;
      stab_mesg->native = nullptr;
      stab_mesg->raw_size = 4;
      ASSERT_GE(H5O_unprotect(&(ro_oe_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);

      EXPECT_LT(H5G__is_empty_test(ro_oe), 0);
      EXPECT_LT(H5G__verify_cached_stabs_test(ro_oe), 0);

      oh = H5O_protect(&(ro_oe_g->oloc), H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      stab_mesg = FindTestOhMesg(oh, H5O_STAB_ID);
      ASSERT_NE(stab_mesg, nullptr);
      stab_mesg->native = orig_native;
      stab_mesg->raw_size = orig_sz;
      ASSERT_GE(H5O_unprotect(&(ro_oe_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);
    }

    // Corrupt STAB raw_size on /old_pop/sub -> 754 + 780.
    {
      H5O_t* oh = H5O_protect(&(ro_sub_g->oloc), H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      H5O_mesg_t* stab_mesg = FindTestOhMesg(oh, H5O_STAB_ID);
      ASSERT_NE(stab_mesg, nullptr);
      void* orig_native = stab_mesg->native;
      const size_t orig_sz = stab_mesg->raw_size;
      stab_mesg->native = nullptr;
      stab_mesg->raw_size = 4;
      ASSERT_GE(H5O_unprotect(&(ro_sub_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);

      EXPECT_LT(H5G__verify_cached_stabs_test(ro_pop), 0);

      oh = H5O_protect(&(ro_sub_g->oloc), H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      stab_mesg = FindTestOhMesg(oh, H5O_STAB_ID);
      ASSERT_NE(stab_mesg, nullptr);
      stab_mesg->native = orig_native;
      stab_mesg->raw_size = orig_sz;
      ASSERT_GE(H5O_unprotect(&(ro_sub_g->oloc), oh, H5AC__NO_FLAGS_SET), 0);
    }

    // Trigger H5O_unprotect failure (770, 781) and H5AC_unprotect failure (776)
    // in H5G__verify_cached_stabs_test_cb via a pre-protected dirtied RO entry.
    {
      H5O_stab_t ro_pop_stab{};
      ASSERT_NE(H5O_msg_read(&(ro_pop_g->oloc), H5O_STAB_ID, &ro_pop_stab),
                nullptr);
      H5B_cache_ud_t ro_bt_ud{ro_f, H5B_SNODE, -1, H5F_grp_btree_shared(ro_f)};
      auto* ro_bt = static_cast<H5B_t*>(
          H5AC_protect(ro_f, H5AC_BT, ro_pop_stab.btree_addr, &ro_bt_ud,
                       H5AC__READ_ONLY_FLAG));
      ASSERT_NE(ro_bt, nullptr);
      const haddr_t ro_node_addr = ro_bt->child[0];
      ASSERT_GE(H5AC_unprotect(ro_f, H5AC_BT, ro_pop_stab.btree_addr, ro_bt,
                               H5AC__NO_FLAGS_SET),
                0);

      auto* ro_sn = static_cast<H5G_node_t*>(H5AC_protect(
          ro_f, H5AC_SNODE, ro_node_addr, ro_f, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(ro_sn, nullptr);
      haddr_t ro_d_addr = HADDR_UNDEF;
      for (unsigned i = 0; i < ro_sn->nsyms; ++i) {
        if (ro_sn->entry[i].type != H5G_CACHED_STAB) {
          ro_d_addr = ro_sn->entry[i].header;
          break;
        }
      }
      ASSERT_TRUE(H5_addr_defined(ro_d_addr));
      ASSERT_GE(H5AC_unprotect(ro_f, H5AC_SNODE, ro_node_addr, ro_sn,
                               H5AC__NO_FLAGS_SET),
                0);

      // 7c. H5O_unprotect fail on target object header -> 770 + 781 + 782.
      H5O_loc_t d_oloc{};
      H5O_loc_reset(&d_oloc);
      d_oloc.file = ro_f;
      d_oloc.addr = ro_d_addr;
      H5O_t* d_oh = H5O_protect(&d_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(d_oh, nullptr);
      ASSERT_EQ(d_oh->cache_info.ro_ref_count, 1u);
      ASSERT_EQ(d_oh->cache_info.size, d_oh->chunk[0].size);
      d_oh->cache_info.dirtied = true;
      EXPECT_LT(H5G__verify_cached_stabs_test(ro_pop), 0);
      d_oh->cache_info.dirtied = false;
      EXPECT_GE(H5O_unprotect(&d_oloc, d_oh, H5AC__NO_FLAGS_SET), 0);
      EXPECT_GE(H5O_unprotect(&d_oloc, d_oh, H5AC__NO_FLAGS_SET), 0);

      // 7d. H5AC_unprotect fail on symbol table node -> 776.
      ro_sn = static_cast<H5G_node_t*>(H5AC_protect(
          ro_f, H5AC_SNODE, ro_node_addr, ro_f, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(ro_sn, nullptr);
      ASSERT_EQ(ro_sn->cache_info.ro_ref_count, 1u);
      ASSERT_EQ(ro_sn->cache_info.size, ro_sn->node_size);
      ro_sn->cache_info.dirtied = true;
      EXPECT_LT(H5G__verify_cached_stabs_test(ro_pop), 0);
      ro_sn->cache_info.dirtied = false;
      EXPECT_GE(H5AC_unprotect(ro_f, H5AC_SNODE, ro_node_addr, ro_sn,
                               H5AC__NO_FLAGS_SET),
                0);
      EXPECT_GE(H5AC_unprotect(ro_f, H5AC_SNODE, ro_node_addr, ro_sn,
                               H5AC__NO_FLAGS_SET),
                0);
    }
  }

  // 8. Leaf H5G_node_t & B-tree child corruptions in /old_pop for 730, 745,
  //    758, 762, 766, 780.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_stab_t pop_stab{};
    ASSERT_NE(H5O_msg_read(&(old_pop_g->oloc), H5O_STAB_ID, &pop_stab),
              nullptr);
    H5B_cache_ud_t bt_ud{old_f, H5B_SNODE, -1, H5F_grp_btree_shared(old_f)};
    auto* bt = static_cast<H5B_t*>(H5AC_protect(
        old_f, H5AC_BT, pop_stab.btree_addr, &bt_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(bt, nullptr);
    ASSERT_EQ(bt->level, 0u);
    ASSERT_EQ(bt->nchildren, 1u);
    const haddr_t node_addr = bt->child[0];
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_BT, pop_stab.btree_addr, bt,
                             H5AC__NO_FLAGS_SET),
              0);

    auto* sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    ASSERT_EQ(sn->nsyms, 2u);
    int sub_idx = -1;
    int d_idx = -1;
    for (unsigned i = 0; i < sn->nsyms; ++i) {
      if (sn->entry[i].type == H5G_CACHED_STAB) {
        sub_idx = static_cast<int>(i);
      } else {
        d_idx = static_cast<int>(i);
      }
    }
    ASSERT_GE(sub_idx, 0);
    ASSERT_GE(d_idx, 0);

    const H5G_entry_t orig_sub_ent = sn->entry[sub_idx];
    const H5G_entry_t orig_d_ent = sn->entry[d_idx];
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);

    auto restore_entries = [&] {
      auto* s = static_cast<H5G_node_t*>(H5AC_protect(
          old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
      if (s) {
        s->entry[sub_idx] = orig_sub_ent;
        s->entry[d_idx] = orig_d_ent;
        H5AC_unprotect(old_f, H5AC_SNODE, node_addr, s, H5AC__NO_FLAGS_SET);
      }
    };
    absl::Cleanup clean_sn = restore_entries;

    // 8a. Target group entry not cached -> 758 + 780.
    sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[sub_idx].type = H5G_NOTHING_CACHED;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5G__verify_cached_stabs_test(old_pop), 0);

    // 8b. Target group entry cached btree_addr mismatch -> 762 + 780.
    sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[sub_idx] = orig_sub_ent;
    sn->entry[sub_idx].cache.stab.btree_addr += 1;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5G__verify_cached_stabs_test(old_pop), 0);

    // 8c. Dataset entry (non-group) claiming H5G_CACHED_STAB -> 766 + 780.
    sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[sub_idx] = orig_sub_ent;
    sn->entry[d_idx].type = H5G_CACHED_STAB;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5G__verify_cached_stabs_test(old_pop), 0);

    // 8d. Target object header bad address -> 745.
    sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[d_idx] = orig_d_ent;
    sn->entry[d_idx].header = kBadAddr;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5G__verify_cached_stabs_test(old_pop), 0);

    // 8e. B-tree leaf child bad address -> H5G__verify_cached_stabs_test_cb
    //     cannot protect symbol table node (730).
    sn = static_cast<H5G_node_t*>(
        H5AC_protect(old_f, H5AC_SNODE, node_addr, old_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[d_idx] = orig_d_ent;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);

    bt = static_cast<H5B_t*>(H5AC_protect(
        old_f, H5AC_BT, pop_stab.btree_addr, &bt_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(bt, nullptr);
    bt->child[0] = kBadAddr;
    ASSERT_GE(H5AC_unprotect(old_f, H5AC_BT, pop_stab.btree_addr, bt,
                             H5AC__NO_FLAGS_SET),
              0);
    absl::Cleanup restore_bt = [&] {
      auto* b = static_cast<H5B_t*>(H5AC_protect(
          old_f, H5AC_BT, pop_stab.btree_addr, &bt_ud, H5AC__READ_ONLY_FLAG));
      if (b) {
        b->child[0] = node_addr;
        H5AC_unprotect(old_f, H5AC_BT, pop_stab.btree_addr, b,
                       H5AC__NO_FLAGS_SET);
      }
    };
    EXPECT_LT(H5G__verify_cached_stabs_test(old_pop), 0);
  }
}

}  // namespace
}  // namespace hdf5
