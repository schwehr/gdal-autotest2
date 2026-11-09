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
// Tests group object header class callbacks, B-tree/heap storage queries, and
// group copy user-data handling (third_party/hdf5/src/H5Goh.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Goh.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from internal cache
// and object header structs. Match that layout across internal headers.
#ifndef NDEBUG
#define H5G_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B2_FRIEND
#define H5G_FRIEND
#define H5HF_FRIEND
#define H5O_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5Opkg.h"
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

// RAII wrapper for pushing/popping an HDF5 API context and restoring metadata
// tagging around direct internal library calls.
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

void PopulateLinks(hid_t loc, int count, const char* prefix) {
  for (int i = 0; i < count; ++i) {
    const std::string name = std::string(prefix) + std::to_string(i);
    ASSERT_GE(H5Lcreate_soft("/nowhere", loc, name.c_str(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
  }
}

TEST(H5GohTest, ObjClassDescriptorFields) {
  const H5O_obj_class_t* cls = &H5O_OBJ_GROUP[0];
  EXPECT_EQ(cls->type, H5O_TYPE_GROUP);
  EXPECT_STREQ(cls->name, "group");
  EXPECT_NE(cls->get_copy_file_udata, nullptr);
  EXPECT_NE(cls->free_copy_file_udata, nullptr);
  EXPECT_NE(cls->isa, nullptr);
  EXPECT_NE(cls->open, nullptr);
  EXPECT_NE(cls->create, nullptr);
  EXPECT_NE(cls->get_oloc, nullptr);
  EXPECT_NE(cls->bh_info, nullptr);
  EXPECT_EQ(cls->flush, nullptr);
}

TEST(H5GohTest, NativeMetaSizeCoversAllGroupFormats) {
  // Old-style symbol table group.
  ScopedFile old_file =
      CreateFileWithBounds("old.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(old_file.valid());
  {
    ScopedGroup grp(H5Gcreate2(old_file, "/stab_grp", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    PopulateLinks(grp, 4, "link_");

    H5O_native_info_t ninfo{};
    ASSERT_GE(
        H5Oget_native_info(grp, &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0u);
    EXPECT_GT(ninfo.meta_size.obj.heap_size, 0u);
  }

  // New-format file: compact, dense without corder index, and dense with index.
  ScopedFile new_file =
      CreateFileWithBounds("new.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(new_file.valid());

  {
    ScopedGroup compact(H5Gcreate2(new_file, "/compact", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());
    PopulateLinks(compact, 2, "c_");

    H5O_native_info_t ninfo{};
    ASSERT_GE(
        H5Oget_native_info(compact, &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_EQ(ninfo.meta_size.obj.index_size, 0u);
    EXPECT_EQ(ninfo.meta_size.obj.heap_size, 0u);
  }

  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 2, 1), 0);
    ScopedGroup dense_name(
        H5Gcreate2(new_file, "/dense_name", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(dense_name.valid());
    PopulateLinks(dense_name, 5, "n_");

    H5O_native_info_t ninfo{};
    ASSERT_GE(
        H5Oget_native_info(dense_name, &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0u);
    EXPECT_GT(ninfo.meta_size.obj.heap_size, 0u);
  }

  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 2, 1), 0);
    ASSERT_GE(H5Pset_link_creation_order(
                  gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);
    ScopedGroup dense_corder(
        H5Gcreate2(new_file, "/dense_corder", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(dense_corder.valid());
    PopulateLinks(dense_corder, 5, "o_");

    H5O_native_info_t ninfo{};
    ASSERT_GE(
        H5Oget_native_info(dense_corder, &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_GT(ninfo.meta_size.obj.index_size, 0u);
    EXPECT_GT(ninfo.meta_size.obj.heap_size, 0u);
  }
}

TEST(H5GohTest, CopyGroupExercisesCopyUdataWithAndWithoutPipeline) {
  ScopedFile src =
      CreateFileWithBounds("copy_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ScopedFile dst =
      CreateFileWithBounds("copy_dst.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Plain group subtree.
  {
    ScopedGroup g(
        H5Gcreate2(src, "/plain", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    ScopedGroup sub(
        H5Gcreate2(g, "child", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
  }

  // Group configured with a filter pipeline message (frees udata->src_pline).
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_deflate(gcpl, 6), 0);
    ScopedGroup filtered(
        H5Gcreate2(src, "/filtered", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(filtered.valid());
    PopulateLinks(filtered, 2, "f_");
  }

  ASSERT_GE(H5Ocopy(src, "/plain", dst, "/plain_copy", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src, "/filtered", dst, "/filtered_copy", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  ScopedObject opened(H5Oopen(dst, "/filtered_copy", H5P_DEFAULT));
  EXPECT_TRUE(opened.valid());
}

TEST(H5GohTest, DirectObjClassCallbacksSuccessAndErrors) {
  ScopedFile file =
      CreateFileWithBounds("direct.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  const H5O_obj_class_t* cls = &H5O_OBJ_GROUP[0];
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // get_copy_file_udata / free_copy_file_udata directly.
  void* udata = cls->get_copy_file_udata();
  ASSERT_NE(udata, nullptr);
  cls->free_copy_file_udata(udata);

  // get_oloc valid & invalid.
  H5O_loc_t* grp_oloc = cls->get_oloc(grp);
  ASSERT_NE(grp_oloc, nullptr);
  EXPECT_NE(grp_oloc->file, nullptr);
  EXPECT_TRUE(H5_addr_defined(grp_oloc->addr));
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(cls->get_oloc(H5I_INVALID_HID), nullptr);
  }

  // isa & bh_info on group vs dataset object header.
  H5O_t* grp_oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(grp_oh, nullptr);
  EXPECT_GT(cls->isa(grp_oh), 0);
  H5_ih_info_t bh_info{};
  EXPECT_GE(cls->bh_info(grp_oloc, grp_oh, &bh_info), 0);
  EXPECT_GT(bh_info.index_size, 0u);
  EXPECT_GT(bh_info.heap_size, 0u);
  ASSERT_GE(H5O_unprotect(grp_oloc, grp_oh, H5AC__NO_FLAGS_SET), 0);

  haddr_t dset_addr = HADDR_UNDEF;
  {
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file, "/dset", H5T_NATIVE_INT, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    H5O_loc_t* dset_oloc = H5O_OBJ_DATASET[0].get_oloc(dset);
    ASSERT_NE(dset_oloc, nullptr);
    dset_addr = dset_oloc->addr;
    H5O_t* dset_oh = H5O_protect(dset_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(dset_oh, nullptr);
    EXPECT_EQ(cls->isa(dset_oh), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(cls->bh_info(dset_oloc, dset_oh, &bh_info), 0);
    }
    ASSERT_GE(H5O_unprotect(dset_oloc, dset_oh, H5AC__NO_FLAGS_SET), 0);
  }

  // Corrupt a scratch group's STAB btree_addr to cover bh_info stab failure.
  {
    ScopedGroup bad_grp(H5Gcreate2(file, "/bad_stab", H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(bad_grp.valid());
    H5O_loc_t* bad_oloc = cls->get_oloc(bad_grp);
    ASSERT_NE(bad_oloc, nullptr);
    H5O_stab_t orig_stab{};
    ASSERT_NE(H5O_msg_read(bad_oloc, H5O_STAB_ID, &orig_stab), nullptr);
    H5O_stab_t corrupt_stab = orig_stab;
    corrupt_stab.btree_addr = HADDR_UNDEF;
    ASSERT_GE(H5O_msg_write(bad_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE,
                            &corrupt_stab),
              0);
    H5O_t* bad_oh = H5O_protect(bad_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(bad_oh, nullptr);
    {
      ScopedErrorSilence silence;
      H5_ih_info_t dummy_bh{};
      EXPECT_LT(cls->bh_info(bad_oloc, bad_oh, &dummy_bh), 0);
    }
    ASSERT_GE(H5O_unprotect(bad_oloc, bad_oh, H5AC__NO_FLAGS_SET), 0);
    // Restore original STAB so closing cleans up without errors.
    ASSERT_GE(
        H5O_msg_write(bad_oloc, H5O_STAB_ID, 0, H5O_UPDATE_FORCE, &orig_stab),
        0);
  }

  // open callback: H5G_open takes shallow ownership and resets loc->oloc/path,
  // so pass deep copies of valid_loc and error locations.
  H5G_t* opened_base = static_cast<H5G_t*>(H5VL_object_verify(grp, H5I_GROUP));
  ASSERT_NE(opened_base, nullptr);
  H5O_loc_t tmp_oloc{};
  H5G_name_t tmp_path{};
  H5O_loc_reset(&tmp_oloc);
  H5G_name_reset(&tmp_path);
  ASSERT_GE(H5O_loc_copy_deep(&tmp_oloc, grp_oloc), 0);
  ASSERT_GE(H5G_name_copy(&tmp_path, H5G_nameof(opened_base), H5_COPY_DEEP), 0);
  H5G_loc_t valid_loc{&tmp_oloc, &tmp_path};
  H5I_type_t opened_type = H5I_UNINIT;
  void* reopened = cls->open(&valid_loc, &opened_type);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(opened_type, H5I_GROUP);
  ASSERT_GE(H5G_close(static_cast<H5G_t*>(reopened)), 0);

  H5O_loc_t bad_oloc{};
  H5O_loc_reset(&bad_oloc);
  bad_oloc.file = grp_oloc->file;
  bad_oloc.addr = static_cast<haddr_t>(0x7ffffff0);
  H5G_name_t bad_path{};
  H5G_name_reset(&bad_path);
  H5G_loc_t bad_loc{&bad_oloc, &bad_path};
  {
    ScopedErrorSilence silence;
    H5I_type_t err_type = H5I_UNINIT;
    EXPECT_EQ(cls->open(&bad_loc, &err_type), nullptr);
  }

  // Attempting to open a closed dataset object header as a group fails in
  // H5G__open_oid because it has neither STAB nor LINFO messages.
  H5O_loc_t dset_copy_oloc{};
  H5G_name_t dset_copy_path{};
  H5O_loc_reset(&dset_copy_oloc);
  H5G_name_reset(&dset_copy_path);
  dset_copy_oloc.file = grp_oloc->file;
  dset_copy_oloc.addr = dset_addr;
  H5G_loc_t non_grp_loc{&dset_copy_oloc, &dset_copy_path};
  {
    ScopedErrorSilence silence;
    H5I_type_t err_type = H5I_UNINIT;
    EXPECT_EQ(cls->open(&non_grp_loc, &err_type), nullptr);
  }

  // create callback: valid creation + unpin + close, and invalid gcpl path.
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file, H5I_FILE));
  ASSERT_NE(f, nullptr);
  H5G_obj_create_t crt_info{};
  crt_info.gcpl_id = H5P_GROUP_CREATE_DEFAULT;
  crt_info.cache_type = H5G_NOTHING_CACHED;
  H5G_loc_t out_loc{};
  void* created = cls->create(f, &crt_info, &out_loc);
  ASSERT_NE(created, nullptr);
  EXPECT_NE(out_loc.oloc, nullptr);
  EXPECT_NE(out_loc.path, nullptr);
  ASSERT_GE(H5O_dec_rc_by_loc(out_loc.oloc), 0);
  ASSERT_GE(H5G_close(static_cast<H5G_t*>(created)), 0);

  H5G_obj_create_t bad_crt{};
  bad_crt.gcpl_id = H5I_INVALID_HID;
  bad_crt.cache_type = H5G_NOTHING_CACHED;
  H5G_loc_t unused_loc{};
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(cls->create(f, &bad_crt, &unused_loc), nullptr);
  }

  // Corrupt LINFO B-tree / fractal heap addresses on a new-format group to
  // cover bh_info dense error branches cleanly, restoring before close.
  {
    ScopedFile v18_file =
        CreateFileWithBounds("v18_bh_err.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(v18_file.valid());
    ScopedGroup v18_grp(H5Gcreate2(v18_file, "/g", H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(v18_grp.valid());
    H5O_loc_t* linfo_oloc = cls->get_oloc(v18_grp);
    ASSERT_NE(linfo_oloc, nullptr);
    H5O_linfo_t orig_linfo{};
    ASSERT_NE(H5O_msg_read(linfo_oloc, H5O_LINFO_ID, &orig_linfo), nullptr);
    constexpr haddr_t kBadAddr = 0x7ffffff0;
    for (int field = 0; field < 3; ++field) {
      H5O_linfo_t bad_linfo = orig_linfo;
      if (field == 0) {
        bad_linfo.name_bt2_addr = kBadAddr;
      } else if (field == 1) {
        bad_linfo.corder_bt2_addr = kBadAddr;
      } else {
        bad_linfo.fheap_addr = kBadAddr;
      }
      ASSERT_GE(H5O_msg_write(linfo_oloc, H5O_LINFO_ID, 0, H5O_UPDATE_FORCE,
                              &bad_linfo),
                0);
      H5O_t* bad_oh = H5O_protect(linfo_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(bad_oh, nullptr);
      {
        ScopedErrorSilence silence;
        H5_ih_info_t dummy_bh{};
        EXPECT_LT(cls->bh_info(linfo_oloc, bad_oh, &dummy_bh), 0);
      }
      ASSERT_GE(H5O_unprotect(linfo_oloc, bad_oh, H5AC__NO_FLAGS_SET), 0);
    }
    ASSERT_GE(H5O_msg_write(linfo_oloc, H5O_LINFO_ID, 0, H5O_UPDATE_FORCE,
                            &orig_linfo),
              0);
  }
}

TEST(H5GohTest, DenseAndCorruptHeaderBhInfoErrorBranches) {
  std::vector<uint8_t> image;
  {
    ScopedFile f = CreateFileWithBounds("bh_deep.h5", H5F_LIBVER_V18,
                                        H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());

    ScopedGroup compact(
        H5Gcreate2(f, "/compact", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_link_phase_change(gcpl, 2, 1), 0);
    ASSERT_GE(H5Pset_link_creation_order(
                  gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);
    ScopedGroup dense(
        H5Gcreate2(f, "/dense", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(dense.valid());
    // With 512-byte B-tree nodes, 85 links force depth >= 1 on both indices.
    PopulateLinks(dense, 85, "deep_");

    compact.reset();
    dense.reset();
    ASSERT_GE(H5Fflush(f, H5F_SCOPE_GLOBAL), 0);
    ssize_t sz = H5Fget_file_image(f, nullptr, 0);
    ASSERT_GT(sz, 0);
    image.resize(static_cast<size_t>(sz));
    ASSERT_EQ(H5Fget_file_image(f, image.data(), image.size()), sz);
  }

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl, image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("bh_deep_ro.h5", H5F_ACC_RDONLY, ro_fapl));
  ASSERT_TRUE(ro_file.valid());
  auto* ro_f = static_cast<H5F_t*>(H5VL_object_verify(ro_file, H5I_FILE));
  ASSERT_NE(ro_f, nullptr);

  const H5O_obj_class_t* cls = &H5O_OBJ_GROUP[0];
  constexpr haddr_t kBadAddr = 0x7ffffff0;

  // 1. Fail H5O_msg_read_oh(H5O_LINFO_ID) via invalid version byte in raw buf.
  {
    ScopedGroup compact(H5Gopen2(ro_file, "/compact", H5P_DEFAULT));
    ASSERT_TRUE(compact.valid());
    H5O_loc_t* oloc = cls->get_oloc(compact);
    ASSERT_NE(oloc, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    H5O_mesg_t* linfo_mesg = nullptr;
    for (size_t i = 0; i < oh->nmesgs; ++i) {
      if (oh->mesg[i].type && oh->mesg[i].type->id == H5O_LINFO_ID) {
        linfo_mesg = &oh->mesg[i];
        break;
      }
    }
    ASSERT_NE(linfo_mesg, nullptr);
    ASSERT_NE(linfo_mesg->raw, nullptr);
    ASSERT_GE(linfo_mesg->raw_size, 2u);
    void* saved_native = linfo_mesg->native;
    uint8_t saved_ver = linfo_mesg->raw[0];
    linfo_mesg->native = nullptr;
    linfo_mesg->raw[0] = 99;
    {
      ScopedErrorSilence silence;
      H5_ih_info_t bh{};
      EXPECT_LT(cls->bh_info(oloc, oh, &bh), 0);
    }
    linfo_mesg->raw[0] = saved_ver;
    linfo_mesg->native = saved_native;
    ASSERT_GE(H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET), 0);
  }

  // 2. Fail H5B2_size (name & corder indices) and H5HF_size + H5HF_close.
  ScopedGroup dense(H5Gopen2(ro_file, "/dense", H5P_DEFAULT));
  ASSERT_TRUE(dense.valid());
  H5O_loc_t* d_oloc = cls->get_oloc(dense);
  ASSERT_NE(d_oloc, nullptr);

  ScopedApiContext ctx(d_oloc->addr);
  ASSERT_TRUE(ctx.valid());
  H5O_linfo_t linfo{};
  ASSERT_NE(H5O_msg_read(d_oloc, H5O_LINFO_ID, &linfo), nullptr);

  // Corrupt name_bt2 root child node address cleanly while opened.
  {
    H5B2_t* bt2 = H5B2_open(ro_f, linfo.name_bt2_addr, nullptr);
    ASSERT_NE(bt2, nullptr);
    ASSERT_GT(bt2->hdr->depth, 0);
    haddr_t saved_root_addr = bt2->hdr->root.addr;
    bt2->hdr->root.addr = kBadAddr;
    H5O_t* oh = H5O_protect(d_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    {
      ScopedErrorSilence silence;
      H5_ih_info_t bh{};
      EXPECT_LT(cls->bh_info(d_oloc, oh, &bh), 0);
    }
    ASSERT_GE(H5O_unprotect(d_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    bt2->hdr->root.addr = saved_root_addr;
    ASSERT_GE(H5B2_close(bt2), 0);
  }

  // Corrupt corder_bt2 root child node address cleanly while opened.
  {
    H5B2_t* bt2 = H5B2_open(ro_f, linfo.corder_bt2_addr, nullptr);
    ASSERT_NE(bt2, nullptr);
    ASSERT_GT(bt2->hdr->depth, 0);
    haddr_t saved_root_addr = bt2->hdr->root.addr;
    bt2->hdr->root.addr = kBadAddr;
    H5O_t* oh = H5O_protect(d_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    {
      ScopedErrorSilence silence;
      H5_ih_info_t bh{};
      EXPECT_LT(cls->bh_info(d_oloc, oh, &bh), 0);
    }
    ASSERT_GE(H5O_unprotect(d_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    bt2->hdr->root.addr = saved_root_addr;
    ASSERT_GE(H5B2_close(bt2), 0);
  }

  // Corrupt fractal heap huge_bt2_addr so both H5HF_size and H5HF_close fail,
  // then decrement the leftover component refcount and unprotect cleanly.
  {
    H5HF_hdr_t* hdr =
        H5HF__hdr_protect(ro_f, linfo.fheap_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    ASSERT_FALSE(H5_addr_defined(hdr->huge_bt2_addr));
    ASSERT_EQ(hdr->huge_nobjs, 0u);
    ASSERT_EQ(hdr->huge_size, 0u);
    hdr->huge_bt2_addr = kBadAddr;
    ASSERT_GE(H5AC_unprotect(ro_f, H5AC_FHEAP_HDR, linfo.fheap_addr, hdr,
                             H5AC__NO_FLAGS_SET),
              0);

    H5O_t* oh = H5O_protect(d_oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    {
      ScopedErrorSilence silence;
      H5_ih_info_t bh{};
      EXPECT_LT(cls->bh_info(d_oloc, oh, &bh), 0);
    }
    ASSERT_GE(H5O_unprotect(d_oloc, oh, H5AC__NO_FLAGS_SET), 0);

    hdr = H5HF__hdr_protect(ro_f, linfo.fheap_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(hdr, nullptr);
    hdr->huge_bt2_addr = HADDR_UNDEF;
    EXPECT_GE(H5HF__hdr_decr(hdr), 0);
    ASSERT_GE(H5AC_unprotect(ro_f, H5AC_FHEAP_HDR, linfo.fheap_addr, hdr,
                             H5AC__NO_FLAGS_SET),
              0);
  }
}

}  // namespace
}  // namespace hdf5
