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
// Tests old-style symbol table B-link tree leaf nodes: creation, splitting,
// key comparison/encoding/decoding, lookup, removal, iteration, link-table
// building, size accounting, group copying, and debugging output
// (third_party/hdf5/src/H5Gnode.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gnode.c

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5B_t and H5G_node_t.
#ifndef NDEBUG
#define H5GNODE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B_FRIEND
#define H5G_FRIEND
#include "third_party/hdf5/src/H5Bpkg.h"
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
#include "third_party/hdf5/src/H5HLprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GNODE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GNODE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
  explicit ScopedApiContext(hid_t vol_id = H5I_INVALID_HID,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
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
  bool vol_wrapped_ = false;
};

ScopedFile CreateSymTableFile(const char* name, unsigned ik = 2,
                              unsigned lk = 2) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ScopedPlist fapl = MakeCoreFapl();
  if (!fcpl.valid() || !fapl.valid() ||
      H5Pset_sym_k(fcpl.get(), ik, lk) < 0 ||
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                           H5F_LIBVER_LATEST) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
}

ScopedDataset CreateScalarIntDataset(hid_t loc_id, const char* path) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) {
    return ScopedDataset();
  }
  return ScopedDataset(H5Dcreate2(loc_id, path, H5T_NATIVE_INT32, space.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
}

TEST(H5GnodeTest, NodeInsertCoversAllSplitPositionsAndDuplicateError) {
  // Using ik=2, lk=2 keeps each symbol table node capped at 2*lk = 4 entries
  // so every split insertion branch triggers deterministically.
  ScopedFile file = CreateSymTableFile("gnode_splits.h5", /*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());

  ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
  ASSERT_TRUE(target.valid());

  // 1. Verify old-style symbol table storage is active.
  H5G_info_t root_info{};
  ASSERT_GE(H5Gget_info(file.get(), &root_info), 0);
  EXPECT_EQ(root_info.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);

  // 2. Test all 4 split insertion positions on separate 4-entry groups:
  // idx < K, idx == K, K < idx < 2*K, and idx == 2*K.
  struct SplitCase {
    const char* group_name;
    const char* fifth_key;
  };
  constexpr SplitCase kSplitCases[] = {
      {.group_name = "/split_lt_k", .fifth_key = "m05"},
      {.group_name = "/split_eq_k", .fifth_key = "m40"},
      {.group_name = "/split_mid_rt", .fifth_key = "m60"},
      {.group_name = "/split_end_rt", .fifth_key = "m80"},
  };
  for (const auto& sc : kSplitCases) {
    ScopedGroup grp(H5Gcreate2(file.get(), sc.group_name, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    for (const char* key : {"m10", "m30", "m50", "m70"}) {
      ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), key,
                               H5P_DEFAULT, H5P_DEFAULT),
                0);
    }
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), sc.fifth_key,
                             H5P_DEFAULT, H5P_DEFAULT),
              0);

    H5G_info_t ginfo{};
    ASSERT_GE(H5Gget_info(grp.get(), &ginfo), 0);
    EXPECT_EQ(ginfo.nlinks, 5U);

    // Duplicate insertion fails inside H5G__node_insert.
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lcreate_hard(file.get(), "/target", grp.get(), sc.fifth_key,
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 3. Interleaved ascending/descending insertions create a multi-level B-tree
  // and exercise both rt_key->offset > lt_key->offset and < in H5G__node_cmp2.
  ScopedGroup multi(
      H5Gcreate2(file.get(), "/multi", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(multi.valid());
  for (int i = 0; i < 20; ++i) {
    const std::string hi = "z_" + std::to_string(99 - i);
    const std::string lo = "a_" + std::to_string(10 + i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", multi.get(), hi.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", multi.get(), lo.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  EXPECT_GT(H5Lexists(multi.get(), "a_10", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(multi.get(), "z_99", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(multi.get(), "m_missing", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(multi.get(), "00_before_all", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(multi.get(), "zz_after_all", H5P_DEFAULT), 0);
}

TEST(H5GnodeTest, EncodeAndDecodeKeysPersistAcrossInMemoryReopen) {
  std::vector<uint8_t> image;

  {
    ScopedFile file = CreateSymTableFile("gnode_codec.h5", /*ik=*/2, /*lk=*/2);
    ASSERT_TRUE(file.valid());
    ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
    ASSERT_TRUE(target.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), "/persisted", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    for (int i = 0; i < 24; ++i) {
      const std::string link = "entry_" + std::to_string(100 + (23 - i));
      ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), link.c_str(),
                               H5P_DEFAULT, H5P_DEFAULT),
                0);
    }
    grp.reset();
    target.reset();

    // Flush forces dirty B-tree nodes through H5G__node_encode_key.
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t img_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(img_size, 0);
    image.resize(static_cast<size_t>(img_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              img_size);
  }

  // Reopen from memory image to exercise H5G__node_decode_key on B-tree reads.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("gnode_codec_reopen.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  H5G_info_t ginfo{};
  ASSERT_GE(H5Gget_info_by_name(reopened.get(), "/persisted", &ginfo,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(ginfo.storage_type, H5G_STORAGE_TYPE_SYMBOL_TABLE);
  EXPECT_EQ(ginfo.nlinks, 24U);
  EXPECT_GT(H5Lexists(reopened.get(), "/persisted/entry_100", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(reopened.get(), "/persisted/entry_123", H5P_DEFAULT), 0);
}

TEST(H5GnodeTest, RemoveHandlesAllNodePositionsSoftLinksAndWholeGroup) {
  ScopedFile file = CreateSymTableFile("gnode_remove.h5", /*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
  ASSERT_TRUE(target.valid());

  // 1. Single 4-entry node: remove left-most, middle, right-most, and final.
  ScopedGroup single(
      H5Gcreate2(file.get(), "/single", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(single.valid());
  for (const char* name : {"k1", "k2", "k3", "k4"}) {
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", single.get(), name,
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ldelete(single.get(), "missing", H5P_DEFAULT), 0);
  }
  ASSERT_GE(H5Ldelete(single.get(), "k1", H5P_DEFAULT), 0);  // left-most (idx 0)
  ASSERT_GE(H5Ldelete(single.get(), "k3", H5P_DEFAULT), 0);  // middle
  ASSERT_GE(H5Ldelete(single.get(), "k4", H5P_DEFAULT), 0);  // right-most
  ASSERT_GE(H5Ldelete(single.get(), "k2", H5P_DEFAULT), 0);  // last (nsyms==1)

  H5G_info_t single_info{};
  ASSERT_GE(H5Gget_info(single.get(), &single_info), 0);
  EXPECT_EQ(single_info.nlinks, 0U);

  // 2. Soft link removal cleans up soft link values in the local heap.
  ASSERT_GE(H5Lcreate_soft("/target", single.get(), "soft_item", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ldelete(single.get(), "soft_item", H5P_DEFAULT), 0);

  // 3. Whole-group deletion with hard and soft links across multiple nodes
  // triggers H5G__node_remove with udata->common.name == NULL.
  ScopedGroup doomed(
      H5Gcreate2(file.get(), "/doomed", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(doomed.valid());
  for (int i = 0; i < 8; ++i) {
    const std::string hname = "h_" + std::to_string(i);
    const std::string sname = "s_" + std::to_string(i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", doomed.get(), hname.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_soft("/target", doomed.get(), sname.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  doomed.reset();
  ASSERT_GE(H5Ldelete(file.get(), "/doomed", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(file.get(), "/doomed", H5P_DEFAULT), 0);
}

TEST(H5GnodeTest, IterateByIdxBuildTableAndSizeCoverCallbacks) {
  ScopedFile file = CreateSymTableFile("gnode_iter.h5", /*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
  ASSERT_TRUE(target.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/items", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  for (int i = 0; i < 18; ++i) {
    const std::string name =
        (i < 10 ? "n_0" : "n_") + std::to_string(i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), name.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 1. Increasing iteration: skip, early stop, and callback error.
  std::vector<std::string> visited;
  auto collect_cb = [](hid_t /*g*/, const char* name,
                       const H5L_info2_t* /*info*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return 0;
  };
  hsize_t idx = 5;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx, collect_cb,
                        &visited),
            0);
  EXPECT_EQ(visited.size(), 13U);
  EXPECT_EQ(visited.front(), "n_05");

  auto stop_cb = [](hid_t /*g*/, const char* /*name*/,
                    const H5L_info2_t* /*info*/, void* op_data) -> herr_t {
    int* count = static_cast<int*>(op_data);
    return (++(*count) >= 3) ? 1 : 0;
  };
  int stop_count = 0;
  idx = 0;
  EXPECT_EQ(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx, stop_cb,
                        &stop_count),
            1);
  EXPECT_EQ(stop_count, 3);

  auto err_cb = [](hid_t /*g*/, const char* /*name*/,
                   const H5L_info2_t* /*info*/, void* /*op_data*/) -> herr_t {
    return -1;
  };
  {
    ScopedErrorSilence silence;
    idx = 0;
    EXPECT_LT(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, &idx, err_cb,
                          nullptr),
              0);
  }

  // 2. Decreasing iteration exercises H5G__node_build_table and realloc growth.
  visited.clear();
  idx = 0;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_DEC, &idx, collect_cb,
                        &visited),
            0);
  EXPECT_EQ(visited.size(), 18U);
  EXPECT_EQ(visited.front(), "n_17");
  EXPECT_EQ(visited.back(), "n_00");

  // 3. By-index lookup across later nodes exercises H5G__node_by_idx skip.
  char name_buf[32] = {};
  ASSERT_GT(H5Lget_name_by_idx(file.get(), "/items", H5_INDEX_NAME, H5_ITER_INC,
                               14, name_buf, sizeof(name_buf), H5P_DEFAULT),
            0);
  EXPECT_STREQ(name_buf, "n_14");

  // 4. Native meta-size info traverses B-tree via H5G__node_iterate_size.
  H5O_native_info_t ninfo{};
  ASSERT_GE(H5Oget_native_info(grp.get(), &ninfo, H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_GT(ninfo.meta_size.obj.index_size, 0U);
  EXPECT_GT(ninfo.meta_size.obj.heap_size, 0U);
}

TEST(H5GnodeTest, CopyGroupCoversHardSubgroupAndSoftLinkExpansion) {
  ScopedFile src_file = CreateSymTableFile("gnode_copy_src.h5", 2, 2);
  ScopedFile dst_file = CreateSymTableFile("gnode_copy_dst.h5", 2, 2);
  ASSERT_TRUE(src_file.valid());
  ASSERT_TRUE(dst_file.valid());

  ScopedGroup src_grp(H5Gcreate2(src_file.get(), "/src", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_grp.valid());

  ScopedDataset d1 = CreateScalarIntDataset(src_grp.get(), "d1");
  ASSERT_TRUE(d1.valid());
  const int32_t val = 77;
  ASSERT_GE(H5Dwrite(d1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &val),
            0);

  ScopedGroup sub_grp(H5Gcreate2(src_grp.get(), "sub_grp", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub_grp.valid());
  ScopedDataset sub_ds = CreateScalarIntDataset(sub_grp.get(), "inner");
  ASSERT_TRUE(sub_ds.valid());

  ASSERT_GE(H5Lcreate_soft("/src/d1", src_grp.get(), "soft_ok", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/src/no_such_target", src_grp.get(),
                           "soft_dangling", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Add a few extra links so the source group spans multiple symbol nodes.
  for (int i = 0; i < 6; ++i) {
    const std::string link = "extra_" + std::to_string(i);
    ASSERT_GE(H5Lcreate_hard(src_grp.get(), "d1", src_grp.get(), link.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 1. Copy without soft-link expansion preserves soft links.
  ASSERT_GE(H5Ocopy(src_file.get(), "/src", dst_file.get(), "/copy_plain",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info(dst_file.get(), "/copy_plain/soft_ok", &linfo,
                        H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  // 2. Copy with H5O_COPY_EXPAND_SOFT_LINK_FLAG expands soft_ok into a dataset
  // and keeps soft_dangling as a soft link.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_SOFT_LINK_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/src", dst_file.get(), "/copy_expanded",
                    ocpypl.get(), H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lget_info(dst_file.get(), "/copy_expanded/soft_ok", &linfo,
                        H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);
  ASSERT_GE(H5Lget_info(dst_file.get(), "/copy_expanded/soft_dangling", &linfo,
                        H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  // 3. Soft link with a missing intermediate group causes H5G__loc_addr to
  // fail inside H5G__node_copy when expanding soft links.
  ScopedGroup src_broken(H5Gcreate2(src_file.get(), "/src_broken", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(src_broken.valid());
  ASSERT_GE(H5Lcreate_soft("/src/no_such_dir/missing", src_broken.get(),
                           "bad_slink", H5P_DEFAULT, H5P_DEFAULT),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(src_file.get(), "/src_broken", dst_file.get(),
                      "/dst_broken", ocpypl.get(), H5P_DEFAULT),
              0);
  }

  // 4. Direct H5G__node_copy bounds checks with truncated src_block_size on
  // single-entry hard-link and soft-link nodes cover strnlen overflow guards.
  auto check_copy_truncation = [&](hid_t grp_id, bool expand_soft,
                                   bool use_lval_off) {
    H5G_loc_t gloc{};
    ASSERT_GE(H5G_loc(grp_id, &gloc), 0);
    H5O_stab_t grp_stab{};
    H5O_stab_t dst_stab{};
    auto* src_h5f = static_cast<H5F_t*>(H5VL_object_verify(src_file.get(), H5I_FILE));
    auto* dst_h5f = static_cast<H5F_t*>(H5VL_object_verify(dst_file.get(), H5I_FILE));
    ASSERT_NE(src_h5f, nullptr);
    ASSERT_NE(dst_h5f, nullptr);

    ScopedGroup dst_tmp(H5Gcreate2(dst_file.get(),
                                   expand_soft ? "/tmp_exp"
                                   : (use_lval_off ? "/tmp_soft" : "/tmp_hard"),
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dst_tmp.valid());
    H5G_loc_t dloc{};
    ASSERT_GE(H5G_loc(dst_tmp.get(), &dloc), 0);

    ScopedApiContext ctx(src_file.get(), gloc.oloc->addr);
    ASSERT_TRUE(ctx.valid());
    ASSERT_NE(H5O_msg_read(gloc.oloc, 0x0011 /*H5O_STAB_ID*/, &grp_stab),
              nullptr);
    ASSERT_NE(H5O_msg_read(dloc.oloc, 0x0011 /*H5O_STAB_ID*/, &dst_stab),
              nullptr);

    H5B_cache_ud_t bud{src_h5f, H5B_SNODE, H5B_UNKNOWN_NODELEVEL,
                       H5F_grp_btree_shared(src_h5f)};
    auto* bt = static_cast<H5B_t*>(H5AC_protect(
        src_h5f, H5AC_BT, grp_stab.btree_addr, &bud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(bt, nullptr);
    const haddr_t snode_addr = bt->child[0];
    ASSERT_GE(H5AC_unprotect(src_h5f, H5AC_BT, grp_stab.btree_addr, bt,
                             H5AC__NO_FLAGS_SET),
              0);

    auto* sn = static_cast<H5G_node_t*>(
        H5AC_protect(src_h5f, H5AC_SNODE, snode_addr, src_h5f, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(sn, nullptr);
    const size_t cut_off = use_lval_off
                               ? sn->entry[0].cache.slink.lval_offset + 2
                               : sn->entry[0].name_off + 2;
    ASSERT_GE(
        H5AC_unprotect(src_h5f, H5AC_SNODE, snode_addr, sn, H5AC__NO_FLAGS_SET),
        0);

    H5HL_t* src_heap =
        H5HL_protect(src_h5f, grp_stab.heap_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(src_heap, nullptr);
    absl::Cleanup unprotect_heap = [src_heap] { H5HL_unprotect(src_heap); };

    H5O_copy_t cpy_info{};
    cpy_info.expand_soft_link = expand_soft;
    H5G_bt_it_cpy_t cpy_ud{};
    cpy_ud.src_oloc = gloc.oloc;
    cpy_ud.src_heap = src_heap;
    cpy_ud.src_block_size = cut_off;
    cpy_ud.dst_file = dst_h5f;
    cpy_ud.dst_stab = &dst_stab;
    cpy_ud.cpy_info = &cpy_info;

    ScopedErrorSilence silence;
    EXPECT_LT(H5G__node_copy(src_h5f, nullptr, snode_addr, nullptr, &cpy_ud),
              0);
  };

  ScopedGroup only_soft(H5Gcreate2(src_file.get(), "/only_soft", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(only_soft.valid());
  ASSERT_GE(H5Lcreate_soft("/src/d1_long_target", only_soft.get(),
                           "soft_long_name", H5P_DEFAULT, H5P_DEFAULT),
            0);

  check_copy_truncation(only_soft.get(), /*expand_soft=*/false,
                        /*use_lval_off=*/true);
  check_copy_truncation(only_soft.get(), /*expand_soft=*/true,
                        /*use_lval_off=*/true);
}

TEST(H5GnodeTest, NodeDebugDumpsLeafAndBtreeNodesAndHandlesErrors) {
  ScopedFile file = CreateSymTableFile("gnode_debug.h5", /*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
  ASSERT_TRUE(target.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/dbg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  for (int i = 0; i < 8; ++i) {
    const std::string name = "item_" + std::to_string(i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), name.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  auto* h5f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(h5f, nullptr);
  H5G_loc_t grp_loc{};
  ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);

  H5O_stab_t stab{};
  {
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());
    ASSERT_NE(H5O_msg_read(grp_loc.oloc, H5O_STAB_ID, &stab), nullptr);
  }

  auto capture_debug = [&](haddr_t addr, haddr_t heap_addr, herr_t* rc_out) {
    char* raw = nullptr;
    size_t len = 0;
    FILE* stream = open_memstream(&raw, &len);
    if (stream == nullptr) {
      *rc_out = -1;
      return std::string();
    }
    absl::Cleanup free_raw = [&raw] { std::free(raw); };
    ScopedApiContext ctx(file.get());
    if (!ctx.valid()) {
      std::fclose(stream);
      *rc_out = -1;
      return std::string();
    }
    *rc_out = H5G_node_debug(h5f, addr, stream, 0, 20, heap_addr);
    std::fclose(stream);
    return raw ? std::string(raw, len) : std::string();
  };

  // Locate a leaf SNODE address from the root B-tree node.
  haddr_t leaf_snode_addr = HADDR_UNDEF;
  {
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());
    H5B_cache_ud_t cache_udata{h5f, H5B_SNODE, H5B_UNKNOWN_NODELEVEL,
                               H5F_grp_btree_shared(h5f)};
    haddr_t cur_addr = stab.btree_addr;
    while (true) {
      auto* bt = static_cast<H5B_t*>(H5AC_protect(
          h5f, H5AC_BT, cur_addr, &cache_udata, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(bt, nullptr);
      const unsigned cur_level = bt->level;
      const haddr_t next_addr = bt->child[0];
      ASSERT_GE(
          H5AC_unprotect(h5f, H5AC_BT, cur_addr, bt, H5AC__NO_FLAGS_SET), 0);
      if (cur_level == 0) {
        leaf_snode_addr = next_addr;
        break;
      }
      cur_addr = next_addr;
    }
  }
  ASSERT_NE(leaf_snode_addr, HADDR_UNDEF);

  // 1. Debug dirty leaf SNODE (pre-flush), then flush and debug clean SNODE.
  herr_t rc = -1;
  const std::string dirty_leaf =
      capture_debug(leaf_snode_addr, stab.heap_addr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(dirty_leaf, HasSubstr("Symbol Table Node..."));
  EXPECT_THAT(dirty_leaf, HasSubstr("Yes"));

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  const std::string clean_leaf =
      capture_debug(leaf_snode_addr, stab.heap_addr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(clean_leaf, HasSubstr("No"));

  // 2. Debug leaf SNODE without heap address -> warning message branch.
  const std::string no_heap_leaf =
      capture_debug(leaf_snode_addr, HADDR_UNDEF, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(no_heap_leaf, HasSubstr("Warning: Invalid heap address given"));

  // 3. Debug B-tree node with heap (H5G_node_debug) and without heap
  // (H5B_debug directly to avoid upstream null-deref in H5HL_heap_get_size).
  const std::string btree_with_heap =
      capture_debug(stab.btree_addr, stab.heap_addr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(btree_with_heap, HasSubstr("Heap offset:"));
  EXPECT_THAT(btree_with_heap, HasSubstr("Name:"));

  {
    char* raw = nullptr;
    size_t len = 0;
    FILE* stream = open_memstream(&raw, &len);
    ASSERT_NE(stream, nullptr);
    absl::Cleanup free_raw = [&raw] { std::free(raw); };
    ScopedApiContext ctx(file.get());
    ASSERT_TRUE(ctx.valid());
    H5G_bt_common_t no_heap_udata{};
    rc = H5B_debug(h5f, stab.btree_addr, stream, 0, 20, H5B_SNODE,
                   &no_heap_udata);
    std::fclose(stream);
    EXPECT_GE(rc, 0);
    const std::string btree_no_heap = raw ? std::string(raw, len) : "";
    EXPECT_THAT(btree_no_heap, HasSubstr("Cannot get name; heap address"));
  }

  // 4. Error branches: invalid heap_addr and invalid node/btree addr.
  {
    ScopedErrorSilence silence;
    (void)capture_debug(leaf_snode_addr, stab.btree_addr, &rc);
    EXPECT_LT(rc, 0);
    (void)capture_debug(stab.heap_addr, stab.heap_addr, &rc);
    EXPECT_LT(rc, 0);
  }
}

TEST(H5GnodeTest, VerifyStructureCoversCmp2AndDirectNodeCallbacksCoverErrors) {
  ScopedFile file = CreateSymTableFile("gnode_verify_err.h5", /*ik=*/2, /*lk=*/2);
  ASSERT_TRUE(file.valid());
  ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
  ASSERT_TRUE(target.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/verify", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  // Insert in descending then ascending order so adjacent internal B-tree keys
  // exercise both rt_key->offset > lt_key->offset and <= in H5G__node_cmp2.
  for (int i = 25; i >= 0; --i) {
    const std::string link = "k_" + std::to_string(10 + i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), link.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  for (int i = 0; i < 6; ++i) {
    const std::string link = "z_" + std::to_string(10 + i);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), link.c_str(),
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  auto* h5f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(h5f, nullptr);
  H5G_loc_t grp_loc{};
  ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
  H5G_loc_t tgt_loc{};
  ASSERT_GE(H5G_loc(target.get(), &tgt_loc), 0);

  H5O_stab_t stab{};
  {
    ScopedApiContext ctx(file.get(), grp_loc.oloc->addr);
    ASSERT_TRUE(ctx.valid());
    ASSERT_NE(H5O_msg_read(grp_loc.oloc, 0x0011 /*H5O_STAB_ID*/, &stab),
              nullptr);
  }

  {
    ScopedApiContext ctx(file.get(), grp_loc.oloc->addr);
    ASSERT_TRUE(ctx.valid());
    H5HL_t* heap = H5HL_protect(h5f, stab.heap_addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprotect_heap = [heap] { H5HL_unprotect(heap); };

    H5G_bt_common_t common{};
    common.heap = heap;
    common.block_size = H5HL_heap_get_size(heap);

    // 1. Structural verification traverses internal B-tree nodes -> cmp2.
    EXPECT_GE(H5B__verify_structure(h5f, stab.btree_addr, H5B_SNODE, &common),
              0);

    // 2. Out-of-bounds key offsets hit H5HL_offset_into NULL guards in cmp2/cmp3.
    struct TestNodeKey {
      size_t offset;
    };
    ASSERT_EQ(sizeof(TestNodeKey), H5B_SNODE[0].sizeof_nkey);
    TestNodeKey bad_key{common.block_size + 64};
    TestNodeKey ok_key{0};
    {
      ScopedErrorSilence silence;
      EXPECT_LT((H5B_SNODE[0].cmp2)(&bad_key, &common, &ok_key), 0);
      EXPECT_LT((H5B_SNODE[0].cmp2)(&ok_key, &common, &bad_key), 0);
      common.name = "k_20";
      EXPECT_LT((H5B_SNODE[0].cmp3)(&bad_key, &common, &ok_key), 0);
      EXPECT_LT((H5B_SNODE[0].cmp3)(&ok_key, &common, &bad_key), 0);

      // 3. Failing H5G__node_found callback hits callback error branch.
      H5G_bt_lkp_t lkp{};
      lkp.common = common;
      lkp.common.name = "k_20";
      lkp.op = [](const H5G_entry_t*, void*) -> herr_t { return -1; };
      bool found = false;
      EXPECT_LT(H5B_find(h5f, H5B_SNODE, stab.btree_addr, &found, &lkp), 0);

      // 4. Failing H5G__node_by_idx callback hits callback error branch.
      H5G_bt_it_idx_common_t by_idx{};
      by_idx.idx = 0;
      by_idx.num_objs = 0;
      by_idx.op = [](const H5G_entry_t*, void*) -> herr_t { return -1; };
      EXPECT_LT(
          H5B_iterate(h5f, H5B_SNODE, stab.btree_addr, H5G__node_by_idx, &by_idx),
          0);
    }
  }

  // 5. Direct H5G__stab_insert duplicate and H5G__stab_remove missing-key calls
  // hit H5G__node_insert duplicate check and H5G__node_remove not-found guard.
  {
    ScopedApiContext ctx(file.get(), grp_loc.oloc->addr);
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    H5O_link_t dup_lnk{};
    dup_lnk.type = H5L_TYPE_HARD;
    dup_lnk.cset = H5T_CSET_ASCII;
    dup_lnk.name = const_cast<char*>("k_20");
    dup_lnk.u.hard.addr = tgt_loc.oloc->addr;
    EXPECT_LT(
        H5G__stab_insert(grp_loc.oloc, &dup_lnk, H5O_TYPE_DATASET, nullptr), 0);

    EXPECT_LT(H5G__stab_remove(grp_loc.oloc, grp_loc.path->full_path_r,
                               "k_20_nonexistent"),
              0);
  }
}

TEST(H5GnodeTest, CorruptedLeafSnodeSignatureErrorsCleanlyAcrossOperations) {
  std::vector<uint8_t> image;
  haddr_t leaf_snode_addr = HADDR_UNDEF;

  {
    ScopedFile file = CreateSymTableFile("gnode_corrupt.h5", /*ik=*/2, /*lk=*/2);
    ASSERT_TRUE(file.valid());
    ScopedDataset target = CreateScalarIntDataset(file.get(), "/target");
    ASSERT_TRUE(target.valid());
    ScopedGroup grp(H5Gcreate2(file.get(), "/bad_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), "a1",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/target", grp.get(), "a2",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);

    auto* h5f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
    ASSERT_NE(h5f, nullptr);
    H5G_loc_t grp_loc{};
    ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
    H5O_stab_t stab{};
    {
      ScopedApiContext ctx(file.get(), grp_loc.oloc->addr);
      ASSERT_TRUE(ctx.valid());
      ASSERT_NE(H5O_msg_read(grp_loc.oloc, 0x0011 /*H5O_STAB_ID*/, &stab),
                nullptr);
      H5B_cache_ud_t bud{h5f, H5B_SNODE, H5B_UNKNOWN_NODELEVEL,
                         H5F_grp_btree_shared(h5f)};
      auto* bt = static_cast<H5B_t*>(H5AC_protect(
          h5f, H5AC_BT, stab.btree_addr, &bud, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(bt, nullptr);
      leaf_snode_addr = bt->child[0];
      ASSERT_GE(
          H5AC_unprotect(h5f, H5AC_BT, stab.btree_addr, bt, H5AC__NO_FLAGS_SET),
          0);
    }

    grp.reset();
    target.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t sz = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(sz, 0);
    image.resize(static_cast<size_t>(sz));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()), sz);
  }

  ASSERT_NE(leaf_snode_addr, HADDR_UNDEF);
  ASSERT_LE(leaf_snode_addr + 4, image.size());
  ASSERT_EQ(std::memcmp(image.data() + leaf_snode_addr, "SNOD", 4), 0);
  // Corrupt the SNOD magic so H5AC_protect(..., H5AC_SNODE, ...) fails cleanly.
  std::memcpy(image.data() + leaf_snode_addr, "XXXX", 4);

  auto open_corrupt_image = [&](unsigned flags) {
    ScopedPlist fapl = MakeCoreFapl();
    if (!fapl.valid() ||
        H5Pset_file_image(fapl.get(), image.data(), image.size()) < 0) {
      return ScopedFile();
    }
    return ScopedFile(H5Fopen("gnode_corrupt_reopen.h5", flags, fapl.get()));
  };

  auto noop_iter = [](hid_t /*g*/, const char* /*name*/,
                      const H5L_info2_t* /*info*/, void* /*op_data*/) -> herr_t {
    return 0;
  };

  // 1. Read-only operations fail in found, iterate, sumup, by_idx, build_table,
  // and copy when H5AC_protect rejects the corrupted SNOD node.
  {
    ScopedFile ro = open_corrupt_image(H5F_ACC_RDONLY);
    ASSERT_TRUE(ro.valid());
    ScopedFile dst = CreateSymTableFile("gnode_corrupt_dst.h5", 2, 2);
    ASSERT_TRUE(dst.valid());
    ScopedErrorSilence silence;

    EXPECT_LT(H5Lexists(ro.get(), "/bad_grp/a1", H5P_DEFAULT), 0);
    hsize_t idx = 0;
    EXPECT_LT(H5Literate_by_name2(ro.get(), "/bad_grp", H5_INDEX_NAME,
                                  H5_ITER_INC, &idx, noop_iter, nullptr,
                                  H5P_DEFAULT),
              0);
    idx = 0;
    EXPECT_LT(H5Literate_by_name2(ro.get(), "/bad_grp", H5_INDEX_NAME,
                                  H5_ITER_DEC, &idx, noop_iter, nullptr,
                                  H5P_DEFAULT),
              0);
    H5G_info_t ginfo{};
    EXPECT_LT(H5Gget_info_by_name(ro.get(), "/bad_grp", &ginfo, H5P_DEFAULT), 0);
    char buf[16] = {};
    EXPECT_LT(H5Lget_name_by_idx(ro.get(), "/bad_grp", H5_INDEX_NAME, H5_ITER_INC,
                                 0, buf, sizeof(buf), H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(ro.get(), "/bad_grp", dst.get(), "/dst_grp", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
  }

  // 2. Direct stab insert & remove on a RDWR copy hit H5AC_protect failure in
  // H5G__node_insert and H5G__node_remove.
  {
    ScopedFile rw = open_corrupt_image(H5F_ACC_RDWR);
    ASSERT_TRUE(rw.valid());
    ScopedGroup grp(
        H5Gopen2(rw.get(), "/bad_grp", H5P_DEFAULT));
    ScopedDataset target(H5Dopen2(rw.get(), "/target", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_TRUE(target.valid());
    H5G_loc_t grp_loc{};
    H5G_loc_t tgt_loc{};
    ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
    ASSERT_GE(H5G_loc(target.get(), &tgt_loc), 0);

    ScopedApiContext ctx(rw.get(), grp_loc.oloc->addr);
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    H5O_link_t ins_lnk{};
    ins_lnk.type = H5L_TYPE_HARD;
    ins_lnk.cset = H5T_CSET_ASCII;
    ins_lnk.name = const_cast<char*>("a3");
    ins_lnk.u.hard.addr = tgt_loc.oloc->addr;
    EXPECT_LT(
        H5G__stab_insert(grp_loc.oloc, &ins_lnk, H5O_TYPE_DATASET, nullptr), 0);
    EXPECT_LT(H5G__stab_remove(grp_loc.oloc, grp_loc.path->full_path_r, "a1"),
              0);
  }
}

}  // namespace
}  // namespace hdf5
