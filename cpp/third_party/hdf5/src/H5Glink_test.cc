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
// Tests group link helpers: info extraction, entry conversion, location setup,
// table sorting/iteration/release, and name replacement on unlink
// (third_party/hdf5/src/H5Glink.c).
//
// Note: H5Glink.c lines 303-322 are guarded by #ifndef NDEBUG and are not
// compiled in google3 :hdf5 (-DNDEBUG).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Glink.c

#ifndef NDEBUG
#define H5GLINK_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5G_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5g_test_try_param
#include "third_party/hdf5/src/H5Gpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5HLprivate.h"
#include "third_party/hdf5/src/H5Ldevelop.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5RSprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5GLINK_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5GLINK_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5L_type_t kUdWithQueryId = static_cast<H5L_type_t>(H5L_TYPE_UD_MIN + 21);
constexpr H5L_type_t kUdFailQueryId = static_cast<H5L_type_t>(H5L_TYPE_UD_MIN + 22);
constexpr H5L_type_t kUdNoQueryId = static_cast<H5L_type_t>(H5L_TYPE_UD_MIN + 23);
constexpr H5L_type_t kUdUnregisteredId =
    static_cast<H5L_type_t>(H5L_TYPE_UD_MIN + 24);

hid_t DummyUdTraverse(const char* /*link_name*/, hid_t /*cur_group*/,
                      const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                      hid_t /*lapl_id*/, hid_t /*dxpl_id*/) {
  return H5I_INVALID_HID;
}

ssize_t SuccessUdQuery(const char* /*link_name*/, const void* /*lnkdata*/,
                       size_t lnkdata_size, void* /*buf*/,
                       size_t /*buf_size*/) {
  return static_cast<ssize_t>(lnkdata_size + 3);
}

ssize_t FailingUdQuery(const char* /*link_name*/, const void* /*lnkdata*/,
                       size_t /*lnkdata_size*/, void* /*buf*/,
                       size_t /*buf_size*/) {
  return -1;
}

H5F_t* UnwrapFile(const ScopedFile& file) {
  return static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
}

H5G_t* UnwrapGroup(const ScopedGroup& grp) {
  return static_cast<H5G_t*>(H5VL_object_verify(grp.get(), H5I_GROUP));
}

TEST(H5GlinkTest, LinkToInfoAllLinkClassesAndErrors) {
  ScopedFile file = CreateInMemoryFile("glink_info.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5O_loc_t* link_loc = H5G_oloc(UnwrapGroup(grp));
  ASSERT_NE(link_loc, nullptr);

  // Register custom UD link classes with query_func (ok, failing, nullptr).
  const H5L_class_t cls_ok = {
      H5L_LINK_CLASS_T_VERS, kUdWithQueryId, "ud_ok", nullptr, nullptr, nullptr,
      DummyUdTraverse, nullptr, SuccessUdQuery};
  const H5L_class_t cls_fail = {
      H5L_LINK_CLASS_T_VERS, kUdFailQueryId, "ud_fail", nullptr, nullptr,
      nullptr, DummyUdTraverse, nullptr, FailingUdQuery};
  const H5L_class_t cls_null_q = {
      H5L_LINK_CLASS_T_VERS, kUdNoQueryId, "ud_nullq", nullptr, nullptr,
      nullptr, DummyUdTraverse, nullptr, nullptr};

  ASSERT_GE(H5Lregister(&cls_ok), 0);
  absl::Cleanup unreg_ok = [] { H5Lunregister(kUdWithQueryId); };
  ASSERT_GE(H5Lregister(&cls_fail), 0);
  absl::Cleanup unreg_fail = [] { H5Lunregister(kUdFailQueryId); };
  ASSERT_GE(H5Lregister(&cls_null_q), 0);
  absl::Cleanup unreg_null_q = [] { H5Lunregister(kUdNoQueryId); };

  // 1. info == nullptr is a safe no-op.
  H5O_link_t hard_lnk{};
  hard_lnk.type = H5L_TYPE_HARD;
  hard_lnk.cset = H5T_CSET_ASCII;
  hard_lnk.corder_valid = true;
  hard_lnk.corder = 5;
  char hard_name[] = "hard";
  hard_lnk.name = hard_name;
  hard_lnk.u.hard.addr = link_loc->addr;
  ASSERT_GE(H5G_link_to_info(link_loc, &hard_lnk, nullptr), 0);

  // 2. Hard link populates token.
  H5L_info2_t info{};
  ASSERT_GE(H5G_link_to_info(link_loc, &hard_lnk, &info), 0);
  EXPECT_EQ(info.type, H5L_TYPE_HARD);
  EXPECT_TRUE(info.corder_valid);
  EXPECT_EQ(info.corder, 5);

  // 3. Soft link populates val_size = strlen + 1.
  H5O_link_t soft_lnk{};
  soft_lnk.type = H5L_TYPE_SOFT;
  char soft_name[] = "soft";
  char soft_target[] = "/target";
  soft_lnk.name = soft_name;
  soft_lnk.u.soft.name = soft_target;
  ASSERT_GE(H5G_link_to_info(link_loc, &soft_lnk, &info), 0);
  EXPECT_EQ(info.type, H5L_TYPE_SOFT);
  EXPECT_EQ(info.u.val_size, sizeof(soft_target));

  // 4. UD links: query success, no query_func, and unregistered class.
  char ud_name[] = "ud";
  std::array<uint8_t, 4> ud_data = {1, 2, 3, 4};
  H5O_link_t ud_lnk{};
  ud_lnk.name = ud_name;
  ud_lnk.u.ud.udata = ud_data.data();
  ud_lnk.u.ud.size = ud_data.size();

  ud_lnk.type = kUdWithQueryId;
  ASSERT_GE(H5G_link_to_info(link_loc, &ud_lnk, &info), 0);
  EXPECT_EQ(info.u.val_size, 7u);

  ud_lnk.type = kUdNoQueryId;
  ASSERT_GE(H5G_link_to_info(link_loc, &ud_lnk, &info), 0);
  EXPECT_EQ(info.u.val_size, 0u);

  ud_lnk.type = kUdUnregisteredId;
  ASSERT_GE(H5G_link_to_info(link_loc, &ud_lnk, &info), 0);
  EXPECT_EQ(info.u.val_size, 0u);

  ud_lnk.type = H5L_TYPE_MAX;
  ASSERT_GE(H5G_link_to_info(link_loc, &ud_lnk, &info), 0);
  EXPECT_EQ(info.u.val_size, 0u);

  // External link (UD built-in class 64) expects "file\0obj\0" in udata.
  constexpr char ext_payload[] = "\0ext_file.h5\0/ext_obj";
  H5O_link_t ext_lnk{};
  ext_lnk.name = ud_name;
  ext_lnk.type = H5L_TYPE_EXTERNAL;
  ext_lnk.u.ud.udata = const_cast<char*>(ext_payload);
  ext_lnk.u.ud.size = sizeof(ext_payload);
  ASSERT_GE(H5G_link_to_info(link_loc, &ext_lnk, &info), 0);
  EXPECT_EQ(info.type, H5L_TYPE_EXTERNAL);
  EXPECT_EQ(info.u.val_size, sizeof(ext_payload));

  // 5. Negative tests: failing query callback and invalid link types.
  ScopedErrorSilence silence;
  ud_lnk.type = kUdFailQueryId;
  EXPECT_LT(H5G_link_to_info(link_loc, &ud_lnk, &info), 0);

  for (H5L_type_t bad_type :
       {H5L_TYPE_ERROR,
        static_cast<H5L_type_t>(H5L_TYPE_BUILTIN_MAX + 1),
        static_cast<H5L_type_t>(-2)}) {
    H5O_link_t bad_lnk = hard_lnk;
    bad_lnk.type = bad_type;
    EXPECT_LT(H5G_link_to_info(link_loc, &bad_lnk, &info), 0);
  }
}

TEST(H5GlinkTest, LinkToEntCoversGroupDsetUnknownSoftAndErrors) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
      0);
  ScopedFile file(H5Fcreate("glink_ent.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
  H5F_t* f = UnwrapFile(file);
  ASSERT_NE(f, nullptr);

  // Earliest libver bound creates old-style symbol-table groups (H5O_STAB_ID).
  ScopedGroup old_grp(H5Gcreate2(file.get(), "old_grp", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(old_grp.valid());
  H5O_loc_t* grp_oloc = H5G_oloc(UnwrapGroup(old_grp));
  ASSERT_NE(grp_oloc, nullptr);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  H5O_loc_t* dset_oloc = H5O_get_loc(dset.get());
  ASSERT_NE(dset_oloc, nullptr);

  haddr_t heap_addr = HADDR_UNDEF;
  ASSERT_GE(H5HL_create(f, 512, &heap_addr), 0);
  H5HL_t* heap = H5HL_protect(f, heap_addr, H5AC__NO_FLAGS_SET);
  ASSERT_NE(heap, nullptr);
  absl::Cleanup unprotect = [&] { H5HL_unprotect(heap); };

  // 1. H5L_TYPE_HARD + H5O_TYPE_GROUP with CACHED_STAB and NOTHING_CACHED.
  char name_grp[] = "g_link";
  H5O_link_t lnk{};
  lnk.type = H5L_TYPE_HARD;
  lnk.name = name_grp;
  lnk.u.hard.addr = grp_oloc->addr;

  H5G_obj_create_t gcrt{};
  gcrt.cache_type = H5G_CACHED_STAB;
  gcrt.cache.stab.btree_addr = 0x1234;
  gcrt.cache.stab.heap_addr = 0x5678;

  H5G_entry_t ent{};
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_GROUP, &gcrt, &ent), 0);
  EXPECT_EQ(ent.type, H5G_CACHED_STAB);
  EXPECT_EQ(ent.cache.stab.btree_addr, 0x1234u);

  gcrt.cache_type = H5G_NOTHING_CACHED;
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_GROUP, &gcrt, &ent), 0);
  EXPECT_EQ(ent.type, H5G_NOTHING_CACHED);

  // 2. H5L_TYPE_HARD + H5O_TYPE_DATASET -> NOTHING_CACHED.
  char name_dset[] = "d_link";
  lnk.name = name_dset;
  lnk.u.hard.addr = dset_oloc->addr;
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_DATASET, nullptr, &ent),
            0);
  EXPECT_EQ(ent.type, H5G_NOTHING_CACHED);
  EXPECT_EQ(ent.header, dset_oloc->addr);

  // 3. H5L_TYPE_HARD + H5O_TYPE_UNKNOWN on symbol-table group -> CACHED_STAB.
  lnk.u.hard.addr = grp_oloc->addr;
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_UNKNOWN, nullptr, &ent),
            0);
  EXPECT_EQ(ent.type, H5G_CACHED_STAB);

  // 4. H5L_TYPE_HARD + H5O_TYPE_UNKNOWN on dataset (no STAB) -> NOTHING_CACHED.
  lnk.u.hard.addr = dset_oloc->addr;
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_UNKNOWN, nullptr, &ent),
            0);
  EXPECT_EQ(ent.type, H5G_NOTHING_CACHED);

  // 5. H5L_TYPE_SOFT -> CACHED_SLINK.
  char name_soft[] = "s_link";
  char target_soft[] = "/old_grp";
  lnk.type = H5L_TYPE_SOFT;
  lnk.name = name_soft;
  lnk.u.soft.name = target_soft;
  ASSERT_GE(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_UNKNOWN, nullptr, &ent),
            0);
  EXPECT_EQ(ent.type, H5G_CACHED_SLINK);

  // 6. Failure cases: invalid ohdr addr on H5O_TYPE_UNKNOWN & bad link types.
  ScopedErrorSilence silence;
  lnk.type = H5L_TYPE_HARD;
  lnk.u.hard.addr = HADDR_UNDEF;
  EXPECT_LT(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_UNKNOWN, nullptr, &ent), 0);

  for (H5L_type_t bad_type :
       {H5L_TYPE_ERROR, H5L_TYPE_EXTERNAL, H5L_TYPE_MAX}) {
    lnk.type = bad_type;
    EXPECT_LT(H5G__link_to_ent(f, heap, &lnk, H5O_TYPE_UNKNOWN, nullptr, &ent),
              0);
  }
}

TEST(H5GlinkTest, LinkToLocAndNameReplace) {
  ScopedFile file = CreateInMemoryFile("glink_loc.h5");
  ASSERT_TRUE(file.valid());
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_ctx = [] { H5CX_pop(false); };
  ScopedGroup grp(H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedGroup child(H5Gcreate2(grp.get(), "child", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(child.valid());

  H5G_loc_t grp_loc{};
  ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
  H5G_loc_t child_loc{};
  ASSERT_GE(H5G_loc(child.get(), &child_loc), 0);

  H5O_loc_t obj_oloc{};
  H5G_name_t obj_path{};
  H5G_loc_t obj_loc = {&obj_oloc, &obj_path};
  H5G_loc_reset(&obj_loc);

  char cname[] = "child";
  H5O_link_t hard_lnk{};
  hard_lnk.type = H5L_TYPE_HARD;
  hard_lnk.name = cname;
  hard_lnk.u.hard.addr = child_loc.oloc->addr;
  ASSERT_GE(H5G__link_to_loc(&grp_loc, &hard_lnk, &obj_loc), 0);
  EXPECT_EQ(obj_oloc.file, grp_loc.oloc->file);
  EXPECT_EQ(obj_oloc.addr, child_loc.oloc->addr);
  ASSERT_GE(H5G_name_free(&obj_path), 0);

  H5G_loc_reset(&obj_loc);
  char sname[] = "slink";
  char starget[] = "/grp";
  H5O_link_t soft_lnk{};
  soft_lnk.type = H5L_TYPE_SOFT;
  soft_lnk.name = sname;
  soft_lnk.u.soft.name = starget;
  ASSERT_GE(H5G__link_to_loc(&grp_loc, &soft_lnk, &obj_loc), 0);
  EXPECT_EQ(obj_oloc.file, grp_loc.oloc->file);
  EXPECT_EQ(obj_oloc.addr, HADDR_UNDEF);
  ASSERT_GE(H5G_name_free(&obj_path), 0);

  // Unknown built-in link fails H5G__link_to_loc.
  {
    ScopedErrorSilence silence;
    H5O_link_t bad_lnk = hard_lnk;
    bad_lnk.type = static_cast<H5L_type_t>(H5L_TYPE_BUILTIN_MAX + 1);
    EXPECT_LT(H5G__link_to_loc(&grp_loc, &bad_lnk, &obj_loc), 0);
  }

  // H5G__link_name_replace: nullptr path (no-op) and non-null path.
  H5F_t* f = UnwrapFile(file);
  ASSERT_NE(f, nullptr);
  ASSERT_GE(H5G__link_name_replace(f, nullptr, &hard_lnk), 0);

  H5RS_str_t* full_path = H5RS_create("/grp");
  ASSERT_NE(full_path, nullptr);
  absl::Cleanup decr_path = [&] { H5RS_decr(full_path); };
  ASSERT_GE(H5G__link_name_replace(f, full_path, &hard_lnk), 0);
}

TEST(H5GlinkTest, SortIterateAndReleaseLinkTable) {
  // 1. Empty table sort and release.
  H5G_link_table_t empty{};
  empty.nlinks = 0;
  empty.lnks = nullptr;
  EXPECT_GE(H5G__link_sort_table(&empty, H5_INDEX_NAME, H5_ITER_INC), 0);
  EXPECT_GE(H5G__link_release_table(&empty), 0);

  // 2. Non-empty table allocated with HDF5 allocators (released via
  //    H5G__link_release_table -> H5O_msg_reset + H5MM_xfree).
  constexpr size_t kN = 4;
  H5G_link_table_t ltable{};
  ltable.nlinks = kN;
  ltable.lnks = static_cast<H5O_link_t*>(H5MM_calloc(kN * sizeof(H5O_link_t)));
  ASSERT_NE(ltable.lnks, nullptr);
  bool released = false;
  absl::Cleanup cleanup_table = [&] {
    if (!released) {
      H5G__link_release_table(&ltable);
    }
  };

  ltable.lnks[0].type = H5L_TYPE_HARD;
  ltable.lnks[0].name = H5MM_xstrdup("delta");
  ltable.lnks[0].corder = 30;

  ltable.lnks[1].type = H5L_TYPE_SOFT;
  ltable.lnks[1].name = H5MM_xstrdup("alpha");
  ltable.lnks[1].u.soft.name = H5MM_xstrdup("/target");
  ltable.lnks[1].corder = 10;

  ltable.lnks[2].type = kUdUnregisteredId;
  ltable.lnks[2].name = H5MM_xstrdup("charlie");
  ltable.lnks[2].u.ud.udata = H5MM_calloc(4);
  ltable.lnks[2].u.ud.size = 4;
  ltable.lnks[2].corder = 20;

  ltable.lnks[3].type = H5L_TYPE_HARD;
  ltable.lnks[3].name = H5MM_xstrdup("bravo");
  ltable.lnks[3].corder = 20;  // Equal corder tests equality branch.

  // Sort by NAME: NATIVE, DEC, INC.
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_NAME, H5_ITER_NATIVE), 0);
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_NAME, H5_ITER_DEC), 0);
  EXPECT_STREQ(ltable.lnks[0].name, "delta");
  EXPECT_STREQ(ltable.lnks[3].name, "alpha");

  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_NAME, H5_ITER_INC), 0);
  EXPECT_STREQ(ltable.lnks[0].name, "alpha");
  EXPECT_STREQ(ltable.lnks[3].name, "delta");

  // Sort by CRT_ORDER: NATIVE, DEC, INC (plus swapped/pre-sorted passes so both
  // `<` and `>` branches in H5G__link_cmp_corder_dec/inc are triggered).
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_NATIVE),
            0);
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_DEC), 0);
  EXPECT_EQ(ltable.lnks[0].corder, 30);
  EXPECT_EQ(ltable.lnks[3].corder, 10);

  // Re-sort descending table with DEC so (30 > 20) triggers `-1` branch, then
  // scramble ends and sort with DEC once more.
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_DEC), 0);
  std::swap(ltable.lnks[0], ltable.lnks[2]);
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_DEC), 0);
  EXPECT_EQ(ltable.lnks[0].corder, 30);
  EXPECT_EQ(ltable.lnks[3].corder, 10);

  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_INC), 0);
  // Re-sort ascending table with INC and scrambled middle so (10 < 20)
  // triggers `-1` branch.
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_INC), 0);
  std::swap(ltable.lnks[1], ltable.lnks[3]);
  ASSERT_GE(H5G__link_sort_table(&ltable, H5_INDEX_CRT_ORDER, H5_ITER_INC), 0);
  EXPECT_EQ(ltable.lnks[0].corder, 10);
  EXPECT_EQ(ltable.lnks[3].corder, 30);

  // Iterate table: full pass with skip, stop > 0, and callback failure < 0.
  std::vector<std::string> seen;
  auto collect = [](const H5O_link_t* lnk, void* data) -> herr_t {
    static_cast<std::vector<std::string>*>(data)->emplace_back(lnk->name);
    return H5_ITER_CONT;
  };
  hsize_t last_lnk = 0;
  EXPECT_EQ(H5G__link_iterate_table(&ltable, 1, &last_lnk, collect, &seen),
            H5_ITER_CONT);
  EXPECT_EQ(last_lnk, 4u);
  EXPECT_EQ(seen.size(), 3u);

  auto stop_first = [](const H5O_link_t* /*lnk*/, void* /*d*/) -> herr_t {
    return 9;
  };
  EXPECT_EQ(H5G__link_iterate_table(&ltable, 0, nullptr, stop_first, nullptr),
            9);

  {
    ScopedErrorSilence silence;
    auto fail_cb = [](const H5O_link_t* /*lnk*/, void* /*d*/) -> herr_t {
      return -1;
    };
    EXPECT_LT(H5G__link_iterate_table(&ltable, 0, nullptr, fail_cb, nullptr),
              0);
  }

  ASSERT_GE(H5G__link_release_table(&ltable), 0);
  released = true;
}

}  // namespace
}  // namespace hdf5
