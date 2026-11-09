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
// Tests dense attribute storage (fractal heap + v2 B-tree name and creation
// order indices, compact/dense phase changes, SOHM-shared attributes, rename,
// iteration, removal, and internal error/cleanup paths) in
// third_party/hdf5/src/H5Adense.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Adense.c

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5ADENSE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5B2private.h (pulled by H5Apkg.h) uses `try` as a parameter name, which is
// both a C++ keyword and macro-defined under -fno-exceptions.
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
extern "C" {
#define H5A_FRIEND
#define H5F_FRIEND
#define H5O_FRIEND
#define H5SM_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HFprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#ifdef H5_HAVE__FLOAT16
typedef uint16_t H5__Float16;
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5ADENSE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ADENSE_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAreArray;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__IGNORE_TAG) {
        H5AC_tag(tag, &prev_tag_);
        tagged_ = true;
      }
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(false);
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

// Reads out the H5F_t* and H5O_ainfo_t for an open object (group/dataset).
bool GetObjectAinfo(hid_t oid, H5F_t** out_f, H5O_ainfo_t* out_ainfo) {
  ScopedApiContext ctx;
  if (!ctx.valid()) return false;
  H5O_loc_t* loc = H5O_get_loc(oid);
  if (loc == nullptr || loc->file == nullptr) return false;
  H5O_t* oh = H5O_protect(loc, H5AC__READ_ONLY_FLAG, false);
  if (oh == nullptr) return false;
  absl::Cleanup unprotect = [&] { H5O_unprotect(loc, oh, H5AC__NO_FLAGS_SET); };
  std::memset(out_ainfo, 0, sizeof(*out_ainfo));
  out_ainfo->fheap_addr = HADDR_UNDEF;
  out_ainfo->name_bt2_addr = HADDR_UNDEF;
  out_ainfo->corder_bt2_addr = HADDR_UNDEF;
  if (H5A__get_ainfo(loc->file, oh, out_ainfo) < 0) return false;
  *out_f = loc->file;
  return H5_addr_defined(out_ainfo->fheap_addr);
}

struct SohmIndexSnapshot {
  haddr_t index_addr = HADDR_UNDEF;
  haddr_t heap_addr = HADDR_UNDEF;
};

bool GetSohmIndexAddrs(H5F_t* f, unsigned index_num, SohmIndexSnapshot* out) {
  ScopedApiContext ctx(H5AC__SOHM_TAG);
  if (!ctx.valid()) return false;
  H5SM_table_cache_ud_t cache_udata{f};
  auto* table = static_cast<H5SM_master_table_t*>(
      H5AC_protect(f, H5AC_SOHM_TABLE, f->shared->sohm_addr, &cache_udata,
                   H5AC__READ_ONLY_FLAG));
  if (table == nullptr) return false;
  absl::Cleanup unprotect = [&] {
    H5AC_unprotect(f, H5AC_SOHM_TABLE, f->shared->sohm_addr, table,
                   H5AC__NO_FLAGS_SET);
  };
  if (index_num >= table->num_indexes) return false;
  out->index_addr = table->indexes[index_num].index_addr;
  out->heap_addr = table->indexes[index_num].heap_addr;
  return true;
}

bool SetSohmIndexAddrs(H5F_t* f, unsigned index_num,
                       const SohmIndexSnapshot& vals) {
  ScopedApiContext ctx(H5AC__SOHM_TAG);
  if (!ctx.valid()) return false;
  H5SM_table_cache_ud_t cache_udata{f};
  auto* table = static_cast<H5SM_master_table_t*>(
      H5AC_protect(f, H5AC_SOHM_TABLE, f->shared->sohm_addr, &cache_udata,
                   H5AC__NO_FLAGS_SET));
  if (table == nullptr) return false;
  absl::Cleanup unprotect = [&] {
    H5AC_unprotect(f, H5AC_SOHM_TABLE, f->shared->sohm_addr, table,
                   H5AC__NO_FLAGS_SET);
  };
  if (index_num >= table->num_indexes) return false;
  table->indexes[index_num].index_addr = vals.index_addr;
  table->indexes[index_num].heap_addr = vals.heap_addr;
  return true;
}

ScopedType CreateLargeCompoundType() {
  ScopedType ctype(H5Tcreate(H5T_COMPOUND, 32));
  if (!ctype.valid()) return ScopedType();
  if (H5Tinsert(ctype, "f0", 0, H5T_NATIVE_INT64) < 0 ||
      H5Tinsert(ctype, "f1", 8, H5T_NATIVE_INT64) < 0 ||
      H5Tinsert(ctype, "f2", 16, H5T_NATIVE_INT64) < 0 ||
      H5Tinsert(ctype, "f3", 24, H5T_NATIVE_INT64) < 0) {
    return ScopedType();
  }
  return ctype;
}

ScopedPlist MakeDenseGroupPlist(unsigned max_compact, unsigned min_dense,
                                bool index_corder) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) return ScopedPlist();
  if (H5Pset_attr_phase_change(gcpl, max_compact, min_dense) < 0) {
    return ScopedPlist();
  }
  if (index_corder) {
    if (H5Pset_attr_creation_order(
            gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      return ScopedPlist();
    }
  }
  return gcpl;
}

ScopedPlist MakeSohmFcpl(unsigned min_mesg_size, unsigned max_list = 2,
                         unsigned min_btree = 1) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) return ScopedPlist();
  if (H5Pset_shared_mesg_nindexes(fcpl, 1) < 0 ||
      H5Pset_shared_mesg_index(fcpl, 0, H5O_SHMESG_ATTR_FLAG, min_mesg_size) <
          0 ||
      H5Pset_shared_mesg_phase_change(fcpl, max_list, min_btree) < 0) {
    return ScopedPlist();
  }
  return fcpl;
}

void CreateIntAttr(hid_t loc, const char* name, int32_t value) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedAttr attr(H5Acreate2(loc, name, H5T_NATIVE_INT32, scalar, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(attr.valid()) << name;
  ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &value), 0);
}

int32_t ReadIntAttr(hid_t loc, const char* name) {
  ScopedAttr attr(H5Aopen(loc, name, H5P_DEFAULT));
  if (!attr.valid()) return -999999;
  int32_t out = -999999;
  if (H5Aread(attr, H5T_NATIVE_INT32, &out) < 0) return -999999;
  return out;
}

std::vector<std::string> CollectAttrNames(hid_t loc, H5_index_t idx_type,
                                          H5_iter_order_t order) {
  std::vector<std::string> names;
  auto cb = [](hid_t, const char* name, const H5A_info_t*,
               void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return 0;
  };
  hsize_t idx = 0;
  if (H5Aiterate2(loc, idx_type, order, &idx, cb, &names) < 0) {
    return {};
  }
  return names;
}

TEST(H5AdenseTest, PhaseChangeTransitionsPersistenceAndLargeAttr) {
  // 1. Create groups with compact->dense phase change (max_compact=2,
  //    min_dense=1) both with and without creation-order indexing, plus a
  //    default group that transitions due to >64KB attribute message size.
  ScopedFile file = CreateInMemoryFile("dense_phase_change.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl_plain = MakeDenseGroupPlist(2, 1, /*index_corder=*/false);
  ASSERT_TRUE(gcpl_plain.valid());
  ScopedPlist gcpl_corder = MakeDenseGroupPlist(2, 1, /*index_corder=*/true);
  ASSERT_TRUE(gcpl_corder.valid());

  ScopedGroup g_plain(
      H5Gcreate2(file, "plain", H5P_DEFAULT, gcpl_plain, H5P_DEFAULT));
  ASSERT_TRUE(g_plain.valid());
  ScopedGroup g_corder(
      H5Gcreate2(file, "corder", H5P_DEFAULT, gcpl_corder, H5P_DEFAULT));
  ASSERT_TRUE(g_corder.valid());
  ScopedGroup g_large(
      H5Gcreate2(file, "large", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g_large.valid());

  // Dataset with max_compact=0, min_dense=0 always stores attributes densely.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_attr_phase_change(dcpl, 0, 0), 0);
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(file, "d_always_dense", H5T_NATIVE_INT32,
                                scalar, H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  CreateIntAttr(dset, "d0", 77);
  EXPECT_EQ(ReadIntAttr(dset, "d0"), 77);

  // 2. Push both groups across compact -> dense threshold (>2 attributes).
  for (int32_t i = 0; i < 5; ++i) {
    const std::string name = std::format("attr_{}", i);
    CreateIntAttr(g_plain, name.c_str(), i * 10);
    CreateIntAttr(g_corder, name.c_str(), i * 100);
  }
  EXPECT_GT(H5Aexists(g_plain, "attr_4"), 0);
  EXPECT_EQ(H5Aexists(g_plain, "no_such_attr"), 0);
  EXPECT_GT(H5Aexists(g_corder, "attr_3"), 0);

  // Modify an existing dense attribute (>128B attr serialized size vs small).
  {
    ScopedAttr a(H5Aopen(g_corder, "attr_2", H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    constexpr int32_t kUpdated = 9999;
    ASSERT_GE(H5Awrite(a, H5T_NATIVE_INT32, &kUpdated), 0);
  }
  EXPECT_EQ(ReadIntAttr(g_corder, "attr_2"), 9999);

  // Add an attribute larger than H5A_ATTR_BUF_SIZE (128 bytes) to exercise
  // heap buffer fallback in H5WB_actual on both insert and write.
  constexpr hsize_t kMedDims = 64;
  ScopedSpace med_space(H5Screate_simple(1, &kMedDims, nullptr));
  ASSERT_TRUE(med_space.valid());
  std::vector<int32_t> med_vals(kMedDims, 55);
  {
    ScopedAttr a_med(H5Acreate2(g_corder, "medium_heap_buf", H5T_NATIVE_INT32,
                                med_space, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_med.valid());
    ASSERT_GE(H5Awrite(a_med, H5T_NATIVE_INT32, med_vals.data()), 0);
    med_vals[0] = 777;
    ASSERT_GE(H5Awrite(a_med, H5T_NATIVE_INT32, med_vals.data()), 0);
  }

  // Create a single >64KB attribute on default-threshold group to trigger
  // dense.
  constexpr hsize_t kLargeDims = 17000;
  ScopedSpace large_space(H5Screate_simple(1, &kLargeDims, nullptr));
  ASSERT_TRUE(large_space.valid());
  std::vector<int32_t> large_vals(kLargeDims, 123456);
  {
    ScopedAttr a_large(H5Acreate2(g_large, "big_dense", H5T_NATIVE_INT32,
                                  large_space, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_large.valid());
    ASSERT_GE(H5Awrite(a_large, H5T_NATIVE_INT32, large_vals.data()), 0);
  }
  EXPECT_GT(H5Aexists(g_large, "big_dense"), 0);

  // 3. Flush and reopen in-memory image to verify dense metadata round-trip.
  ASSERT_GE(H5Fflush(file, H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file, nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size), 0);
  ASSERT_EQ(H5Fget_file_image(file, image.data(), image.size()), image_size);

  dset.reset();
  g_large.reset();
  g_corder.reset();
  g_plain.reset();
  file.reset();

  ScopedPlist reopen_fapl = MakeCoreFapl();
  ASSERT_TRUE(reopen_fapl.valid());
  ASSERT_GE(H5Pset_file_image(reopen_fapl, image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopened.h5", H5F_ACC_RDWR, reopen_fapl));
  ASSERT_TRUE(reopened.valid());

  ScopedGroup rg_plain(H5Gopen2(reopened, "plain", H5P_DEFAULT));
  ASSERT_TRUE(rg_plain.valid());
  EXPECT_EQ(ReadIntAttr(rg_plain, "attr_1"), 10);

  ScopedGroup rg_corder(H5Gopen2(reopened, "corder", H5P_DEFAULT));
  ASSERT_TRUE(rg_corder.valid());
  EXPECT_EQ(ReadIntAttr(rg_corder, "attr_2"), 9999);
  std::vector<int32_t> med_read(kMedDims, -1);
  {
    ScopedAttr ra(H5Aopen(rg_corder, "medium_heap_buf", H5P_DEFAULT));
    ASSERT_TRUE(ra.valid());
    ASSERT_GE(H5Aread(ra, H5T_NATIVE_INT32, med_read.data()), 0);
  }
  EXPECT_EQ(med_read[0], 777);

  // Delete all attributes on plain group below min_dense (1 -> 0) to trigger
  // dense -> compact revert and H5A__dense_delete.
  for (int i = 0; i < 5; ++i) {
    const std::string name = std::format("attr_{}", i);
    ASSERT_GE(H5Adelete(rg_plain, name.c_str()), 0);
  }
  EXPECT_THAT(CollectAttrNames(rg_plain, H5_INDEX_NAME, H5_ITER_INC),
              IsEmpty());
}

TEST(H5AdenseTest, IterateOrdersSkipCallbacksAndH5Ocopy) {
  ScopedFile file = CreateInMemoryFile("dense_iter_copy.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl = MakeDenseGroupPlist(2, 1, /*index_corder=*/true);
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(H5Gcreate2(file, "src", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr std::array<const char*, 5> kInsertOrder = {"echo", "bravo", "delta",
                                                       "alpha", "charlie"};
  for (int32_t i = 0; i < static_cast<int32_t>(kInsertOrder.size()); ++i) {
    CreateIntAttr(grp, kInsertOrder[i], i + 1);
  }

  // Exercise sorted name/corder INC and DEC table paths, plus CRT_ORDER NATIVE.
  EXPECT_THAT(CollectAttrNames(grp, H5_INDEX_NAME, H5_ITER_INC),
              ElementsAre("alpha", "bravo", "charlie", "delta", "echo"));
  EXPECT_THAT(CollectAttrNames(grp, H5_INDEX_NAME, H5_ITER_DEC),
              ElementsAre("echo", "delta", "charlie", "bravo", "alpha"));
  EXPECT_THAT(CollectAttrNames(grp, H5_INDEX_CRT_ORDER, H5_ITER_INC),
              ElementsAre("echo", "bravo", "delta", "alpha", "charlie"));
  EXPECT_THAT(CollectAttrNames(grp, H5_INDEX_CRT_ORDER, H5_ITER_DEC),
              ElementsAre("charlie", "alpha", "delta", "bravo", "echo"));
  EXPECT_THAT(CollectAttrNames(grp, H5_INDEX_CRT_ORDER, H5_ITER_NATIVE),
              ElementsAre("echo", "bravo", "delta", "alpha", "charlie"));

  // Exercise H5_INDEX_NAME + H5_ITER_NATIVE (hashed B-tree order), skip,
  // positive short-circuit stop + resume, and negative error propagation.
  const std::vector<std::string> native_all =
      CollectAttrNames(grp, H5_INDEX_NAME, H5_ITER_NATIVE);
  EXPECT_THAT(native_all, UnorderedElementsAreArray(kInsertOrder));

  struct StopState {
    std::vector<std::string> seen;
    int stop_at = -1;
    herr_t ret = 0;
  };
  auto cb = [](hid_t, const char* name, const H5A_info_t*,
               void* op_data) -> herr_t {
    auto* st = static_cast<StopState*>(op_data);
    st->seen.emplace_back(name);
    if (st->stop_at >= 0 && static_cast<int>(st->seen.size()) >= st->stop_at) {
      return st->ret;
    }
    return 0;
  };

  StopState part1;
  part1.stop_at = 2;
  part1.ret = 5;
  hsize_t idx = 1;  // skip = 1
  EXPECT_EQ(H5Aiterate2(grp, H5_INDEX_NAME, H5_ITER_NATIVE, &idx, cb, &part1),
            5);
  EXPECT_EQ(idx, 3u);
  EXPECT_THAT(part1.seen, ElementsAre(native_all[1], native_all[2]));

  StopState part2;
  EXPECT_EQ(H5Aiterate2(grp, H5_INDEX_NAME, H5_ITER_NATIVE, &idx, cb, &part2),
            0);
  EXPECT_EQ(idx, 5u);
  EXPECT_THAT(part2.seen, ElementsAre(native_all[3], native_all[4]));

  // Error return inside native iteration callback triggers HERROR in
  // H5A__dense_iterate_bt2_cb and H5A__dense_iterate.
  StopState err_st;
  err_st.stop_at = 1;
  err_st.ret = -4;
  idx = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5Aiterate2(grp, H5_INDEX_NAME, H5_ITER_NATIVE, &idx, cb, &err_st), -4);
  }

  // Open by H5_INDEX_NAME + H5_ITER_NATIVE -> invokes H5A_ATTR_OP_LIB inside
  // H5A__dense_iterate_bt2_cb.
  {
    ScopedAttr by_native(H5Aopen_by_idx(grp, ".", H5_INDEX_NAME, H5_ITER_NATIVE,
                                        1, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(by_native.valid());
  }

  // Copy dense-attribute group via H5Ocopy to exercise
  // H5A__dense_post_copy_file_all -> H5A__dense_insert on destination.
  ASSERT_GE(H5Ocopy(file, "src", file, "dst_copy", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedGroup dst(H5Gopen2(file, "dst_copy", H5P_DEFAULT));
  ASSERT_TRUE(dst.valid());
  EXPECT_THAT(CollectAttrNames(dst, H5_INDEX_CRT_ORDER, H5_ITER_INC),
              ElementsAre("echo", "bravo", "delta", "alpha", "charlie"));
  EXPECT_EQ(ReadIntAttr(dst, "delta"), 3);
}

TEST(H5AdenseTest, RenameAndRemoveByNameAndByIdxAllModes) {
  ScopedFile file = CreateInMemoryFile("dense_rename_remove.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl_plain = MakeDenseGroupPlist(2, 1, /*index_corder=*/false);
  ScopedPlist gcpl_corder = MakeDenseGroupPlist(2, 1, /*index_corder=*/true);
  ScopedGroup g_plain(
      H5Gcreate2(file, "plain", H5P_DEFAULT, gcpl_plain, H5P_DEFAULT));
  ScopedGroup g_corder(
      H5Gcreate2(file, "corder", H5P_DEFAULT, gcpl_corder, H5P_DEFAULT));
  ASSERT_TRUE(g_plain.valid());
  ASSERT_TRUE(g_corder.valid());

  for (int32_t i = 0; i < 8; ++i) {
    const std::string name = std::format("k_{}", i);
    CreateIntAttr(g_plain, name.c_str(), i);
    CreateIntAttr(g_corder, name.c_str(), i + 100);
  }

  // Rename without and with creation order index.
  ASSERT_GE(H5Arename(g_plain, "k_3", "k_3_renamed"), 0);
  EXPECT_EQ(H5Aexists(g_plain, "k_3"), 0);
  EXPECT_EQ(ReadIntAttr(g_plain, "k_3_renamed"), 3);

  ASSERT_GE(H5Arename(g_corder, "k_4", "k_4_renamed"), 0);
  EXPECT_EQ(H5Aexists(g_corder, "k_4"), 0);
  EXPECT_EQ(ReadIntAttr(g_corder, "k_4_renamed"), 104);
  EXPECT_THAT(CollectAttrNames(g_corder, H5_INDEX_CRT_ORDER, H5_ITER_INC),
              ElementsAre("k_0", "k_1", "k_2", "k_3", "k_4_renamed", "k_5",
                          "k_6", "k_7"));

  // Delete renamed attribute by name (both indices updated), then delete by
  // index via CRT_ORDER NATIVE, NAME NATIVE, and sorted table INC, plus OOB.
  ASSERT_GE(H5Adelete(g_corder, "k_4_renamed"), 0);
  ASSERT_GE(H5Adelete_by_idx(g_corder, ".", H5_INDEX_CRT_ORDER, H5_ITER_NATIVE,
                             0, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_idx(g_corder, ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_idx(g_corder, ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_idx(g_plain, ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             H5P_DEFAULT),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Adelete_by_idx(g_corder, ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               H5P_DEFAULT),
              0);
  }

  // Delete an entire group holding dense attributes via H5Ldelete to exercise
  // H5A__dense_delete (including corder index teardown).
  g_corder.reset();
  g_plain.reset();
  ASSERT_GE(H5Ldelete(file, "corder", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file, "plain", H5P_DEFAULT), 0);
}

TEST(H5AdenseTest, SharedObjectHeaderMessagesCoverSohmBranches) {
  // Enable SOHM for attribute messages >= 48 bytes with small list->btree
  // phase change thresholds.
  ScopedPlist fcpl = MakeSohmFcpl(/*min_mesg_size=*/48, /*max_list=*/2, 1);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file = CreateInMemoryFile("dense_sohm.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl_no_corder =
      MakeDenseGroupPlist(2, 1, /*index_corder=*/false);
  ScopedPlist gcpl_corder = MakeDenseGroupPlist(2, 1, /*index_corder=*/true);
  ASSERT_TRUE(gcpl_no_corder.valid());
  ASSERT_TRUE(gcpl_corder.valid());

  ScopedGroup g_no_corder(
      H5Gcreate2(file, "no_corder", H5P_DEFAULT, gcpl_no_corder, H5P_DEFAULT));
  ScopedGroup g_corder(
      H5Gcreate2(file, "corder", H5P_DEFAULT, gcpl_corder, H5P_DEFAULT));
  ScopedGroup g_other(
      H5Gcreate2(file, "other", H5P_DEFAULT, gcpl_no_corder, H5P_DEFAULT));
  ScopedGroup g_wipe(
      H5Gcreate2(file, "wipe", H5P_DEFAULT, gcpl_no_corder, H5P_DEFAULT));
  ASSERT_TRUE(g_no_corder.valid());
  ASSERT_TRUE(g_corder.valid());
  ASSERT_TRUE(g_other.valid());
  ASSERT_TRUE(g_wipe.valid());

  // Committed datatype shared across SOHM attributes exercises H5O__attr_link.
  ScopedType committed_i64(H5Tcopy(H5T_NATIVE_INT64));
  ASSERT_TRUE(committed_i64.valid());
  ASSERT_GE(H5Tcommit2(file, "c_i64", committed_i64, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  // 1. Insert small attributes (< 48B) first while shared_fheap_addr is still
  //    undefined, exercising non-shared rename/write/iterate/exists paths
  //    inside a SOHM-capable file.
  for (int i = 0; i < 3; ++i) {
    const std::string name = std::format("small_{}", i);
    CreateIntAttr(g_no_corder, name.c_str(), i);
    CreateIntAttr(g_wipe, name.c_str(), i);
  }
  ASSERT_GE(H5Arename(g_no_corder, "small_0", "small_renamed"), 0);
  EXPECT_GT(H5Aexists(g_no_corder, "small_renamed"), 0);

  // 2. Helper creating shareable 16-int64 (>= 128B > 48B min_mesg_size) attrs.
  constexpr hsize_t kVecLen = 16;
  ScopedSpace vec_space(H5Screate_simple(1, &kVecLen, nullptr));
  ASSERT_TRUE(vec_space.valid());
  auto create_shared_attr = [&](hid_t loc, const char* name, int64_t seed) {
    std::array<int64_t, kVecLen> vals{};
    vals.fill(seed);
    ScopedAttr a(H5Acreate2(loc, name, committed_i64, vec_space, H5P_DEFAULT,
                            H5P_DEFAULT));
    ASSERT_TRUE(a.valid()) << name;
    ASSERT_GE(H5Awrite(a, H5T_NATIVE_INT64, vals.data()), 0);
  };

  // Compact -> dense conversion with already-shared compact attributes covers
  // `shared_mesg > 0` in H5A__dense_insert.
  for (int i = 0; i < 4; ++i) {
    const std::string name = std::format("sh_{}", i);
    create_shared_attr(g_no_corder, name.c_str(), 100 + i);
    create_shared_attr(g_corder, name.c_str(), 200 + i);
  }
  create_shared_attr(g_wipe, "wipe_sh_0", 300);
  create_shared_attr(g_wipe, "wipe_sh_1", 301);

  // Also pre-populate `g_other` with `target_rc2` having identical payload so
  // renaming `sh_2` -> `target_rc2` yields SOHM ref_count == 2 (> 1).
  create_shared_attr(g_other, "target_rc2", 102);

  // Read & modify shared attributes in both no_corder and corder groups
  // (exercises H5A__dense_write_bt2_cb shared path + H5A__dense_write_bt2_cb2).
  {
    std::array<int64_t, kVecLen> new_vals{};
    new_vals.fill(7777);
    ScopedAttr a1(H5Aopen(g_no_corder, "sh_0", H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ASSERT_GE(H5Awrite(a1, H5T_NATIVE_INT64, new_vals.data()), 0);

    ScopedAttr a2(H5Aopen(g_corder, "sh_0", H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ASSERT_GE(H5Awrite(a2, H5T_NATIVE_INT64, new_vals.data()), 0);
  }
  {
    std::array<int64_t, kVecLen> read_vals{};
    ScopedAttr ra(H5Aopen(g_corder, "sh_0", H5P_DEFAULT));
    ASSERT_TRUE(ra.valid());
    ASSERT_GE(H5Aread(ra, H5T_NATIVE_INT64, read_vals.data()), 0);
    EXPECT_EQ(read_vals[0], 7777);
  }

  // Rename shared attribute with attr_rc == 1 and with attr_rc == 2.
  ASSERT_GE(H5Arename(g_no_corder, "sh_1", "sh_1_unique_rc1"), 0);
  ASSERT_GE(H5Arename(g_no_corder, "sh_2", "target_rc2"), 0);
  EXPECT_GT(H5Aexists(g_no_corder, "sh_1_unique_rc1"), 0);
  EXPECT_GT(H5Aexists(g_no_corder, "target_rc2"), 0);

  // Iterate over mixed non-shared + shared attributes in NATIVE order.
  EXPECT_FALSE(
      CollectAttrNames(g_no_corder, H5_INDEX_NAME, H5_ITER_NATIVE).empty());
  EXPECT_FALSE(
      CollectAttrNames(g_corder, H5_INDEX_CRT_ORDER, H5_ITER_NATIVE).empty());

  // Delete shared attributes by name and by index:
  // On `g_no_corder` with H5_INDEX_NAME + H5_ITER_NATIVE, other_bt2_addr is
  // HADDR_UNDEF so H5A__dense_remove_by_idx_bt2_cb takes `use_sh_loc = true`!
  // On `g_corder` with H5_INDEX_NAME + H5_ITER_NATIVE, other_bt2_addr is
  // defined so it copies the attr and uses `&fh_udata.attr->sh_loc`.
  ASSERT_GE(H5Adelete(g_no_corder, "target_rc2"), 0);
  ASSERT_GE(H5Adelete(g_corder, "sh_1"), 0);

  // Remove shared attributes by index in no_corder until a shared record is
  // deleted via use_sh_loc == true.
  while (
      !CollectAttrNames(g_no_corder, H5_INDEX_NAME, H5_ITER_NATIVE).empty()) {
    ASSERT_GE(H5Adelete_by_idx(g_no_corder, ".", H5_INDEX_NAME, H5_ITER_NATIVE,
                               0, H5P_DEFAULT),
              0);
  }
  ASSERT_GE(H5Adelete_by_idx(g_corder, ".", H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             H5P_DEFAULT),
            0);

  // Unlink g_wipe containing both shared and non-shared dense attributes to
  // cover both branches of H5A__dense_delete_bt2_cb.
  g_wipe.reset();
  ASSERT_GE(H5Ldelete(file, "wipe", H5P_DEFAULT), 0);
}

TEST(H5AdenseTest, MultiLevelBtreeChurnAndInternalDensePaths) {
  ScopedFile file = CreateInMemoryFile("dense_btree_churn.h5");
  ASSERT_TRUE(file.valid());

  // Always dense + creation-order indexed so >29 records force multi-level v2
  // B-trees in both name and corder indices.
  ScopedPlist gcpl = MakeDenseGroupPlist(0, 0, /*index_corder=*/true);
  ASSERT_TRUE(gcpl.valid());
  ScopedGroup grp(H5Gcreate2(file, "many", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr int kCount = 120;
  for (int32_t i = 0; i < kCount; ++i) {
    const std::string name = std::format("m_{:04d}", i);
    CreateIntAttr(grp, name.c_str(), i);
  }

  // Delete middle, odd, then reverse attributes to induce v2 B-tree merges
  // and redistributions across multi-level internal/leaf nodes.
  for (int32_t i = kCount / 3; i < (2 * kCount) / 3; ++i) {
    const std::string name = std::format("m_{:04d}", i);
    ASSERT_GE(H5Adelete(grp, name.c_str()), 0);
  }
  for (int32_t i = kCount - 1; i >= (2 * kCount) / 3; i -= 2) {
    const std::string name = std::format("m_{:04d}", i);
    ASSERT_GE(H5Adelete(grp, name.c_str()), 0);
  }
  EXPECT_EQ(ReadIntAttr(grp, "m_0000"), 0);

  // Exercise internal H5A__dense_iterate with deprecated H5A_ATTR_OP_APP in
  // H5_INDEX_NAME + H5_ITER_NATIVE order (reachable only via internal call).
  H5F_t* f = nullptr;
  H5O_ainfo_t ainfo{};
  ASSERT_TRUE(GetObjectAinfo(grp, &f, &ainfo));

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    struct AppState {
      int calls = 0;
      herr_t ret = 0;
    } state;
    H5A_attr_iter_op_t op{};
    op.op_type = H5A_ATTR_OP_APP;
    op.u.app_op = [](hid_t, const char*, void* data) -> herr_t {
      auto* st = static_cast<AppState*>(data);
      ++st->calls;
      return st->calls >= 3 ? st->ret : 0;
    };

    state.ret = 11;
    hsize_t last_attr = 0;
    EXPECT_EQ(H5A__dense_iterate(f, grp, &ainfo, H5_INDEX_NAME, H5_ITER_NATIVE,
                                 /*skip=*/1, &last_attr, &op, &state),
              11);
    EXPECT_EQ(state.calls, 3);
    EXPECT_EQ(last_attr, 4u);

    // Direct negative internal checks on valid ainfo (missing attr, bad remove)
    // and on a stack-local corrupted copy of ainfo to cover failure cleanup.
    ScopedErrorSilence silence;
    bool exists = true;
    EXPECT_GE(H5A__dense_exists(f, &ainfo, "does_not_exist", &exists), 0);
    EXPECT_FALSE(exists);
    EXPECT_EQ(H5A__dense_open(f, &ainfo, "does_not_exist"), nullptr);
    EXPECT_LT(H5A__dense_rename(f, &ainfo, "does_not_exist", "x"), 0);
    EXPECT_LT(H5A__dense_remove(f, &ainfo, "does_not_exist"), 0);

    H5A_t* raw_attr = H5A__dense_open(f, &ainfo, "m_0000");
    ASSERT_NE(raw_attr, nullptr);
    absl::Cleanup close_raw_attr = [&] { H5A__close(raw_attr); };

    H5O_ainfo_t bad_fheap = ainfo;
    bad_fheap.fheap_addr = 0x12345678ULL;
    EXPECT_EQ(H5A__dense_open(f, &bad_fheap, "m_0000"), nullptr);
    EXPECT_LT(H5A__dense_write(f, &bad_fheap, raw_attr), 0);
  }
}

TEST(H5AdenseTest, CorruptedAinfoDupFsmAndVersionErrors) {
  // 1. H5A__dense_create free-space manager allocation failures (SUPER vs
  // BTREE)
  //    on a fresh core file whose FSM pointers are still null/UNDEF.
  //    Under Core VFD (H5FD_FLMAP_DICHOTOMY), metadata maps to H5FD_MEM_SUPER,
  //    so corrupting SUPER fails H5HF_create (192); temporarily remapping
  //    H5FD_MEM_BTREE -> H5F_MEM_PAGE_BTREE fails H5B2_create (220).
  {
    ScopedFile fsm_file = CreateInMemoryFile("dense_fsm_fail.h5");
    ASSERT_TRUE(fsm_file.valid());
    H5F_t* fsm_f = static_cast<H5F_t*>(H5VL_object_verify(fsm_file, H5I_FILE));
    ASSERT_NE(fsm_f, nullptr);
    ASSERT_EQ(fsm_f->shared->fs_man[H5F_MEM_PAGE_SUPER], nullptr);
    ASSERT_EQ(fsm_f->shared->fs_man[H5F_MEM_PAGE_BTREE], nullptr);
    ASSERT_EQ(fsm_f->shared->fs_addr[H5F_MEM_PAGE_SUPER], HADDR_UNDEF);
    ASSERT_EQ(fsm_f->shared->fs_addr[H5F_MEM_PAGE_BTREE], HADDR_UNDEF);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    H5O_ainfo_t new_ainfo{};
    new_ainfo.fheap_addr = HADDR_UNDEF;
    new_ainfo.name_bt2_addr = HADDR_UNDEF;
    new_ainfo.corder_bt2_addr = HADDR_UNDEF;

    {
      fsm_f->shared->fs_addr[H5F_MEM_PAGE_SUPER] = 0x12345678ULL;
      absl::Cleanup restore_super = [&] {
        fsm_f->shared->fs_addr[H5F_MEM_PAGE_SUPER] = HADDR_UNDEF;
      };
      EXPECT_LT(H5A__dense_create(fsm_f, &new_ainfo), 0);
    }
    {
      const H5FD_mem_t orig_btree_map =
          fsm_f->shared->fs_type_map[H5FD_MEM_BTREE];
      fsm_f->shared->fs_type_map[H5FD_MEM_BTREE] = H5FD_MEM_BTREE;
      fsm_f->shared->fs_addr[H5F_MEM_PAGE_BTREE] = 0x12345678ULL;
      absl::Cleanup restore_btree = [&] {
        fsm_f->shared->fs_addr[H5F_MEM_PAGE_BTREE] = HADDR_UNDEF;
        fsm_f->shared->fs_type_map[H5FD_MEM_BTREE] = orig_btree_map;
      };
      EXPECT_LT(H5A__dense_create(fsm_f, &new_ainfo), 0);
    }
  }

  // 2. Non-SOHM dense error paths across bad fheap/name_bt2/corder_bt2,
  //    mismatched corder_bt2, duplicate name/crt_idx, encode failure, UTF-8
  //    version-bounds failure, table iterate negative return, and bad
  //    sohm_addr.
  ScopedFile file = CreateInMemoryFile("dense_ainfo_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGroupPlist(0, 0, /*index_corder=*/true);
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup gc(H5Gcreate2(file, "gc", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ScopedGroup gc_del(
      H5Gcreate2(file, "gc_del", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ScopedGroup g_only0(
      H5Gcreate2(file, "g_only0", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ScopedGroup g_no0(H5Gcreate2(file, "g_no0", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(gc.valid());
  ASSERT_TRUE(gc_del.valid());
  ASSERT_TRUE(g_only0.valid());
  ASSERT_TRUE(g_no0.valid());

  CreateIntAttr(gc, "a0", 10);
  CreateIntAttr(gc, "a1", 20);
  {
    ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
    ASSERT_TRUE(acpl.valid());
    ASSERT_GE(H5Pset_char_encoding(acpl, H5T_CSET_UTF8), 0);
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedAttr u8(
        H5Acreate2(gc, "u8", H5T_NATIVE_INT32, scalar, acpl, H5P_DEFAULT));
    ASSERT_TRUE(u8.valid());
    int32_t v = 99;
    ASSERT_GE(H5Awrite(u8, H5T_NATIVE_INT32, &v), 0);
  }
  CreateIntAttr(gc_del, "d0", 1);
  CreateIntAttr(g_only0, "only0", 0);
  CreateIntAttr(g_no0, "x0", 0);
  CreateIntAttr(g_no0, "x1", 1);
  ASSERT_GE(H5Adelete(g_no0, "x0"), 0);

  H5F_t* f = nullptr;
  H5O_ainfo_t ainfo{};
  H5O_ainfo_t ainfo_del{};
  H5O_ainfo_t ainfo_only0{};
  H5O_ainfo_t ainfo_no0{};
  ASSERT_TRUE(GetObjectAinfo(gc, &f, &ainfo));
  ASSERT_TRUE(GetObjectAinfo(gc_del, &f, &ainfo_del));
  ASSERT_TRUE(GetObjectAinfo(g_only0, &f, &ainfo_only0));
  ASSERT_TRUE(GetObjectAinfo(g_no0, &f, &ainfo_no0));

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  H5A_t* raw_a0 = H5A__dense_open(f, &ainfo, "a0");
  ASSERT_NE(raw_a0, nullptr);
  absl::Cleanup close_a0 = [&] { H5A__close(raw_a0); };

  H5A_t* raw_a1 = H5A__dense_open(f, &ainfo, "a1");
  ASSERT_NE(raw_a1, nullptr);
  absl::Cleanup close_a1 = [&] { H5A__close(raw_a1); };

  H5A_t* raw_only0 = H5A__dense_open(f, &ainfo_only0, "only0");
  ASSERT_NE(raw_only0, nullptr);
  absl::Cleanup close_only0 = [&] { H5A__close(raw_only0); };

  H5A_attr_iter_op_t app_op{};
  app_op.op_type = H5A_ATTR_OP_APP2;
  app_op.u.app_op2 = [](hid_t, const char*, const H5A_info_t*,
                        void*) -> herr_t { return -1; };
  hsize_t last_attr = 0;

  // Table-based iterate returning negative user callback status (line 1221).
  EXPECT_LT(H5A__dense_iterate(f, gc, &ainfo, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_attr, &app_op, nullptr),
            0);

  // Version bounds failure in H5A__dense_rename (line 915).
  {
    const H5F_libver_t orig_hb = f->shared->high_bound;
    f->shared->high_bound = H5F_LIBVER_EARLIEST;
    absl::Cleanup restore_hb = [&] { f->shared->high_bound = orig_hb; };
    EXPECT_LT(H5A__dense_rename(f, &ainfo, "u8", "u8_new"), 0);
  }

  // Datatype encoding failure in write (662) and insert (494).
  {
    ASSERT_EQ(raw_a0->shared->dt->shared->type, H5T_INTEGER);
    const H5T_order_t orig_order = raw_a0->shared->dt->shared->u.atomic.order;
    raw_a0->shared->dt->shared->u.atomic.order = H5T_ORDER_NONE;
    absl::Cleanup restore_order = [&] {
      raw_a0->shared->dt->shared->u.atomic.order = orig_order;
    };
    EXPECT_LT(H5A__dense_write(f, &ainfo, raw_a0), 0);

    char* orig_name = raw_a0->shared->name;
    raw_a0->shared->name = H5MM_xstrdup("bad_enc_attr");
    absl::Cleanup restore_name = [&] {
      H5MM_xfree(raw_a0->shared->name);
      raw_a0->shared->name = orig_name;
    };
    EXPECT_LT(H5A__dense_insert(f, &ainfo, raw_a0), 0);
  }

  // Duplicate name in rename (949) and insert (520), plus duplicate crt_idx in
  // corder B-tree (531).
  EXPECT_LT(H5A__dense_rename(f, &ainfo, "a0", "a1"), 0);
  EXPECT_LT(H5A__dense_insert(f, &ainfo, raw_a1), 0);
  {
    char* orig_only0_name = raw_only0->shared->name;
    raw_only0->shared->name = H5MM_xstrdup("dup_crt_idx_0");
    absl::Cleanup restore_only0_name = [&] {
      H5MM_xfree(raw_only0->shared->name);
      raw_only0->shared->name = orig_only0_name;
    };
    EXPECT_LT(H5A__dense_insert(f, &ainfo_only0, raw_only0), 0);
  }

  // Corrupt fheap_addr (466, 875, 1168, 1327, 1560, 1656, 1793).
  // (Non-native H5_ITER_INC error paths for 1217 and 1601 are tested under
  // bad_name below because H5A__dense_build_table leaks its attrs array if
  // H5B2_open succeeds and iteration subsequently fails.)
  bool exists = false;
  H5O_ainfo_t bad_fheap = ainfo;
  bad_fheap.fheap_addr = 0x12345678ULL;
  EXPECT_LT(H5A__dense_insert(f, &bad_fheap, raw_a0), 0);
  EXPECT_LT(H5A__dense_rename(f, &bad_fheap, "a0", "a0_r"), 0);
  EXPECT_LT(H5A__dense_iterate(f, gc, &bad_fheap, H5_INDEX_NAME, H5_ITER_NATIVE,
                               0, &last_attr, &app_op, nullptr),
            0);
  EXPECT_LT(H5A__dense_remove(f, &bad_fheap, "a0"), 0);
  EXPECT_LT(
      H5A__dense_remove_by_idx(f, &bad_fheap, H5_INDEX_NAME, H5_ITER_NATIVE, 0),
      0);
  EXPECT_LT(H5A__dense_exists(f, &bad_fheap, "a0", &exists), 0);
  EXPECT_LT(H5A__dense_delete(f, &bad_fheap), 0);

  // Corrupt name_bt2_addr (366, 504, 745, 879, 1192, 1217, 1351, 1584, 1601,
  // 1680, 1807).
  H5O_ainfo_t bad_name = ainfo;
  bad_name.name_bt2_addr = 0x12345678ULL;
  EXPECT_EQ(H5A__dense_open(f, &bad_name, "a0"), nullptr);
  {
    char* orig_name = raw_a0->shared->name;
    raw_a0->shared->name = H5MM_xstrdup("bad_name_bt2_ins");
    absl::Cleanup restore_name = [&] {
      H5MM_xfree(raw_a0->shared->name);
      raw_a0->shared->name = orig_name;
    };
    EXPECT_LT(H5A__dense_insert(f, &bad_name, raw_a0), 0);
  }
  EXPECT_LT(H5A__dense_write(f, &bad_name, raw_a0), 0);
  EXPECT_LT(H5A__dense_rename(f, &bad_name, "a0", "a0_r"), 0);
  EXPECT_LT(H5A__dense_iterate(f, gc, &bad_name, H5_INDEX_NAME, H5_ITER_NATIVE,
                               0, &last_attr, &app_op, nullptr),
            0);
  EXPECT_LT(H5A__dense_iterate(f, gc, &bad_name, H5_INDEX_NAME, H5_ITER_INC, 0,
                               &last_attr, &app_op, nullptr),
            0);
  EXPECT_LT(H5A__dense_remove(f, &bad_name, "a0"), 0);
  EXPECT_LT(
      H5A__dense_remove_by_idx(f, &bad_name, H5_INDEX_NAME, H5_ITER_NATIVE, 0),
      0);
  EXPECT_LT(
      H5A__dense_remove_by_idx(f, &bad_name, H5_INDEX_NAME, H5_ITER_INC, 0), 0);
  EXPECT_LT(H5A__dense_exists(f, &bad_name, "a0", &exists), 0);
  EXPECT_LT(H5A__dense_delete(f, &bad_name), 0);

  // Mismatched valid corder_bt2_addr missing crt_idx=1 (1269, 1464).
  H5O_ainfo_t mixed_corder = ainfo_no0;
  mixed_corder.corder_bt2_addr = ainfo_only0.corder_bt2_addr;
  EXPECT_LT(H5A__dense_remove(f, &mixed_corder, "x1"), 0);
  EXPECT_LT(H5A__dense_remove_by_idx(f, &mixed_corder, H5_INDEX_NAME,
                                     H5_ITER_NATIVE, 0),
            0);

  // Corrupt corder_bt2_addr (527, 924, 1262, 1458, 1595, 1609, 1819).
  H5O_ainfo_t bad_corder = ainfo;
  bad_corder.corder_bt2_addr = 0x12345678ULL;
  EXPECT_LT(H5A__dense_rename(f, &bad_corder, "a0", "a0_r"), 0);
  EXPECT_LT(H5A__dense_remove(f, &bad_corder, "a0"), 0);
  EXPECT_LT(H5A__dense_remove_by_idx(f, &bad_corder, H5_INDEX_NAME,
                                     H5_ITER_NATIVE, 0),
            0);
  EXPECT_LT(
      H5A__dense_remove_by_idx(f, &bad_corder, H5_INDEX_NAME, H5_ITER_INC, 0),
      0);
  {
    char* orig_name = raw_a0->shared->name;
    raw_a0->shared->name = H5MM_xstrdup("bad_corder_ins");
    absl::Cleanup restore_name = [&] {
      H5MM_xfree(raw_a0->shared->name);
      raw_a0->shared->name = orig_name;
    };
    EXPECT_LT(H5A__dense_insert(f, &bad_corder, raw_a0), 0);
  }
  H5O_ainfo_t bad_corder_del = ainfo_del;
  bad_corder_del.corder_bt2_addr = 0x12345678ULL;
  EXPECT_LT(H5A__dense_delete(f, &bad_corder_del), 0);

  // Bogus f->shared->sohm_addr triggering H5SM_type_shared failure
  // (346, 430, 721, 855, 1172, 1331, 1564, 1660).
  {
    f->shared->sohm_addr = 0x12345678ULL;
    absl::Cleanup restore_sohm = [&] { f->shared->sohm_addr = HADDR_UNDEF; };
    EXPECT_EQ(H5A__dense_open(f, &ainfo_no0, "x1"), nullptr);
    EXPECT_LT(H5A__dense_insert(f, &ainfo_no0, raw_a0), 0);
    EXPECT_LT(H5A__dense_write(f, &ainfo_no0, raw_a0), 0);
    EXPECT_LT(H5A__dense_rename(f, &ainfo_no0, "x1", "x1_r"), 0);
    EXPECT_LT(
        H5A__dense_iterate(f, g_no0, &ainfo_no0, H5_INDEX_NAME, H5_ITER_NATIVE,
                           0, &last_attr, &app_op, nullptr),
        0);
    EXPECT_LT(H5A__dense_remove(f, &ainfo_no0, "x1"), 0);
    EXPECT_LT(H5A__dense_remove_by_idx(f, &ainfo_no0, H5_INDEX_NAME,
                                       H5_ITER_NATIVE, 0),
              0);
    EXPECT_LT(H5A__dense_exists(f, &ainfo_no0, "x1", &exists), 0);
  }
}

TEST(H5AdenseTest, SohmSharedFailurePathsAndTwoIndexRename) {
  // Part A: Single-index SOHM for attributes (H5O_SHMESG_ATTR_FLAG, min 48B).
  {
    ScopedPlist fcpl = MakeSohmFcpl(/*min_mesg_size=*/48, /*max_list=*/2, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("dense_sohm_err_attr.h5", H5F_ACC_TRUNC, fcpl);
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl_nc = MakeDenseGroupPlist(0, 0, /*index_corder=*/false);
    ScopedPlist gcpl_c = MakeDenseGroupPlist(0, 0, /*index_corder=*/true);
    ASSERT_TRUE(gcpl_nc.valid());
    ASSERT_TRUE(gcpl_c.valid());

    ScopedGroup g_c(H5Gcreate2(file, "g_c", H5P_DEFAULT, gcpl_c, H5P_DEFAULT));
    ScopedGroup g_nc(
        H5Gcreate2(file, "g_nc", H5P_DEFAULT, gcpl_nc, H5P_DEFAULT));
    ScopedGroup g_rm(
        H5Gcreate2(file, "g_rm", H5P_DEFAULT, gcpl_nc, H5P_DEFAULT));
    ScopedGroup g_wipe(
        H5Gcreate2(file, "g_wipe", H5P_DEFAULT, gcpl_nc, H5P_DEFAULT));
    ScopedGroup g_only0(
        H5Gcreate2(file, "g_only0", H5P_DEFAULT, gcpl_c, H5P_DEFAULT));
    ScopedGroup g_no0(
        H5Gcreate2(file, "g_no0", H5P_DEFAULT, gcpl_c, H5P_DEFAULT));
    ASSERT_TRUE(g_c.valid());
    ASSERT_TRUE(g_nc.valid());
    ASSERT_TRUE(g_rm.valid());
    ASSERT_TRUE(g_wipe.valid());
    ASSERT_TRUE(g_only0.valid());
    ASSERT_TRUE(g_no0.valid());

    constexpr hsize_t kVecLen = 16;
    ScopedSpace vec_space(H5Screate_simple(1, &kVecLen, nullptr));
    ASSERT_TRUE(vec_space.valid());
    auto make_sh = [&](hid_t loc, const char* name, int64_t seed) {
      std::array<int64_t, kVecLen> vals{};
      vals.fill(seed);
      ScopedAttr a(H5Acreate2(loc, name, H5T_NATIVE_INT64, vec_space,
                              H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid()) << name;
      ASSERT_GE(H5Awrite(a, H5T_NATIVE_INT64, vals.data()), 0);
    };

    make_sh(g_c, "sc0", 1);
    make_sh(g_nc, "snc0", 2);
    make_sh(g_rm, "srm0", 3);
    make_sh(g_wipe, "sw0", 4);
    make_sh(g_only0, "sonly0", 5);
    make_sh(g_no0, "sno0", 6);
    make_sh(g_no0, "sno1", 7);
    ASSERT_GE(H5Adelete(g_no0, "sno0"), 0);
    CreateIntAttr(g_nc, "small_unshared", 123);

    H5F_t* f = nullptr;
    H5O_ainfo_t a_c{}, a_nc{}, a_rm{}, a_wipe{}, a_only0{}, a_no0{};
    ASSERT_TRUE(GetObjectAinfo(g_c, &f, &a_c));
    ASSERT_TRUE(GetObjectAinfo(g_nc, &f, &a_nc));
    ASSERT_TRUE(GetObjectAinfo(g_rm, &f, &a_rm));
    ASSERT_TRUE(GetObjectAinfo(g_wipe, &f, &a_wipe));
    ASSERT_TRUE(GetObjectAinfo(g_only0, &f, &a_only0));
    ASSERT_TRUE(GetObjectAinfo(g_no0, &f, &a_no0));

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    H5A_t* raw_sc0 = H5A__dense_open(f, &a_c, "sc0");
    ASSERT_NE(raw_sc0, nullptr);
    absl::Cleanup close_sc0 = [&] { H5A__close(raw_sc0); };

    H5A_t* raw_sno1 = H5A__dense_open(f, &a_no0, "sno1");
    ASSERT_NE(raw_sno1, nullptr);
    absl::Cleanup close_sno1 = [&] { H5A__close(raw_sno1); };

    H5A_t* raw_unshared_big = H5A__dense_open(f, &a_nc, "snc0");
    ASSERT_NE(raw_unshared_big, nullptr);
    absl::Cleanup close_unshared = [&] { H5A__close(raw_unshared_big); };
    raw_unshared_big->sh_loc.type = H5O_SHARE_TYPE_UNSHARED;

    // 1. Write shared attr with bad corder_bt2 (621, 767) and mismatched
    //    corder_bt2 missing crt_idx=1 (638).
    H5O_ainfo_t bad_c = a_c;
    bad_c.corder_bt2_addr = 0x12345678ULL;
    EXPECT_LT(H5A__dense_write(f, &bad_c, raw_sc0), 0);

    H5O_ainfo_t mixed_c = a_no0;
    mixed_c.corder_bt2_addr = a_only0.corder_bt2_addr;
    EXPECT_LT(H5A__dense_write(f, &mixed_c, raw_sno1), 0);

    SohmIndexSnapshot orig_idx0{};
    ASSERT_TRUE(GetSohmIndexAddrs(f, 0, &orig_idx0));
    ASSERT_NE(orig_idx0.heap_addr, HADDR_UNDEF);
    ASSERT_NE(orig_idx0.index_addr, HADDR_UNDEF);

    // 2. Corrupt SOHM heap_addr with valid index_addr: covers 446, 460, 360,
    //    735, 869, 1186, 1345, 1578, 1674.
    {
      SohmIndexSnapshot bad_heap = orig_idx0;
      bad_heap.heap_addr = 0x12345678ULL;
      ASSERT_TRUE(SetSohmIndexAddrs(f, 0, bad_heap));
      absl::Cleanup restore_idx0 = [&] { SetSohmIndexAddrs(f, 0, orig_idx0); };

      H5A_attr_iter_op_t app_op{};
      app_op.op_type = H5A_ATTR_OP_APP2;
      app_op.u.app_op2 = [](hid_t, const char*, const H5A_info_t*,
                            void*) -> herr_t { return 0; };
      hsize_t last_attr = 0;
      bool exists = false;

      EXPECT_LT(H5A__dense_insert(f, &a_nc, raw_unshared_big), 0);
      EXPECT_LT(H5A__dense_insert(f, &a_nc, raw_sc0), 0);
      EXPECT_EQ(H5A__dense_open(f, &a_nc, "small_unshared"), nullptr);
      const H5O_shared_t orig_sc0_sh = raw_sc0->sh_loc;
      absl::Cleanup restore_sc0_sh = [&] { raw_sc0->sh_loc = orig_sc0_sh; };
      EXPECT_LT(H5A__dense_write(f, &a_c, raw_sc0), 0);
      EXPECT_LT(H5A__dense_rename(f, &a_nc, "small_unshared", "s_ren"), 0);
      EXPECT_LT(
          H5A__dense_iterate(f, g_nc, &a_nc, H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                             &last_attr, &app_op, nullptr),
          0);
      EXPECT_LT(H5A__dense_remove(f, &a_nc, "small_unshared"), 0);
      EXPECT_LT(
          H5A__dense_remove_by_idx(f, &a_nc, H5_INDEX_NAME, H5_ITER_NATIVE, 0),
          0);
      EXPECT_LT(H5A__dense_exists(f, &a_nc, "small_unshared", &exists), 0);
    }

    // 3. Valid heap_addr + corrupt SOHM index_addr: covers
    // H5O__attr_update_shared
    //    (610) and H5SM_delete failures (1277, 1480, 1738).
    {
      SohmIndexSnapshot bad_index = orig_idx0;
      bad_index.index_addr = 0x12345678ULL;
      ASSERT_TRUE(SetSohmIndexAddrs(f, 0, bad_index));
      absl::Cleanup restore_idx0 = [&] { SetSohmIndexAddrs(f, 0, orig_idx0); };

      const H5O_shared_t orig_sc0_sh = raw_sc0->sh_loc;
      absl::Cleanup restore_sc0_sh = [&] { raw_sc0->sh_loc = orig_sc0_sh; };
      EXPECT_LT(H5A__dense_write(f, &a_c, raw_sc0), 0);
      EXPECT_LT(H5A__dense_remove(f, &a_rm, "srm0"), 0);
      EXPECT_LT(
          H5A__dense_remove_by_idx(f, &a_rm, H5_INDEX_NAME, H5_ITER_NATIVE, 0),
          0);
      EXPECT_LT(H5A__dense_delete(f, &a_wipe), 0);
    }
  }

  // Part B: Single-index SOHM for datatypes only (H5O_SHMESG_DTYPE_FLAG):
  // H5SM_type_shared(ATTR) is FALSE, attr->dt is SOHM -> corrupt dtype
  // index_addr while keeping heap_addr valid covers H5O__attr_link (976) and
  // H5O__attr_delete (1283, 1486, 1758).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl, 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl, 0, H5O_SHMESG_DTYPE_FLAG, 16), 0);
    ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl, 10, 5), 0);
    ScopedFile file =
        CreateInMemoryFile("dense_sohm_err_dtype.h5", H5F_ACC_TRUNC, fcpl);
    ASSERT_TRUE(file.valid());

    ScopedType ctype = CreateLargeCompoundType();
    ASSERT_TRUE(ctype.valid());
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    std::array<int64_t, 4> payload{1, 2, 3, 4};

    ScopedPlist gcpl = MakeDenseGroupPlist(0, 0, /*index_corder=*/false);
    ASSERT_TRUE(gcpl.valid());
    ScopedGroup g_ren(
        H5Gcreate2(file, "g_ren", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ScopedGroup g_rm(H5Gcreate2(file, "g_rm", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ScopedGroup g_del(
        H5Gcreate2(file, "g_del", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(g_ren.valid());
    ASSERT_TRUE(g_rm.valid());
    ASSERT_TRUE(g_del.valid());

    auto create_cmp_attr = [&](hid_t loc, const char* name) {
      ScopedAttr a(
          H5Acreate2(loc, name, ctype, scalar, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid()) << name;
      ASSERT_GE(H5Awrite(a, ctype, payload.data()), 0);
    };
    create_cmp_attr(g_ren, "dt_ren");
    create_cmp_attr(g_rm, "dt_rm");
    create_cmp_attr(g_del, "dt_del");

    H5F_t* f = nullptr;
    H5O_ainfo_t a_ren{}, a_rm{}, a_del{};
    ASSERT_TRUE(GetObjectAinfo(g_ren, &f, &a_ren));
    ASSERT_TRUE(GetObjectAinfo(g_rm, &f, &a_rm));
    ASSERT_TRUE(GetObjectAinfo(g_del, &f, &a_del));

    SohmIndexSnapshot orig_dt{};
    ASSERT_TRUE(GetSohmIndexAddrs(f, 0, &orig_dt));
    ASSERT_NE(orig_dt.heap_addr, HADDR_UNDEF);
    ASSERT_NE(orig_dt.index_addr, HADDR_UNDEF);

    SohmIndexSnapshot bad_dt_idx = orig_dt;
    bad_dt_idx.index_addr = 0x12345678ULL;
    ASSERT_TRUE(SetSohmIndexAddrs(f, 0, bad_dt_idx));
    absl::Cleanup restore_dt = [&] { SetSohmIndexAddrs(f, 0, orig_dt); };

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;

    EXPECT_LT(H5A__dense_rename(f, &a_ren, "dt_ren", "dt_ren_new"), 0);
    EXPECT_LT(H5A__dense_remove(f, &a_rm, "dt_rm"), 0);
    EXPECT_LT(
        H5A__dense_remove_by_idx(f, &a_rm, H5_INDEX_NAME, H5_ITER_NATIVE, 0),
        0);
    EXPECT_LT(H5A__dense_delete(f, &a_del), 0);
  }

  // Part C: Two-index SOHM (0 = ATTR min 48B, 1 = DTYPE min 16B).
  // Corrupting index[1].index_addr causes rename with attr_rc == 1 to fail in
  // H5O__attr_link (line 969) after H5SM_get_refcount(ATTR) succeeds!
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl, 2), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl, 0, H5O_SHMESG_ATTR_FLAG, 48), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl, 1, H5O_SHMESG_DTYPE_FLAG, 16), 0);
    ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl, 10, 5), 0);
    ScopedFile file =
        CreateInMemoryFile("dense_sohm_err_2idx.h5", H5F_ACC_TRUNC, fcpl);
    ASSERT_TRUE(file.valid());

    ScopedType ctype = CreateLargeCompoundType();
    ASSERT_TRUE(ctype.valid());
    constexpr hsize_t kCount = 4;
    ScopedSpace arr_space(H5Screate_simple(1, &kCount, nullptr));
    ASSERT_TRUE(arr_space.valid());
    std::array<int64_t, 4 * 4> vals{};
    vals.fill(42);

    ScopedPlist gcpl = MakeDenseGroupPlist(0, 0, /*index_corder=*/false);
    ASSERT_TRUE(gcpl.valid());
    ScopedGroup grp(H5Gcreate2(file, "g2", H5P_DEFAULT, gcpl, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    // Write >= 2 attrs with ctype so dt enters Index 1 heap while attr enters
    // Index 0 heap (with attr_rc == 1 for "c_ren").
    for (const char* name : {"c_ref", "c_ren"}) {
      ScopedAttr a(
          H5Acreate2(grp, name, ctype, arr_space, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      ASSERT_GE(H5Awrite(a, ctype, vals.data()), 0);
      vals[0] += 1;
    }

    H5F_t* f = nullptr;
    H5O_ainfo_t ainfo{};
    ASSERT_TRUE(GetObjectAinfo(grp, &f, &ainfo));

    SohmIndexSnapshot orig_idx1{};
    ASSERT_TRUE(GetSohmIndexAddrs(f, 1, &orig_idx1));
    ASSERT_NE(orig_idx1.heap_addr, HADDR_UNDEF);
    ASSERT_NE(orig_idx1.index_addr, HADDR_UNDEF);

    SohmIndexSnapshot bad_idx1 = orig_idx1;
    bad_idx1.index_addr = 0x12345678ULL;
    ASSERT_TRUE(SetSohmIndexAddrs(f, 1, bad_idx1));
    absl::Cleanup restore_idx1 = [&] { SetSohmIndexAddrs(f, 1, orig_idx1); };

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5A__dense_rename(f, &ainfo, "c_ren", "c_ren_2"), 0);
  }
}

TEST(H5AdenseTest, CorruptedHeapObjectDecodeFailures) {
  ScopedFile file = CreateInMemoryFile("dense_bad_fheap_obj.h5");
  ASSERT_TRUE(file.valid());
  ScopedPlist gcpl = MakeDenseGroupPlist(0, 0, /*index_corder=*/false);
  ASSERT_TRUE(gcpl.valid());

  ScopedGroup grp(H5Gcreate2(file, "g", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  CreateIntAttr(grp, "corrupt_me", 7);

  H5F_t* f = nullptr;
  H5O_ainfo_t ainfo{};
  ASSERT_TRUE(GetObjectAinfo(grp, &f, &ainfo));

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Insert invalid attribute version byte (0xff > H5O_ATTR_VERSION_LATEST) into
  // the group's fractal heap and repoint the name B-tree record's heap ID.
  {
    H5HF_t* fheap = H5HF_open(f, ainfo.fheap_addr);
    ASSERT_NE(fheap, nullptr);
    absl::Cleanup close_fheap = [&] { H5HF_close(fheap); };

    H5B2_t* bt2_name = H5B2_open(f, ainfo.name_bt2_addr, nullptr);
    ASSERT_NE(bt2_name, nullptr);
    absl::Cleanup close_bt2 = [&] { H5B2_close(bt2_name); };

    const std::array<uint8_t, 4> bad_obj{0xff, 0x00, 0x00, 0x00};
    H5O_fheap_id_t bad_id{};
    ASSERT_GE(H5HF_insert(fheap, bad_obj.size(), bad_obj.data(), &bad_id), 0);

    H5A_bt2_ud_common_t udata{};
    udata.f = f;
    udata.fheap = fheap;
    udata.shared_fheap = nullptr;
    udata.name = "corrupt_me";
    udata.name_hash =
        H5_checksum_lookup3("corrupt_me", std::strlen("corrupt_me"), 0);
    auto mod_cb = [](void* raw_rec, void* op_data, bool* changed) -> herr_t {
      auto* rec = static_cast<H5A_dense_bt2_name_rec_t*>(raw_rec);
      rec->id = *static_cast<const H5O_fheap_id_t*>(op_data);
      *changed = true;
      return 0;
    };
    ASSERT_GE(H5B2_modify(bt2_name, &udata, false, mod_cb, &bad_id), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(H5A__dense_open(f, &ainfo, "corrupt_me"), nullptr);
  EXPECT_LT(H5A__dense_rename(f, &ainfo, "corrupt_me", "renamed"), 0);
  bool exists = false;
  EXPECT_LT(H5A__dense_exists(f, &ainfo, "corrupt_me", &exists), 0);

  H5A_attr_iter_op_t app_op{};
  app_op.op_type = H5A_ATTR_OP_APP2;
  app_op.u.app_op2 = [](hid_t, const char*, const H5A_info_t*,
                        void*) -> herr_t { return 0; };
  hsize_t last_attr = 0;
  EXPECT_LT(H5A__dense_iterate(f, grp, &ainfo, H5_INDEX_NAME, H5_ITER_NATIVE, 0,
                               &last_attr, &app_op, nullptr),
            0);
  EXPECT_LT(
      H5A__dense_remove_by_idx(f, &ainfo, H5_INDEX_NAME, H5_ITER_NATIVE, 0), 0);
  EXPECT_LT(H5A__dense_delete(f, &ainfo), 0);
}

}  // namespace
}  // namespace hdf5
