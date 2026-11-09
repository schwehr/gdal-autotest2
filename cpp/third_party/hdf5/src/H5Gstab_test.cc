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
// Tests old-style symbol-table group operations: creation, heap-hint sizing,
// insertion/splitting, iteration, indexing, lookup, removal, deletion, and
// symbol table message validation (third_party/hdf5/src/H5Gstab.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gstab.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across all
// internal headers included after hdf5_internal_prelude.h.
#ifndef NDEBUG
#define H5G_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B_FRIEND
#define H5G_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5Bpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5G_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5G_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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

ScopedFile CreateOldFormatFile(const char* name = "old_stab.h5") {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST) <
      0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
}

ScopedFile CreateNewFormatFile(const char* name = "new_fmt.h5") {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_LATEST) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
}

herr_t CollectLinkNamesCb(const H5O_link_t* lnk, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->push_back(lnk->name);
  return 0;
}

herr_t StopAfterOneCb(const H5O_link_t* lnk, void* op_data) {
  auto* count = static_cast<int*>(op_data);
  (*count)++;
  return 1;
}

herr_t ErrorIterCb(const H5O_link_t* /*lnk*/, void* /*op_data*/) { return -1; }

H5O_loc_t* GetGroupOloc(hid_t grp_id) {
  auto* grp = static_cast<H5G_t*>(H5VL_object_verify(grp_id, H5I_GROUP));
  return grp ? H5G_oloc(grp) : nullptr;
}

TEST(H5GstabTest, CreateWithDefaultSmallAndLargeHeapSizeHints) {
  ScopedFile file = CreateOldFormatFile();
  ASSERT_TRUE(file.valid());

  // Default lheap_size_hint == 0 (computes heap_hint from est_num_entries).
  ScopedGroup g_def(
      H5Gcreate2(file, "/g_def", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_def.valid());

  // Small non-zero lheap_size_hint clamped to H5HL_SIZEOF_FREE(f) + 2.
  ScopedPlist gcpl_small(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_small.valid());
  ASSERT_GE(H5Pset_local_heap_size_hint(gcpl_small, 4), 0);
  ScopedGroup g_small(
      H5Gcreate2(file, "/g_small", H5P_DEFAULT, gcpl_small, H5P_DEFAULT));
  ASSERT_TRUE(g_small.valid());

  // Larger lheap_size_hint preserved.
  ScopedPlist gcpl_large(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_large.valid());
  ASSERT_GE(H5Pset_local_heap_size_hint(gcpl_large, 4096), 0);
  ScopedGroup g_large(
      H5Gcreate2(file, "/g_large", H5P_DEFAULT, gcpl_large, H5P_DEFAULT));
  ASSERT_TRUE(g_large.valid());

  H5G_info_t info{};
  ASSERT_GE(H5Gget_info(g_small, &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  ASSERT_GE(H5Gget_info(g_large, &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
}

TEST(H5GstabTest, SplitNodesIterateLookupIndexAndRemove) {
  ScopedFile file = CreateOldFormatFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "/stab", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Insert >40 links so symbol table leaf nodes (default 2*K = 32) split.
  std::vector<std::string> expected_names;
  for (int i = 0; i < 42; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "entry_%02d_long_name", i);
    expected_names.emplace_back(buf);
    if (i % 3 == 0) {
      ScopedGroup sub(
          H5Gcreate2(grp, buf, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(sub.valid());
    } else if (i % 3 == 1) {
      ScopedSpace space(H5Screate(H5S_SCALAR));
      ASSERT_TRUE(space.valid());
      ScopedDataset ds(H5Dcreate2(grp, buf, H5T_NATIVE_INT, space, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(ds.valid());
    } else {
      ASSERT_GE(
          H5Lcreate_soft("/target", grp, buf, H5P_DEFAULT, H5P_DEFAULT), 0);
    }
  }
  std::sort(expected_names.begin(), expected_names.end());

  H5O_loc_t* oloc = GetGroupOloc(grp);
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  hsize_t count = 0;
  ASSERT_GE(H5G__stab_count(oloc, &count), 0);
  EXPECT_EQ(count, expected_names.size());

  // Iterate INC, DEC, skip > 0, stop early, error callback, out-of-bounds.
  {
    std::vector<std::string> inc_names;
    hsize_t last_lnk = 0;
    EXPECT_EQ(H5G__stab_iterate(oloc, H5_ITER_INC, 0, &last_lnk,
                                CollectLinkNamesCb, &inc_names),
              0);
    EXPECT_EQ(inc_names, expected_names);
    EXPECT_EQ(last_lnk, expected_names.size());
  }
  {
    std::vector<std::string> dec_names;
    hsize_t last_lnk = 0;
    EXPECT_EQ(H5G__stab_iterate(oloc, H5_ITER_DEC, 5, &last_lnk,
                                CollectLinkNamesCb, &dec_names),
              0);
    std::vector<std::string> expected_rev(expected_names.rbegin() + 5,
                                          expected_names.rend());
    EXPECT_EQ(dec_names, expected_rev);
    EXPECT_EQ(last_lnk, expected_names.size());
  }
  {
    int stop_count = 0;
    hsize_t last_lnk = 0;
    EXPECT_EQ(H5G__stab_iterate(oloc, H5_ITER_NATIVE, 0, &last_lnk,
                                StopAfterOneCb, &stop_count),
              1);
    EXPECT_EQ(stop_count, 1);

    hsize_t dec_last = 0;
    stop_count = 0;
    EXPECT_EQ(H5G__stab_iterate(oloc, H5_ITER_DEC, 0, &dec_last, StopAfterOneCb,
                                &stop_count),
              1);
    EXPECT_EQ(stop_count, 1);
  }
  {
    ScopedErrorSilence silence;
    int dummy = 0;
    hsize_t last_lnk = 0;
    EXPECT_LT(H5G__stab_iterate(oloc, H5_ITER_INC, 0, &last_lnk, ErrorIterCb,
                                &dummy),
              0);
    EXPECT_LT(H5G__stab_iterate(oloc, H5_ITER_DEC, 0, &last_lnk, ErrorIterCb,
                                &dummy),
              0);
    last_lnk = 0;
    EXPECT_LT(
        H5G__stab_iterate(oloc, H5_ITER_INC, expected_names.size(), &last_lnk,
                          CollectLinkNamesCb, &dummy),
        0);
    last_lnk = 0;
    EXPECT_LT(
        H5G__stab_iterate(oloc, H5_ITER_DEC, expected_names.size(), &last_lnk,
                          CollectLinkNamesCb, &dummy),
        0);
  }

  // get_name_by_idx: INC, DEC, length-only (nullptr), truncated, out-of-bounds.
  {
    std::array<char, 64> name_buf{};
    size_t name_len = 0;
    ASSERT_GE(H5G__stab_get_name_by_idx(oloc, H5_ITER_INC, 0, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
    EXPECT_STREQ(name_buf.data(), expected_names.front().c_str());
    EXPECT_EQ(name_len, expected_names.front().size());

    ASSERT_GE(H5G__stab_get_name_by_idx(oloc, H5_ITER_DEC, 0, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
    EXPECT_STREQ(name_buf.data(), expected_names.back().c_str());

    size_t query_len = 0;
    ASSERT_GE(
        H5G__stab_get_name_by_idx(oloc, H5_ITER_INC, 3, nullptr, 0, &query_len),
        0);
    EXPECT_EQ(query_len, expected_names[3].size());

    std::array<char, 6> small_buf{};
    ASSERT_GE(H5G__stab_get_name_by_idx(oloc, H5_ITER_INC, 3, small_buf.data(),
                                        small_buf.size(), &query_len),
              0);
    EXPECT_STREQ(small_buf.data(), expected_names[3].substr(0, 5).c_str());

    ScopedErrorSilence silence;
    EXPECT_LT(H5G__stab_get_name_by_idx(oloc, H5_ITER_INC, 999, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
  }

  // lookup (found hard group, found soft link, missing) & lookup_by_idx.
  {
    bool found = false;
    H5O_link_t lnk{};
    ASSERT_GE(H5G__stab_lookup(oloc, expected_names[2].c_str(), &found, &lnk),
              0);
    ASSERT_TRUE(found);
    EXPECT_EQ(lnk.type, H5L_TYPE_SOFT);
    EXPECT_STREQ(lnk.u.soft.name, "/target");
    H5O_msg_reset(H5O_LINK_ID, &lnk);

    found = true;
    lnk = H5O_link_t{};
    ASSERT_GE(H5G__stab_lookup(oloc, "does_not_exist", &found, &lnk), 0);
    EXPECT_FALSE(found);

    lnk = H5O_link_t{};
    ASSERT_GE(H5G__stab_lookup_by_idx(oloc, H5_ITER_INC, 0, &lnk), 0);
    EXPECT_STREQ(lnk.name, expected_names.front().c_str());
    H5O_msg_reset(H5O_LINK_ID, &lnk);

    lnk = H5O_link_t{};
    ASSERT_GE(H5G__stab_lookup_by_idx(oloc, H5_ITER_DEC, 0, &lnk), 0);
    EXPECT_STREQ(lnk.name, expected_names.back().c_str());
    H5O_msg_reset(H5O_LINK_ID, &lnk);

    ScopedErrorSilence silence;
    lnk = H5O_link_t{};
    EXPECT_LT(H5G__stab_lookup_by_idx(oloc, H5_ITER_INC, 999, &lnk), 0);
  }

  // remove and remove_by_idx (INC & DEC), plus out-of-bounds error.
  ASSERT_GE(H5G__stab_remove(oloc, nullptr, expected_names[2].c_str()), 0);
  ASSERT_GE(H5G__stab_remove_by_idx(oloc, nullptr, H5_ITER_DEC, 1), 0);
  ASSERT_GE(H5G__stab_remove_by_idx(oloc, nullptr, H5_ITER_INC, 0), 0);

  hsize_t after_count = 0;
  ASSERT_GE(H5G__stab_count(oloc, &after_count), 0);
  EXPECT_EQ(after_count, expected_names.size() - 3);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G__stab_remove(oloc, nullptr, "not_present"), 0);
    EXPECT_LT(H5G__stab_remove_by_idx(oloc, nullptr, H5_ITER_INC, 999), 0);
  }
}

TEST(H5GstabTest, StandaloneComponentsBhSizeDeleteAndNonStabErrors) {
  ScopedFile old_file = CreateOldFormatFile("comp.h5");
  ScopedFile new_file = CreateNewFormatFile("new_no_stab.h5");
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(old_file, H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Direct H5G__stab_create_components + insert_real + bh_size + delete.
  H5O_stab_t stab{};
  ASSERT_GE(H5G__stab_create_components(f, &stab, 64), 0);

  H5O_link_t soft_lnk{};
  char link_name[] = "standalone_soft";
  char target_name[] = "/some/target";
  soft_lnk.type = H5L_TYPE_SOFT;
  soft_lnk.corder_valid = false;
  soft_lnk.corder = 0;
  soft_lnk.cset = H5T_CSET_ASCII;
  soft_lnk.name = link_name;
  soft_lnk.u.soft.name = target_name;
  ASSERT_GE(H5G__stab_insert_real(f, &stab, &soft_lnk, H5O_TYPE_UNKNOWN, nullptr),
            0);

  H5_ih_info_t bh_info{};
  ASSERT_GE(H5G__stab_bh_size(f, &stab, &bh_info), 0);
  EXPECT_GT(bh_info.index_size, 0u);
  EXPECT_GT(bh_info.heap_size, 0u);

  ASSERT_GE(H5G__stab_delete(f, &stab), 0);

  // Also trigger H5G__stab_delete via public H5Ldelete of an old-style group.
  {
    ScopedGroup doomed(
        H5Gcreate2(old_file, "/doomed", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(doomed.valid());
    ASSERT_GE(
        H5Lcreate_soft("/x", doomed, "child", H5P_DEFAULT, H5P_DEFAULT), 0);
  }
  ASSERT_GE(H5Ldelete(old_file, "/doomed", H5P_DEFAULT), 0);

  // Missing STAB message errors on a new-format compact group.
  ScopedGroup new_grp(H5Gcreate2(new_file, "/compact", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
  ASSERT_TRUE(new_grp.valid());
  H5O_loc_t* new_oloc = GetGroupOloc(new_grp);
  ASSERT_NE(new_oloc, nullptr);

  ScopedErrorSilence silence;
  hsize_t dummy_cnt = 0;
  hsize_t last_lnk = 0;
  size_t name_len = 0;
  bool found = false;
  H5O_link_t lnk{};
  EXPECT_LT(H5G__stab_insert(new_oloc, &soft_lnk, H5O_TYPE_UNKNOWN, nullptr),
            0);
  EXPECT_LT(H5G__stab_remove(new_oloc, nullptr, "x"), 0);
  EXPECT_LT(H5G__stab_remove_by_idx(new_oloc, nullptr, H5_ITER_INC, 0), 0);
  EXPECT_LT(H5G__stab_iterate(new_oloc, H5_ITER_INC, 0, &last_lnk,
                              CollectLinkNamesCb, &found),
            0);
  EXPECT_LT(H5G__stab_count(new_oloc, &dummy_cnt), 0);
  EXPECT_LT(H5G__stab_get_name_by_idx(new_oloc, H5_ITER_INC, 0, nullptr, 0,
                                      &name_len),
            0);
  EXPECT_LT(H5G__stab_lookup(new_oloc, "x", &found, &lnk), 0);
  EXPECT_LT(H5G__stab_lookup_by_idx(new_oloc, H5_ITER_INC, 0, &lnk), 0);
  EXPECT_LT(H5G__stab_valid(new_oloc, &stab), 0);
}

TEST(H5GstabTest, StabValidRepairsCorruptBtreeOrHeapAndReportsInvalid) {
  ScopedFile file = CreateOldFormatFile("repair.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "/repair", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(H5Lcreate_soft("/t", grp, "item", H5P_DEFAULT, H5P_DEFAULT), 0);

  H5O_loc_t* oloc = GetGroupOloc(grp);
  ASSERT_NE(oloc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_stab_t orig_stab{};
  ASSERT_NE(H5O_msg_read(oloc, H5O_STAB_ID, &orig_stab), nullptr);

  // Valid stab passes directly.
  EXPECT_GE(H5G__stab_valid(oloc, &orig_stab), 0);

  // Corrupt group header btree_addr; invalid alt fails, valid alt repairs it.
  H5O_stab_t bad_stab = orig_stab;
  bad_stab.btree_addr = HADDR_UNDEF;
  ASSERT_GE(H5O_msg_write(oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_stab),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G__stab_valid(oloc, &bad_stab), 0);
  }
  EXPECT_GE(H5G__stab_valid(oloc, &orig_stab), 0);

  // Corrupt group header heap_addr; invalid alt fails, valid alt repairs it.
  bad_stab = orig_stab;
  bad_stab.heap_addr = HADDR_UNDEF;
  ASSERT_GE(H5O_msg_write(oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_stab),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G__stab_valid(oloc, &bad_stab), 0);
  }
  EXPECT_GE(H5G__stab_valid(oloc, &orig_stab), 0);

  H5O_stab_t verified{};
  ASSERT_NE(H5O_msg_read(oloc, H5O_STAB_ID, &verified), nullptr);
  EXPECT_EQ(verified.btree_addr, orig_stab.btree_addr);
  EXPECT_EQ(verified.heap_addr, orig_stab.heap_addr);
}

std::vector<uint8_t> ExtractStabFileImage(hid_t file_id) {
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

ScopedFile OpenStabFileImage(const std::vector<uint8_t>& image, unsigned flags,
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

TEST(H5GstabTest, InternalCorruptionAndReadOnlyErrors) {
  constexpr haddr_t kBadAddr = 0xdeadbeef00ULL;

  ScopedFile rw_file = CreateOldFormatFile("corrupt_stab.h5");
  ASSERT_TRUE(rw_file.valid());

  ScopedGroup grp(
      H5Gcreate2(rw_file, "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(H5Lcreate_soft("/t", grp, "s", H5P_DEFAULT, H5P_DEFAULT), 0);
  {
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(grp, "h", H5T_NATIVE_INT, space, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  ScopedGroup scratch(
      H5Gcreate2(rw_file, "/scratch", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(scratch.valid());
  ASSERT_GE(H5Lcreate_soft("/t", scratch, "z", H5P_DEFAULT, H5P_DEFAULT), 0);

  ASSERT_GE(H5Fflush(rw_file, H5F_SCOPE_GLOBAL), 0);
  std::vector<uint8_t> ro_img = ExtractStabFileImage(rw_file);
  ASSERT_FALSE(ro_img.empty());
  ScopedFile ro_file = OpenStabFileImage(ro_img, H5F_ACC_RDONLY, "ro_stab.h5");
  ASSERT_TRUE(ro_file.valid());

  auto* rw_f = static_cast<H5F_t*>(H5VL_object_verify(rw_file, H5I_FILE));
  auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
  ASSERT_NE(rw_f, nullptr);
  ASSERT_NE(ro_f, nullptr);

  ScopedGroup ro_grp(H5Gopen2(ro_file, "/g", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());
  H5O_loc_t* ro_oloc = GetGroupOloc(ro_grp);
  H5O_loc_t* g_oloc = GetGroupOloc(grp);
  H5O_loc_t* scr_oloc = GetGroupOloc(scratch);
  ASSERT_NE(ro_oloc, nullptr);
  ASSERT_NE(g_oloc, nullptr);
  ASSERT_NE(scr_oloc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_stab_t orig_stab{};
  ASSERT_NE(H5O_msg_read(g_oloc, H5O_STAB_ID, &orig_stab), nullptr);
  H5O_stab_t scr_stab{};
  ASSERT_NE(H5O_msg_read(scr_oloc, H5O_STAB_ID, &scr_stab), nullptr);

  {
    ScopedErrorSilence silence;

    // 1. Read-only create failures & invalid create args.
    H5O_stab_t tmp_stab{};
    EXPECT_LT(H5G__stab_create_components(ro_f, &tmp_stab, 64), 0);
    H5O_ginfo_t ginfo = H5G_CRT_GROUP_INFO_DEF;
    EXPECT_LT(H5G__stab_create(ro_oloc, &ginfo, &tmp_stab), 0);

    // 2. Duplicate link insert and bad-heap direct insert.
    H5O_link_t dup_lnk{};
    dup_lnk.type = H5L_TYPE_SOFT;
    dup_lnk.cset = H5T_CSET_ASCII;
    dup_lnk.name = const_cast<char*>("s");
    dup_lnk.u.soft.name = const_cast<char*>("/other");
    EXPECT_LT(
        H5G__stab_insert(g_oloc, &dup_lnk, H5O_TYPE_DATASET, nullptr), 0);

    H5O_stab_t bad_heap_stab = orig_stab;
    bad_heap_stab.heap_addr = kBadAddr;
    EXPECT_LT(H5G__stab_insert_real(rw_f, &bad_heap_stab, &dup_lnk,
                                    H5O_TYPE_DATASET, nullptr),
              0);

    // 3. stab_delete errors without modifying /scratch's persistent state.
    EXPECT_LT(H5G__stab_delete(rw_f, &bad_heap_stab), 0);
    H5O_stab_t scr_bad_bt = scr_stab;
    scr_bad_bt.btree_addr = kBadAddr;
    EXPECT_LT(H5G__stab_delete(rw_f, &scr_bad_bt), 0);

    // 4. stab_bh_size bad btree and bad heap errors.
    H5_ih_info_t bh_info{};
    H5O_stab_t bad_bt_stab = orig_stab;
    bad_bt_stab.btree_addr = kBadAddr;
    EXPECT_LT(H5G__stab_bh_size(rw_f, &bad_bt_stab, &bh_info), 0);
    EXPECT_LT(H5G__stab_bh_size(rw_f, &bad_heap_stab, &bh_info), 0);

    // 5. Corrupt group stab.heap_addr -> protect fails across stab helpers.
    absl::Cleanup restore_stab = [&] {
      H5O_msg_write(g_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &orig_stab);
    };
    ASSERT_GE(H5O_msg_write(g_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE,
                            &bad_heap_stab),
              0);
    EXPECT_LT(H5G__stab_remove(g_oloc, nullptr, "s"), 0);
    std::vector<std::string> names;
    hsize_t last_lnk = 0;
    EXPECT_LT(H5G__stab_iterate(g_oloc, H5_ITER_INC, 0, &last_lnk,
                                CollectLinkNamesCb, &names),
              0);
    std::array<char, 64> name_buf{};
    size_t name_len = 0;
    EXPECT_LT(H5G__stab_get_name_by_idx(g_oloc, H5_ITER_INC, 0, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
    bool found = false;
    H5O_link_t lnk{};
    EXPECT_LT(H5G__stab_lookup(g_oloc, "s", &found, &lnk), 0);
    EXPECT_LT(H5G__stab_lookup_by_idx(g_oloc, H5_ITER_INC, 0, &lnk), 0);

    // 6. Corrupt group stab.btree_addr -> iterate/count/index/lookup errors.
    ASSERT_GE(
        H5O_msg_write(g_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_bt_stab),
        0);
    EXPECT_LT(H5G__stab_iterate(g_oloc, H5_ITER_DEC, 0, &last_lnk,
                                CollectLinkNamesCb, &names),
              0);
    hsize_t cnt = 0;
    EXPECT_LT(H5G__stab_count(g_oloc, &cnt), 0);
    EXPECT_LT(H5G__stab_get_name_by_idx(g_oloc, H5_ITER_DEC, 0, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
    EXPECT_LT(H5G__stab_get_name_by_idx(g_oloc, H5_ITER_INC, 0, name_buf.data(),
                                        name_buf.size(), &name_len),
              0);
    EXPECT_LT(H5G__stab_lookup(g_oloc, "s", &found, &lnk), 0);
    EXPECT_LT(H5G__stab_lookup_by_idx(g_oloc, H5_ITER_DEC, 0, &lnk), 0);
    EXPECT_LT(H5G__stab_lookup_by_idx(g_oloc, H5_ITER_INC, 0, &lnk), 0);
  }

  // 7. Reopen an image whose /g header has a bad btree_addr in RDONLY mode to
  //    exercise H5G__stab_valid's write-back failure when repairing.
  {
    std::vector<uint8_t> corrupt_img;
    {
      ScopedFile tmp_f = CreateOldFormatFile("valid_ro_wr.h5");
      ASSERT_TRUE(tmp_f.valid());
      ScopedGroup tmp_g(
          H5Gcreate2(tmp_f, "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(tmp_g.valid());
      ASSERT_GE(H5Lcreate_soft("/t", tmp_g, "s", H5P_DEFAULT, H5P_DEFAULT), 0);
      H5O_loc_t* tmp_oloc = GetGroupOloc(tmp_g);
      H5O_stab_t tmp_orig{};
      ASSERT_NE(H5O_msg_read(tmp_oloc, H5O_STAB_ID, &tmp_orig), nullptr);
      H5O_stab_t tmp_bad = tmp_orig;
      tmp_bad.btree_addr = kBadAddr;
      ASSERT_GE(H5O_msg_write(tmp_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE,
                              &tmp_bad),
                0);
      tmp_g.reset();
      ASSERT_GE(H5Fflush(tmp_f, H5F_SCOPE_GLOBAL), 0);
      corrupt_img = ExtractStabFileImage(tmp_f);
      ASSERT_FALSE(corrupt_img.empty());

      ScopedFile ro_rep_f =
          OpenStabFileImage(corrupt_img, H5F_ACC_RDONLY, "ro_rep.h5");
      ASSERT_TRUE(ro_rep_f.valid());
      ScopedGroup ro_rep_g(H5Gopen2(ro_rep_f, "/g", H5P_DEFAULT));
      ASSERT_TRUE(ro_rep_g.valid());
      H5O_loc_t* ro_rep_oloc = GetGroupOloc(ro_rep_g);
      ASSERT_NE(ro_rep_oloc, nullptr);
      ScopedErrorSilence silence;
      EXPECT_LT(H5G__stab_valid(ro_rep_oloc, &tmp_orig), 0);
    }
  }

  // 8. Corrupt symbol-table leaf entry offsets in /scratch (1 soft link "z")
  //    for lookup, lookup_by_idx, and get_name_by_idx conversion/name errors.
  {
    H5B_cache_ud_t bt_ud{rw_f, H5B_SNODE, -1, H5F_grp_btree_shared(rw_f)};
    auto* bt = static_cast<H5B_t*>(H5AC_protect(
        rw_f, H5AC_BT, scr_stab.btree_addr, &bt_ud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(bt, nullptr);
    ASSERT_EQ(bt->level, 0u);
    ASSERT_EQ(bt->nchildren, 1u);
    const haddr_t node_addr = bt->child[0];
    ASSERT_GE(H5AC_unprotect(rw_f, H5AC_BT, scr_stab.btree_addr, bt,
                             H5AC__NO_FLAGS_SET),
              0);

    auto* sn = static_cast<H5G_node_t*>(
        H5AC_protect(rw_f, H5AC_SNODE, node_addr, rw_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    ASSERT_EQ(sn->nsyms, 1u);
    const size_t orig_lval_off = sn->entry[0].cache.slink.lval_offset;
    const size_t orig_name_off = sn->entry[0].name_off;
    sn->entry[0].cache.slink.lval_offset = 999999;
    ASSERT_GE(H5AC_unprotect(rw_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);

    ScopedErrorSilence silence;
    bool found = false;
    H5O_link_t lnk{};
    EXPECT_LT(H5G__stab_lookup(scr_oloc, "z", &found, &lnk), 0);
    EXPECT_LT(H5G__stab_lookup_by_idx(scr_oloc, H5_ITER_INC, 0, &lnk), 0);

    sn = static_cast<H5G_node_t*>(
        H5AC_protect(rw_f, H5AC_SNODE, node_addr, rw_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[0].cache.slink.lval_offset = orig_lval_off;
    sn->entry[0].name_off = 999999;
    ASSERT_GE(H5AC_unprotect(rw_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);

    std::array<char, 64> name_buf{};
    size_t name_len = 0;
    EXPECT_LT(H5G__stab_get_name_by_idx(scr_oloc, H5_ITER_INC, 0,
                                        name_buf.data(), name_buf.size(),
                                        &name_len),
              0);

    sn = static_cast<H5G_node_t*>(
        H5AC_protect(rw_f, H5AC_SNODE, node_addr, rw_f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    sn->entry[0].name_off = orig_name_off;
    ASSERT_GE(H5AC_unprotect(rw_f, H5AC_SNODE, node_addr, sn,
                             H5AC__NO_FLAGS_SET),
              0);
  }
}

}  // namespace
}  // namespace hdf5
