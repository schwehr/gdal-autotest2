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
// Tests dense group link storage (fractal heap + v2 B-tree name/corder
// indexes, filter pipeline, long link buffers, index/table lookups, iteration,
// removal, shrink to compact, deletion, and internal error paths) for
// third_party/hdf5/src/H5Gdense.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gdense.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from cache
// entries in internal headers. Match those struct layouts here.
#ifndef NDEBUG
#define H5GDENSE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

// H5B2private.h (pulled via H5Gpkg.h) uses `try` as a C parameter name, which
// collides with the C++ keyword and Google3's `-fno-exceptions` `try` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5GDENSE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GDENSE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::IsEmpty;
using ::testing::SizeIs;

// RAII wrapper for pushing/popping an HDF5 API context.
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

ScopedFile CreateLatestInMemoryFile() {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid() ||
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
          0) {
    return ScopedFile();
  }
  return ScopedFile(
      H5Fcreate("h5gdense_test.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
}

ScopedPlist MakeDenseGcpl(unsigned max_compact, unsigned min_dense,
                          unsigned crt_order_flags,
                          bool with_filter = false) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_link_phase_change(gcpl.get(), max_compact, min_dense) < 0) {
    gcpl.reset();
    return gcpl;
  }
  if (crt_order_flags != 0 &&
      H5Pset_link_creation_order(gcpl.get(), crt_order_flags) < 0) {
    gcpl.reset();
    return gcpl;
  }
  if (with_filter && H5Pset_fletcher32(gcpl.get()) < 0) {
    gcpl.reset();
    return gcpl;
  }
  return gcpl;
}

TEST(H5GdenseTest, IndexedCorderAndFilterPipelineOperations) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGcpl(
      /*max_compact=*/4, /*min_dense=*/2,
      H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED, /*with_filter=*/true);
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "dense_idx", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Populate > max_compact links, including one > 128 bytes to exercise
  // H5WB_actual / H5WB_unwrap dynamic buffer allocation in H5G__dense_insert.
  const std::string long_name = "z_long_link_" + std::string(180, 'a');
  const std::string long_target = "/target_" + std::string(180, 'b');
  ASSERT_GE(H5Lcreate_soft(long_target.c_str(), grp.get(), long_name.c_str(),
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Keep `m_open_group` open across its eventual deletion to trigger
  // H5G__link_name_replace in both remove and remove_by_idx paths.
  ScopedGroup open_target(H5Gcreate2(grp.get(), "m_open_group", H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(open_target.valid());
  ScopedGroup open_idx_target(H5Gcreate2(grp.get(), "d_open_by_idx", H5P_DEFAULT,
                                         H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(open_idx_target.valid());

  for (int i = 0; i < 8; ++i) {
    ScopedGroup child(
        H5Gcreate2(grp.get(), absl::StrFormat("link_%02d", i).c_str(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(child.valid());
  }

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(ginfo.nlinks, 11u);

  // H5G__dense_lookup: present, long link, and absent.
  EXPECT_EQ(H5Lexists(grp.get(), long_name.c_str(), H5P_DEFAULT), 1);
  EXPECT_EQ(H5Lexists(grp.get(), "absent_link", H5P_DEFAULT), 0);
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(grp.get(), long_name.c_str(), &linfo, H5P_DEFAULT), 0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  // H5G__dense_lookup_by_idx across direct B-tree and sorted table paths.
  ASSERT_GE(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                                0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);
  ASSERT_GE(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_DEC,
                                0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);
  ASSERT_GE(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_NAME, H5_ITER_NATIVE,
                                0, &linfo, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);  // "d_open_by_idx"

  // H5G__dense_get_name_by_idx: NULL name buffer, exact buffer, and truncated
  // buffer on BOTH B-tree path (CRT_ORDER/INC, NAME/NATIVE) and table path
  // (NAME/INC, NAME/DEC).
  EXPECT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                               0, nullptr, 0, H5P_DEFAULT),
            static_cast<ssize_t>(long_name.size()));
  EXPECT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               nullptr, 0, H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("d_open_by_idx")));

  char trunc_bt2[6]{};
  EXPECT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                               1, trunc_bt2, sizeof(trunc_bt2), H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("m_open_group")));
  EXPECT_STREQ(trunc_bt2, "m_ope");

  char trunc_tbl[5]{};
  EXPECT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               trunc_tbl, sizeof(trunc_tbl), H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("d_open_by_idx")));
  EXPECT_STREQ(trunc_tbl, "d_op");

  char full_buf[256]{};
  ASSERT_GT(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                               full_buf, sizeof(full_buf), H5P_DEFAULT),
            0);

  // H5G__dense_iterate: NATIVE (with skip > 0), INC/DEC (table path), early
  // stop (> 0), and callback failure (< 0).
  std::vector<std::string> visited;
  auto collect_cb = [](hid_t, const char* name, const H5L_info2_t*,
                       void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return H5_ITER_CONT;
  };
  hsize_t iter_idx = 2;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &iter_idx,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited, SizeIs(9));

  visited.clear();
  iter_idx = 0;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_NATIVE,
                        &iter_idx, collect_cb, &visited),
            0);
  EXPECT_THAT(visited, SizeIs(11));

  visited.clear();
  iter_idx = 3;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_DEC, &iter_idx,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited, SizeIs(8));

  auto stop_cb = [](hid_t, const char*, const H5L_info2_t*, void*) -> herr_t {
    return 1;
  };
  iter_idx = 0;
  EXPECT_EQ(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &iter_idx,
                        stop_cb, nullptr),
            1);

  {
    ScopedErrorSilence silence;
    auto err_cb = [](hid_t, const char*, const H5L_info2_t*,
                     void*) -> herr_t { return -1; };
    iter_idx = 0;
    EXPECT_LT(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_NATIVE, &iter_idx,
                          err_cb, nullptr),
              0);
    iter_idx = 0;
    EXPECT_LT(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &iter_idx,
                          err_cb, nullptr),
              0);

    // Out-of-bounds queries across B-tree and table paths.
    EXPECT_LT(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_CRT_ORDER,
                                  H5_ITER_INC, 11, &linfo, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                  11, &linfo, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER,
                                 H5_ITER_INC, 11, full_buf, sizeof(full_buf),
                                 H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                 11, full_buf, sizeof(full_buf), H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                               11, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 11,
                               H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ldelete(grp.get(), "missing_dense_link", H5P_DEFAULT), 0);
  }

  // Exercise all removal paths:
  // 1) H5G__dense_remove (with open target for name replace)
  ASSERT_GE(H5Ldelete(grp.get(), "m_open_group", H5P_DEFAULT), 0);
  open_target.reset();
  // 2) H5G__dense_remove_by_idx via table path (NAME + INC removes
  //    "d_open_by_idx", still open)
  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  open_idx_target.reset();
  // 3) H5G__dense_remove_by_idx via B-tree path with NAME/NATIVE (removes from
  //    corder "other" index) and CRT_ORDER/INC (removes from name "other"
  //    index).
  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);

  // Shrink below min_dense (= 2): deleting down to 1 link transitions dense ->
  // compact (H5G__dense_delete(..., adj_link = false) with index_corder=true).
  for (int i = 0; i < 6; ++i) {
    ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                               0, H5P_DEFAULT),
              0);
  }
  ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(ginfo.nlinks, 1u);

  // Also test H5G__dense_delete(..., adj_link = true) on a populated
  // creation-order-indexed dense group.
  ScopedGroup to_delete(H5Gcreate2(file.get(), "dense_to_delete", H5P_DEFAULT,
                                   gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(to_delete.valid());
  for (int i = 0; i < 6; ++i) {
    ScopedGroup sub(
        H5Gcreate2(to_delete.get(), absl::StrFormat("sub_%02d", i).c_str(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
  }
  to_delete.reset();
  ASSERT_GE(H5Ldelete(file.get(), "dense_to_delete", H5P_DEFAULT), 0);
}

TEST(H5GdenseTest, TrackedUnindexedAndUntrackedDenseGroups) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Creation order TRACKED but NOT INDEXED: exercises table fallbacks for
  // H5_INDEX_CRT_ORDER (INC/DEC) and native fallback to name_bt2_addr.
  ScopedPlist tracked_gcpl =
      MakeDenseGcpl(/*max_compact=*/3, /*min_dense=*/2, H5P_CRT_ORDER_TRACKED);
  ASSERT_TRUE(tracked_gcpl.valid());
  ScopedGroup tracked(H5Gcreate2(file.get(), "tracked_unindexed", H5P_DEFAULT,
                                 tracked_gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(tracked.valid());

  for (int i = 0; i < 6; ++i) {
    ScopedGroup sub(
        H5Gcreate2(tracked.get(), absl::StrFormat("k_%02d", 5 - i).c_str(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
  }

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(tracked.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(ginfo.nlinks, 6u);

  // H5_INDEX_CRT_ORDER + H5_ITER_INC uses build_table; H5_ITER_NATIVE uses
  // name_bt2_addr fallback in lookup_by_idx, get_name_by_idx, and iterate.
  char name_buf[32]{};
  ASSERT_GT(H5Lget_name_by_idx(tracked.get(), ".", H5_INDEX_CRT_ORDER,
                               H5_ITER_INC, 0, name_buf, sizeof(name_buf),
                               H5P_DEFAULT),
            0);
  EXPECT_STREQ(name_buf, "k_05");
  ASSERT_GT(H5Lget_name_by_idx(tracked.get(), ".", H5_INDEX_CRT_ORDER,
                               H5_ITER_NATIVE, 0, name_buf, sizeof(name_buf),
                               H5P_DEFAULT),
            0);

  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info_by_idx2(tracked.get(), ".", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, 5, &linfo, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lget_info_by_idx2(tracked.get(), ".", H5_INDEX_CRT_ORDER,
                                H5_ITER_NATIVE, 0, &linfo, H5P_DEFAULT),
            0);

  int count = 0;
  hsize_t idx = 0;
  auto count_cb = [](hid_t, const char*, const H5L_info2_t*, void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return H5_ITER_CONT;
  };
  ASSERT_GE(H5Literate2(tracked.get(), H5_INDEX_CRT_ORDER, H5_ITER_NATIVE, &idx,
                        count_cb, &count),
            0);
  EXPECT_EQ(count, 6);

  // Delete by CRT_ORDER/INC (table path -> H5G__dense_remove), then shrink
  // below min_dense to test H5G__dense_delete(..., adj_link = false) with
  // index_corder == false.
  for (int i = 0; i < 5; ++i) {
    ASSERT_GE(H5Ldelete_by_idx(tracked.get(), ".", H5_INDEX_CRT_ORDER,
                               H5_ITER_INC, 0, H5P_DEFAULT),
              0);
  }
  ASSERT_GE(H5Gget_info(tracked.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_COMPACT);

  // 2. Untracked dense group deleted while populated (`adj_link = true`,
  // `index_corder = false`).
  ScopedPlist untracked_gcpl =
      MakeDenseGcpl(/*max_compact=*/3, /*min_dense=*/2, /*crt_order_flags=*/0);
  ASSERT_TRUE(untracked_gcpl.valid());
  ScopedGroup untracked(H5Gcreate2(file.get(), "untracked_del", H5P_DEFAULT,
                                   untracked_gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(untracked.valid());
  for (int i = 0; i < 5; ++i) {
    ScopedGroup sub(
        H5Gcreate2(untracked.get(), absl::StrFormat("u_%02d", i).c_str(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
  }
  // Exercise remove_by_idx on NAME/NATIVE when index_corder == false (so
  // other_bt2_addr == HADDR_UNDEF).
  ASSERT_GE(H5Ldelete_by_idx(untracked.get(), ".", H5_INDEX_NAME,
                             H5_ITER_NATIVE, 0, H5P_DEFAULT),
            0);
  untracked.reset();
  ASSERT_GE(H5Ldelete(file.get(), "untracked_del", H5P_DEFAULT), 0);
}

TEST(H5GdenseTest, InternalEmptyBuildTableAndRemoveErrors) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5O_linfo_t linfo{};
  linfo.index_corder = false;
  linfo.track_corder = false;
  linfo.nlinks = 0;
  linfo.fheap_addr = HADDR_UNDEF;
  linfo.name_bt2_addr = HADDR_UNDEF;
  linfo.corder_bt2_addr = HADDR_UNDEF;

  ASSERT_GE(H5G__dense_create(f, &linfo, /*pline=*/nullptr), 0);
  auto delete_dense = absl::MakeCleanup([&]() {
    if (H5_addr_defined(linfo.fheap_addr)) {
      EXPECT_GE(H5G__dense_delete(f, &linfo, /*adj_link=*/false), 0);
    }
  });

  // Exercise empty ltable branch (`ltable->nlinks == 0`) in
  // H5G__dense_build_table and out-of-bounds checks on empty dense storage.
  H5G_link_table_t ltable{99, nullptr};
  ASSERT_GE(H5G__dense_build_table(f, &linfo, H5_INDEX_NAME, H5_ITER_INC,
                                   &ltable),
            0);
  EXPECT_EQ(ltable.nlinks, 0u);
  EXPECT_EQ(ltable.lnks, nullptr);

  ScopedErrorSilence silence;
  H5O_link_t dummy_lnk{};
  EXPECT_LT(H5G__dense_lookup_by_idx(f, &linfo, H5_INDEX_NAME, H5_ITER_INC, 0,
                                     &dummy_lnk),
            0);
  char buf[16]{};
  size_t name_len = 0;
  EXPECT_LT(H5G__dense_get_name_by_idx(f, &linfo, H5_INDEX_NAME, H5_ITER_INC,
                                       0, buf, sizeof(buf), &name_len),
            0);
  EXPECT_LT(H5G__dense_remove_by_idx(f, &linfo, nullptr, H5_INDEX_NAME,
                                     H5_ITER_INC, 0),
            0);
  EXPECT_LT(H5G__dense_remove(f, &linfo, nullptr, "nonexistent"), 0);
}

TEST(H5GdenseTest, InternalErrorPathsWithCorruptMetadataAndReadOnlyFile) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGcpl(
      /*max_compact=*/3, /*min_dense=*/2,
      H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "ro_dense", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  for (int i = 0; i < 4; ++i) {
    ScopedGroup sub(
        H5Gcreate2(grp.get(), absl::StrFormat("m_%02d", i).c_str(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
  }
  grp.reset();

  // Contiguous 2KB dataset of 0xAB bytes provides an uncached, valid-EOA
  // address whose magic fails H5HF_open / H5B2_open / H5B2_delete / H5HF_delete.
  const hsize_t dim = 2048;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_alloc_time(dcpl.get(), H5D_ALLOC_TIME_EARLY), 0);
  ScopedDataset bogus_dset(H5Dcreate2(file.get(), "bogus_blob", H5T_NATIVE_UINT8,
                                      space.get(), H5P_DEFAULT, dcpl.get(),
                                      H5P_DEFAULT));
  ASSERT_TRUE(bogus_dset.valid());
  std::vector<uint8_t> fill(dim, 0xAB);
  ASSERT_GE(H5Dwrite(bogus_dset.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, fill.data()),
            0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const haddr_t bogus_addr = H5Dget_offset(bogus_dset.get());
  ASSERT_TRUE(H5_addr_defined(bogus_addr));
  bogus_dset.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    H5O_linfo_t linfo{};
    linfo.index_corder = true;
    linfo.track_corder = true;
    linfo.nlinks = 1;
    linfo.fheap_addr = HADDR_UNDEF;
    linfo.name_bt2_addr = HADDR_UNDEF;
    linfo.corder_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5G__dense_create(f, &linfo, nullptr), 0);
    auto delete_main = absl::MakeCleanup([&]() {
      if (H5_addr_defined(linfo.fheap_addr)) {
        EXPECT_GE(H5G__dense_delete(f, &linfo, /*adj_link=*/false), 0);
      }
    });

    H5O_link_t soft_lnk{};
    soft_lnk.type = H5L_TYPE_SOFT;
    soft_lnk.cset = H5T_CSET_ASCII;
    soft_lnk.corder_valid = true;
    soft_lnk.corder = 0;
    std::string lnk_name = "probe_link";
    std::string target = "/target";
    soft_lnk.name = lnk_name.data();
    soft_lnk.u.soft.name = target.data();
    ASSERT_GE(H5G__dense_insert(f, &linfo, &soft_lnk), 0);

    H5O_linfo_t empty_other{};
    empty_other.index_corder = true;
    empty_other.track_corder = true;
    empty_other.nlinks = 0;
    empty_other.fheap_addr = HADDR_UNDEF;
    empty_other.name_bt2_addr = HADDR_UNDEF;
    empty_other.corder_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5G__dense_create(f, &empty_other, nullptr), 0);
    auto delete_empty = absl::MakeCleanup([&]() {
      if (H5_addr_defined(empty_other.fheap_addr)) {
        EXPECT_GE(H5G__dense_delete(f, &empty_other, /*adj_link=*/false), 0);
      }
    });

    ScopedErrorSilence silence;
    bool found = false;
    H5O_link_t out_lnk{};
    char name_buf[32]{};
    size_t name_len = 0;
    auto noop_cb = [](const H5O_link_t*, void*) -> herr_t {
      return H5_ITER_CONT;
    };

    // 1. Bad fheap_addr fails H5HF_open in every dense operation.
    H5O_linfo_t bad_fheap = linfo;
    bad_fheap.fheap_addr = bogus_addr;
    EXPECT_LT(H5G__dense_insert(f, &bad_fheap, &soft_lnk), 0);
    EXPECT_LT(
        H5G__dense_lookup(f, &bad_fheap, "probe_link", &found, &out_lnk), 0);
    EXPECT_LT(H5G__dense_lookup_by_idx(f, &bad_fheap, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0, &out_lnk),
              0);
    EXPECT_LT(H5G__dense_iterate(f, &bad_fheap, H5_INDEX_NAME, H5_ITER_NATIVE,
                                 0, nullptr, noop_cb, nullptr),
              0);
    EXPECT_LT(H5G__dense_get_name_by_idx(f, &bad_fheap, H5_INDEX_NAME,
                                         H5_ITER_NATIVE, 0, name_buf,
                                         sizeof(name_buf), &name_len),
              0);
    EXPECT_LT(H5G__dense_remove(f, &bad_fheap, nullptr, "probe_link"), 0);
    EXPECT_LT(H5G__dense_remove_by_idx(f, &bad_fheap, nullptr, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0),
              0);
    EXPECT_LT(H5G__dense_delete(f, &bad_fheap, /*adj_link=*/true), 0);

    // 2. Valid fheap_addr, bad name_bt2_addr fails H5B2_open & table fallbacks.
    H5O_linfo_t bad_name_bt2 = linfo;
    bad_name_bt2.name_bt2_addr = bogus_addr;
    EXPECT_LT(H5G__dense_insert(f, &bad_name_bt2, &soft_lnk), 0);
    EXPECT_LT(
        H5G__dense_lookup(f, &bad_name_bt2, "probe_link", &found, &out_lnk),
        0);
    EXPECT_LT(H5G__dense_lookup_by_idx(f, &bad_name_bt2, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0, &out_lnk),
              0);
    EXPECT_LT(H5G__dense_lookup_by_idx(f, &bad_name_bt2, H5_INDEX_NAME,
                                       H5_ITER_INC, 0, &out_lnk),
              0);
    EXPECT_LT(H5G__dense_iterate(f, &bad_name_bt2, H5_INDEX_NAME,
                                 H5_ITER_NATIVE, 0, nullptr, noop_cb, nullptr),
              0);
    EXPECT_LT(H5G__dense_iterate(f, &bad_name_bt2, H5_INDEX_NAME, H5_ITER_INC,
                                 0, nullptr, noop_cb, nullptr),
              0);
    EXPECT_LT(H5G__dense_get_name_by_idx(f, &bad_name_bt2, H5_INDEX_NAME,
                                         H5_ITER_NATIVE, 0, name_buf,
                                         sizeof(name_buf), &name_len),
              0);
    EXPECT_LT(H5G__dense_get_name_by_idx(f, &bad_name_bt2, H5_INDEX_NAME,
                                         H5_ITER_INC, 0, name_buf,
                                         sizeof(name_buf), &name_len),
              0);
    EXPECT_LT(H5G__dense_remove(f, &bad_name_bt2, nullptr, "probe_link"), 0);
    EXPECT_LT(H5G__dense_remove_by_idx(f, &bad_name_bt2, nullptr, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0),
              0);
    EXPECT_LT(H5G__dense_remove_by_idx(f, &bad_name_bt2, nullptr, H5_INDEX_NAME,
                                       H5_ITER_INC, 0),
              0);
    EXPECT_LT(H5G__dense_delete(f, &bad_name_bt2, /*adj_link=*/false), 0);

    // 3. Valid fheap + name_bt2, duplicate name/corder and bad corder_bt2_addr
    //    fail H5G__dense_insert and H5G__dense_remove_fh_cb cleanly.
    soft_lnk.name = lnk_name.data();
    soft_lnk.corder = 99;
    EXPECT_LT(H5G__dense_insert(f, &linfo, &soft_lnk), 0);

    std::string dup_corder_name = "dup_corder_link";
    soft_lnk.name = dup_corder_name.data();
    soft_lnk.corder = 0;
    EXPECT_LT(H5G__dense_insert(f, &linfo, &soft_lnk), 0);

    H5O_linfo_t bad_corder_bt2 = linfo;
    bad_corder_bt2.corder_bt2_addr = bogus_addr;
    soft_lnk.corder = 1;
    std::string lnk2_name = "probe_link_2";
    soft_lnk.name = lnk2_name.data();
    EXPECT_LT(H5G__dense_insert(f, &bad_corder_bt2, &soft_lnk), 0);
    EXPECT_LT(H5G__dense_remove(f, &bad_corder_bt2, nullptr, "probe_link"), 0);

    // Mismatched empty corder_bt2 fails H5B2_remove in remove_fh_cb cleanly.
    bad_corder_bt2.corder_bt2_addr = empty_other.corder_bt2_addr;
    EXPECT_LT(H5G__dense_remove(f, &bad_corder_bt2, nullptr, "probe_link"), 0);

    // Hard link targeting an invalid object header address fails
    // H5O_link_delete inside H5G__dense_remove_fh_cb cleanly.
    H5O_linfo_t hard_linfo{};
    ASSERT_GE(H5G__dense_create(f, &hard_linfo, nullptr), 0);
    auto delete_hard = absl::MakeCleanup([&]() {
      if (H5_addr_defined(hard_linfo.fheap_addr)) {
        EXPECT_GE(H5G__dense_delete(f, &hard_linfo, /*adj_link=*/false), 0);
      }
    });
    H5O_link_t hard_lnk{};
    hard_lnk.type = H5L_TYPE_HARD;
    hard_lnk.cset = H5T_CSET_ASCII;
    std::string hard_name = "bad_hard";
    hard_lnk.name = hard_name.data();
    hard_lnk.u.hard.addr = bogus_addr;
    ASSERT_GE(H5G__dense_insert(f, &hard_linfo, &hard_lnk), 0);
    EXPECT_LT(H5G__dense_remove(f, &hard_linfo, nullptr, "bad_hard"), 0);

    // 4. H5G__dense_delete error branches for corder_bt2_addr and fheap_addr.
    H5O_linfo_t del_corder_err{};
    del_corder_err.index_corder = true;
    del_corder_err.track_corder = true;
    ASSERT_GE(H5G__dense_create(f, &del_corder_err, nullptr), 0);
    const haddr_t real_corder_addr = del_corder_err.corder_bt2_addr;
    const haddr_t real_fheap_addr = del_corder_err.fheap_addr;
    del_corder_err.corder_bt2_addr = bogus_addr;
    EXPECT_LT(H5G__dense_delete(f, &del_corder_err, /*adj_link=*/false), 0);
    EXPECT_GE(H5B2_delete(f, real_corder_addr, nullptr, nullptr, nullptr), 0);
    EXPECT_GE(H5HF_delete(f, real_fheap_addr), 0);

    H5O_linfo_t del_fheap_err{};
    ASSERT_GE(H5G__dense_create(f, &del_fheap_err, nullptr), 0);
    const haddr_t real_fheap_addr_2 = del_fheap_err.fheap_addr;
    del_fheap_err.fheap_addr = bogus_addr;
    EXPECT_LT(H5G__dense_delete(f, &del_fheap_err, /*adj_link=*/false), 0);
    EXPECT_GE(H5HF_delete(f, real_fheap_addr_2), 0);

    // 5. Single corrupt entry stored in fractal heap + name_bt2 hits
    //    H5O_msg_decode errors inside fh callbacks cleanly without leaks.
    H5O_linfo_t corrupt_linfo{};
    ASSERT_GE(H5G__dense_create(f, &corrupt_linfo, nullptr), 0);
    H5HF_t* cfheap = H5HF_open(f, corrupt_linfo.fheap_addr);
    ASSERT_NE(cfheap, nullptr);
    H5B2_t* cbt2 = H5B2_open(f, corrupt_linfo.name_bt2_addr, nullptr);
    ASSERT_NE(cbt2, nullptr);
    const uint8_t bad_bytes[4] = {0xff, 0x00, 0xaa, 0x55};
    H5G_bt2_ud_ins_t ins{};
    ASSERT_GE(H5HF_insert(cfheap, sizeof(bad_bytes), bad_bytes, ins.id), 0);
    ins.common.name_hash = H5_checksum_lookup3("bad", 3, 0);
    ASSERT_GE(H5B2_insert(cbt2, &ins), 0);
    ASSERT_GE(H5B2_close(cbt2), 0);
    ASSERT_GE(H5HF_close(cfheap), 0);

    EXPECT_LT(H5G__dense_lookup(f, &corrupt_linfo, "bad", &found, &out_lnk), 0);

    EXPECT_LT(H5G__dense_lookup_by_idx(f, &corrupt_linfo, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0, &out_lnk),
              0);
    EXPECT_LT(H5G__dense_iterate(f, &corrupt_linfo, H5_INDEX_NAME,
                                 H5_ITER_NATIVE, 0, nullptr, noop_cb, nullptr),
              0);
    EXPECT_LT(H5G__dense_get_name_by_idx(f, &corrupt_linfo, H5_INDEX_NAME,
                                         H5_ITER_NATIVE, 0, name_buf,
                                         sizeof(name_buf), &name_len),
              0);
    EXPECT_LT(H5G__dense_remove_by_idx(f, &corrupt_linfo, nullptr,
                                       H5_INDEX_NAME, H5_ITER_NATIVE, 0),
              0);
    EXPECT_GE(H5G__dense_delete(f, &corrupt_linfo, /*adj_link=*/false), 0);
  }

  // 6. Read-only reopen hits write-intent failures in create / insert /
  //    remove_by_idx (table path).
  ASSERT_GE(H5Fclose(file.release()), 0);

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("h5gdense_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "ro_dense", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());
  auto* f_ro = static_cast<H5F_t*>(H5VL_object_verify(ro_file.get(), H5I_FILE));
  ASSERT_NE(f_ro, nullptr);

  ScopedApiContext ro_api_ctx;
  ASSERT_TRUE(ro_api_ctx.valid());

  H5G_loc_t ro_gloc{};
  ASSERT_GE(H5G_loc(ro_grp.get(), &ro_gloc), 0);
  H5O_linfo_t ro_linfo{};
  ASSERT_GE(H5G__obj_get_linfo(ro_gloc.oloc, &ro_linfo), 0);

  ScopedErrorSilence ro_silence;
  H5O_linfo_t ro_new{};
  EXPECT_LT(H5G__dense_create(f_ro, &ro_new, nullptr), 0);

  H5O_link_t ro_lnk{};
  ro_lnk.type = H5L_TYPE_SOFT;
  ro_lnk.cset = H5T_CSET_ASCII;
  std::string ro_name = "ro_link";
  std::string ro_target = "/target";
  ro_lnk.name = ro_name.data();
  ro_lnk.u.soft.name = ro_target.data();
  EXPECT_LT(H5G__dense_insert(f_ro, &ro_linfo, &ro_lnk), 0);
  EXPECT_LT(H5G__dense_remove_by_idx(f_ro, &ro_linfo, nullptr, H5_INDEX_NAME,
                                     H5_ITER_INC, 0),
            0);
  ro_grp.reset();
  EXPECT_GE(H5Fclose(ro_file.release()), 0);
}

}  // namespace
}  // namespace hdf5
