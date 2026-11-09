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
// Tests abstract group object operations across symbol-table, compact, and
// dense link storage formats (third_party/hdf5/src/H5Gobj.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gobj.c

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts before pulling
// private/package headers.
#ifndef NDEBUG
#define H5G_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5G_FRIEND
#define H5O_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Pprivate.h"
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

ScopedFile CreateFileWithBounds(const char* name, H5F_libver_t low,
                                H5F_libver_t high,
                                unsigned flags = H5F_ACC_TRUNC) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_libver_bounds(fapl, low, high) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, flags, H5P_DEFAULT, fapl));
}

H5O_loc_t* GetGroupOloc(hid_t grp_id) {
  auto* grp = static_cast<H5G_t*>(H5VL_object_verify(grp_id, H5I_GROUP));
  return grp ? H5G_oloc(grp) : nullptr;
}

std::vector<uint8_t> ExtractFileImage(hid_t file_id) {
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

ScopedFile OpenFileImage(const std::vector<uint8_t>& image, unsigned flags,
                         const char* name = "reopen_obj.h5") {
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

herr_t CollectLinkNamesCb(const H5O_link_t* lnk, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->push_back(lnk->name);
  return 0;
}

TEST(H5GobjTest, ObjCreateVariantsAndValidationErrors) {
  ScopedFile old_file = CreateFileWithBounds("create_old.h5",
                                             H5F_LIBVER_EARLIEST,
                                             H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("create_new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  // 1. V18 triggers new-format group; pipeline + track_corder on old_file also
  // triggers use_at_least_v18.
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_deflate(gcpl, 6), 0);
    ASSERT_GE(H5Pset_link_creation_order(gcpl, H5P_CRT_ORDER_TRACKED), 0);
    ASSERT_GE(H5Pset_est_link_info(gcpl, 2, 16), 0);
    ScopedGroup g(H5Gcreate2(old_file, "/v18_with_pline", H5P_DEFAULT, gcpl,
                             H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    H5G_info_t info{};
    ASSERT_GE(H5Gget_info(g, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_COMPACT);
  }

  // 2. compact == false branch (est_num_entries > max_compact).
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 4, 2), 0);
    ASSERT_GE(H5Pset_est_link_info(gcpl, 10, 16), 0);
    ScopedGroup g(
        H5Gcreate2(new_file, "/non_compact_est", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  // 3. compact == true && size_of_links >= H5O_MESG_MAX_SIZE branch.
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 1000, 500), 0);
    ASSERT_GE(H5Pset_est_link_info(gcpl, 1000, 200), 0);
    ScopedGroup g(
        H5Gcreate2(new_file, "/huge_est", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  // 4. Direct H5G__obj_create & H5G__obj_create_real error branches.
  ASSERT_GE(H5Fflush(new_file, H5F_SCOPE_GLOBAL), 0);
  std::vector<uint8_t> ro_img = ExtractFileImage(new_file);
  ASSERT_FALSE(ro_img.empty());
  ScopedFile ro_file = OpenFileImage(ro_img, H5F_ACC_RDONLY, "ro_obj.h5");
  ASSERT_TRUE(ro_file.valid());

  auto* rw_f = static_cast<H5F_t*>(H5VL_object_verify(new_file, H5I_FILE));
  auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
  ASSERT_NE(rw_f, nullptr);
  ASSERT_NE(ro_f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  H5G_obj_create_t gcrt{};
  H5O_loc_t oloc{};

  // Bad plist ID & wrong plist class (non-GCPL lacks "group info").
  gcrt.gcpl_id = H5I_INVALID_HID;
  EXPECT_LT(H5G__obj_create(rw_f, &gcrt, &oloc), 0);
  gcrt.gcpl_id = H5P_DATASET_CREATE_DEFAULT;
  EXPECT_LT(H5G__obj_create(rw_f, &gcrt, &oloc), 0);

  // Read-only file & index_corder without track_corder in H5G__obj_create_real.
  H5O_ginfo_t ginfo = H5G_CRT_GROUP_INFO_DEF;
  H5O_linfo_t linfo = H5G_CRT_LINK_INFO_DEF;
  H5O_pline_t pline = H5O_CRT_PIPELINE_DEF;
  gcrt.gcpl_id = H5P_GROUP_CREATE_DEFAULT;
  EXPECT_LT(
      H5G__obj_create_real(ro_f, &ginfo, &linfo, &pline, &gcrt, &oloc), 0);

  linfo.index_corder = true;
  linfo.track_corder = false;
  EXPECT_LT(
      H5G__obj_create_real(rw_f, &ginfo, &linfo, &pline, &gcrt, &oloc), 0);
}

TEST(H5GobjTest, GetLinfoRecomputesNlinksWhenReopenedFromDisk) {
  std::vector<uint8_t> image;
  {
    ScopedFile f = CreateFileWithBounds("linfo_disk.h5", H5F_LIBVER_V18,
                                        H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());

    ScopedGroup compact(
        H5Gcreate2(f, "/compact", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());
    ASSERT_GE(
        H5Lcreate_soft("/t", compact, "a", H5P_DEFAULT, H5P_DEFAULT), 0);
    ASSERT_GE(
        H5Lcreate_soft("/t", compact, "b", H5P_DEFAULT, H5P_DEFAULT), 0);

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 2, 1), 0);
    ScopedGroup dense(
        H5Gcreate2(f, "/dense", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(dense.valid());
    for (int i = 0; i < 4; ++i) {
      const std::string name = "d_" + std::to_string(i);
      ASSERT_GE(
          H5Lcreate_soft("/t", dense, name.c_str(), H5P_DEFAULT, H5P_DEFAULT),
          0);
    }
    compact.reset();
    dense.reset();
    ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    image = ExtractFileImage(f);
  }
  ASSERT_FALSE(image.empty());

  // Reopening decodes H5O_LINFO_ID with linfo.nlinks == HSIZET_MAX, forcing
  // H5G__obj_get_linfo to query H5O_get_nlinks (compact) and H5B2_get_nrec
  // (dense).
  ScopedFile reopened = OpenFileImage(image, H5F_ACC_RDONLY, "linfo_reopen.h5");
  ASSERT_TRUE(reopened.valid());

  ScopedGroup compact(H5Gopen2(reopened, "/compact", H5P_DEFAULT));
  ScopedGroup dense(H5Gopen2(reopened, "/dense", H5P_DEFAULT));
  ASSERT_TRUE(compact.valid());
  ASSERT_TRUE(dense.valid());

  H5G_info_t cinfo{};
  H5G_info_t dinfo{};
  ASSERT_GE(H5Gget_info(compact, &cinfo), 0);
  ASSERT_GE(H5Gget_info(dense, &dinfo), 0);
  EXPECT_EQ(cinfo.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(cinfo.nlinks, 2u);
  EXPECT_EQ(dinfo.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(dinfo.nlinks, 4u);
}

TEST(H5GobjTest, InsertAndRemoveFormatConversionsAndCorder) {
  // 1. Old-style stab group -> converted to compact on UTF-8 link insert.
  {
    ScopedFile old_f = CreateFileWithBounds("stab_conv.h5", H5F_LIBVER_EARLIEST,
                                            H5F_LIBVER_LATEST);
    ASSERT_TRUE(old_f.valid());
    ScopedGroup grp(
        H5Gcreate2(old_f, "/stab_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ScopedGroup sub(
        H5Gcreate2(grp, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
    ASSERT_GE(H5Lcreate_soft("/sub", grp, "slink", H5P_DEFAULT, H5P_DEFAULT),
              0);

    H5G_info_t before{};
    ASSERT_GE(H5Gget_info(grp, &before), 0);
    EXPECT_EQ(before.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);

    // Insert a UTF-8 link; invokes H5G__obj_stab_to_new_cb for "sub" and "slink".
    ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
    ASSERT_TRUE(lcpl.valid());
    ASSERT_GE(H5Pset_char_encoding(lcpl, H5T_CSET_UTF8), 0);
    ASSERT_GE(H5Lcreate_soft("/target", grp, "utf8_\xc3\xa9", lcpl, H5P_DEFAULT),
              0);

    H5G_info_t after{};
    ASSERT_GE(H5Gget_info(grp, &after), 0);
    EXPECT_EQ(after.storage_type, H5G_STORAGE_TYPE_COMPACT);
    EXPECT_EQ(after.nlinks, 3u);
  }

  // 2. Compact -> dense (with pipeline message + corder tracking) -> compact ->
  // empty (resets max_corder = 0).
  {
    ScopedFile new_f =
        CreateFileWithBounds("conv.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(new_f.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_deflate(gcpl, 6), 0);
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 4, 2), 0);
    ASSERT_GE(H5Pset_link_creation_order(
                  gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);

    ScopedGroup grp(
        H5Gcreate2(new_f, "/tracked", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    for (int i = 0; i < 6; ++i) {
      const std::string name = "k_" + std::to_string(i);
      ASSERT_GE(H5Lcreate_soft("/t", grp, name.c_str(), H5P_DEFAULT, H5P_DEFAULT),
                0);
    }

    H5G_info_t info{};
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_DENSE);
    EXPECT_EQ(info.nlinks, 6u);
    EXPECT_EQ(info.max_corder, 6);

    // Delete via by_idx in dense storage (both CRT_ORDER and NAME) until
    // nlinks < min_dense (2) -> converts dense back to compact!
    ASSERT_GE(H5Ldelete_by_idx(grp, ".", H5_INDEX_CRT_ORDER, H5_ITER_DEC, 0,
                               H5P_DEFAULT),
              0);
    ASSERT_GE(
        H5Ldelete_by_idx(grp, ".", H5_INDEX_NAME, H5_ITER_INC, 0, H5P_DEFAULT),
        0);
    ASSERT_GE(H5Ldelete(grp, "k_2", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(grp, "k_3", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(grp, "k_4", H5P_DEFAULT), 0);

    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_COMPACT);
    EXPECT_EQ(info.nlinks, 1u);

    // Delete last link by idx in compact -> resets max_corder to 0.
    ASSERT_GE(
        H5Ldelete_by_idx(grp, ".", H5_INDEX_NAME, H5_ITER_INC, 0, H5P_DEFAULT),
        0);
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.nlinks, 0u);
    EXPECT_EQ(info.max_corder, 0);
  }

  // 3. Always-dense (0, 0): insert 1, remove 1 -> nlinks == 0 triggers
  // H5G__dense_delete directly without compact conversion.
  {
    ScopedFile f = CreateFileWithBounds("always_dense.h5", H5F_LIBVER_V18,
                                        H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 0, 0), 0);

    ScopedGroup grp(H5Gcreate2(f, "/always", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_GE(H5Lcreate_soft("/x", grp, "only", H5P_DEFAULT, H5P_DEFAULT), 0);

    H5G_info_t info{};
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_DENSE);

    ASSERT_GE(H5Ldelete(grp, "only", H5P_DEFAULT), 0);
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_COMPACT);
    EXPECT_EQ(info.nlinks, 0u);
  }

  // 4. Huge link (> 65536 bytes): forces compact -> dense and blocks
  // dense -> compact conversion (can_convert == false) when nlinks < min_dense.
  {
    ScopedFile f =
        CreateFileWithBounds("huge_link.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 8, 6), 0);

    ScopedGroup grp(H5Gcreate2(f, "/huge", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_GE(H5Lcreate_soft("/small", grp, "small_1", H5P_DEFAULT, H5P_DEFAULT),
              0);

    const std::string huge_name(66000, 'h');
    ASSERT_GE(H5Lcreate_soft("/small", grp, huge_name.c_str(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);

    H5G_info_t info{};
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_DENSE);
    EXPECT_EQ(info.nlinks, 2u);

    // Deleting small_1 keeps nlinks == 1 (< min_dense == 6), but huge_name
    // exceeds H5O_MESG_MAX_SIZE so group stays DENSE.
    ASSERT_GE(H5Ldelete(grp, "small_1", H5P_DEFAULT), 0);
    ASSERT_GE(H5Gget_info(grp, &info), 0);
    EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_DENSE);
    EXPECT_EQ(info.nlinks, 1u);
  }
}

TEST(H5GobjTest, IterateInfoNamesLookupAndErrorsAcrossAllFormats) {
  ScopedFile old_file =
      CreateFileWithBounds("ops_old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("ops_new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ScopedFile mnt_child = CreateFileWithBounds("ops_child.h5", H5F_LIBVER_V18,
                                              H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());
  ASSERT_TRUE(mnt_child.valid());

  // Old-style group.
  ScopedGroup g_stab(
      H5Gcreate2(old_file, "/stab", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_stab.valid());
  ASSERT_GE(H5Lcreate_soft("/a", g_stab, "beta", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/b", g_stab, "alpha", H5P_DEFAULT, H5P_DEFAULT), 0);

  // Compact group (untracked corder).
  ScopedGroup g_compact(
      H5Gcreate2(new_file, "/compact", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_compact.valid());
  ASSERT_GE(
      H5Lcreate_soft("/a", g_compact, "beta", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(
      H5Lcreate_soft("/b", g_compact, "alpha", H5P_DEFAULT, H5P_DEFAULT), 0);

  // Dense group (tracked + indexed corder).
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl, 2, 1), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup g_dense(
      H5Gcreate2(new_file, "/dense", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(g_dense.valid());
  ASSERT_GE(H5Lcreate_soft("/z", g_dense, "zeta", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/m", g_dense, "mu", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/a", g_dense, "alpha", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Mounted group in g_compact to verify mounted == true in H5G__obj_info.
  ScopedGroup g_mnt(
      H5Gcreate2(new_file, "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_mnt.valid());
  ASSERT_GE(H5Fmount(new_file, "/mnt", mnt_child, H5P_DEFAULT), 0);
  absl::Cleanup unmount_cleanup = [&] { H5Funmount(new_file, "/mnt"); };

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_loc_t* stab_oloc = GetGroupOloc(g_stab);
  H5O_loc_t* compact_oloc = GetGroupOloc(g_compact);
  H5O_loc_t* dense_oloc = GetGroupOloc(g_dense);
  H5O_loc_t* mnt_oloc = GetGroupOloc(g_mnt);
  ASSERT_NE(stab_oloc, nullptr);
  ASSERT_NE(compact_oloc, nullptr);
  ASSERT_NE(dense_oloc, nullptr);
  ASSERT_NE(mnt_oloc, nullptr);

  // H5G__obj_info across stab, compact, dense, and mounted.
  H5G_info_t info{};
  EXPECT_GE(H5G__obj_info(stab_oloc, &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  EXPECT_EQ(info.nlinks, 2u);

  EXPECT_GE(H5G__obj_info(compact_oloc, &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_COMPACT);
  EXPECT_EQ(info.nlinks, 2u);

  EXPECT_GE(H5G__obj_info(dense_oloc, &info), 0);
  EXPECT_EQ(info.storage_type, H5G_STORAGE_TYPE_DENSE);
  EXPECT_EQ(info.nlinks, 3u);

  EXPECT_GE(H5G__obj_info(mnt_oloc, &info), 0);
  EXPECT_TRUE(info.mounted);

  // H5G__obj_iterate across all 3 storage formats.
  {
    std::vector<std::string> names;
    hsize_t last = 0;
    EXPECT_EQ(H5G__obj_iterate(stab_oloc, H5_INDEX_NAME, H5_ITER_INC, 0, &last,
                               CollectLinkNamesCb, &names),
              0);
    EXPECT_THAT(names, ElementsAre("alpha", "beta"));

    names.clear();
    last = 0;
    EXPECT_EQ(H5G__obj_iterate(compact_oloc, H5_INDEX_NAME, H5_ITER_DEC, 0,
                               &last, CollectLinkNamesCb, &names),
              0);
    EXPECT_THAT(names, ElementsAre("beta", "alpha"));

    names.clear();
    last = 0;
    EXPECT_EQ(H5G__obj_iterate(dense_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC, 1,
                               &last, CollectLinkNamesCb, &names),
              0);
    EXPECT_THAT(names, ElementsAre("mu", "alpha"));
  }

  // H5G_obj_get_name_by_idx across all 3 formats.
  {
    std::array<char, 32> buf{};
    size_t len = 0;
    EXPECT_GE(H5G_obj_get_name_by_idx(stab_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                                      buf.data(), buf.size(), &len),
              0);
    EXPECT_STREQ(buf.data(), "alpha");

    EXPECT_GE(H5G_obj_get_name_by_idx(compact_oloc, H5_INDEX_NAME, H5_ITER_DEC,
                                      0, buf.data(), buf.size(), &len),
              0);
    EXPECT_STREQ(buf.data(), "beta");

    EXPECT_GE(H5G_obj_get_name_by_idx(dense_oloc, H5_INDEX_CRT_ORDER,
                                      H5_ITER_INC, 0, buf.data(), buf.size(),
                                      &len),
              0);
    EXPECT_STREQ(buf.data(), "zeta");
  }

  // H5G__obj_lookup and H5G_obj_lookup_by_idx across all 3 formats.
  for (H5O_loc_t* oloc : {stab_oloc, compact_oloc, dense_oloc}) {
    bool found = false;
    H5O_link_t lnk{};
    EXPECT_GE(H5G__obj_lookup(oloc, "alpha", &found, &lnk), 0);
    ASSERT_TRUE(found);
    H5O_msg_reset(H5O_LINK_ID, &lnk);

    found = false;
    lnk = H5O_link_t{};
    EXPECT_GE(H5G__obj_lookup(oloc, "nonexistent", &found, &lnk), 0);
    EXPECT_FALSE(found);

    lnk = H5O_link_t{};
    EXPECT_GE(H5G_obj_lookup_by_idx(oloc, H5_INDEX_NAME, H5_ITER_INC, 0, &lnk),
              0);
    EXPECT_STREQ(lnk.name, "alpha");
    H5O_msg_reset(H5O_LINK_ID, &lnk);
  }
  {
    H5O_link_t lnk{};
    EXPECT_GE(H5G_obj_lookup_by_idx(dense_oloc, H5_INDEX_CRT_ORDER, H5_ITER_DEC,
                                    0, &lnk),
              0);
    EXPECT_STREQ(lnk.name, "alpha");
    H5O_msg_reset(H5O_LINK_ID, &lnk);
  }

  // Error paths: out-of-bounds skip/index and CRT_ORDER on untracked/stab.
  ScopedErrorSilence silence;
  hsize_t last = 0;
  size_t name_len = 0;
  std::array<char, 32> buf{};
  H5O_link_t lnk{};
  std::vector<std::string> dummy;

  EXPECT_LT(H5G__obj_iterate(compact_oloc, H5_INDEX_NAME, H5_ITER_INC, 99,
                             &last, CollectLinkNamesCb, &dummy),
            0);
  EXPECT_LT(H5G__obj_iterate(compact_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                             &last, CollectLinkNamesCb, &dummy),
            0);
  EXPECT_LT(H5G__obj_iterate(stab_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC, 0,
                             &last, CollectLinkNamesCb, &dummy),
            0);

  EXPECT_LT(H5G_obj_get_name_by_idx(compact_oloc, H5_INDEX_CRT_ORDER,
                                    H5_ITER_INC, 0, buf.data(), buf.size(),
                                    &name_len),
            0);
  EXPECT_LT(H5G_obj_get_name_by_idx(stab_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC,
                                    0, buf.data(), buf.size(), &name_len),
            0);
  EXPECT_LT(H5G_obj_get_name_by_idx(dense_oloc, H5_INDEX_NAME, H5_ITER_INC, 99,
                                    buf.data(), buf.size(), &name_len),
            0);
  EXPECT_LT(H5G_obj_get_name_by_idx(compact_oloc, H5_INDEX_NAME, H5_ITER_INC,
                                    99, buf.data(), buf.size(), &name_len),
            0);
  EXPECT_LT(H5G_obj_get_name_by_idx(stab_oloc, H5_INDEX_NAME, H5_ITER_INC, 99,
                                    buf.data(), buf.size(), &name_len),
            0);

  EXPECT_LT(H5G_obj_lookup_by_idx(compact_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC,
                                  0, &lnk),
            0);
  EXPECT_LT(
      H5G_obj_lookup_by_idx(stab_oloc, H5_INDEX_CRT_ORDER, H5_ITER_INC, 0, &lnk),
      0);
  EXPECT_LT(
      H5G_obj_lookup_by_idx(dense_oloc, H5_INDEX_NAME, H5_ITER_INC, 99, &lnk),
      0);
  EXPECT_LT(H5G_obj_lookup_by_idx(compact_oloc, H5_INDEX_NAME, H5_ITER_INC, 99,
                                  &lnk),
            0);
  EXPECT_LT(
      H5G_obj_lookup_by_idx(stab_oloc, H5_INDEX_NAME, H5_ITER_INC, 99, &lnk),
      0);

  EXPECT_LT(H5G_obj_remove(dense_oloc, nullptr, "nonexistent"), 0);
  EXPECT_LT(H5G_obj_remove(stab_oloc, nullptr, "nonexistent"), 0);

  EXPECT_LT(H5G_obj_remove_by_idx(compact_oloc, nullptr, H5_INDEX_CRT_ORDER,
                                  H5_ITER_INC, 0),
            0);
  EXPECT_LT(H5G_obj_remove_by_idx(stab_oloc, nullptr, H5_INDEX_CRT_ORDER,
                                  H5_ITER_INC, 0),
            0);
  EXPECT_LT(H5G_obj_remove_by_idx(dense_oloc, nullptr, H5_INDEX_NAME,
                                  H5_ITER_INC, 99),
            0);
  EXPECT_LT(H5G_obj_remove_by_idx(compact_oloc, nullptr, H5_INDEX_NAME,
                                  H5_ITER_INC, 99),
            0);
  EXPECT_LT(H5G_obj_remove_by_idx(stab_oloc, nullptr, H5_INDEX_NAME,
                                  H5_ITER_INC, 99),
            0);
}

herr_t FailIterCb(const H5O_link_t* /*lnk*/, void* /*op_data*/) { return -1; }

H5O_mesg_t* FindOhMesg(H5O_t* oh, unsigned type_id) {
  for (size_t i = 0; i < oh->nmesgs; ++i) {
    if (oh->mesg[i].type && oh->mesg[i].type->id == type_id) {
      return &oh->mesg[i];
    }
  }
  return nullptr;
}

TEST(H5GobjTest, PropListErrorsBadOlocAndLinkMutationErrors) {
  constexpr haddr_t kBadAddr = 0xdeadbeef00ULL;

  ScopedFile old_file = CreateFileWithBounds(
      "err_old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ScopedFile new_file =
      CreateFileWithBounds("err_new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  ASSERT_TRUE(new_file.valid());

  ScopedGroup stab_g(
      H5Gcreate2(old_file, "/stab", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(stab_g.valid());
  ASSERT_GE(H5Lcreate_soft("/t", stab_g, "s0", H5P_DEFAULT, H5P_DEFAULT), 0);

  ScopedGroup compact_g(
      H5Gcreate2(new_file, "/compact", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(compact_g.valid());
  ASSERT_GE(H5Lcreate_soft("/t", compact_g, "c0", H5P_DEFAULT, H5P_DEFAULT), 0);

  ScopedPlist gcpl_dense(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_dense.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_dense, 2, 1), 0);
  ScopedGroup dense_g(
      H5Gcreate2(new_file, "/dense", H5P_DEFAULT, gcpl_dense, H5P_DEFAULT));
  ASSERT_TRUE(dense_g.valid());
  for (int i = 0; i < 4; ++i) {
    const std::string name = "d" + std::to_string(i);
    ASSERT_GE(
        H5Lcreate_soft("/t", dense_g, name.c_str(), H5P_DEFAULT, H5P_DEFAULT),
        0);
  }

  ScopedGroup adj_g(
      H5Gcreate2(new_file, "/adj", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(adj_g.valid());

  ScopedPlist gcpl_c2d(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_c2d.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl_c2d, 2, 1), 0);
  ScopedGroup c2d_g(
      H5Gcreate2(new_file, "/c2d", H5P_DEFAULT, gcpl_c2d, H5P_DEFAULT));
  ASSERT_TRUE(c2d_g.valid());
  ASSERT_GE(H5Lcreate_soft("/t", c2d_g, "u0", H5P_DEFAULT, H5P_DEFAULT), 0);
  ASSERT_GE(H5Lcreate_soft("/t", c2d_g, "u1", H5P_DEFAULT, H5P_DEFAULT), 0);

  ASSERT_GE(H5Fflush(new_file, H5F_SCOPE_GLOBAL), 0);
  std::vector<uint8_t> ro_img = ExtractFileImage(new_file);
  ASSERT_FALSE(ro_img.empty());
  ScopedFile ro_file = OpenFileImage(ro_img, H5F_ACC_RDONLY, "ro_new_err.h5");
  ASSERT_TRUE(ro_file.valid());

  auto* new_f = static_cast<H5F_t*>(H5VL_object_verify(new_file, H5I_FILE));
  auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
  ASSERT_NE(new_f, nullptr);
  ASSERT_NE(ro_f, nullptr);

  H5O_loc_t* stab_oloc = GetGroupOloc(stab_g);
  H5O_loc_t* compact_oloc = GetGroupOloc(compact_g);
  H5O_loc_t* dense_oloc = GetGroupOloc(dense_g);
  H5O_loc_t* adj_oloc = GetGroupOloc(adj_g);
  H5O_loc_t* c2d_oloc = GetGroupOloc(c2d_g);
  ASSERT_NE(stab_oloc, nullptr);
  ASSERT_NE(compact_oloc, nullptr);
  ASSERT_NE(dense_oloc, nullptr);
  ASSERT_NE(adj_oloc, nullptr);
  ASSERT_NE(c2d_oloc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedGroup ro_compact(H5Gopen2(ro_file, "/compact", H5P_DEFAULT));
  ASSERT_TRUE(ro_compact.valid());
  H5O_loc_t* ro_compact_oloc = GetGroupOloc(ro_compact);
  ASSERT_NE(ro_compact_oloc, nullptr);

  H5O_stab_t orig_stab{};
  ASSERT_NE(H5O_msg_read(stab_oloc, H5O_STAB_ID, &orig_stab), nullptr);
  H5O_linfo_t orig_dense_linfo{};
  ASSERT_NE(H5O_msg_read(dense_oloc, H5O_LINFO_ID, &orig_dense_linfo), nullptr);

  ScopedErrorSilence silence;

  // Missing link info / pipeline properties on GCPL + read-only obj_create.
  {
    ScopedPlist gcpl_no_linfo(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_no_linfo.valid());
    ASSERT_GE(H5Premove(gcpl_no_linfo, H5G_CRT_LINK_INFO_NAME), 0);
    H5G_obj_create_t gcrt{};
    gcrt.gcpl_id = gcpl_no_linfo;
    H5O_loc_t out_oloc{};
    EXPECT_LT(H5G__obj_create(new_f, &gcrt, &out_oloc), 0);

    ScopedPlist gcpl_no_pline(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_no_pline.valid());
    ASSERT_GE(H5Premove(gcpl_no_pline, H5O_CRT_PIPELINE_NAME), 0);
    gcrt.gcpl_id = gcpl_no_pline;
    EXPECT_LT(H5G__obj_create(new_f, &gcrt, &out_oloc), 0);

    gcrt.gcpl_id = H5P_GROUP_CREATE_DEFAULT;
    EXPECT_LT(H5G__obj_create(ro_f, &gcrt, &out_oloc), 0);
  }

  // Bad object header address triggers H5G__obj_get_linfo / H5G_open failure.
  {
    H5O_loc_t bad_oloc{};
    bad_oloc.file = new_f;
    bad_oloc.addr = kBadAddr;
    bad_oloc.holding_file = false;

    H5O_linfo_t linfo{};
    EXPECT_LT(H5G__obj_get_linfo(&bad_oloc, &linfo), 0);

    H5O_link_t lnk{};
    lnk.type = H5L_TYPE_SOFT;
    lnk.cset = H5T_CSET_ASCII;
    lnk.name = const_cast<char*>("x");
    lnk.u.soft.name = const_cast<char*>("/t");
    EXPECT_LT(H5G_obj_insert(&bad_oloc, &lnk, false, H5O_TYPE_UNKNOWN, nullptr),
              0);

    hsize_t last_lnk = 0;
    int dummy = 0;
    EXPECT_LT(H5G__obj_iterate(&bad_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_lnk, FailIterCb, &dummy),
              0);
    H5G_info_t ginfo{};
    EXPECT_LT(H5G__obj_info(&bad_oloc, &ginfo), 0);
    std::array<char, 32> buf{};
    size_t name_len = 0;
    EXPECT_LT(H5G_obj_get_name_by_idx(&bad_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                                      buf.data(), buf.size(), &name_len),
              0);
    EXPECT_LT(H5G_obj_remove(&bad_oloc, nullptr, "x"), 0);
    EXPECT_LT(H5G_obj_remove_by_idx(&bad_oloc, nullptr, H5_INDEX_NAME,
                                    H5_ITER_INC, 0),
              0);
    bool found = false;
    EXPECT_LT(H5G__obj_lookup(&bad_oloc, "x", &found, &lnk), 0);
    EXPECT_LT(H5G_obj_lookup_by_idx(&bad_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                                    &lnk),
              0);
  }

  // Error callback across all three group formats + remove compact missing.
  {
    hsize_t last_lnk = 0;
    int dummy = 0;
    EXPECT_LT(H5G__obj_iterate(dense_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_lnk, FailIterCb, &dummy),
              0);
    EXPECT_LT(H5G__obj_iterate(compact_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_lnk, FailIterCb, &dummy),
              0);
    EXPECT_LT(H5G__obj_iterate(stab_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_lnk, FailIterCb, &dummy),
              0);
    EXPECT_LT(H5G_obj_remove(ro_compact_oloc, nullptr, "c0"), 0);
  }

  // Duplicate/read-only insertions across stab, dense, compact, and hard-link
  // adj_link failure on invalid target address.
  {
    H5O_link_t dup{};
    dup.type = H5L_TYPE_SOFT;
    dup.cset = H5T_CSET_ASCII;
    dup.u.soft.name = const_cast<char*>("/other");
    dup.name = const_cast<char*>("s0");
    EXPECT_LT(
        H5G_obj_insert(stab_oloc, &dup, false, H5O_TYPE_DATASET, nullptr), 0);
    dup.name = const_cast<char*>("d0");
    EXPECT_LT(
        H5G_obj_insert(dense_oloc, &dup, false, H5O_TYPE_UNKNOWN, nullptr), 0);
    dup.name = const_cast<char*>("c_ro");
    EXPECT_LT(
        H5G_obj_insert(ro_compact_oloc, &dup, false, H5O_TYPE_UNKNOWN, nullptr),
        0);

    H5O_linfo_t adj_orig{};
    ASSERT_NE(H5O_msg_read(adj_oloc, H5O_LINFO_ID, &adj_orig), nullptr);
    H5O_link_t bad_hard{};
    bad_hard.type = H5L_TYPE_HARD;
    bad_hard.cset = H5T_CSET_ASCII;
    bad_hard.name = const_cast<char*>("bad_hard");
    bad_hard.u.hard.addr = kBadAddr;
    EXPECT_LT(
        H5G_obj_insert(adj_oloc, &bad_hard, true, H5O_TYPE_UNKNOWN, nullptr),
        0);
    EXPECT_GE(H5O_msg_remove(adj_oloc, H5O_LINK_ID, 0, false), 0);
    EXPECT_GE(H5O_msg_write(adj_oloc, H5O_LINFO_ID, 0, 0, &adj_orig), 0);
  }

  // Corrupt stab btree_addr and heap_addr cleanly, restoring after checks.
  {
    absl::Cleanup restore_stab = [&] {
      H5O_msg_write(stab_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &orig_stab);
    };
    H5O_stab_t bad_stab = orig_stab;
    bad_stab.btree_addr = kBadAddr;
    ASSERT_GE(
        H5O_msg_write(stab_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_stab),
        0);
    H5G_info_t ginfo{};
    EXPECT_LT(H5G__obj_info(stab_oloc, &ginfo), 0);

    bad_stab = orig_stab;
    bad_stab.heap_addr = kBadAddr;
    ASSERT_GE(
        H5O_msg_write(stab_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &bad_stab),
        0);
    bool found = false;
    H5O_link_t lnk{};
    EXPECT_LT(H5G__obj_lookup(stab_oloc, "s0", &found, &lnk), 0);
  }

  // Corrupt dense name_bt2_addr cleanly, including nlinks == HSIZET_MAX path.
  {
    absl::Cleanup restore_dense = [&] {
      H5O_msg_write(dense_oloc, H5O_LINFO_ID, 0, 0, &orig_dense_linfo);
    };
    H5O_linfo_t bad_linfo = orig_dense_linfo;
    bad_linfo.name_bt2_addr = kBadAddr;
    ASSERT_GE(H5O_msg_write(dense_oloc, H5O_LINFO_ID, 0, 0, &bad_linfo), 0);
    bool found = false;
    H5O_link_t lnk{};
    EXPECT_LT(H5G__obj_lookup(dense_oloc, "d0", &found, &lnk), 0);
    EXPECT_LT(H5G_obj_lookup_by_idx(dense_oloc, H5_INDEX_NAME, H5_ITER_INC, 0,
                                    &lnk),
              0);

    bad_linfo.nlinks = HSIZET_MAX;
    ASSERT_GE(H5O_msg_write(dense_oloc, H5O_LINFO_ID, 0, 0, &bad_linfo), 0);
    H5O_linfo_t out_linfo{};
    EXPECT_LT(H5G__obj_get_linfo(dense_oloc, &out_linfo), 0);
  }

  // Duplicate H5O_LINK_ID inside compact->dense conversion hits
  // H5G__obj_compact_to_dense_cb error path cleanly.
  {
    H5O_linfo_t c2d_orig{};
    ASSERT_NE(H5O_msg_read(c2d_oloc, H5O_LINFO_ID, &c2d_orig), nullptr);
    H5O_link_t dup_u0{};
    dup_u0.type = H5L_TYPE_SOFT;
    dup_u0.cset = H5T_CSET_ASCII;
    dup_u0.name = const_cast<char*>("u0");
    dup_u0.u.soft.name = const_cast<char*>("/dup");
    ASSERT_GE(H5O_msg_create(c2d_oloc, H5O_LINK_ID, 0, 0, &dup_u0), 0);
    absl::Cleanup clean_dup = [&] {
      H5O_msg_remove(c2d_oloc, H5O_LINK_ID, 2, false);
    };

    H5O_link_t add_u2{};
    add_u2.type = H5L_TYPE_SOFT;
    add_u2.cset = H5T_CSET_ASCII;
    add_u2.name = const_cast<char*>("u2");
    add_u2.u.soft.name = const_cast<char*>("/t");
    EXPECT_LT(
        H5G_obj_insert(c2d_oloc, &add_u2, false, H5O_TYPE_UNKNOWN, nullptr), 0);
    ASSERT_GE(H5O_msg_write(c2d_oloc, H5O_LINFO_ID, 0, 0, &c2d_orig), 0);
  }
}

TEST(H5GobjTest, CorruptMessagesReadOnlyAndConversionErrors) {
  // 1. Old-format file reopened RDONLY -> UTF-8 link triggers H5O_msg_create
  //    failure during stab->new conversion.
  {
    std::vector<uint8_t> old_img;
    {
      ScopedFile f = CreateFileWithBounds("ro_old_utf8.h5", H5F_LIBVER_EARLIEST,
                                          H5F_LIBVER_LATEST);
      ASSERT_TRUE(f.valid());
      ScopedGroup g(
          H5Gcreate2(f, "/s", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
      ASSERT_GE(H5Lcreate_soft("/t", g, "a", H5P_DEFAULT, H5P_DEFAULT), 0);
      g.reset();
      ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
      old_img = ExtractFileImage(f);
      ASSERT_FALSE(old_img.empty());
    }
    ScopedFile ro_f = OpenFileImage(old_img, H5F_ACC_RDONLY, "ro_old_u8.h5");
    ASSERT_TRUE(ro_f.valid());
    ScopedGroup ro_g(H5Gopen2(ro_f, "/s", H5P_DEFAULT));
    ASSERT_TRUE(ro_g.valid());
    H5O_loc_t* oloc = GetGroupOloc(ro_g);
    ASSERT_NE(oloc, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    H5O_link_t utf8_lnk{};
    utf8_lnk.type = H5L_TYPE_SOFT;
    utf8_lnk.cset = H5T_CSET_UTF8;
    utf8_lnk.name = const_cast<char*>("u8");
    utf8_lnk.u.soft.name = const_cast<char*>("/t");
    EXPECT_LT(
        H5G_obj_insert(oloc, &utf8_lnk, false, H5O_TYPE_UNKNOWN, nullptr), 0);
  }

  // 2. New-format image with compact/dense groups reopened RDONLY and RDWR to
  //    exercise decode failures (GINFO, PLINE, LINK) and RDONLY dense_create.
  std::vector<uint8_t> new_img;
  {
    ScopedFile f =
        CreateFileWithBounds("corrupt_new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());

    ScopedPlist gcpl_max1(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_max1.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl_max1, 1, 1), 0);
    ScopedGroup g_max1(
        H5Gcreate2(f, "/max1", H5P_DEFAULT, gcpl_max1, H5P_DEFAULT));
    ASSERT_TRUE(g_max1.valid());
    ASSERT_GE(H5Lcreate_soft("/t", g_max1, "k0", H5P_DEFAULT, H5P_DEFAULT), 0);

    ScopedPlist gcpl_def(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_def.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl_def, 1, 1), 0);
    ASSERT_GE(H5Pset_deflate(gcpl_def, 6), 0);
    ScopedGroup g_def(
        H5Gcreate2(f, "/def", H5P_DEFAULT, gcpl_def, H5P_DEFAULT));
    ASSERT_TRUE(g_def.valid());
    ASSERT_GE(H5Lcreate_soft("/t", g_def, "d0", H5P_DEFAULT, H5P_DEFAULT), 0);

    ScopedPlist gcpl_d42(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_d42.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl_d42, 4, 2), 0);
    ScopedGroup g_d42(
        H5Gcreate2(f, "/d42", H5P_DEFAULT, gcpl_d42, H5P_DEFAULT));
    ASSERT_TRUE(g_d42.valid());
    for (int i = 0; i < 5; ++i) {
      const std::string name = "x" + std::to_string(i);
      ASSERT_GE(
          H5Lcreate_soft("/t", g_d42, name.c_str(), H5P_DEFAULT, H5P_DEFAULT),
          0);
    }
    for (int i = 0; i < 3; ++i) {
      const std::string name = "x" + std::to_string(i);
      ASSERT_GE(H5Ldelete(g_d42, name.c_str(), H5P_DEFAULT), 0);
    }

    g_max1.reset();
    g_def.reset();
    g_d42.reset();
    ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    new_img = ExtractFileImage(f);
    ASSERT_FALSE(new_img.empty());
  }

  // RDONLY checks: H5G__dense_create fails + H5G__compact_lookup LINK decode.
  {
    ScopedFile ro_f = OpenFileImage(new_img, H5F_ACC_RDONLY, "ro_corrupt.h5");
    ASSERT_TRUE(ro_f.valid());
    ScopedGroup ro_max1(H5Gopen2(ro_f, "/max1", H5P_DEFAULT));
    ASSERT_TRUE(ro_max1.valid());
    H5O_loc_t* oloc = GetGroupOloc(ro_max1);
    ASSERT_NE(oloc, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    H5O_link_t k1{};
    k1.type = H5L_TYPE_SOFT;
    k1.cset = H5T_CSET_ASCII;
    k1.name = const_cast<char*>("k1");
    k1.u.soft.name = const_cast<char*>("/t");
    EXPECT_LT(H5G_obj_insert(oloc, &k1, false, H5O_TYPE_UNKNOWN, nullptr), 0);

    H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    H5O_mesg_t* lnk_mesg = FindOhMesg(oh, H5O_LINK_ID);
    ASSERT_NE(lnk_mesg, nullptr);
    ASSERT_NE(lnk_mesg->raw, nullptr);
    ASSERT_GT(lnk_mesg->raw_size, 0u);
    void* orig_native = lnk_mesg->native;
    const uint8_t orig_b0 = lnk_mesg->raw[0];
    lnk_mesg->native = nullptr;
    lnk_mesg->raw[0] = 99;
    ASSERT_GE(H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET), 0);

    bool found = false;
    H5O_link_t out_lnk{};
    EXPECT_LT(H5G__obj_lookup(oloc, "k0", &found, &out_lnk), 0);

    oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    lnk_mesg = FindOhMesg(oh, H5O_LINK_ID);
    ASSERT_NE(lnk_mesg, nullptr);
    lnk_mesg->native = orig_native;
    lnk_mesg->raw[0] = orig_b0;
    ASSERT_GE(H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET), 0);
  }

  // RDWR checks: GINFO decode fail on compact & dense, PLINE decode fail.
  {
    ScopedFile rw_f = OpenFileImage(new_img, H5F_ACC_RDWR, "rw_corrupt.h5");
    ASSERT_TRUE(rw_f.valid());
    ScopedGroup rw_max1(H5Gopen2(rw_f, "/max1", H5P_DEFAULT));
    ScopedGroup rw_def(H5Gopen2(rw_f, "/def", H5P_DEFAULT));
    ScopedGroup rw_d42(H5Gopen2(rw_f, "/d42", H5P_DEFAULT));
    ASSERT_TRUE(rw_max1.valid());
    ASSERT_TRUE(rw_def.valid());
    ASSERT_TRUE(rw_d42.valid());

    H5O_loc_t* max1_oloc = GetGroupOloc(rw_max1);
    H5O_loc_t* def_oloc = GetGroupOloc(rw_def);
    H5O_loc_t* d42_oloc = GetGroupOloc(rw_d42);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    // 2a. Corrupt GINFO in /max1 -> H5G_obj_insert fails at line 466.
    {
      H5O_t* oh = H5O_protect(max1_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      H5O_mesg_t* ginfo_mesg = FindOhMesg(oh, H5O_GINFO_ID);
      ASSERT_NE(ginfo_mesg, nullptr);
      ASSERT_NE(ginfo_mesg->raw, nullptr);
      void* orig_native = ginfo_mesg->native;
      const uint8_t orig_b0 = ginfo_mesg->raw[0];
      ginfo_mesg->native = nullptr;
      ginfo_mesg->raw[0] = 99;
      ASSERT_GE(H5O_unprotect(max1_oloc, oh, H5AC__NO_FLAGS_SET), 0);

      H5O_link_t lnk{};
      lnk.type = H5L_TYPE_SOFT;
      lnk.cset = H5T_CSET_ASCII;
      lnk.name = const_cast<char*>("k1");
      lnk.u.soft.name = const_cast<char*>("/t");
      EXPECT_LT(
          H5G_obj_insert(max1_oloc, &lnk, false, H5O_TYPE_UNKNOWN, nullptr), 0);

      oh = H5O_protect(max1_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      ginfo_mesg = FindOhMesg(oh, H5O_GINFO_ID);
      ASSERT_NE(ginfo_mesg, nullptr);
      ginfo_mesg->native = orig_native;
      ginfo_mesg->raw[0] = orig_b0;
      ASSERT_GE(H5O_unprotect(max1_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    }

    // 2b. Corrupt PLINE in /def -> H5G_obj_insert fails at line 486.
    {
      H5O_t* oh = H5O_protect(def_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      H5O_mesg_t* pline_mesg = FindOhMesg(oh, H5O_PLINE_ID);
      ASSERT_NE(pline_mesg, nullptr);
      ASSERT_NE(pline_mesg->raw, nullptr);
      void* orig_native = pline_mesg->native;
      const uint8_t orig_b0 = pline_mesg->raw[0];
      pline_mesg->native = nullptr;
      pline_mesg->raw[0] = 99;
      ASSERT_GE(H5O_unprotect(def_oloc, oh, H5AC__NO_FLAGS_SET), 0);

      H5O_link_t lnk{};
      lnk.type = H5L_TYPE_SOFT;
      lnk.cset = H5T_CSET_ASCII;
      lnk.name = const_cast<char*>("d1");
      lnk.u.soft.name = const_cast<char*>("/t");
      EXPECT_LT(
          H5G_obj_insert(def_oloc, &lnk, false, H5O_TYPE_UNKNOWN, nullptr), 0);

      oh = H5O_protect(def_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      pline_mesg = FindOhMesg(oh, H5O_PLINE_ID);
      ASSERT_NE(pline_mesg, nullptr);
      pline_mesg->native = orig_native;
      pline_mesg->raw[0] = orig_b0;
      ASSERT_GE(H5O_unprotect(def_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    }

    // 2c. Corrupt GINFO in /d42 (nlinks == 2, min_dense == 2) -> removing "x3"
    //     hits H5G__obj_remove_update_linfo failure at line 848.
    {
      H5O_t* oh = H5O_protect(d42_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      H5O_mesg_t* ginfo_mesg = FindOhMesg(oh, H5O_GINFO_ID);
      ASSERT_NE(ginfo_mesg, nullptr);
      ASSERT_NE(ginfo_mesg->raw, nullptr);
      void* orig_native = ginfo_mesg->native;
      const uint8_t orig_b0 = ginfo_mesg->raw[0];
      ginfo_mesg->native = nullptr;
      ginfo_mesg->raw[0] = 99;
      ASSERT_GE(H5O_unprotect(d42_oloc, oh, H5AC__NO_FLAGS_SET), 0);

      EXPECT_LT(H5G_obj_remove(d42_oloc, nullptr, "x3"), 0);

      oh = H5O_protect(d42_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      ginfo_mesg = FindOhMesg(oh, H5O_GINFO_ID);
      ASSERT_NE(ginfo_mesg, nullptr);
      ginfo_mesg->native = orig_native;
      ginfo_mesg->raw[0] = orig_b0;
      ASSERT_GE(H5O_unprotect(d42_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
