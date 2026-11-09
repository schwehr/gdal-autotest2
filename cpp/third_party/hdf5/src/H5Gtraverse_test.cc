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
// Tests group hierarchy path traversal, soft links, user-defined links (v0 and
// v1), external links, mount-point traversal, intermediate group creation, and
// internal error/ownership paths (third_party/hdf5/src/H5Gtraverse.c).
// Note: google3 builds :hdf5 with -DNDEBUG and deprecated symbols enabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Gtraverse.c

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across
// H5private.h -> H5CXprivate.h -> H5Gpkg.h.
#ifndef NDEBUG
#define H5GTRAVERSE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5G_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
}  // extern "C"

#ifdef H5GTRAVERSE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GTRAVERSE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::StrEq;

// RAII guard for pushing/popping a thread-local API context when calling
// internal H5G_traverse / H5G__traverse_special routines directly.
class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool pushed() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

constexpr H5L_type_t kUdClassV1 = static_cast<H5L_type_t>(200);
constexpr H5L_type_t kUdClassV0 = static_cast<H5L_type_t>(201);
constexpr H5L_type_t kUdClassTemp = static_cast<H5L_type_t>(202);

struct TraverseCapture {
  int calls = 0;
  bool saw_grp_loc = false;
  bool saw_lnk = false;
  bool saw_obj_loc = false;
  std::string last_name;
  H5G_own_loc_t return_own_loc = H5G_OWN_NONE;
  herr_t return_code = 0;
};

extern "C" {

static hid_t UdTraverseV1(const char* /*link_name*/, hid_t cur_group,
                          const void* lnkdata, size_t lnkdata_size,
                          hid_t lapl_id, hid_t /*dxpl_id*/) {
  if (!lnkdata || lnkdata_size == 0) {
    return H5I_INVALID_HID;
  }
  const std::string_view cmd(static_cast<const char*>(lnkdata),
                             lnkdata_size - 1);
  if (cmd == "fail") {
    return H5I_INVALID_HID;
  }
  if (cmd == "bad_type") {
    // Valid non-location hid_t to trigger H5G_loc failure and cleanup.
    return H5Screate(H5S_SCALAR);
  }
  if (cmd == "stale_id") {
    // Already-closed positive hid_t to trigger H5I_dec_ref failure in cleanup.
    hid_t stale = H5Screate(H5S_SCALAR);
    if (stale >= 0) {
      H5Sclose(stale);
    }
    return stale;
  }
  return H5Oopen(cur_group, static_cast<const char*>(lnkdata), lapl_id);
}

static hid_t UdTraverseV0(const char* /*link_name*/, hid_t cur_group,
                          const void* lnkdata, size_t lnkdata_size,
                          hid_t lapl_id) {
  if (!lnkdata || lnkdata_size == 0) {
    return H5I_INVALID_HID;
  }
  return H5Oopen(cur_group, static_cast<const char*>(lnkdata), lapl_id);
}

static herr_t CaptureTraverseOp(H5G_loc_t* grp_loc, const char* name,
                                const H5O_link_t* lnk, H5G_loc_t* obj_loc,
                                void* op_data, H5G_own_loc_t* own_loc) {
  auto* cap = static_cast<TraverseCapture*>(op_data);
  ++cap->calls;
  cap->saw_grp_loc = (grp_loc != nullptr);
  cap->saw_lnk = (lnk != nullptr);
  cap->saw_obj_loc = (obj_loc != nullptr);
  cap->last_name = name ? name : "";
  *own_loc = cap->return_own_loc;

  if (cap->return_code >= 0) {
    if ((cap->return_own_loc & H5G_OWN_OBJ_LOC) && obj_loc != nullptr) {
      H5G_loc_free(obj_loc);
    }
    if ((cap->return_own_loc & H5G_OWN_GRP_LOC) && grp_loc != nullptr) {
      H5G_loc_free(grp_loc);
    }
  }
  return cap->return_code;
}

}  // extern "C"

TEST(H5GtraverseTest, AbsoluteRelativeDotTrailingSlashAndLongPaths) {
  ScopedFile file = CreateInMemoryFile("trav_basic.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup a(
      H5Gcreate2(file.get(), "/a", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a.valid());
  ScopedGroup ab(
      H5Gcreate2(a.get(), "b", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ab.valid());
  ScopedGroup abc(
      H5Gcreate2(ab.get(), "c", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(abc.valid());

  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(abc.get(), "dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  const int32_t expected = 123;
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &expected),
            0);

  // 1. Absolute, relative, multi-slash, "." no-op components, and trailing
  // slash paths.
  {
    ScopedGroup g_root(H5Gopen2(file.get(), "/", H5P_DEFAULT));
    EXPECT_TRUE(g_root.valid());
    ScopedGroup g_slashes(H5Gopen2(file.get(), "///", H5P_DEFAULT));
    EXPECT_TRUE(g_slashes.valid());
    ScopedGroup g_dot(H5Gopen2(a.get(), ".", H5P_DEFAULT));
    EXPECT_TRUE(g_dot.valid());
    ScopedGroup g_dots(H5Gopen2(file.get(), "/./a/./b//./c/", H5P_DEFAULT));
    EXPECT_TRUE(g_dots.valid());
    ScopedGroup g_rel(H5Gopen2(a.get(), "b/c", H5P_DEFAULT));
    EXPECT_TRUE(g_rel.valid());
    ScopedObject o_dot(H5Oopen(dset.get(), ".", H5P_DEFAULT));
    EXPECT_TRUE(o_dot.valid());
  }

  // 2. Path exceeding 1024-byte stack component buffer forces H5WB heap path.
  const std::string long_missing = "/a/b/" + std::string(1500, 'x');
  EXPECT_EQ(0, H5Lexists(file.get(), long_missing.c_str(), H5P_DEFAULT));

  // 3. Error branches: missing final/intermediate component, duplicate create,
  // lookup through non-group object, and invalid op on "/" or ".".
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Gopen2(file.get(), "/a/missing", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Gopen2(file.get(), "/a/no_such_dir/c", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Gcreate2(file.get(), "/a/b", H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(dset.get(), "child", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Oopen(file.get(), "/a/b/c/dset/child", H5P_DEFAULT));
    EXPECT_LT(H5Ldelete(file.get(), "/", H5P_DEFAULT), 0);
    H5L_info2_t linfo{};
    EXPECT_LT(H5Lget_info2(file.get(), ".", &linfo, H5P_DEFAULT), 0);
  }
}

TEST(H5GtraverseTest, SoftLinkResolutionDanglingAndNlinksExhaustion) {
  ScopedFile file = CreateInMemoryFile("trav_slink.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup target(
      H5Gcreate2(file.get(), "/target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(target.valid());
  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(target.get(), "leaf", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();
  target.reset();

  // Valid absolute & relative soft links, chains, dangling links, and cycles.
  ASSERT_GE(
      H5Lcreate_soft("/target", file.get(), "/s_abs", H5P_DEFAULT, H5P_DEFAULT),
      0);
  ASSERT_GE(H5Lcreate_soft("target/leaf", file.get(), "/s_rel", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(
      H5Lcreate_soft("/s_s2", file.get(), "/s_s1", H5P_DEFAULT, H5P_DEFAULT),
      0);
  ASSERT_GE(
      H5Lcreate_soft("/s_s3", file.get(), "/s_s2", H5P_DEFAULT, H5P_DEFAULT),
      0);
  ASSERT_GE(
      H5Lcreate_soft("/target", file.get(), "/s_s3", H5P_DEFAULT, H5P_DEFAULT),
      0);
  ASSERT_GE(H5Lcreate_soft("/no_such_target", file.get(), "/s_dangling",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/no_dir/no_obj", file.get(), "/s_dangling_deep",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/s_loop_b", file.get(), "/s_loop_a", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/s_loop_a", file.get(), "/s_loop_b", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // 1. Follow valid soft links (final and intermediate components).
  {
    ScopedObject o_abs(H5Oopen(file.get(), "/s_abs", H5P_DEFAULT));
    EXPECT_TRUE(o_abs.valid());
    ScopedDataset d_rel(H5Dopen2(file.get(), "/s_rel", H5P_DEFAULT));
    EXPECT_TRUE(d_rel.valid());
    ScopedDataset d_intmd(H5Dopen2(file.get(), "/s_abs/leaf", H5P_DEFAULT));
    EXPECT_TRUE(d_intmd.valid());

    // H5Lget_info2 sets H5G_TARGET_SLINK, so last_comp inspects the link
    // without following it, while intermediate soft links are still followed.
    H5L_info2_t linfo{};
    ASSERT_GE(H5Lget_info2(file.get(), "/s_dangling", &linfo, H5P_DEFAULT), 0);
    EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);
    ASSERT_GE(H5Lget_info2(file.get(), "/s_abs/leaf", &linfo, H5P_DEFAULT), 0);
    EXPECT_EQ(linfo.type, H5L_TYPE_HARD);
  }

  // 2. H5Oexists_by_name uses H5G_TARGET_EXISTS: true on valid soft link,
  // false on dangling soft link whose parent exists, error on missing intmd.
  EXPECT_GT(H5Oexists_by_name(file.get(), "/s_abs", H5P_DEFAULT), 0);
  EXPECT_EQ(0, H5Oexists_by_name(file.get(), "/s_dangling", H5P_DEFAULT));
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Oexists_by_name(file.get(), "/s_dangling_deep", H5P_DEFAULT),
              0);
    // Without H5G_TARGET_EXISTS, following a dangling soft link fails.
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(file.get(), "/s_dangling", H5P_DEFAULT));
    // Soft-link cycle exceeds H5L_NUM_LINKS (16).
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(file.get(), "/s_loop_a", H5P_DEFAULT));
  }

  // 3. Explicit nlinks limits: 2 fails on 3-hop chain, 3 succeeds, and prior
  // context nlinks limit is restored between traversals.
  ScopedPlist lapl2(H5Pcreate(H5P_LINK_ACCESS));
  ScopedPlist lapl3(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl2.valid());
  ASSERT_TRUE(lapl3.valid());
  ASSERT_GE(H5Pset_nlinks(lapl2.get(), 2), 0);
  ASSERT_GE(H5Pset_nlinks(lapl3.get(), 3), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(file.get(), "/s_s1", lapl2.get()));
  }
  ScopedObject o_chain(H5Oopen(file.get(), "/s_s1", lapl3.get()));
  EXPECT_TRUE(o_chain.valid());
}

TEST(H5GtraverseTest, IntermediateGroupCreationInheritsParentAndObjectSettings) {
  // 1. Earliest-format file: root group uses symbol table format without GINFO,
  // LINFO, or PLINE messages, so intermediate groups fall back to def_ginfo,
  // def_linfo, and def_pline.
  ScopedPlist old_fapl = MakeCoreFapl();
  ASSERT_TRUE(old_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(old_fapl.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_LATEST),
            0);
  ScopedFile old_file(H5Fcreate("trav_intmd_old.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                old_fapl.get()));
  ASSERT_TRUE(old_file.valid());

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  // Soft link creation sets H5G_CRT_INTMD_GROUP without H5G_CRT_OBJ.
  ASSERT_GE(H5Lcreate_soft("/old_a", old_file.get(), "/old_a/old_b/slink",
                           lcpl.get(), H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(old_file.get(), "/old_a/old_b/slink", H5P_DEFAULT), 0);

  // 2. Default v1.8+ format file: parent group with GINFO (link phase change),
  // LINFO (creation order), and PLINE (deflate filter) propagates settings.
  ScopedFile file = CreateInMemoryFile("trav_intmd.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist parent_gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(parent_gcpl.valid());
  ASSERT_GE(H5Pset_link_phase_change(parent_gcpl.get(), 4, 2), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                parent_gcpl.get(),
                H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_deflate(parent_gcpl.get(), 6), 0);

  ScopedGroup styled_parent(H5Gcreate2(file.get(), "/styled_parent",
                                       H5P_DEFAULT, parent_gcpl.get(),
                                       H5P_DEFAULT));
  ASSERT_TRUE(styled_parent.valid());

  // Group creation sets H5G_CRT_OBJ with H5O_TYPE_GROUP (gcpl propagated).
  ScopedPlist child_gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(child_gcpl.valid());
  ASSERT_GE(H5Pset_local_heap_size_hint(child_gcpl.get(), 128), 0);
  ScopedGroup leaf_grp(
      H5Gcreate2(styled_parent.get(), "intmd1/intmd2/leaf_grp", lcpl.get(),
                 child_gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(leaf_grp.valid());

  // Verify intermediate group inherited creation-order tracking from parent.
  H5L_info2_t linfo{};
  ASSERT_GE(
      H5Lget_info2(styled_parent.get(), "intmd1/intmd2", &linfo, H5P_DEFAULT),
      0);
  EXPECT_TRUE(linfo.corder_valid);

  // 3. Dataset creation sets H5G_CRT_OBJ with H5O_TYPE_DATASET (dcpl
  // propagated).
  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_obj_track_times(dcpl.get(), false), 0);
  ScopedDataset leaf_dset(
      H5Dcreate2(file.get(), "/dset_intmd1/dset_intmd2/leaf_dset",
                 H5T_NATIVE_INT32, space.get(), lcpl.get(), dcpl.get(),
                 H5P_DEFAULT));
  ASSERT_TRUE(leaf_dset.valid());

  // 4. Committed datatype sets H5G_CRT_OBJ with H5O_TYPE_NAMED_DATATYPE (falls
  // through group/dataset branches).
  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dtype_intmd/leaf_type", dtype.get(),
                       lcpl.get(), H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(file.get(), "/dtype_intmd/leaf_type", H5P_DEFAULT), 0);
}

TEST(H5GtraverseTest, UserDefinedLinkTraversalV1V0AndErrorPaths) {
  const H5L_class_t cls_v1 = {
      H5L_LINK_CLASS_T_VERS,
      kUdClassV1,
      "test_ud_v1",
      nullptr,
      nullptr,
      nullptr,
      UdTraverseV1,
      nullptr,
      nullptr,
  };
  const H5L_class_0_t cls_v0 = {
      H5L_LINK_CLASS_T_VERS_0,
      kUdClassV0,
      "test_ud_v0",
      nullptr,
      nullptr,
      nullptr,
      UdTraverseV0,
      nullptr,
      nullptr,
  };
  const H5L_class_t cls_tmp = {
      H5L_LINK_CLASS_T_VERS,
      kUdClassTemp,
      "test_ud_tmp",
      nullptr,
      nullptr,
      nullptr,
      UdTraverseV1,
      nullptr,
      nullptr,
  };

  ASSERT_GE(H5Lregister(&cls_v1), 0);
  ASSERT_GE(H5Lregister(reinterpret_cast<const H5L_class_t*>(&cls_v0)), 0);
  ASSERT_GE(H5Lregister(&cls_tmp), 0);
  absl::Cleanup unregister_classes = [] {
    if (H5Lis_registered(kUdClassV1) > 0) H5Lunregister(kUdClassV1);
    if (H5Lis_registered(kUdClassV0) > 0) H5Lunregister(kUdClassV0);
    if (H5Lis_registered(kUdClassTemp) > 0) H5Lunregister(kUdClassTemp);
  };

  ScopedFile file = CreateInMemoryFile("trav_ud.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup targets(
      H5Gcreate2(file.get(), "/targets", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(targets.valid());
  ScopedGroup inner(
      H5Gcreate2(targets.get(), "inner", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(inner.valid());
  constexpr hsize_t kOne = 1;
  ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(inner.get(), "dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  dset.reset();

  // Create a second-hop UD link inside /targets/inner -> /targets/inner/dset.
  constexpr char kDsetPath[] = "/targets/inner/dset";
  ASSERT_GE(H5Lcreate_ud(inner.get(), "ud_hop2", kUdClassV1, kDsetPath,
                         sizeof(kDsetPath), H5P_DEFAULT, H5P_DEFAULT),
            0);
  inner.reset();
  targets.reset();

  constexpr char kInnerPath[] = "/targets/inner";
  constexpr char kFailCmd[] = "fail";
  constexpr char kBadTypeCmd[] = "bad_type";
  constexpr char kStaleIdCmd[] = "stale_id";
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_v1_ok", kUdClassV1, kInnerPath,
                         sizeof(kInnerPath), H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_v0_ok", kUdClassV0, kInnerPath,
                         sizeof(kInnerPath), H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_fail", kUdClassV1, kFailCmd,
                         sizeof(kFailCmd), H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_bad_type", kUdClassV1, kBadTypeCmd,
                         sizeof(kBadTypeCmd), H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_stale_id", kUdClassV1, kStaleIdCmd,
                         sizeof(kStaleIdCmd), H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "/ud_unreg", kUdClassTemp, kInnerPath,
                         sizeof(kInnerPath), H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 1. Valid v1 and v0 UD links (as final and intermediate components).
  {
    ScopedGroup g1(H5Gopen2(file.get(), "/ud_v1_ok", H5P_DEFAULT));
    EXPECT_TRUE(g1.valid());
    ScopedGroup g0(H5Gopen2(file.get(), "/ud_v0_ok", H5P_DEFAULT));
    EXPECT_TRUE(g0.valid());
    ScopedDataset d_via_ud(
        H5Dopen2(file.get(), "/ud_v1_ok/dset", H5P_DEFAULT));
    EXPECT_TRUE(d_via_ud.valid());

    // H5Lget_info2 sets H5G_TARGET_UDLINK: skips trav_func on last_comp, but
    // still traverses intermediate UD links (!last_comp).
    H5L_info2_t linfo{};
    ASSERT_GE(H5Lget_info2(file.get(), "/ud_fail", &linfo, H5P_DEFAULT), 0);
    EXPECT_EQ(linfo.type, kUdClassV1);
    ASSERT_GE(
        H5Lget_info2(file.get(), "/ud_v1_ok/ud_hop2", &linfo, H5P_DEFAULT), 0);
    EXPECT_EQ(linfo.type, kUdClassV1);
  }

  // 2. H5Oexists_by_name pauses/resumes error stack around UD traversal.
  EXPECT_GT(H5Oexists_by_name(file.get(), "/ud_v1_ok", H5P_DEFAULT), 0);
  EXPECT_EQ(0, H5Oexists_by_name(file.get(), "/ud_fail", H5P_DEFAULT));

  // 3. Multi-hop UD traversal nlinks exhaustion (1 fails, 2 succeeds).
  ScopedPlist lapl1(H5Pcreate(H5P_LINK_ACCESS));
  ScopedPlist lapl2(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl1.valid());
  ASSERT_TRUE(lapl2.valid());
  ASSERT_GE(H5Pset_nlinks(lapl1.get(), 1), 0);
  ASSERT_GE(H5Pset_nlinks(lapl2.get(), 2), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Oopen(file.get(), "/ud_v1_ok/ud_hop2", lapl1.get()));
  }
  {
    ScopedObject d_hop2(
        H5Oopen(file.get(), "/ud_v1_ok/ud_hop2", lapl2.get()));
    EXPECT_TRUE(d_hop2.valid());
  }

  // 4. Unregistered class, invalid ID, non-location ID, and stale ID return.
  ASSERT_GE(H5Lunregister(kUdClassTemp), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(file.get(), "/ud_unreg", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID, H5Oopen(file.get(), "/ud_fail", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Oopen(file.get(), "/ud_bad_type", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Oopen(file.get(), "/ud_stale_id", H5P_DEFAULT));
  }
}

TEST(H5GtraverseTest, ExternalLinkHoldingFileAndIntermediateGroupCreation) {
  TempDir dir("trav_extlink_dir");
  const std::string ext_path = dir.child("ext_target.h5").string();
  const std::string main_path = dir.child("ext_main.h5").string();

  // 1. Create external file and close it so external-link traversal is the
  // sole holder keeping the target file open via H5O_loc_hold_file().
  {
    ScopedFile ext_file(
        H5Fcreate(ext_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ext_file.valid());
    ScopedGroup sub(H5Gcreate2(ext_file.get(), "/sub", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(sub.valid());
    ScopedGroup deep(
        H5Gcreate2(sub.get(), "deep", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(deep.valid());
    constexpr hsize_t kOne = 1;
    ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(deep.get(), "val", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const int32_t expected = 777;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &expected),
              0);
  }

  ScopedFile main_file(
      H5Fcreate(main_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(main_file.valid());
  ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/sub", main_file.get(),
                               "/ext_sub", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/", main_file.get(),
                               "/ext_root", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 2. Traverse multi-component path inside the held external file
  // (exercises grp_loc->oloc->holding_file propagation in
  // H5G__traverse_special).
  {
    ScopedDataset dset(
        H5Dopen2(main_file.get(), "/ext_sub/deep/val", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    int32_t actual = 0;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, &actual),
              0);
    EXPECT_EQ(actual, 777);
  }

  // 3. Create intermediate groups through an external link (exercises
  // grp_loc.oloc->holding_file propagation inside H5G_CRT_INTMD_GROUP).
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);
  {
    ScopedGroup created(H5Gcreate2(main_file.get(),
                                   "/ext_root/created_a/created_b/leaf",
                                   lcpl.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(created.valid());
  }
  main_file.reset();

  // 4. Verify the intermediate groups persisted in ext_target.h5.
  ScopedFile reopened_ext(
      H5Fopen(ext_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(reopened_ext.valid());
  EXPECT_GT(
      H5Lexists(reopened_ext.get(), "/created_a/created_b/leaf", H5P_DEFAULT),
      0);
}

TEST(H5GtraverseTest, MountPointTraversalAndTargetMountBypass) {
  ScopedFile parent = CreateInMemoryFile("trav_mnt_parent.h5");
  ScopedFile child = CreateInMemoryFile("trav_mnt_child.h5");
  ASSERT_TRUE(parent.valid());
  ASSERT_TRUE(child.valid());

  ScopedGroup mnt_grp(
      H5Gcreate2(parent.get(), "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(mnt_grp.valid());
  mnt_grp.reset();

  ScopedGroup child_sub(H5Gcreate2(child.get(), "/child_sub", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(child_sub.valid());
  child_sub.reset();

  ASSERT_GE(H5Fmount(parent.get(), "/mnt", child.get(), H5P_DEFAULT), 0);
  absl::Cleanup unmount = [&] { H5Funmount(parent.get(), "/mnt"); };

  // 1. Public traversal crosses mount point into child file.
  {
    ScopedGroup opened(H5Gopen2(parent.get(), "/mnt/child_sub", H5P_DEFAULT));
    EXPECT_TRUE(opened.valid());
  }

  // 2. Direct internal traversal: H5G_TARGET_MOUNT stops mount traversal on
  // last_comp ("/mnt" stays in parent), while intermediate "/mnt/child_sub"
  // still crosses the mount (!last_comp).
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5G_loc_t root_loc{};
  ASSERT_GE(H5G_loc(parent.get(), &root_loc), 0);

  TraverseCapture cap{};
  ASSERT_GE(H5G_traverse(&root_loc, "/mnt", H5G_TARGET_MOUNT, CaptureTraverseOp,
                         &cap),
            0);
  EXPECT_EQ(cap.calls, 1);
  EXPECT_TRUE(cap.saw_obj_loc);
  EXPECT_THAT(cap.last_name, StrEq("mnt"));

  TraverseCapture cap_sub{};
  ASSERT_GE(H5G_traverse(&root_loc, "/mnt/child_sub", H5G_TARGET_MOUNT,
                         CaptureTraverseOp, &cap_sub),
            0);
  EXPECT_EQ(cap_sub.calls, 1);
  EXPECT_TRUE(cap_sub.saw_obj_loc);
  EXPECT_THAT(cap_sub.last_name, StrEq("child_sub"));
}

TEST(H5GtraverseTest, InternalTraverseArgsOwnershipAndReadOnlyFailure) {
  TempFile ro_file("trav_readonly.h5");
  {
    ScopedFile wfile(
        H5Fcreate(ro_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(wfile.valid());
    ScopedGroup grp(
        H5Gcreate2(wfile.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    constexpr hsize_t kOne = 1;
    ScopedSpace space(H5Screate_simple(1, &kOne, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(wfile.get(), "/dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  ScopedFile file(H5Fopen(ro_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5G_loc_t loc{};
  ASSERT_GE(H5G_loc(file.get(), &loc), 0);

  // 1. Parameter checks on H5G_traverse (null/empty name, null loc, null op).
  {
    ScopedErrorSilence silence;
    TraverseCapture cap{};
    EXPECT_LT(
        H5G_traverse(&loc, nullptr, H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
    EXPECT_LT(
        H5G_traverse(&loc, "", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap), 0);
    EXPECT_LT(H5G_traverse(nullptr, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp,
                           &cap),
              0);
    EXPECT_LT(H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, nullptr, nullptr),
              0);
  }

  // 2. Ownership combinations on H5G_traverse: OBJ_LOC on "." and on named
  // component, GRP_LOC, BOTH, and error return resetting BOTH/OBJ_LOC -> NONE.
  {
    TraverseCapture cap{};
    cap.return_own_loc = H5G_OWN_OBJ_LOC;
    ASSERT_GE(
        H5G_traverse(&loc, ".", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap), 0);
    EXPECT_FALSE(cap.saw_grp_loc);
    EXPECT_FALSE(cap.saw_lnk);
    EXPECT_TRUE(cap.saw_obj_loc);
  }
  {
    TraverseCapture cap{};
    cap.return_own_loc = H5G_OWN_OBJ_LOC;
    ASSERT_GE(
        H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
    EXPECT_TRUE(cap.saw_grp_loc);
    EXPECT_TRUE(cap.saw_lnk);
    EXPECT_TRUE(cap.saw_obj_loc);
  }
  {
    TraverseCapture cap{};
    cap.return_own_loc = H5G_OWN_GRP_LOC;
    ASSERT_GE(
        H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
  }
  {
    TraverseCapture cap{};
    cap.return_own_loc = H5G_OWN_BOTH;
    ASSERT_GE(
        H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
  }
  {
    ScopedErrorSilence silence;
    TraverseCapture cap{};
    cap.return_own_loc = H5G_OWN_BOTH;
    cap.return_code = -1;
    EXPECT_LT(
        H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
    cap.return_own_loc = H5G_OWN_OBJ_LOC;
    EXPECT_LT(
        H5G_traverse(&loc, ".", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap), 0);
  }

  // 3. Intermediate group creation against a read-only file fails inside
  // H5G__obj_create_real.
  {
    ScopedErrorSilence silence;
    TraverseCapture cap{};
    EXPECT_LT(H5G_traverse(&loc, "/new_dir/leaf", H5G_CRT_INTMD_GROUP,
                           CaptureTraverseOp, &cap),
              0);
  }

  // 4. H5CX_get_nlinks failures via invalid/wrong-class LAPL in API context,
  // and H5G_open failure when UD link parent location is a dataset.
  ScopedPlist non_lapl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(non_lapl.valid());
  H5O_link_t soft_lnk{};
  std::memset(&soft_lnk, 0, sizeof(soft_lnk));
  char soft_name[] = "soft_link";
  char soft_val[] = "/g1";
  soft_lnk.type = H5L_TYPE_SOFT;
  soft_lnk.cset = H5T_CSET_ASCII;
  soft_lnk.name = soft_name;
  soft_lnk.u.soft.name = soft_val;

  H5O_link_t ud_lnk{};
  std::memset(&ud_lnk, 0, sizeof(ud_lnk));
  char ud_name[] = "ud_link";
  ud_lnk.type = kUdClassV1;
  ud_lnk.cset = H5T_CSET_ASCII;
  ud_lnk.name = ud_name;

  H5O_loc_t tmp_oloc{};
  H5G_name_t tmp_path{};
  H5G_loc_t tmp_obj_loc{&tmp_oloc, &tmp_path};
  H5G_loc_reset(&tmp_obj_loc);
  bool obj_exists = false;

  {
    ScopedErrorSilence silence;
    TraverseCapture cap{};
    ScopedApiContext bad_trav_ctx;
    ASSERT_TRUE(bad_trav_ctx.pushed());
    H5CX_set_lapl(non_lapl.get());
    EXPECT_LT(
        H5G_traverse(&loc, "/g1", H5G_TARGET_NORMAL, CaptureTraverseOp, &cap),
        0);
  }
  {
    ScopedErrorSilence silence;
    ScopedApiContext bad_soft_ctx;
    ASSERT_TRUE(bad_soft_ctx.pushed());
    H5CX_set_lapl(non_lapl.get());
    EXPECT_LT(H5G__traverse_special(&loc, &soft_lnk, H5G_TARGET_NORMAL, true,
                                    &tmp_obj_loc, &obj_exists),
              0);
  }
  {
    ScopedErrorSilence silence;
    ScopedApiContext bad_ud_ctx;
    ASSERT_TRUE(bad_ud_ctx.pushed());
    H5CX_set_lapl(non_lapl.get());
    EXPECT_LT(H5G__traverse_special(&loc, &ud_lnk, H5G_TARGET_NORMAL, true,
                                    &tmp_obj_loc, &obj_exists),
              0);
  }

  const H5L_class_t cls_v1 = {
      H5L_LINK_CLASS_T_VERS,
      kUdClassV1,
      "test_ud_nongrp",
      nullptr,
      nullptr,
      nullptr,
      UdTraverseV1,
      nullptr,
      nullptr,
  };
  ASSERT_GE(H5Lregister(&cls_v1), 0);
  absl::Cleanup unregister_v1 = [] {
    if (H5Lis_registered(kUdClassV1) > 0) H5Lunregister(kUdClassV1);
  };

  // Deep-copy the dataset location, then close ro_dset so H5FO no longer
  // contains an open H5D_shared_t at this address; H5G_open will then enter
  // H5G__open_oid and fail cleanly with "not a group".
  H5O_loc_t dset_oloc_copy{};
  H5G_name_t dset_path_copy{};
  H5G_loc_t dset_loc_copy{&dset_oloc_copy, &dset_path_copy};
  H5G_loc_reset(&dset_loc_copy);
  absl::Cleanup free_dset_loc = [&] { H5G_loc_free(&dset_loc_copy); };
  {
    ScopedDataset ro_dset(H5Dopen2(file.get(), "/dset", H5P_DEFAULT));
    ASSERT_TRUE(ro_dset.valid());
    H5G_loc_t dset_loc{};
    ASSERT_GE(H5G_loc(ro_dset.get(), &dset_loc), 0);
    ASSERT_GE(H5G_loc_copy(&dset_loc_copy, &dset_loc, H5_COPY_DEEP), 0);
  }
  {
    ScopedErrorSilence silence;
    ScopedApiContext nongrp_ud_ctx;
    ASSERT_TRUE(nongrp_ud_ctx.pushed());
    EXPECT_LT(H5G__traverse_special(&dset_loc_copy, &ud_lnk, H5G_TARGET_NORMAL,
                                    true, &tmp_obj_loc, &obj_exists),
              0);
  }
}

}  // namespace
}  // namespace hdf5
