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
// Tests H5Ocopy / H5O__copy / H5O_copy_header_map: copying groups, datasets,
// and committed datatypes; copy options (shallow, expand soft/ext links, without
// attrs, preserve nulls, merge committed datatypes); large multi-chunk headers;
// cycles and diamond hard links; SWMR destination copies; and error handling
// (third_party/hdf5/src/H5Ocopy.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ocopy.c

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5O_COPY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
// H5Fpkg.h declares a `bool try` parameter in H5F__efc_open, which collides
// with the C++ `try` keyword.
#pragma push_macro("try")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5SLprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
H5FL_EXTERN(H5O_addr_map_t);
}  // extern "C"
#ifdef H5O_COPY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_COPY_TEST_RESTORE_UNDEF_NDEBUG
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

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

void AttachCompactU8Attr(hid_t obj_id, const char* name, size_t nbytes,
                         uint8_t fill) {
  const hsize_t dim = static_cast<hsize_t>(nbytes);
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name, H5T_NATIVE_UCHAR, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  std::vector<uint8_t> buf(nbytes, fill);
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_UCHAR, buf.data()), 0);
}

void AttachIntAttr(hid_t obj_id, const char* name, int value) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name, H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &value), 0);
}

void AttachArrayIntAttr(hid_t obj_id, const char* name,
                        const std::vector<int>& values) {
  const hsize_t dim = static_cast<hsize_t>(values.size());
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(obj_id, name, H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, values.data()), 0);
}

H5O_mcdt_search_ret_t RecordingMcdtCb(void* op_data) {
  auto* ret = static_cast<H5O_mcdt_search_ret_t*>(op_data);
  return *ret;
}

TEST(H5OcopyTest, CopyHierarchyLinksCyclesWithoutAttrAndPreserveNull) {
  ScopedFile src = CreateInMemoryFile("copy_src.h5");
  ScopedFile dst = CreateInMemoryFile("copy_dst.h5");
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Populate nested groups, soft links (valid & dangling), datasets (compact,
  // contiguous, chunked+deflate), hard-link diamond, and parent cycle.
  ScopedGroup tree(
      H5Gcreate2(src.get(), "/tree", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(tree.valid());
  AttachIntAttr(tree.get(), "tree_attr", 7);

  ScopedGroup sub(
      H5Gcreate2(tree.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  // Hard link back to ancestor (/tree) creates an active cycle -> exercises
  // addr_map->is_locked deferred inc_ref_count in H5O_copy_header_map.
  ASSERT_GE(H5Lcreate_hard(src.get(), "/tree", sub.get(), "cycle_to_tree",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Shared dataset linked twice inside /tree -> exercises unlocked addr_map
  // hit in H5O_copy_header_map and H5O_link.
  {
    const hsize_t dims[1] = {8};
    const hsize_t cdims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, cdims), 0);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);
    ScopedDataset dset(H5Dcreate2(tree.get(), "chunked_dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    AttachIntAttr(dset.get(), "dset_attr", 42);
    const int data[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data),
              0);
  }
  ASSERT_GE(H5Lcreate_hard(tree.get(), "chunked_dset", sub.get(), "dset_alias",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Soft links: one valid inside sub, one dangling.
  ASSERT_GE(H5Lcreate_soft("/tree/chunked_dset", sub.get(), "soft_valid",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/nonexistent", sub.get(), "soft_dangling",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Multi-chunk object with deleted attribute (NULL message + CONT message)
  // to exercise both default NULL removal and H5O_COPY_PRESERVE_NULL_FLAG, plus
  // >255B and >65535B chunk 0 consolidation in destination object headers.
  {
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 200, 190), 0);
    ScopedGroup huge(H5Gcreate2(src.get(), "/huge_hdr", H5P_DEFAULT, gcpl.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(huge.valid());
    // Fence chunk 0 from EOF so subsequent attributes allocate continuation
    // chunks instead of extending chunk 0 in place.
    ScopedGroup blocker(H5Gcreate2(src.get(), "/huge_blocker", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());
    for (int i = 0; i < 120; ++i) {
      AttachCompactU8Attr(huge.get(), absl::StrCat("k_", i).c_str(), 600,
                          static_cast<uint8_t>(i));
    }
    // Delete a few attributes to leave NULL messages in source header chunks.
    ASSERT_GE(H5Adelete(huge.get(), "k_5"), 0);
    ASSERT_GE(H5Adelete(huge.get(), "k_20"), 0);

    H5O_native_info_t info{};
    ASSERT_GE(H5Oget_native_info(huge.get(), &info, H5O_NATIVE_INFO_HDR), 0);
    EXPECT_GE(info.hdr.nchunks, 2U);
  }

  // 1. Default recursive copy of /tree (skipping dangling/soft link expansion).
  ASSERT_GE(H5Ocopy(src.get(), "/tree", dst.get(), "/tree_full", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  H5O_info2_t full_info{};
  ASSERT_GE(H5Oget_info_by_name3(dst.get(), "/tree_full", &full_info,
                                  H5O_INFO_BASIC | H5O_INFO_NUM_ATTRS,
                                  H5P_DEFAULT),
            0);
  EXPECT_GE(full_info.rc, 2U);  // Root + cycle backlink
  EXPECT_EQ(full_info.num_attrs, 1U);

  // 2. Shallow hierarchy + without attrs + expand soft link (on sub group's
  // target without dangling link).
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(
                ocpypl.get(),
                H5O_COPY_SHALLOW_HIERARCHY_FLAG | H5O_COPY_WITHOUT_ATTR_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/tree", dst.get(), "/tree_shallow",
                    ocpypl.get(), H5P_DEFAULT),
            0);
  H5O_info2_t shallow_info{};
  ASSERT_GE(H5Oget_info_by_name3(dst.get(), "/tree_shallow", &shallow_info,
                                  H5O_INFO_NUM_ATTRS, H5P_DEFAULT),
            0);
  EXPECT_EQ(shallow_info.num_attrs, 0U);
  EXPECT_EQ(H5Lexists(dst.get(), "/tree_shallow/sub", H5P_DEFAULT), 1);
  // /tree_shallow/sub was NOT copied deeply because depth > max_depth(1).
  EXPECT_EQ(H5Lexists(dst.get(), "/tree_shallow/sub/dset_alias", H5P_DEFAULT),
            0);

  // Expand valid soft link specifically.
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_SOFT_LINK_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/tree/sub/soft_valid", dst.get(),
                    "/expanded_dset", ocpypl.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(dst.get(), "/expanded_dset", H5P_DEFAULT), 1);

  // 3. Copy huge multi-chunk object both WITHOUT and WITH PRESERVE_NULL_FLAG
  //    (hits >65535B chunk 0 4-byte size flag + CONT->NULL preservation).
  ASSERT_GE(H5Ocopy(src.get(), "/huge_hdr", dst.get(), "/huge_compacted",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  H5O_native_info_t compacted_info{};
  ASSERT_GE(H5Oget_native_info_by_name(dst.get(), "/huge_compacted",
                                        &compacted_info, H5O_NATIVE_INFO_HDR,
                                        H5P_DEFAULT),
            0);
  EXPECT_EQ(compacted_info.hdr.nchunks, 1U);
  EXPECT_GT(compacted_info.hdr.space.total, 65535U);

  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_PRESERVE_NULL_FLAG), 0);
  ASSERT_GE(H5Ocopy(src.get(), "/huge_hdr", dst.get(), "/huge_preserve_null",
                    ocpypl.get(), H5P_DEFAULT),
            0);
  H5O_native_info_t preserved_info{};
  ASSERT_GE(H5Oget_native_info_by_name(dst.get(), "/huge_preserve_null",
                                        &preserved_info, H5O_NATIVE_INFO_HDR,
                                        H5P_DEFAULT),
            0);
  EXPECT_EQ(preserved_info.hdr.nchunks, 1U);
  EXPECT_GT(preserved_info.hdr.space.free, 0U);
}

TEST(H5OcopyTest, HeaderVersionsSmallGapsNullsAndBoundsErrors) {
  // Exercise v1 (< H5O_MIN_SIZE) and v2 (gap and null padding, medium chunk0
  // >255B, tracked/indexed attrs, no times), v1 diamond hard links, and object
  // header / datatype version bounds errors.
  ScopedPlist v1_fapl = MakeCoreFapl();
  ScopedPlist v110_fapl = MakeCoreFapl();
  ScopedPlist v2_fapl = MakeCoreFapl();
  ASSERT_TRUE(v1_fapl.valid());
  ASSERT_TRUE(v110_fapl.valid());
  ASSERT_TRUE(v2_fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(v1_fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);
  ASSERT_GE(H5Pset_libver_bounds(v110_fapl.get(), H5F_LIBVER_V110,
                                 H5F_LIBVER_V110),
            0);
  ASSERT_GE(
      H5Pset_libver_bounds(v2_fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile f_v1(
      H5Fcreate("v1.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v1_fapl.get()));
  ScopedFile f_v1_dst(
      H5Fcreate("v1_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v1_fapl.get()));
  ScopedFile f_v110(
      H5Fcreate("v110.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v110_fapl.get()));
  ScopedFile f_v2(
      H5Fcreate("v2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v2_fapl.get()));
  ScopedFile f_v2_dst(
      H5Fcreate("v2_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v2_fapl.get()));
  ASSERT_TRUE(f_v1.valid());
  ASSERT_TRUE(f_v1_dst.valid());
  ASSERT_TRUE(f_v110.valid());
  ASSERT_TRUE(f_v2.valid());
  ASSERT_TRUE(f_v2_dst.valid());

  // v1 committed datatype -> small v1 header, plus < H5O_MIN_SIZE with delta
  // < msghdr_size (H5T_C_S1 without times: raw_size 8 + msghdr 8 = 16 -> delta
  // 6 < 8, padded up to 8-byte NULL message).
  {
    ScopedPlist tcpl_notime(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl_notime.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl_notime.get(), false), 0);

    ScopedType t(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(t.valid());
    ASSERT_GE(H5Tcommit2(f_v1.get(), "/v1_i32", t.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v1.get(), "/v1_i32", f_v2_dst.get(), "/v1_i32_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedType t_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(t_str.valid());
    ASSERT_GE(H5Tcommit2(f_v1.get(), "/v1_cs1_notime", t_str.get(), H5P_DEFAULT,
                         tcpl_notime.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v1.get(), "/v1_cs1_notime", f_v1_dst.get(),
                      "/v1_cs1_copy", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // v1 symbol-table group diamond hard link -> H5G__node_copy passes non-null
  // obj_type/udata on both initial copy and alias lookup in H5O_copy_header_map.
  {
    ScopedGroup v1_dia(H5Gcreate2(f_v1.get(), "/v1_diamond", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(v1_dia.valid());
    ScopedGroup leaf(H5Gcreate2(v1_dia.get(), "leaf", H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
    ScopedGroup sub(H5Gcreate2(v1_dia.get(), "sub", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(leaf.valid());
    ASSERT_TRUE(sub.valid());
    ASSERT_GE(H5Lcreate_hard(v1_dia.get(), "leaf", sub.get(), "leaf_alias",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v1.get(), "/v1_diamond", f_v1_dst.get(),
                      "/v1_diamond_copy", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // v2 small objects with varying sizes/flags to hit dst_oh_gap and dst_oh_null,
  // plus >255B chunk0 size bit (H5O_HDR_CHUNK0_2).
  {
    ScopedPlist gcpl_notime(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_notime.valid());
    ASSERT_GE(H5Pset_obj_track_times(gcpl_notime.get(), false), 0);
    ScopedGroup g_empty(H5Gcreate2(f_v2.get(), "/v2_empty_notimes", H5P_DEFAULT,
                                   gcpl_notime.get(), H5P_DEFAULT));
    ASSERT_TRUE(g_empty.valid());

    // Reference dataset created with H5F_LIBVER_LATEST (dtype version >= 4)
    // fails pre_copy_file version bounds check when copied to H5F_LIBVER_V110.
    ScopedSpace sc_v2(H5Screate(H5S_SCALAR));
    ScopedDataset d_ref(H5Dcreate2(f_v2.get(), "/v2_ref_dset", H5T_STD_REF,
                                   sc_v2.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(d_ref.valid());

    // Group containing a link to /v2_ref_dset so copying it to f_v110 fails
    // in post_copy_file (H5O__link_post_copy_file) after oh_dst chunk0 alloc.
    ScopedGroup g_v2_child(H5Gcreate2(f_v2.get(), "/v2_with_ref", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_v2_child.valid());
    ASSERT_GE(H5Lcreate_hard(f_v2.get(), "/v2_ref_dset", g_v2_child.get(),
                             "ref_child", H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedGroup g_med(H5Gcreate2(f_v2.get(), "/v2_medium_grp", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_med.valid());
    AttachCompactU8Attr(g_med.get(), "med_attr", 320, 0x5A);
    ASSERT_GE(H5Ocopy(f_v2.get(), "/v2_medium_grp", f_v2_dst.get(),
                      "/dst_v2_medium_grp", H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedPlist tcpl_notime(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl_notime.valid());
    ASSERT_GE(H5Pset_obj_track_times(tcpl_notime.get(), false), 0);

    // Small committed datatypes with no attrs -> hit dst_oh_size < H5O_MIN_SIZE.
    ScopedType t1(H5Tcopy(H5T_STD_I8LE));
    ASSERT_GE(H5Tcommit2(f_v2.get(), "/v2_i8", t1.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    // Opaque type with a 4-char tag -> DTYPE raw_size=16, +4 msghdr=20 < 22 ->
    // delta=2 < 4 hits dst_oh_gap in v2 object headers.
    ScopedType topq_tag(H5Tcreate(H5T_OPAQUE, 1));
    ASSERT_TRUE(topq_tag.valid());
    ASSERT_GE(H5Tset_tag(topq_tag.get(), "tag1"), 0);
    ASSERT_GE(H5Tcommit2(f_v2.get(), "/opq_tagged", topq_tag.get(), H5P_DEFAULT,
                         tcpl_notime.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v2.get(), "/opq_tagged", f_v2_dst.get(),
                      "/dst_opq_tagged", H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Compact opaque types spanning delta >= msghdr_size (dst_oh_null).
    for (int sz = 1; sz <= 4; ++sz) {
      ScopedType topq(H5Tcreate(H5T_OPAQUE, static_cast<size_t>(sz)));
      ASSERT_TRUE(topq.valid());
      ASSERT_GE(H5Tcommit2(f_v2.get(), absl::StrCat("/opq_", sz).c_str(),
                           topq.get(), H5P_DEFAULT, tcpl_notime.get(),
                           H5P_DEFAULT),
                0);
      ASSERT_GE(H5Ocopy(f_v2.get(), absl::StrCat("/opq_", sz).c_str(),
                        f_v2_dst.get(), absl::StrCat("/dst_opq_", sz).c_str(),
                        H5P_DEFAULT, H5P_DEFAULT),
                0);
    }

    // Tracked + indexed creation-order attributes without times on dataset.
    ScopedPlist dcpl_crt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_obj_track_times(dcpl_crt.get(), false), 0);
    ASSERT_GE(H5Pset_attr_creation_order(
                  dcpl_crt.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset d(H5Dcreate2(f_v2.get(), "/v2_tracked_dset", H5T_NATIVE_INT,
                               sc.get(), H5P_DEFAULT, dcpl_crt.get(),
                               H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    AttachIntAttr(d.get(), "a1", 1);

    ASSERT_GE(H5Ocopy(f_v2.get(), "/v2_empty_notimes", f_v2_dst.get(),
                      "/dst_v2_empty", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v2.get(), "/v2_i8", f_v2_dst.get(), "/dst_v2_i8",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(f_v2.get(), "/v2_tracked_dset", f_v2_dst.get(),
                      "/dst_v2_tracked", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Error: destination already exists, missing source, bad destination parent,
  // non-group intermediate destination path, v2->v1 ohdr bounds error, and
  // dtype pre_copy_file version bounds error.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(f_v2.get(), "/v2_i8", f_v2_dst.get(), "/dst_v2_i8",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(f_v2.get(), "/v2_i8", f_v2_dst.get(),
                      "/dst_v2_tracked/bad_child", H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(f_v2.get(), "/missing_obj", f_v2_dst.get(), "/new_name",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(f_v2.get(), "/v2_i8", f_v2_dst.get(), "/no_dir/obj",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(f_v2.get(), "/v2_ref_dset", f_v110.get(),
                      "/fail_ref_to_v110", H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(f_v2.get(), "/v2_with_ref", f_v110.get(),
                      "/fail_grp_ref_v110", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5OcopyTest, ExpandExternalLinksAndSwmrDestinationErrors) {
  TempDir dir("h5ocopy_ext");
  const std::string ext_target_path = dir.child("ext_target.h5").string();
  const std::string ext_host_path = dir.child("ext_host.h5").string();
  const std::string swmr_dst_path = dir.child("swmr_dst.h5").string();

  // Create external target file with a dataset and committed datatype.
  {
    ScopedFile ext_target(H5Fcreate(ext_target_path.c_str(), H5F_ACC_TRUNC,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ext_target.valid());
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset ds(H5Dcreate2(ext_target.get(), "/ext_dset", H5T_NATIVE_INT,
                                sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    const int v = 99;
    ASSERT_GE(
        H5Dwrite(ds.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v),
        0);

    ScopedType t_ext(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(ext_target.get(), "/ext_i32", t_ext.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Host file with external links + soft link to external link.
  {
    ScopedFile host(H5Fcreate(ext_host_path.c_str(), H5F_ACC_TRUNC,
                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(host.valid());
    ASSERT_GE(H5Lcreate_external(ext_target_path.c_str(), "/ext_dset",
                                 host.get(), "/ext_link_dset", H5P_DEFAULT,
                                 H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external(ext_target_path.c_str(), "/ext_i32",
                                 host.get(), "/ext_link_dt", H5P_DEFAULT,
                                 H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_soft("/ext_link_dset", host.get(), "/soft_to_ext",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Destination SWMR file (using H5F_LIBVER_V110 + H5Fstart_swmr_write) so
  // oh_dst->swmr_write == true -> H5AC_proxy_entry_create branch is covered.
  ScopedPlist swmr_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(swmr_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(swmr_fapl.get(), H5F_LIBVER_V110,
                                 H5F_LIBVER_V110),
            0);
  ScopedFile swmr_dst(H5Fcreate(swmr_dst_path.c_str(), H5F_ACC_TRUNC,
                                H5P_DEFAULT, swmr_fapl.get()));
  ASSERT_TRUE(swmr_dst.valid());
  ASSERT_GE(H5Fstart_swmr_write(swmr_dst.get()), 0);

  ScopedFile host_ro(
      H5Fopen(ext_host_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(host_ro.valid());

  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_EXT_LINK_FLAG), 0);
  ASSERT_GE(H5Ocopy(host_ro.get(), "/ext_link_dset", swmr_dst.get(),
                    "/copied_from_ext", ocpypl.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(swmr_dst.get(), "/copied_from_ext", H5P_DEFAULT), 1);

  ASSERT_GE(H5Ocopy(host_ro.get(), "/ext_link_dt", swmr_dst.get(),
                    "/copied_dt_from_ext", ocpypl.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(swmr_dst.get(), "/copied_dt_from_ext", H5P_DEFAULT), 1);

  {
    ScopedErrorSilence silence;
    // Missing parent group in destination.
    EXPECT_LT(H5Ocopy(swmr_dst.get(), "/copied_from_ext", swmr_dst.get(),
                      "/missing_parent/fail", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5OcopyTest, MergeCommittedDatatypesSameFileSuggestionsCallbacksAndAttrs) {
  TempDir dir("h5ocopy_mcdt");
  const std::string ext_path = dir.child("mcdt_ext.h5").string();
  const std::string dst_path = dir.child("mcdt_dst.h5").string();

  // External file containing an integer committed datatype, linked from dst to
  // exercise key1->fileno != key2->fileno in H5O__copy_comm_dt_cmp.
  {
    ScopedFile ext_file(
        H5Fcreate(ext_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ext_file.valid());
    ScopedType t(H5Tcopy(H5T_STD_I16LE));
    ASSERT_GE(H5Tcommit2(ext_file.get(), "/ext_i16", t.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  ScopedFile src = CreateInMemoryFile("mcdt_src.h5");
  ASSERT_TRUE(src.valid());
  ScopedFile dst(
      H5Fcreate(dst_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dst.valid());

  // Committed datatypes in source.
  ScopedType t_match(H5Tcopy(H5T_STD_I32LE));
  ScopedType t_with_attrs(H5Tcopy(H5T_STD_I32LE));
  ScopedType t_nested_attr_dt(H5Tcopy(H5T_STD_U16LE));
  ScopedType t_unmatched_a(H5Tcopy(H5T_IEEE_F32LE));
  ScopedType t_unmatched_b(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_GE(H5Tcommit2(src.get(), "/t_match", t_match.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Tcommit2(src.get(), "/t_nested_attr_dt", t_nested_attr_dt.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Tcommit2(src.get(), "/t_with_attrs", t_with_attrs.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  AttachIntAttr(t_with_attrs.get(), "id", 10);
  AttachArrayIntAttr(t_with_attrs.get(), "vec", {1, 2});
  {
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedAttr nested(H5Acreate2(t_with_attrs.get(), "nested_a",
                                 t_nested_attr_dt.get(), sc.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
    const uint16_t u = 55;
    ASSERT_GE(H5Awrite(nested.get(), H5T_NATIVE_UINT16, &u), 0);
  }

  // Group holding multiple committed dtypes + dataset + attr using committed
  // dtypes, so H5Ocopy triggers dst_dt_list caching and H5O__copy_insert_comm_dt.
  {
    ScopedGroup bundle(
        H5Gcreate2(src.get(), "/bundle", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(bundle.valid());
    ASSERT_GE(H5Tcommit2(bundle.get(), "f32", t_unmatched_a.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Tcommit2(bundle.get(), "f64", t_unmatched_b.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_hard(src.get(), "/t_match", bundle.get(), "link_i32",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset ds(H5Dcreate2(bundle.get(), "ds_using_f32",
                                t_unmatched_a.get(), sc.get(), H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ScopedAttr att(H5Acreate2(ds.get(), "att_using_f64", t_unmatched_b.get(),
                              sc.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(att.valid());
    const double dval = 3.25;
    ASSERT_GE(H5Awrite(att.get(), H5T_NATIVE_DOUBLE, &dval), 0);

    // Group containing two distinct committed datatypes with identical types
    // so STOP callback without suggestions fails on duplicate list insert.
    ScopedGroup dup_dts(H5Gcreate2(src.get(), "/dup_dts", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dup_dts.valid());
    ScopedType dup_a(H5Tcopy(H5T_STD_I32LE));
    ScopedType dup_b(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(dup_dts.get(), "dt_a", dup_a.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Tcommit2(dup_dts.get(), "dt_b", dup_b.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Populate destination with:
  // - non-matching datatypes (/dst_diff)
  // - soft link & external link (/ext_i16_link, opened after dst -> fileno >)
  // - datasets and attrs using both duplicate AND unique anonymous committed
  //   datatypes so MCDT scan hits both new-insert and already-in-skiplist
  // - exact matches (/dst_sugg_i32, /dst_with_attrs_match).
  {
    ScopedType d_diff(H5Tcopy(H5T_STD_I64LE));
    ASSERT_GE(H5Tcommit2(dst.get(), "/dst_diff", d_diff.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedType d_sugg(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(dst.get(), "/dst_sugg_i32", d_sugg.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedType d_anon(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit_anon(dst.get(), d_anon.get(), H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset ds_holder(H5Dcreate2(dst.get(), "/holder_dset", d_anon.get(),
                                       sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
    ASSERT_TRUE(ds_holder.valid());
    ScopedAttr a_holder(H5Acreate2(ds_holder.get(), "attr_comm", d_sugg.get(),
                                   sc.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_holder.valid());

    // Unique anonymous committed datatypes NOT named in dst: dataset uses
    // H5T_STD_U32BE, attribute uses H5T_STD_U8BE.
    ScopedType d_unique_dset(H5Tcopy(H5T_STD_U32BE));
    ASSERT_GE(H5Tcommit_anon(dst.get(), d_unique_dset.get(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
    ScopedType d_unique_attr(H5Tcopy(H5T_STD_U8BE));
    ASSERT_GE(H5Tcommit_anon(dst.get(), d_unique_attr.get(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
    ScopedDataset ds_anon_u(H5Dcreate2(dst.get(), "/anon_unique_dset",
                                       d_unique_dset.get(), sc.get(),
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds_anon_u.valid());
    ScopedAttr a_anon_u(H5Acreate2(ds_anon_u.get(), "attr_unique_comm",
                                   d_unique_attr.get(), sc.get(), H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(a_anon_u.valid());

    // Exact match with attributes on committed datatype.
    ScopedType dst_nest(H5Tcopy(H5T_STD_U16LE));
    ASSERT_GE(H5Tcommit2(dst.get(), "/dst_nest", dst_nest.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedType dst_match_attrs(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(dst.get(), "/dst_match_attrs", dst_match_attrs.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    AttachIntAttr(dst_match_attrs.get(), "id", 10);
    AttachArrayIntAttr(dst_match_attrs.get(), "vec", {1, 2});
    ScopedAttr nested(H5Acreate2(dst_match_attrs.get(), "nested_a",
                                 dst_nest.get(), sc.get(), H5P_DEFAULT,
                                 H5P_DEFAULT));
    const uint16_t u = 55;
    ASSERT_GE(H5Awrite(nested.get(), H5T_NATIVE_UINT16, &u), 0);

    // Soft link & external link in dst.
    ASSERT_GE(H5Lcreate_soft("/dst_diff", dst.get(), "/soft_to_dt",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/ext_i16", dst.get(),
                                 "/ext_i16_link", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 1. Same-file merge committed datatype copy (fileno_src == fileno_dst
  // branch).
  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);
    ASSERT_GE(H5Ocopy(src.get(), "/t_match", src.get(), "/t_samefile_merged",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }

  // 2. Suggested path merge hit + missing path + external path first
  //    (/ext_i16_link has fileno > dst, so following suggestions compare both
  //    fileno < and fileno >) + bad suggested path traversal error.
  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/no_such_path"),
              0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/ext_i16_link"),
              0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/holder_dset"),
              0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/dst_diff"), 0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/dst_sugg_i32"),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/t_match", dst.get(), "/merged_via_sugg",
                      ocpypl.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpypl.get()), 0);

    // Exercise both key1->fileno < key2->fileno and key1->fileno > key2->fileno
    // independently of sec2 vs core fileno ordering.
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/dst_diff"), 0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/ext_i16_link"),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/t_match", dst.get(), "/sugg_pair_ab",
                      ocpypl.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpypl.get()), 0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/ext_i16_link"),
              0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/dst_sugg_i32"),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/t_match", dst.get(), "/sugg_pair_ba",
                      ocpypl.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpypl.get()), 0);

    // Invalid intermediate object in suggested path fails H5G_loc_exists.
    ScopedErrorSilence silence;
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(),
                                                "/holder_dset/bad_sugg"),
              0);
    EXPECT_LT(H5Ocopy(src.get(), "/t_match", dst.get(), "/fail_bad_sugg",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }

  // 3. Global search callback behaviors: CONT (with full traversal & caching),
  //    STOP, ERROR, and invalid enum value.
  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);

    H5O_mcdt_search_ret_t cb_ret = H5O_MCDT_SEARCH_CONT;
    ASSERT_GE(H5Pset_mcdt_search_cb(ocpypl.get(), RecordingMcdtCb, &cb_ret), 0);

    // CONT builds full dst_dt_list, merges /t_match, inserts /bundle/f32 & f64
    // and both unique anonymous committed datatypes.
    ASSERT_GE(H5Ocopy(src.get(), "/bundle", dst.get(), "/copied_bundle",
                      ocpypl.get(), H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/t_with_attrs", dst.get(),
                      "/merged_with_attrs", ocpypl.get(), H5P_DEFAULT),
              0);

    // STOP skips global scan and copies as a separate datatype.
    cb_ret = H5O_MCDT_SEARCH_STOP;
    ASSERT_GE(H5Ocopy(src.get(), "/t_match", dst.get(), "/unmerged_stop",
                      ocpypl.get(), H5P_DEFAULT),
              0);

    // ERROR callback return value and duplicate insert under STOP without
    // suggestion list both abort H5Ocopy cleanly (hitting cleanup paths).
    ScopedErrorSilence silence;
    {
      ScopedPlist ocpypl_stop(H5Pcreate(H5P_OBJECT_COPY));
      ASSERT_GE(H5Pset_copy_object(ocpypl_stop.get(),
                                   H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
                0);
      H5O_mcdt_search_ret_t stop_ret = H5O_MCDT_SEARCH_STOP;
      ASSERT_GE(
          H5Pset_mcdt_search_cb(ocpypl_stop.get(), RecordingMcdtCb, &stop_ret),
          0);
      EXPECT_LT(H5Ocopy(src.get(), "/dup_dts", dst.get(), "/fail_dup_stop",
                        ocpypl_stop.get(), H5P_DEFAULT),
                0);
    }

    cb_ret = H5O_MCDT_SEARCH_ERROR;
    EXPECT_LT(H5Ocopy(src.get(), "/t_match", dst.get(), "/fail_cb_err",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }
}

TEST(H5OcopyTest, SohmSharedMessageRecomputeSizeAndExpandRef) {
  ScopedPlist sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(sohm_fcpl.valid());
  ASSERT_TRUE(core_fapl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(sohm_fcpl.get(), 1), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(sohm_fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1), 0);

  ScopedFile f_sohm(H5Fcreate("sohm.h5", H5F_ACC_TRUNC, sohm_fcpl.get(),
                              core_fapl.get()));
  ScopedFile f_plain = CreateInMemoryFile("plain_sohm.h5");
  ASSERT_TRUE(f_sohm.valid());
  ASSERT_TRUE(f_plain.valid());

  // Identical array attribute on two objects in f_sohm promotes it to SOHM
  // heap with H5O_MSG_FLAG_SHARED set.
  const std::vector<int> shared_vals = {11, 22, 33, 44, 55, 66};
  {
    ScopedGroup ga(H5Gcreate2(f_sohm.get(), "/sohm_a", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT));
    ScopedGroup gb(H5Gcreate2(f_sohm.get(), "/sohm_b", H5P_DEFAULT, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(ga.valid());
    ASSERT_TRUE(gb.valid());
    AttachArrayIntAttr(ga.get(), "shared_attr", shared_vals);
    AttachArrayIntAttr(gb.get(), "shared_attr", shared_vals);
  }

  // Unshared object in f_plain with the exact same attribute, plus ref dataset.
  {
    ScopedGroup g_unshared(H5Gcreate2(f_plain.get(), "/unshared_grp",
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_unshared.valid());
    AttachArrayIntAttr(g_unshared.get(), "shared_attr", shared_vals);

    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset ref_ds(H5Dcreate2(f_plain.get(), "/ref_ds", H5T_STD_REF_OBJ,
                                    sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(ref_ds.valid());
    hobj_ref_t ref = 0;
    ASSERT_GE(
        H5Rcreate(&ref, f_plain.get(), "/unshared_grp", H5R_OBJECT, -1), 0);
    ASSERT_GE(H5Dwrite(ref_ds.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &ref),
              0);
  }

  // Copying shared <-> unshared triggers H5O_MSG_FLAG_SHARED toggling and
  // recompute_size = true in H5O__copy_header_real.
  ASSERT_GE(H5Ocopy(f_sohm.get(), "/sohm_b", f_plain.get(), "/from_sohm",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(f_plain.get(), "/unshared_grp", f_sohm.get(), "/to_sohm",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Aexists_by_name(f_plain.get(), "/from_sohm", "shared_attr",
                              H5P_DEFAULT),
            1);
  EXPECT_EQ(
      H5Aexists_by_name(f_sohm.get(), "/to_sohm", "shared_attr", H5P_DEFAULT),
      1);

  // Exercise H5O_COPY_EXPAND_REFERENCE_FLAG option handling in H5O__copy_header.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_REFERENCE_FLAG), 0);
  ASSERT_GE(H5Ocopy(f_plain.get(), "/ref_ds", f_sohm.get(), "/ref_ds_cp",
                    ocpypl.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(f_sohm.get(), "/ref_ds_cp", H5P_DEFAULT), 1);
}

TEST(H5OcopyTest, CorruptObjectHeaderErrors) {
  TempDir dir("h5ocopy_corrupt");
  const std::string bad_path = dir.child("bad_ohdr.h5").string();
  haddr_t dset_addr = HADDR_UNDEF;

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);
    ScopedFile bad_f(
        H5Fcreate(bad_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(bad_f.valid());
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset ds(H5Dcreate2(bad_f.get(), "/bad_dset", H5T_NATIVE_INT,
                                sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    const H5O_loc_t* oloc = H5O_get_loc(ds.get());
    ASSERT_NE(oloc, nullptr);
    dset_addr = oloc->addr;
    ASSERT_NE(dset_addr, HADDR_UNDEF);
  }

  // Overwrite the v2 OHDR magic signature at dset_addr on disk so H5G_loc_find
  // succeeds shallowly while H5O_open / H5O__obj_class fails.
  {
    std::fstream fs(bad_path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(fs.is_open());
    fs.seekp(static_cast<std::streamoff>(dset_addr));
    const char bogus[4] = {'\xff', '\xff', '\xff', '\xff'};
    fs.write(bogus, sizeof(bogus));
    ASSERT_TRUE(fs.good());
  }

  ScopedFile bad_rw(H5Fopen(bad_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(bad_rw.valid());
  ScopedFile clean = CreateInMemoryFile("clean_for_corrupt.h5");
  ASSERT_TRUE(clean.valid());
  {
    ScopedType t(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(clean.get(), "/t_i32", t.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  ScopedErrorSilence silence;
  // Direct H5Ocopy from corrupt object -> H5O_open failure.
  EXPECT_LT(H5Ocopy(bad_rw.get(), "/bad_dset", clean.get(), "/out", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  // Merge committed datatypes with suggestion pointing at corrupt object ->
  // H5O__copy_search_comm_dt_check fails inside suggestion loop.
  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/bad_dset"), 0);
    EXPECT_LT(H5Ocopy(clean.get(), "/t_i32", bad_rw.get(), "/out_sugg",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }

  // Merge committed datatypes full traversal encounters corrupt object in
  // H5G_visit callback.
  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);
    EXPECT_LT(H5Ocopy(clean.get(), "/t_i32", bad_rw.get(), "/out_visit",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }

  // v1 file with corrupted DTYPE message versions (no v1 OHDR checksum, so
  // H5O__obj_class succeeds while H5O_msg_read/H5O_msg_read_oh fails).
  const std::string bad_v1_path = dir.child("bad_v1_dtype.h5").string();
  haddr_t bad_dt_off = HADDR_UNDEF;
  haddr_t bad_ds_dt_off = HADDR_UNDEF;
  {
    ScopedPlist v1_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(v1_fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(v1_fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile bad_v1(
        H5Fcreate(bad_v1_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, v1_fapl.get()));
    ASSERT_TRUE(bad_v1.valid());

    ScopedType v1_dt(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(bad_v1.get(), "/v1_bad_dt", v1_dt.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset v1_ds(H5Dcreate2(bad_v1.get(), "/v1_bad_ds", H5T_STD_I32LE,
                                   sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(v1_ds.valid());
    ASSERT_GE(H5Fflush(bad_v1.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedApiContext api_ctx;
    auto find_dtype_offset = [](hid_t obj_id) -> haddr_t {
      const H5O_loc_t* oloc = H5O_get_loc(obj_id);
      if (oloc == nullptr) return HADDR_UNDEF;
      H5O_t* oh = H5O_protect(oloc, H5AC__READ_ONLY_FLAG, false);
      if (oh == nullptr) return HADDR_UNDEF;
      haddr_t off = HADDR_UNDEF;
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type != nullptr &&
            oh->mesg[i].type->id == H5O_DTYPE_ID && oh->mesg[i].chunkno == 0) {
          off = oloc->addr +
                static_cast<haddr_t>(oh->mesg[i].raw - oh->chunk[0].image);
          break;
        }
      }
      H5O_unprotect(oloc, oh, H5AC__NO_FLAGS_SET);
      return off;
    };
    bad_dt_off = find_dtype_offset(v1_dt.get());
    bad_ds_dt_off = find_dtype_offset(v1_ds.get());
    ASSERT_NE(bad_dt_off, HADDR_UNDEF);
    ASSERT_NE(bad_ds_dt_off, HADDR_UNDEF);
  }

  {
    std::fstream fs(bad_v1_path,
                    std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(fs.is_open());
    const char bad_ver = '\xf0';  // DTYPE version 15 in upper nibble.
    fs.seekp(static_cast<std::streamoff>(bad_dt_off));
    fs.write(&bad_ver, 1);
    fs.seekp(static_cast<std::streamoff>(bad_ds_dt_off));
    fs.write(&bad_ver, 1);
    ASSERT_TRUE(fs.good());
  }

  ScopedFile bad_v1_rw(
      H5Fopen(bad_v1_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(bad_v1_rw.valid());

  {
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_GE(
        H5Pset_copy_object(ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
        0);
    // Copying corrupted named datatype fails H5O_msg_read_oh in search_comm_dt.
    EXPECT_LT(H5Ocopy(bad_v1_rw.get(), "/v1_bad_dt", clean.get(), "/out_bdt",
                      ocpypl.get(), H5P_DEFAULT),
              0);

    // Suggestion pointing at corrupted named datatype / dataset fails
    // H5O_msg_read in H5O__copy_search_comm_dt_check.
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/v1_bad_dt"), 0);
    EXPECT_LT(H5Ocopy(clean.get(), "/t_i32", bad_v1_rw.get(), "/out_sugg_bdt",
                      ocpypl.get(), H5P_DEFAULT),
              0);

    ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpypl.get()), 0);
    ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpypl.get(), "/v1_bad_ds"), 0);
    EXPECT_LT(H5Ocopy(clean.get(), "/t_i32", bad_v1_rw.get(), "/out_sugg_bds",
                      ocpypl.get(), H5P_DEFAULT),
              0);
  }
}

TEST(H5OcopyTest, InternalCopyErrorPaths) {
  TempDir dir("h5ocopy_int");
  const std::string ro_path = dir.child("ro_internal.h5").string();
  {
    ScopedFile ro_init(
        H5Fcreate(ro_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ro_init.valid());
    ScopedType t(H5Tcopy(H5T_STD_I32LE));
    ASSERT_GE(H5Tcommit2(ro_init.get(), "/ro_i32", t.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
  ScopedFile ro_file(H5Fopen(ro_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro_file.valid());

  ScopedPlist v2_fapl = MakeCoreFapl();
  ASSERT_TRUE(v2_fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(v2_fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);
  ScopedFile src(
      H5Fcreate("int_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v2_fapl.get()));
  ScopedFile dst = CreateInMemoryFile("int_dst.h5");
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  ScopedGroup grp(H5Gcreate2(src.get(), "/v2_grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ScopedGroup dst_anchor(H5Gcreate2(dst.get(), "/dst_anchor", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ScopedType t_i32(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(grp.valid());
  ASSERT_TRUE(dst_anchor.valid());
  ASSERT_TRUE(t_i32.valid());
  ASSERT_GE(H5Tcommit2(src.get(), "/src_i32", t_i32.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  {
    const hsize_t dims[1] = {4};
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset v2_ds(H5Dcreate2(src.get(), "/v2_dset", H5T_NATIVE_INT,
                                   sc.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(v2_ds.valid());

    ScopedSpace sp1d(H5Screate_simple(1, dims, nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, dims), 0);
    ScopedDataset chunked(H5Dcreate2(src.get(), "/chunked_src", H5T_NATIVE_INT,
                                     sp1d.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT));
    ASSERT_TRUE(chunked.valid());
    const int vals[4] = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(chunked.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals),
              0);
  }
  ASSERT_GE(H5Fflush(src.get(), H5F_SCOPE_GLOBAL), 0);

  ScopedApiContext api_ctx;
  ScopedErrorSilence silence;

  // 1. H5O__copy bad / wrong-class ocpypl_id errors.
  {
    H5G_loc_t src_loc{};
    H5G_loc_t dst_loc{};
    ASSERT_GE(H5G_loc(src.get(), &src_loc), 0);
    ASSERT_GE(H5G_loc(dst.get(), &dst_loc), 0);
    EXPECT_LT(H5O__copy(&src_loc, "/src_i32", &dst_loc, "/o1", H5I_INVALID_HID,
                        H5P_LINK_CREATE_DEFAULT),
              0);
    EXPECT_LT(H5O__copy(&src_loc, "/src_i32", &dst_loc, "/o2",
                        H5P_DATASET_CREATE_DEFAULT, H5P_LINK_CREATE_DEFAULT),
              0);
  }

  H5O_loc_t* src_file_oloc = H5O_get_loc(grp.get());
  H5O_loc_t* dst_file_oloc = H5O_get_loc(dst_anchor.get());
  ASSERT_NE(src_file_oloc, nullptr);
  ASSERT_NE(dst_file_oloc, nullptr);

  // 2. Destination object header version bounds failure (v2 dataset into
  //    high_bound == H5F_LIBVER_EARLIEST) exercises H5O__copy_header_real
  //    oh_dst / oloc_dst / cpy_udata cleanup and H5O_copy_header_map failure.
  {
    H5F_t* dst_f = dst_file_oloc->file;
    const H5F_libver_t orig_high = dst_f->shared->high_bound;
    dst_f->shared->high_bound = H5F_LIBVER_EARLIEST;
    auto restore_high = absl::MakeCleanup(
        [&]() { dst_f->shared->high_bound = orig_high; });
    EXPECT_LT(H5Ocopy(src.get(), "/v2_dset", dst.get(), "/fail_oh_bounds",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 3. H5O_copy_header_map with invalid object address -> H5O__obj_class fails.
  {
    H5O_copy_t cpy_info{};
    cpy_info.max_depth = -1;
    cpy_info.lcpl_id = H5P_LINK_CREATE_DEFAULT;
    cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(cpy_info.map_list, nullptr);
    auto destroy_map = absl::MakeCleanup([&]() {
      H5SL_destroy(
          cpy_info.map_list,
          [](void* item, void*, void*) -> herr_t {
            H5FL_FREE(H5O_addr_map_t, static_cast<H5O_addr_map_t*>(item));
            return 0;
          },
          nullptr);
    });

    H5O_loc_t bad_oloc{};
    bad_oloc.file = src_file_oloc->file;
    bad_oloc.addr = 16;  // Superblock offset, not an object header.
    H5O_loc_t out_oloc{};
    out_oloc.file = dst_file_oloc->file;
    out_oloc.addr = HADDR_UNDEF;
    EXPECT_LT(H5O_copy_header_map(&bad_oloc, &out_oloc, &cpy_info, false,
                                  nullptr, nullptr),
              0);

    // 4. Same-file merge_comm_dt on a read-only file succeeds in
    // H5O__copy_header_real and fails H5O_link increment on oloc_dst.
    ScopedType ro_open_dt(H5Topen2(ro_file.get(), "/ro_i32", H5P_DEFAULT));
    ASSERT_TRUE(ro_open_dt.valid());
    const H5O_loc_t* ro_dt_oloc = H5O_get_loc(ro_open_dt.get());
    ASSERT_NE(ro_dt_oloc, nullptr);

    H5O_copy_t merge_info{};
    merge_info.max_depth = -1;
    merge_info.merge_comm_dt = true;
    merge_info.lcpl_id = H5P_LINK_CREATE_DEFAULT;
    merge_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(merge_info.map_list, nullptr);
    auto destroy_merge_map = absl::MakeCleanup([&]() {
      H5SL_destroy(
          merge_info.map_list,
          [](void* item, void*, void*) -> herr_t {
            H5FL_FREE(H5O_addr_map_t, static_cast<H5O_addr_map_t*>(item));
            return 0;
          },
          nullptr);
    });

    H5O_loc_t merge_dst{};
    merge_dst.file = ro_dt_oloc->file;
    merge_dst.addr = HADDR_UNDEF;
    EXPECT_LT(H5O_copy_header_map(ro_dt_oloc, &merge_dst, &merge_info, false,
                                  nullptr, nullptr),
              0);
  }

  // 5. Corrupt cached message raw_size == 0 (line 365) and capped-maxaddr
  //    failures in H5O__msg_copy_file (chunked_src), H5MF_alloc (v2_dset), and
  //    H5O_HDR_CHUNK0_8 + H5MF_alloc (inflated raw_size > 4GiB).
  {
    const H5O_loc_t* grp_oloc = H5O_get_loc(grp.get());
    ASSERT_NE(grp_oloc, nullptr);

    int ginfo_idx = -1;
    size_t orig_sz = 0;
    {
      H5O_t* oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      for (size_t i = 0; i < oh->nmesgs; ++i) {
        if (oh->mesg[i].type != nullptr &&
            (oh->mesg[i].type->id == H5O_GINFO_ID ||
             oh->mesg[i].type->id == H5O_MTIME_NEW_ID)) {
          ginfo_idx = static_cast<int>(i);
          orig_sz = oh->mesg[i].raw_size;
          break;
        }
      }
      ASSERT_GE(ginfo_idx, 0);
      ASSERT_GT(orig_sz, 0U);
      oh->mesg[ginfo_idx].raw_size = 0;
      ASSERT_GE(H5O_unprotect(grp_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    }

    auto restore_raw_size = absl::MakeCleanup([&]() {
      H5O_t* oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
      if (oh != nullptr) {
        oh->mesg[ginfo_idx].raw_size = orig_sz;
        H5O_unprotect(grp_oloc, oh, H5AC__NO_FLAGS_SET);
      }
    });

    EXPECT_LT(H5Ocopy(src.get(), "/v2_grp", dst.get(), "/fail_zero_raw_sz",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Block all VFD allocations in Core VFD dst so H5MF_alloc fails cleanly.
    H5F_t* dst_f = dst_file_oloc->file;
    const unsigned long orig_flags = dst_f->shared->feature_flags;
    const haddr_t orig_lf_maxaddr = dst_f->shared->lf->maxaddr;
    const haddr_t orig_sh_maxaddr = dst_f->shared->maxaddr;
    const haddr_t cur_eoa = H5FD_get_eoa(dst_f->shared->lf, H5FD_MEM_DEFAULT);
    ASSERT_NE(cur_eoa, HADDR_UNDEF);
    dst_f->shared->feature_flags &=
        ~(H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_AGGREGATE_SMALLDATA);
    dst_f->shared->lf->maxaddr = cur_eoa;
    dst_f->shared->maxaddr = cur_eoa;
    auto restore_alloc = absl::MakeCleanup([&]() {
      dst_f->shared->feature_flags = orig_flags;
      dst_f->shared->lf->maxaddr = orig_lf_maxaddr;
      dst_f->shared->maxaddr = orig_sh_maxaddr;
    });

    EXPECT_LT(H5Ocopy(src.get(), "/chunked_src", dst.get(), "/fail_chunked",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(src.get(), "/v2_dset", dst.get(), "/fail_oh_alloc",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    {
      H5O_t* oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      oh->mesg[ginfo_idx].raw_size = static_cast<size_t>(5ULL << 30);
      ASSERT_GE(H5O_unprotect(grp_oloc, oh, H5AC__NO_FLAGS_SET), 0);
    }
    EXPECT_LT(H5Ocopy(src.get(), "/v2_grp", dst.get(), "/fail_chunk0_8",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

}  // namespace
}  // namespace hdf5
