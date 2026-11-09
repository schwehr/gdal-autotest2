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
// Tests object copying property list class initialization, copy options,
// merge committed datatype path lists (set/get/enc/dec/del/copy/cmp/close),
// mcdt search callbacks, and H5Ocopy integration
// (third_party/hdf5/src/H5Pocpypl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pocpypl.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match its internal struct layout.
#ifndef NDEBUG
#define H5POCPYPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
H5FL_EXTERN(H5O_copy_dtype_merge_list_t);
}  // extern "C"
#ifdef H5POCPYPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5POCPYPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Frees a linked list returned by H5Pget(..., H5O_CPY_MERGE_COMM_DT_LIST_NAME,
// ...).
class ScopedMergeList {
 public:
  ScopedMergeList() = default;
  ~ScopedMergeList() { reset(); }

  ScopedMergeList(const ScopedMergeList&) = delete;
  ScopedMergeList& operator=(const ScopedMergeList&) = delete;

  void reset(H5O_copy_dtype_merge_list_t* new_head = nullptr) {
    H5O_copy_dtype_merge_list_t* curr = head_;
    while (curr != nullptr) {
      H5O_copy_dtype_merge_list_t* next = curr->next;
      H5MM_xfree(curr->path);
      H5FL_FREE(H5O_copy_dtype_merge_list_t, curr);
      curr = next;
    }
    head_ = new_head;
  }

  H5O_copy_dtype_merge_list_t** ptr() { return &head_; }
  H5O_copy_dtype_merge_list_t* get() const { return head_; }

  std::vector<std::string> ToVector() const {
    std::vector<std::string> paths;
    for (const H5O_copy_dtype_merge_list_t* curr = head_; curr != nullptr;
         curr = curr->next) {
      paths.emplace_back(curr->path ? curr->path : "");
    }
    return paths;
  }

 private:
  H5O_copy_dtype_merge_list_t* head_ = nullptr;
};

H5O_mcdt_search_ret_t DummyMcdtSearchCb(void* op_data) {
  if (op_data != nullptr) {
    auto* count = static_cast<int*>(op_data);
    ++(*count);
    return H5O_MCDT_SEARCH_STOP;
  }
  return H5O_MCDT_SEARCH_CONT;
}

TEST(H5PocpyplTest, ReinitRegistersObjectCopyClassAndDefaults) {
  // Reinitialize to execute H5P__ocpy_reg_prop() inside the test body.
  ASSERT_GE(H5close(), 0);
  absl::Cleanup ensure_open = [] { (void)H5open(); };
  ASSERT_GE(H5open(), 0);

  unsigned def_opt = 999u;
  ASSERT_GE(H5Pget_copy_object(H5P_OBJECT_COPY_DEFAULT, &def_opt), 0);
  EXPECT_EQ(def_opt, 0u);
  EXPECT_GE(H5Pget_copy_object(H5P_OBJECT_COPY_DEFAULT, nullptr), 0);

  H5O_mcdt_search_cb_t def_cb = DummyMcdtSearchCb;
  int sentinel = 0;
  void* def_ud = &sentinel;
  ASSERT_GE(H5Pget_mcdt_search_cb(H5P_OBJECT_COPY_DEFAULT, &def_cb, &def_ud),
            0);
  EXPECT_EQ(def_cb, nullptr);
  EXPECT_EQ(def_ud, nullptr);

  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());
  EXPECT_GT(H5Pisa_class(ocpy.get(), H5P_OBJECT_COPY), 0);

  unsigned opt = 999u;
  ASSERT_GE(H5Pget_copy_object(ocpy.get(), &opt), 0);
  EXPECT_EQ(opt, 0u);

  ScopedMergeList dt_list;
  ASSERT_GE(H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, dt_list.ptr()),
            0);
  EXPECT_EQ(dt_list.get(), nullptr);
}

TEST(H5PocpyplTest, SetGetAndCopyObjectFlags) {
  constexpr auto kFlagCases = std::to_array<unsigned>({
      0u,
      H5O_COPY_SHALLOW_HIERARCHY_FLAG,
      H5O_COPY_EXPAND_SOFT_LINK_FLAG,
      H5O_COPY_EXPAND_EXT_LINK_FLAG,
      H5O_COPY_EXPAND_REFERENCE_FLAG,
      H5O_COPY_WITHOUT_ATTR_FLAG,
      H5O_COPY_PRESERVE_NULL_FLAG,
      H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG,
      H5O_COPY_SHALLOW_HIERARCHY_FLAG | H5O_COPY_WITHOUT_ATTR_FLAG,
      H5O_COPY_ALL,
  });

  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());

  for (unsigned expected : kFlagCases) {
    ASSERT_GE(H5Pset_copy_object(ocpy.get(), expected), 0);

    unsigned actual = ~expected;
    ASSERT_GE(H5Pget_copy_object(ocpy.get(), &actual), 0);
    EXPECT_EQ(actual, expected);
    EXPECT_GE(H5Pget_copy_object(ocpy.get(), nullptr), 0);

    ScopedPlist copied(H5Pcopy(ocpy.get()));
    ASSERT_TRUE(copied.valid());
    unsigned copied_opt = ~expected;
    ASSERT_GE(H5Pget_copy_object(copied.get(), &copied_opt), 0);
    EXPECT_EQ(copied_opt, expected);
    EXPECT_GT(H5Pequal(ocpy.get(), copied.get()), 0);
  }
}

TEST(H5PocpyplTest, CopyObjectRejectsInvalidArgsAndRemovedProperty) {
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;

  // Unknown option bits outside H5O_COPY_ALL.
  EXPECT_LT(H5Pset_copy_object(ocpy.get(), H5O_COPY_ALL + 1u), 0);
  EXPECT_LT(H5Pset_copy_object(ocpy.get(), 0x80u), 0);
  EXPECT_LT(H5Pset_copy_object(ocpy.get(), ~0u), 0);

  // Invalid, wrong-class, and immutable default property lists.
  EXPECT_LT(H5Pset_copy_object(H5I_INVALID_HID, 0u), 0);
  EXPECT_LT(H5Pset_copy_object(H5P_DEFAULT, 0u), 0);
  EXPECT_LT(H5Pset_copy_object(H5P_OBJECT_COPY_DEFAULT, 0u), 0);
  EXPECT_LT(H5Pset_copy_object(fcpl.get(), 0u), 0);

  unsigned opt = 0u;
  EXPECT_LT(H5Pget_copy_object(H5I_INVALID_HID, &opt), 0);
  EXPECT_LT(H5Pget_copy_object(H5P_DEFAULT, &opt), 0);
  EXPECT_LT(H5Pget_copy_object(H5P_FILE_CREATE_DEFAULT, &opt), 0);
  EXPECT_LT(H5Pget_copy_object(fcpl.get(), &opt), 0);

  // Removing the property forces H5P_set / H5P_get failure branches.
  ASSERT_GE(H5Premove(ocpy.get(), H5O_CPY_OPTION_NAME), 0);
  EXPECT_LT(H5Pset_copy_object(ocpy.get(), H5O_COPY_WITHOUT_ATTR_FLAG), 0);
  EXPECT_LT(H5Pget_copy_object(ocpy.get(), &opt), 0);
}

TEST(H5PocpyplTest, MergeCommittedDtypeListSetGetCopyRemoveAndFree) {
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());

  // 1. Freeing an empty list is a safe no-op.
  ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpy.get()), 0);

  // 2. Adding paths prepends onto the linked list.
  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/first"), 0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/group/second"), 0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/third"), 0);

  // 3. H5Pget invokes H5P__ocpy_merge_comm_dt_list_get (deep copy).
  ScopedMergeList got_list;
  ASSERT_GE(H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, got_list.ptr()),
            0);
  EXPECT_THAT(got_list.ToVector(),
              ElementsAre("/third", "/group/second", "/first"));

  // 4. H5Pcopy invokes H5P__ocpy_merge_comm_dt_list_copy independently.
  ScopedPlist copied(H5Pcopy(ocpy.get()));
  ASSERT_TRUE(copied.valid());

  // 5. H5Pset on a fresh list (from class default) and on an already-modified
  //    list (invoking _set and _del on the overwritten list).
  ScopedPlist via_set(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(via_set.valid());
  H5O_copy_dtype_merge_list_t* raw_ptr = got_list.get();
  ASSERT_GE(H5Pset(via_set.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, &raw_ptr),
            0);
  H5O_copy_dtype_merge_list_t* empty_ptr = nullptr;
  ASSERT_GE(H5Pset(via_set.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, &empty_ptr),
            0);
  ASSERT_GE(H5Pset(via_set.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, &raw_ptr),
            0);

  ScopedMergeList verify_set;
  ASSERT_GE(
      H5Pget(via_set.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, verify_set.ptr()),
      0);
  EXPECT_THAT(verify_set.ToVector(),
              ElementsAre("/third", "/group/second", "/first"));

  // 6. Freeing clears paths and allows adding new ones afterward.
  ASSERT_GE(H5Pfree_merge_committed_dtype_paths(ocpy.get()), 0);
  ScopedMergeList after_free;
  ASSERT_GE(
      H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, after_free.ptr()), 0);
  EXPECT_EQ(after_free.get(), nullptr);

  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/readded"), 0);
  ScopedMergeList after_readd;
  ASSERT_GE(
      H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, after_readd.ptr()),
      0);
  EXPECT_THAT(after_readd.ToVector(), ElementsAre("/readded"));

  // Copy remains unaffected and still holds all three original paths.
  ScopedMergeList copied_list;
  ASSERT_GE(
      H5Pget(copied.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, copied_list.ptr()),
      0);
  EXPECT_THAT(copied_list.ToVector(),
              ElementsAre("/third", "/group/second", "/first"));

  // 7. H5Premove exercises H5P__ocpy_merge_comm_dt_list_del directly.
  ASSERT_GE(H5Premove(copied.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME), 0);
}

TEST(H5PocpyplTest, MergeCommittedDtypeListCompareCoversAllCases) {
  ScopedPlist empty1(H5Pcreate(H5P_OBJECT_COPY));
  ScopedPlist empty2(H5Pcreate(H5P_OBJECT_COPY));
  ScopedPlist single_a(H5Pcreate(H5P_OBJECT_COPY));
  ScopedPlist single_a2(H5Pcreate(H5P_OBJECT_COPY));
  ScopedPlist single_b(H5Pcreate(H5P_OBJECT_COPY));
  ScopedPlist two_ab(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(empty1.valid() && empty2.valid() && single_a.valid() &&
              single_a2.valid() && single_b.valid() && two_ab.valid());

  ASSERT_GE(H5Padd_merge_committed_dtype_path(single_a.get(), "/a"), 0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(single_a2.get(), "/a"), 0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(single_b.get(), "/b"), 0);

  // Remember paths are prepended: add "/b" then "/a" -> ["/a", "/b"].
  ASSERT_GE(H5Padd_merge_committed_dtype_path(two_ab.get(), "/b"), 0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(two_ab.get(), "/a"), 0);

  // Equal cases.
  EXPECT_GT(H5Pequal(empty1.get(), empty2.get()), 0);
  EXPECT_GT(H5Pequal(single_a.get(), single_a2.get()), 0);

  // Path content mismatch (strcmp != 0).
  EXPECT_EQ(H5Pequal(single_a.get(), single_b.get()), 0);

  // Different lengths (dt_list1 != nullptr and dt_list2 != nullptr).
  EXPECT_EQ(H5Pequal(two_ab.get(), single_a.get()), 0);
  EXPECT_EQ(H5Pequal(single_a.get(), two_ab.get()), 0);
  EXPECT_EQ(H5Pequal(single_a.get(), empty1.get()), 0);
  EXPECT_EQ(H5Pequal(empty1.get(), single_a.get()), 0);
}

TEST(H5PocpyplTest, MergeCommittedDtypePathsRejectInvalidArgsAndRemovedProp) {
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;

  // Invalid path arguments.
  EXPECT_LT(H5Padd_merge_committed_dtype_path(ocpy.get(), nullptr), 0);
  EXPECT_LT(H5Padd_merge_committed_dtype_path(ocpy.get(), ""), 0);

  // Invalid, wrong-class, and immutable default property lists.
  EXPECT_LT(H5Padd_merge_committed_dtype_path(H5I_INVALID_HID, "/dt"), 0);
  EXPECT_LT(H5Padd_merge_committed_dtype_path(H5P_DEFAULT, "/dt"), 0);
  EXPECT_LT(H5Padd_merge_committed_dtype_path(H5P_OBJECT_COPY_DEFAULT, "/dt"),
            0);
  EXPECT_LT(H5Padd_merge_committed_dtype_path(fcpl.get(), "/dt"), 0);

  EXPECT_LT(H5Pfree_merge_committed_dtype_paths(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Pfree_merge_committed_dtype_paths(H5P_DEFAULT), 0);
  EXPECT_LT(H5Pfree_merge_committed_dtype_paths(H5P_OBJECT_COPY_DEFAULT), 0);
  EXPECT_LT(H5Pfree_merge_committed_dtype_paths(fcpl.get()), 0);

  // Removing the merge list property forces H5P_peek failure branches.
  ASSERT_GE(H5Premove(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME), 0);
  EXPECT_LT(H5Padd_merge_committed_dtype_path(ocpy.get(), "/dt"), 0);
  EXPECT_LT(H5Pfree_merge_committed_dtype_paths(ocpy.get()), 0);
}

TEST(H5PocpyplTest, EncodeAndDecodePreserveOptionsAndMergePaths) {
  const std::vector<std::vector<std::string>> kPathSets = {
      {},
      {"/only_one"},
      {"/first", "/nested/second", "/third"},
  };

  for (const auto& paths_to_add : kPathSets) {
    ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_TRUE(ocpy.valid());
    ASSERT_GE(
        H5Pset_copy_object(ocpy.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG |
                                           H5O_COPY_WITHOUT_ATTR_FLAG),
        0);

    for (const std::string& p : paths_to_add) {
      ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), p.c_str()), 0);
    }

    // 1. Query required encoding buffer size (*pp == nullptr).
    size_t nalloc = 0;
    ASSERT_GE(H5Pencode2(ocpy.get(), nullptr, &nalloc, H5P_DEFAULT), 0);
    ASSERT_GT(nalloc, 0u);

    // 2. Encode into buffer (*pp != nullptr).
    std::vector<uint8_t> buf(nalloc, 0);
    size_t written = nalloc;
    ASSERT_GE(H5Pencode2(ocpy.get(), buf.data(), &written, H5P_DEFAULT), 0);
    EXPECT_EQ(written, nalloc);

    // 3. Decode back into a new property list and compare.
    ScopedPlist decoded(H5Pdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());

    unsigned decoded_opt = 0u;
    ASSERT_GE(H5Pget_copy_object(decoded.get(), &decoded_opt), 0);
    EXPECT_EQ(decoded_opt,
              H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG | H5O_COPY_WITHOUT_ATTR_FLAG);

    ScopedMergeList orig_list;
    ScopedMergeList dec_list;
    ASSERT_GE(
        H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, orig_list.ptr()),
        0);
    ASSERT_GE(
        H5Pget(decoded.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, dec_list.ptr()),
        0);
    EXPECT_EQ(dec_list.ToVector(), orig_list.ToVector());
    EXPECT_GT(H5Pequal(ocpy.get(), decoded.get()), 0);
  }
}

TEST(H5PocpyplTest, SetGetAndCopyMcdtSearchCallback) {
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());

  int user_ctx = 7;
  ASSERT_GE(H5Pset_mcdt_search_cb(ocpy.get(), DummyMcdtSearchCb, &user_ctx), 0);

  H5O_mcdt_search_cb_t got_cb = nullptr;
  void* got_data = nullptr;
  ASSERT_GE(H5Pget_mcdt_search_cb(ocpy.get(), &got_cb, &got_data), 0);
  EXPECT_EQ(got_cb, DummyMcdtSearchCb);
  EXPECT_EQ(got_data, &user_ctx);

  // Partial or all nullptr out-parameters are permitted.
  got_cb = nullptr;
  EXPECT_GE(H5Pget_mcdt_search_cb(ocpy.get(), &got_cb, nullptr), 0);
  EXPECT_EQ(got_cb, DummyMcdtSearchCb);
  got_data = nullptr;
  EXPECT_GE(H5Pget_mcdt_search_cb(ocpy.get(), nullptr, &got_data), 0);
  EXPECT_EQ(got_data, &user_ctx);
  EXPECT_GE(H5Pget_mcdt_search_cb(ocpy.get(), nullptr, nullptr), 0);

  // H5Pcopy preserves callback and user data.
  ScopedPlist copied(H5Pcopy(ocpy.get()));
  ASSERT_TRUE(copied.valid());
  got_cb = nullptr;
  got_data = nullptr;
  ASSERT_GE(H5Pget_mcdt_search_cb(copied.get(), &got_cb, &got_data), 0);
  EXPECT_EQ(got_cb, DummyMcdtSearchCb);
  EXPECT_EQ(got_data, &user_ctx);

  // Non-null callback with null op_data succeeds.
  ASSERT_GE(H5Pset_mcdt_search_cb(ocpy.get(), DummyMcdtSearchCb, nullptr), 0);
  ASSERT_GE(H5Pget_mcdt_search_cb(ocpy.get(), &got_cb, &got_data), 0);
  EXPECT_EQ(got_cb, DummyMcdtSearchCb);
  EXPECT_EQ(got_data, nullptr);

  // Clearing both callback and user data resets to default.
  ASSERT_GE(H5Pset_mcdt_search_cb(ocpy.get(), nullptr, nullptr), 0);
  ASSERT_GE(H5Pget_mcdt_search_cb(ocpy.get(), &got_cb, &got_data), 0);
  EXPECT_EQ(got_cb, nullptr);
  EXPECT_EQ(got_data, nullptr);
}

TEST(H5PocpyplTest, McdtSearchCallbackRejectsInvalidArgsAndRemovedProp) {
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());

  ScopedErrorSilence silence;

  int user_ctx = 1;
  // Null callback with non-null op_data must fail.
  EXPECT_LT(H5Pset_mcdt_search_cb(ocpy.get(), nullptr, &user_ctx), 0);

  // Invalid, wrong-class, and immutable default property lists.
  EXPECT_LT(
      H5Pset_mcdt_search_cb(H5I_INVALID_HID, DummyMcdtSearchCb, &user_ctx), 0);
  EXPECT_LT(H5Pset_mcdt_search_cb(H5P_DEFAULT, DummyMcdtSearchCb, &user_ctx),
            0);
  EXPECT_LT(H5Pset_mcdt_search_cb(H5P_OBJECT_COPY_DEFAULT, DummyMcdtSearchCb,
                                  &user_ctx),
            0);
  EXPECT_LT(H5Pset_mcdt_search_cb(fcpl.get(), DummyMcdtSearchCb, &user_ctx), 0);

  H5O_mcdt_search_cb_t cb = nullptr;
  void* data = nullptr;
  EXPECT_LT(H5Pget_mcdt_search_cb(H5I_INVALID_HID, &cb, &data), 0);
  EXPECT_LT(H5Pget_mcdt_search_cb(H5P_DEFAULT, &cb, &data), 0);
  EXPECT_LT(H5Pget_mcdt_search_cb(H5P_FILE_CREATE_DEFAULT, &cb, &data), 0);
  EXPECT_LT(H5Pget_mcdt_search_cb(fcpl.get(), &cb, &data), 0);

  // Removing the callback property forces H5P_set / H5P_get failure paths.
  ASSERT_GE(H5Premove(ocpy.get(), H5O_CPY_MCDT_SEARCH_CB_NAME), 0);
  EXPECT_LT(H5Pset_mcdt_search_cb(ocpy.get(), DummyMcdtSearchCb, &user_ctx), 0);
  EXPECT_LT(H5Pget_mcdt_search_cb(ocpy.get(), &cb, &data), 0);
}

TEST(H5PocpyplTest, H5OcopyUsesMergePathsAndMcdtSearchCallback) {
  ScopedFile src_file = CreateInMemoryFile("ocpy_src.h5");
  ASSERT_TRUE(src_file.valid());
  ScopedFile dst_file = CreateInMemoryFile("ocpy_dst.h5");
  ASSERT_TRUE(dst_file.valid());

  // 1. Commit identical enum datatype in both source and destination files.
  ScopedType src_enum(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(src_enum.valid());
  const int32_t v0 = 0;
  ASSERT_GE(H5Tenum_insert(src_enum.get(), "ZERO", &v0), 0);
  ASSERT_GE(H5Tcommit2(src_file.get(), "/src_dtype", src_enum.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset src_dset(H5Dcreate2(src_file.get(), "/src_dset", src_enum.get(),
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(src_dset.valid());
  ASSERT_GE(H5Dwrite(src_dset.get(), src_enum.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &v0),
            0);

  ScopedType dst_enum(H5Tcopy(src_enum.get()));
  ASSERT_TRUE(dst_enum.valid());
  ASSERT_GE(H5Tcommit2(dst_file.get(), "/dst_dtype", dst_enum.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 2. Copy with a suggested path that does not match; callback fires and stops
  //    global search so destination creates an unmerged copy.
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());
  ASSERT_GE(H5Pset_copy_object(ocpy.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
            0);
  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/missing_dtype"), 0);
  int cb_calls = 0;
  ASSERT_GE(H5Pset_mcdt_search_cb(ocpy.get(), DummyMcdtSearchCb, &cb_calls), 0);

  ASSERT_GE(H5Ocopy(src_file.get(), "/src_dset", dst_file.get(),
                    "/dst_dset_unmerged", ocpy.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(cb_calls, 1);

  // 3. Add the actual destination committed datatype path; H5Ocopy merges
  //    directly via the suggestion list without invoking the fallback callback.
  ASSERT_GE(H5Padd_merge_committed_dtype_path(ocpy.get(), "/dst_dtype"), 0);
  ASSERT_GE(H5Ocopy(src_file.get(), "/src_dset", dst_file.get(),
                    "/dst_dset_merged", ocpy.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(cb_calls, 1);

  ScopedDataset dst_merged(
      H5Dopen2(dst_file.get(), "/dst_dset_merged", H5P_DEFAULT));
  ASSERT_TRUE(dst_merged.valid());
  ScopedType merged_dt(H5Dget_type(dst_merged.get()));
  ASSERT_TRUE(merged_dt.valid());
  EXPECT_GT(H5Tcommitted(merged_dt.get()), 0);
  EXPECT_GT(H5Tequal(merged_dt.get(), dst_enum.get()), 0);
}

TEST(H5PocpyplTest, InternalRegPropDuplicateRegistrationFailures) {
  ASSERT_GE(H5open(), 0);
  ASSERT_NE(H5P_CLS_OCPY[0].reg_prop_func, nullptr);
  EXPECT_STREQ(H5P_CLS_OCPY[0].name, "object copy");

  constexpr auto kOcpyPropNames = std::to_array<const char*>({
      H5O_CPY_OPTION_NAME,
      H5O_CPY_MERGE_COMM_DT_LIST_NAME,
      H5O_CPY_MCDT_SEARCH_CB_NAME,
  });

  for (const char* prop_name : kOcpyPropNames) {
    H5P_genclass_t* tmp_cls = H5P__create_class(
        H5P_CLS_ROOT_g, "tmp_ocpy_class", H5P_TYPE_OBJECT_COPY, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(tmp_cls, nullptr);
    absl::Cleanup close_tmp_cls = [&] { H5P__close_class(tmp_cls); };

    const int dummy = 0;
    ASSERT_GE(H5P__register_real(tmp_cls, prop_name, sizeof(dummy), &dummy,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr),
              0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5P_CLS_OCPY[0].reg_prop_func(tmp_cls), 0);
  }
}

TEST(H5PocpyplTest, MergeCommittedDtypeListInternalCopyFailurePaths) {
  // 1. Trigger failure inside H5P__copy_merge_comm_dt_list() during H5Pset():
  //    node 1 copies cleanly so dst_dt_list is non-empty, then node 2 has
  //    path == nullptr, causing H5MM_strdup(nullptr) to return NULL while
  //    tmp_dt_list != nullptr, exercising partial list cleanup and set failure.
  ScopedPlist ocpy(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpy.valid());

  char valid_path[] = "/valid_first_path";
  H5O_copy_dtype_merge_list_t tail_node = {.path = nullptr, .next = nullptr};
  H5O_copy_dtype_merge_list_t head_node = {.path = valid_path,
                                           .next = &tail_node};
  H5O_copy_dtype_merge_list_t* head_ptr = &head_node;

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, &head_ptr),
              0);
  }

  // Verify the original plist property remained clean (NULL).
  ScopedMergeList still_empty;
  ASSERT_GE(
      H5Pget(ocpy.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, still_empty.ptr()),
      0);
  EXPECT_EQ(still_empty.get(), nullptr);

  // 2. Bypass _set via H5P_poke() with an H5FL-allocated node whose path is
  //    NULL so H5Pget() and H5Pcopy_prop() hit the copy failure branches in
  //    _get and _copy without triggering H5P_copy_plist()'s plists underflow;
  //    H5Pclose(poked) frees the H5FL node cleanly via _close.
  ScopedPlist poked(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(poked.valid());
  H5P_genplist_t* raw_plist =
      H5P_object_verify(poked.get(), H5P_OBJECT_COPY, false);
  ASSERT_NE(raw_plist, nullptr);

  H5O_copy_dtype_merge_list_t* bad_node =
      H5FL_CALLOC(H5O_copy_dtype_merge_list_t);
  ASSERT_NE(bad_node, nullptr);
  bad_node->path = nullptr;
  bad_node->next = nullptr;
  ASSERT_GE(H5P_poke(raw_plist, H5O_CPY_MERGE_COMM_DT_LIST_NAME, &bad_node), 0);

  ScopedPlist dst(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(dst.valid());

  ScopedErrorSilence silence;
  H5O_copy_dtype_merge_list_t* out_ptr = nullptr;
  EXPECT_LT(H5Pget(poked.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME, &out_ptr), 0);
  EXPECT_LT(
      H5Pcopy_prop(dst.get(), poked.get(), H5O_CPY_MERGE_COMM_DT_LIST_NAME), 0);
}

}  // namespace
}  // namespace hdf5
