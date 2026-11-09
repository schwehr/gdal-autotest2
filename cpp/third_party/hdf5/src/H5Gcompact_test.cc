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
// Tests compact group link storage operations (insert, lookup, get_name_by_idx,
// iterate, remove, remove_by_idx, read-only / invalid ohdr errors) for
// third_party/hdf5/src/H5Gcompact.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gcompact.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from cache
// entries in internal headers. Match those struct layouts here.
#ifndef NDEBUG
#define H5GCOMPACT_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5GCOMPACT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GCOMPACT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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
  return ScopedFile(H5Fcreate("h5gcompact_test.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
}

ScopedPlist MakeTrackedCompactGcpl(unsigned max_compact = 8,
                                   unsigned min_dense = 6) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_link_phase_change(gcpl.get(), max_compact, min_dense) < 0 ||
      H5Pset_link_creation_order(
          gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    gcpl.reset();
  }
  return gcpl;
}

TEST(H5GcompactTest, EmptyCompactGroupHandlesQueriesAndBoundsErrors) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeTrackedCompactGcpl();
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup empty_grp(H5Gcreate2(file.get(), "empty", H5P_DEFAULT,
                                   gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(empty_grp.valid());

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(empty_grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(ginfo.nlinks, 0u);

  // Iterate over empty compact group (hits nlinks == 0 in build_table).
  int visit_count = 0;
  hsize_t idx = 0;
  auto count_cb = [](hid_t, const char*, const H5L_info2_t*,
                     void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return H5_ITER_CONT;
  };
  EXPECT_GE(H5Literate2(empty_grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                        count_cb, &visit_count),
            0);
  EXPECT_EQ(visit_count, 0);

  // Out-of-bounds / missing link lookups on empty compact group.
  EXPECT_EQ(H5Lexists(empty_grp.get(), "missing", H5P_DEFAULT), 0);
  ScopedErrorSilence silence;
  char name_buf[16];
  EXPECT_LT(H5Lget_name_by_idx(empty_grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                               0, name_buf, sizeof(name_buf), H5P_DEFAULT),
            0);
  H5L_info2_t linfo{};
  EXPECT_LT(H5Lget_info_by_idx2(empty_grp.get(), ".", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(empty_grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete(empty_grp.get(), "missing", H5P_DEFAULT), 0);
}

TEST(H5GcompactTest, InsertLookupGetNameIterateAndRemove) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeTrackedCompactGcpl(/*max_compact=*/10, /*min_dense=*/6);
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "compact", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Insert hard links and soft links in non-alphabetical order.
  ScopedGroup target_keep(H5Gcreate2(grp.get(), "zeta_open", H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target_keep.valid());
  {
    ScopedGroup a(H5Gcreate2(grp.get(), "alpha_link", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
  }
  {
    ScopedGroup m(H5Gcreate2(grp.get(), "middle_node", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(m.valid());
  }
  ASSERT_GE(
      H5Lcreate_soft("/compact/alpha_link", grp.get(), "beta_soft", H5P_DEFAULT,
                     H5P_DEFAULT),
      0);

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(ginfo.nlinks, 4u);

  // H5G__compact_lookup: existing hard link, existing soft link, missing link.
  EXPECT_EQ(H5Lexists(grp.get(), "beta_soft", H5P_DEFAULT), 1);
  EXPECT_EQ(H5Lexists(grp.get(), "nonexistent", H5P_DEFAULT), 0);
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(grp.get(), "beta_soft", &linfo, H5P_DEFAULT), 0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  // H5G__compact_lookup_by_idx across NAME and CRT_ORDER.
  H5L_info2_t idx_info{};
  ASSERT_GE(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 1,
                                &idx_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(idx_info.type, H5L_TYPE_SOFT);  // "beta_soft"

  // H5G__compact_get_name_by_idx: full length query (NULL buffer), full copy,
  // and truncated buffer copy (`*name_len >= name_size`).
  const ssize_t full_len = H5Lget_name_by_idx(
      grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 2, nullptr, 0, H5P_DEFAULT);
  EXPECT_EQ(full_len, static_cast<ssize_t>(std::strlen("middle_node")));

  char exact_buf[32]{};
  ASSERT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_DEC,
                               0, exact_buf, sizeof(exact_buf), H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("beta_soft")));
  EXPECT_STREQ(exact_buf, "beta_soft");

  char trunc_buf[5]{};
  std::memset(trunc_buf, 'X', sizeof(trunc_buf));
  EXPECT_EQ(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 2,
                               trunc_buf, sizeof(trunc_buf), H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("middle_node")));
  EXPECT_STREQ(trunc_buf, "midd");
  EXPECT_EQ(trunc_buf[4], '\0');

  // H5G__compact_iterate: full order, skip > 0, early stop, and callback error.
  std::vector<std::string> visited;
  auto collect_cb = [](hid_t, const char* name, const H5L_info2_t*,
                       void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return H5_ITER_CONT;
  };
  hsize_t iter_idx = 0;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &iter_idx,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited, ElementsAre("alpha_link", "beta_soft", "middle_node",
                                   "zeta_open"));

  visited.clear();
  iter_idx = 2;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, &iter_idx,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited, ElementsAre("middle_node", "beta_soft"));

  auto stop_after_one_cb = [](hid_t, const char*, const H5L_info2_t*,
                              void* op_data) -> herr_t {
    ++(*static_cast<int*>(op_data));
    return 1;
  };
  int stop_calls = 0;
  iter_idx = 0;
  EXPECT_EQ(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_DEC, &iter_idx,
                        stop_after_one_cb, &stop_calls),
            1);
  EXPECT_EQ(stop_calls, 1);

  {
    ScopedErrorSilence silence;
    auto err_cb = [](hid_t, const char*, const H5L_info2_t*,
                     void*) -> herr_t { return -1; };
    iter_idx = 0;
    EXPECT_LT(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &iter_idx,
                          err_cb, nullptr),
              0);

    // Out-of-bounds calls on populated compact group.
    EXPECT_LT(H5Lget_name_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 4,
                                 exact_buf, sizeof(exact_buf), H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lget_info_by_idx2(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                  4, &idx_info, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 4,
                               H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ldelete(grp.get(), "missing_after_scan", H5P_DEFAULT), 0);
  }

  // Delete an object while an open handle exists (`zeta_open`) to exercise
  // H5G__link_name_replace, then delete remaining links by index and name.
  ASSERT_GE(H5Ldelete(grp.get(), "zeta_open", H5P_DEFAULT), 0);
  target_keep.reset();

  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 1,
                             H5P_DEFAULT),
            0);  // Removes "beta_soft"
  ASSERT_GE(H5Ldelete(grp.get(), "middle_node", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete_by_idx(grp.get(), ".", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                             0, H5P_DEFAULT),
            0);  // Removes last remaining link "alpha_link"

  ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
  EXPECT_EQ(ginfo.nlinks, 0u);
}

TEST(H5GcompactTest, InternalReadOnlyAndCorruptObjectHeaderErrors) {
  ScopedFile file = CreateLatestInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeTrackedCompactGcpl();
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "compact", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  {
    ScopedGroup child(H5Gcreate2(grp.get(), "child", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(child.valid());
  }
  grp.reset();

  // Create a contiguous raw dataset filled with non-OHDR bytes so its flushed
  // file address fails H5O_msg_iterate / H5O_msg_remove_op cleanly.
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

  // Reopen the file read-only via file image to hit read-only write errors.
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("h5gcompact_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedGroup ro_grp(H5Gopen2(ro_file.get(), "compact", H5P_DEFAULT));
  ASSERT_TRUE(ro_grp.valid());

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5G_loc_t gloc{};
  ASSERT_GE(H5G_loc(ro_grp.get(), &gloc), 0);
  H5O_linfo_t linfo{};
  ASSERT_GE(H5G__obj_get_linfo(gloc.oloc, &linfo), 0);
  ASSERT_EQ(linfo.nlinks, 1u);

  ScopedErrorSilence silence;

  // 1. Read-only file triggers H5O_msg_create / H5O_msg_remove_op failures.
  H5O_link_t soft_lnk{};
  soft_lnk.type = H5L_TYPE_SOFT;
  soft_lnk.cset = H5T_CSET_ASCII;
  std::string link_name = "ro_soft";
  std::string target = "/nowhere";
  soft_lnk.name = link_name.data();
  soft_lnk.u.soft.name = target.data();
  EXPECT_LT(H5G__compact_insert(gloc.oloc, &soft_lnk), 0);
  EXPECT_LT(H5G__compact_remove(gloc.oloc, nullptr, "child"), 0);
  EXPECT_LT(H5G__compact_remove_by_idx(gloc.oloc, &linfo, nullptr,
                                       H5_INDEX_NAME, H5_ITER_INC, 0),
            0);

  // 2. Non-OHDR address triggers H5G__compact_build_table / lookup errors.
  H5O_loc_t bad_oloc = *gloc.oloc;
  bad_oloc.addr = bogus_addr;

  char name_buf[32]{};
  size_t name_len = 0;
  EXPECT_LT(H5G__compact_get_name_by_idx(&bad_oloc, &linfo, H5_INDEX_NAME,
                                         H5_ITER_INC, 0, name_buf,
                                         sizeof(name_buf), &name_len),
            0);
  EXPECT_LT(H5G__compact_remove(&bad_oloc, nullptr, "child"), 0);
  EXPECT_LT(H5G__compact_remove_by_idx(&bad_oloc, &linfo, nullptr,
                                       H5_INDEX_NAME, H5_ITER_INC, 0),
            0);
  auto noop_lib_cb = [](const H5O_link_t*, void*) -> herr_t {
    return H5_ITER_CONT;
  };
  EXPECT_LT(H5G__compact_iterate(&bad_oloc, &linfo, H5_INDEX_NAME, H5_ITER_INC,
                                 0, nullptr, noop_lib_cb, nullptr),
            0);
  bool found = false;
  H5O_link_t found_lnk{};
  EXPECT_LT(H5G__compact_lookup(&bad_oloc, "child", &found, &found_lnk), 0);
  EXPECT_LT(H5G__compact_lookup_by_idx(&bad_oloc, &linfo, H5_INDEX_NAME,
                                       H5_ITER_INC, 0, &found_lnk),
            0);
}

}  // namespace
}  // namespace hdf5
