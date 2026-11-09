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
// Tests group hierarchy object location operations: ID-to-location lookup,
// copying, resetting, freeing, finding by name or index, existence checks,
// address resolution, data-model and native info queries, and comment getters
// and setters (third_party/hdf5/src/H5Gloc.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gloc.c

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from internal
// cache/header structs pulled in through `H5private.h` and `*pkg.h`.
#ifndef NDEBUG
#define H5GLOC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
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
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GLOC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GLOC_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

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

// Owns the H5O_loc_t and H5G_name_t storage backing an H5G_loc_t.
struct OwnedLoc {
  H5O_loc_t oloc{};
  H5G_name_t path{};
  H5G_loc_t loc{&oloc, &path};

  OwnedLoc() { H5G_loc_reset(&loc); }
  ~OwnedLoc() { H5G_loc_free(&loc); }

  OwnedLoc(const OwnedLoc&) = delete;
  OwnedLoc& operator=(const OwnedLoc&) = delete;
};

TEST(H5GlocTest, LocResolvesValidObjectsAndRejectsNonLocTypes) {
  ScopedFile file = CreateInMemoryFile("gloc_types.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr hsize_t kDim = 4;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file.get(), "/grp/dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT32, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  ScopedType committed_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_type.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/grp/dtype", committed_type.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedType transient_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient_type.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());

  // 1. Valid locatable IDs populate both oloc and path.
  const std::array<hid_t, 5> valid_ids = {
      file.get(), grp.get(), dset.get(), committed_type.get(), attr.get(),
  };
  for (hid_t id : valid_ids) {
    H5G_loc_t loc{};
    EXPECT_GE(H5G_loc(id, &loc), 0);
    EXPECT_NE(loc.oloc, nullptr);
    EXPECT_NE(loc.path, nullptr);
  }

  // 2. Non-locatable IDs and invalid IDs fail in H5G_loc.
  {
    ScopedErrorSilence silence;
    H5G_loc_t loc{};
    EXPECT_LT(H5G_loc(H5I_INVALID_HID, &loc), 0);
    EXPECT_LT(H5G_loc(space.get(), &loc), 0);
    EXPECT_LT(H5G_loc(fapl.get(), &loc), 0);
    EXPECT_LT(H5G_loc(es.get(), &loc), 0);
    EXPECT_LT(H5G_loc(transient_type.get(), &loc), 0);
  }

  // 3. Direct H5G_loc_real checks cover every rejected H5I_type_t switch arm.
  int dummy_obj = 0;
  constexpr auto kRejectedTypes = std::to_array<H5I_type_t>({
      H5I_DATASPACE,
      H5I_MAP,
      H5I_GENPROP_CLS,
      H5I_GENPROP_LST,
      H5I_ERROR_CLASS,
      H5I_ERROR_MSG,
      H5I_ERROR_STACK,
      H5I_VFL,
      H5I_VOL,
      H5I_SPACE_SEL_ITER,
      H5I_EVENTSET,
      H5I_UNINIT,
      H5I_BADID,
      H5I_NTYPES,
      static_cast<H5I_type_t>(30),
  });
  {
    ScopedErrorSilence silence;
    for (H5I_type_t type : kRejectedTypes) {
      H5G_loc_t loc{};
      EXPECT_LT(H5G_loc_real(&dummy_obj, type, &loc), 0);
    }
  }
}

TEST(H5GlocTest, CopyResetAndFreePreserveAndTransferOwnership) {
  ScopedFile file = CreateInMemoryFile("gloc_copy.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/alpha", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5G_loc_t src_loc{};
  ASSERT_GE(H5G_loc(grp.get(), &src_loc), 0);

  OwnedLoc deep_copy;
  ASSERT_GE(H5G_loc_copy(&deep_copy.loc, &src_loc, H5_COPY_DEEP), 0);
  EXPECT_EQ(deep_copy.loc.oloc->addr, src_loc.oloc->addr);
  EXPECT_EQ(deep_copy.loc.oloc->file, src_loc.oloc->file);

  // Move ownership via shallow copy and verify source is reset cleanly.
  OwnedLoc shallow_copy;
  ASSERT_GE(H5G_loc_copy(&shallow_copy.loc, &deep_copy.loc, H5_COPY_SHALLOW),
            0);
  EXPECT_EQ(shallow_copy.loc.oloc->addr, src_loc.oloc->addr);
  EXPECT_EQ(deep_copy.loc.oloc->file, nullptr);
  EXPECT_EQ(deep_copy.loc.path->full_path_r, nullptr);
}

TEST(H5GlocTest, FindExistsAndAddrResolveExistingMissingAndDanglingPaths) {
  ScopedFile file = CreateInMemoryFile("gloc_find.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dir", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/dir/dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Lcreate_soft("/dir/dset", file.get(), "/dir/soft_ok", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/dir/missing", file.get(), "/dir/soft_dangling",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  H5G_loc_t file_loc{};
  ASSERT_GE(H5G_loc(file.get(), &file_loc), 0);

  ScopedApiContext ctx(file.get());
  ASSERT_TRUE(ctx.valid());

  // 1. Existing path resolves via H5G_loc_find, H5G_loc_exists, and H5G__loc_addr.
  OwnedLoc found;
  ASSERT_GE(H5G_loc_find(&file_loc, "/dir/dset", &found.loc), 0);
  EXPECT_NE(found.loc.oloc->addr, HADDR_UNDEF);

  bool exists = false;
  ASSERT_GE(H5G_loc_exists(&file_loc, "/dir/soft_ok", &exists), 0);
  EXPECT_TRUE(exists);

  haddr_t addr = HADDR_UNDEF;
  ASSERT_GE(H5G__loc_addr(&file_loc, "/dir/dset", &addr), 0);
  EXPECT_EQ(addr, found.loc.oloc->addr);

  // 2. Missing leaf inside an existing group yields exists=false and HADDR_UNDEF.
  ASSERT_GE(H5G_loc_exists(&file_loc, "/dir/no_such", &exists), 0);
  EXPECT_FALSE(exists);
  ASSERT_GE(H5G_loc_exists(&file_loc, "/dir/soft_dangling", &exists), 0);
  EXPECT_FALSE(exists);

  addr = found.loc.oloc->addr;
  ASSERT_GE(H5G__loc_addr(&file_loc, "/dir/no_such", &addr), 0);
  EXPECT_EQ(addr, HADDR_UNDEF);

  // 3. Empty name, missing leaf for find, and missing intermediate parent fail.
  {
    ScopedErrorSilence silence;
    OwnedLoc err_loc;
    EXPECT_LT(H5G_loc_find(&file_loc, "", &err_loc.loc), 0);
    EXPECT_LT(H5G_loc_find(&file_loc, "/dir/no_such", &err_loc.loc), 0);
    EXPECT_LT(H5G_loc_exists(&file_loc, "/no_parent/leaf", &exists), 0);
    EXPECT_LT(H5G__loc_addr(&file_loc, "/no_parent/leaf", &addr), 0);
  }
}

TEST(H5GlocTest, FindByIdxHandlesValidSoftAndDanglingLinks) {
  ScopedFile file = CreateInMemoryFile("gloc_idx.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/box", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "a_dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Lcreate_soft("/box/a_dset", grp.get(), "c_soft_ok", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/box/nowhere", grp.get(), "b_dangling", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // 1. Public H5Oopen_by_idx succeeds for the dataset and valid soft link.
  ScopedObject obj0(H5Oopen_by_idx(file.get(), "/box", H5_INDEX_NAME,
                                   H5_ITER_INC, 0, H5P_DEFAULT));
  ASSERT_TRUE(obj0.valid());
  ScopedObject obj2(H5Oopen_by_idx(file.get(), "/box", H5_INDEX_NAME,
                                   H5_ITER_INC, 2, H5P_DEFAULT));
  ASSERT_TRUE(obj2.valid());

  // 2. Dangling soft link (index 1) fails during H5G__traverse_special and
  // cleans up its partially populated obj_loc.
  {
    ScopedErrorSilence silence;
    ScopedObject obj1(H5Oopen_by_idx(file.get(), "/box", H5_INDEX_NAME,
                                     H5_ITER_INC, 1, H5P_DEFAULT));
    EXPECT_FALSE(obj1.valid());

    H5G_loc_t file_loc{};
    ASSERT_GE(H5G_loc(file.get(), &file_loc), 0);
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());

    OwnedLoc out;
    EXPECT_LT(H5G_loc_find_by_idx(&file_loc, "/box", H5_INDEX_NAME, H5_ITER_INC,
                                  1, &out.loc),
              0);
    EXPECT_LT(H5G_loc_find_by_idx(&file_loc, "/box", H5_INDEX_NAME, H5_ITER_INC,
                                  99, &out.loc),
              0);
    EXPECT_LT(H5G_loc_find_by_idx(&file_loc, "/no_such_box", H5_INDEX_NAME,
                                  H5_ITER_INC, 0, &out.loc),
              0);
  }
}

TEST(H5GlocTest, LocInsertLinksAnonGroupAndRejectsDuplicateName) {
  // Symbol-table storage (EARLIEST libver bound) rejects duplicate link names
  // inside H5G_obj_insert, letting us test the failure branch of H5G__loc_insert.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
      0);
  ScopedFile file(
      H5Fcreate("gloc_insert.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup anon(H5Gcreate_anon(file.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon.valid());

  H5G_loc_t file_loc{};
  H5G_loc_t anon_loc{};
  ASSERT_GE(H5G_loc(file.get(), &file_loc), 0);
  ASSERT_GE(H5G_loc(anon.get(), &anon_loc), 0);

  H5G_obj_create_t gcrt{};
  gcrt.gcpl_id = H5P_DEFAULT;
  gcrt.cache_type = H5G_NOTHING_CACHED;

  char link_name[] = "inserted_group";
  {
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5G__loc_insert(&file_loc, link_name, &anon_loc, H5O_TYPE_GROUP,
                              &gcrt),
              0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5G__loc_insert(&file_loc, link_name, &anon_loc, H5O_TYPE_GROUP,
                              &gcrt),
              0);
  }

  ScopedGroup reopened(H5Gopen2(file.get(), "/inserted_group", H5P_DEFAULT));
  EXPECT_TRUE(reopened.valid());
}

TEST(H5GlocTest, InfoAndNativeInfoQueryValidAndMissingObjects) {
  ScopedFile file = CreateInMemoryFile("gloc_info.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/item", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info_by_name3(file.get(), "/item", &oinfo, H5O_INFO_BASIC,
                                 H5P_DEFAULT),
            0);
  EXPECT_EQ(oinfo.type, H5O_TYPE_DATASET);

  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info_by_name(file.get(), "/item", &ninfo,
                                       H5O_NATIVE_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_GT(ninfo.hdr.nmesgs, 0U);

  H5G_loc_t file_loc{};
  ASSERT_GE(H5G_loc(file.get(), &file_loc), 0);

  ScopedApiContext ctx(file.get());
  ASSERT_TRUE(ctx.valid());

  H5O_info2_t internal_oinfo{};
  EXPECT_GE(H5G_loc_info(&file_loc, "/item", &internal_oinfo, H5O_INFO_BASIC),
            0);
  EXPECT_EQ(internal_oinfo.type, H5O_TYPE_DATASET);

  H5O_native_info_t internal_ninfo{};
  EXPECT_GE(H5G_loc_native_info(&file_loc, "/item", &internal_ninfo,
                                H5O_NATIVE_INFO_HDR),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G_loc_info(&file_loc, "/missing", &internal_oinfo,
                           H5O_INFO_BASIC),
              0);
    EXPECT_LT(H5G_loc_info(&file_loc, "/no_grp/missing", &internal_oinfo,
                           H5O_INFO_BASIC),
              0);
    EXPECT_LT(H5G_loc_native_info(&file_loc, "/missing", &internal_ninfo,
                                  H5O_NATIVE_INFO_HDR),
              0);
    EXPECT_LT(H5G_loc_native_info(&file_loc, "/no_grp/missing", &internal_ninfo,
                                  H5O_NATIVE_INFO_HDR),
              0);
  }
}

TEST(H5GlocTest, SetAndGetCommentHandleMissingOverwriteTruncateAndClear) {
  ScopedFile file = CreateInMemoryFile("gloc_comment.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/note", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  H5G_loc_t file_loc{};
  ASSERT_GE(H5G_loc(file.get(), &file_loc), 0);
  ScopedApiContext ctx(file.get());
  ASSERT_TRUE(ctx.valid());

  // 1. Object without a comment returns length 0 and NUL-terminates non-empty
  // output buffers.
  char empty_buf[8] = "XXXXXXX";
  size_t len = 999;
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", empty_buf,
                                sizeof(empty_buf), &len),
            0);
  EXPECT_EQ(len, 0U);
  EXPECT_STREQ(empty_buf, "");

  // 2. Set, overwrite, query length with nullptr, truncate, and full read.
  ASSERT_GE(H5Oset_comment_by_name(file.get(), "/note", "initial", H5P_DEFAULT),
            0);
  ASSERT_GE(H5G_loc_set_comment(&file_loc, "/note", "updated comment"), 0);

  size_t full_len = 0;
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", nullptr, 0, &full_len), 0);
  EXPECT_EQ(full_len, std::strlen("updated comment"));

  char small_buf[8] = {};
  size_t trunc_len = 0;
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", small_buf,
                                sizeof(small_buf), &trunc_len),
            0);
  EXPECT_EQ(trunc_len, full_len);
  EXPECT_EQ(std::memcmp(small_buf, "updated ", 8), 0);

  std::string out_buf(full_len + 1, '\0');
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", out_buf.data(),
                                out_buf.size(), &full_len),
            0);
  EXPECT_THAT(out_buf, HasSubstr("updated comment"));

  // 3. Clearing via empty string or nullptr removes the comment message.
  ASSERT_GE(H5G_loc_set_comment(&file_loc, "/note", ""), 0);
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", nullptr, 0, &len), 0);
  EXPECT_EQ(len, 0U);

  ASSERT_GE(H5G_loc_set_comment(&file_loc, "/note", "again"), 0);
  ASSERT_GE(H5G_loc_set_comment(&file_loc, "/note", nullptr), 0);
  ASSERT_GE(H5G_loc_get_comment(&file_loc, "/note", nullptr, 0, &len), 0);
  EXPECT_EQ(len, 0U);

  // 4. Negative paths: non-existent object and missing parent group.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5G_loc_set_comment(&file_loc, "/missing", "x"), 0);
    EXPECT_LT(H5G_loc_set_comment(&file_loc, "/no_parent/x", "x"), 0);
    EXPECT_LT(H5G_loc_get_comment(&file_loc, "/missing", empty_buf,
                                  sizeof(empty_buf), &len),
              0);
    EXPECT_LT(H5G_loc_get_comment(&file_loc, "/no_parent/x", empty_buf,
                                  sizeof(empty_buf), &len),
              0);
  }
}

}  // namespace
}  // namespace hdf5
