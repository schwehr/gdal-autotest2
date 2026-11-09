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
// Tests internal link management: package init/term, user-defined link class
// registry, link creation (hard, soft, UD, object), value/info/name queries,
// deletion, moving/copying, existence checks (strict and tolerant), iteration,
// and inter-file link expansion (third_party/hdf5/src/H5Lint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Lint.c

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, so internal struct layouts match only when
// NDEBUG is defined across internal headers.
#ifndef NDEBUG
#define H5LINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5L_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Lpkg.h"
}  // extern "C"
#ifdef H5LINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5LINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5L_type_t kUdSuccessId = static_cast<H5L_type_t>(80);
constexpr H5L_type_t kUdFailingId = static_cast<H5L_type_t>(81);
constexpr H5L_type_t kUdNullOpsId = static_cast<H5L_type_t>(82);
constexpr H5L_type_t kUdBaseTableId = static_cast<H5L_type_t>(100);

// Global counters for verifying UD link callback invocations (no global state
// persists because callbacks run strictly synchronously inside each test).
struct UdCallbackLog {
  int create_calls = 0;
  int move_calls = 0;
  int copy_calls = 0;
  int trav_calls = 0;
  int del_calls = 0;
  int query_calls = 0;
  std::string last_create_name;
  std::string last_move_name;
  std::string last_copy_name;
  std::string last_del_name;
  std::string last_payload;
};

UdCallbackLog& GetUdLog() {
  static UdCallbackLog* log = new UdCallbackLog();
  return *log;
}

void ResetUdLog() {
  GetUdLog() = UdCallbackLog{};
}

herr_t UdCreateOk(const char* link_name, hid_t loc_group, const void* lnkdata,
                  size_t lnkdata_size, hid_t /*lcpl_id*/) {
  auto& log = GetUdLog();
  ++log.create_calls;
  log.last_create_name = link_name ? link_name : "";
  if (lnkdata && lnkdata_size > 0) {
    log.last_payload.assign(static_cast<const char*>(lnkdata), lnkdata_size);
  } else {
    log.last_payload.clear();
  }
  return H5Iis_valid(loc_group) > 0 ? 0 : -1;
}

herr_t UdCreateFail(const char* /*link_name*/, hid_t /*loc_group*/,
                    const void* /*lnkdata*/, size_t /*lnkdata_size*/,
                    hid_t /*lcpl_id*/) {
  ++GetUdLog().create_calls;
  return -1;
}

herr_t UdMoveOk(const char* new_name, hid_t new_loc, const void* /*lnkdata*/,
                size_t /*lnkdata_size*/) {
  auto& log = GetUdLog();
  ++log.move_calls;
  log.last_move_name = new_name ? new_name : "";
  return H5Iis_valid(new_loc) > 0 ? 0 : -1;
}

herr_t UdMoveFail(const char* /*new_name*/, hid_t /*new_loc*/,
                  const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  ++GetUdLog().move_calls;
  return -1;
}

herr_t UdCopyOk(const char* new_name, hid_t new_loc, const void* /*lnkdata*/,
                size_t /*lnkdata_size*/) {
  auto& log = GetUdLog();
  ++log.copy_calls;
  log.last_copy_name = new_name ? new_name : "";
  return H5Iis_valid(new_loc) > 0 ? 0 : -1;
}

herr_t UdCopyFail(const char* /*new_name*/, hid_t /*new_loc*/,
                  const void* /*lnkdata*/, size_t /*lnkdata_size*/) {
  ++GetUdLog().copy_calls;
  return -1;
}

hid_t UdTravOk(const char* /*link_name*/, hid_t cur_group,
               const void* lnkdata, size_t lnkdata_size, hid_t /*lapl_id*/,
               hid_t /*dxpl_id*/) {
  ++GetUdLog().trav_calls;
  if (!lnkdata || lnkdata_size == 0) {
    return H5I_INVALID_HID;
  }
  std::string target(static_cast<const char*>(lnkdata), lnkdata_size);
  return H5Oopen(cur_group, target.c_str(), H5P_DEFAULT);
}

herr_t UdDelOk(const char* link_name, hid_t /*file*/, const void* /*lnkdata*/,
               size_t /*lnkdata_size*/) {
  auto& log = GetUdLog();
  ++log.del_calls;
  log.last_del_name = link_name ? link_name : "";
  return 0;
}

ssize_t UdQueryOk(const char* /*link_name*/, const void* lnkdata,
                  size_t lnkdata_size, void* buf, size_t buf_size) {
  ++GetUdLog().query_calls;
  if (buf && buf_size > 0) {
    const size_t copy_len =
        lnkdata_size < buf_size ? lnkdata_size : buf_size - 1;
    if (copy_len > 0 && lnkdata != nullptr) {
      std::memcpy(buf, lnkdata, copy_len);
    }
    static_cast<char*>(buf)[copy_len] = '\0';
  }
  return static_cast<ssize_t>(lnkdata_size + 1);
}

ssize_t UdQueryFail(const char* /*link_name*/, const void* /*lnkdata*/,
                    size_t /*lnkdata_size*/, void* /*buf*/,
                    size_t /*buf_size*/) {
  ++GetUdLog().query_calls;
  return -1;
}

// RAII wrapper for pushing/popping an HDF5 API context around direct calls to
// internal H5L_* routines.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t lcpl_id = H5P_LINK_CREATE_DEFAULT,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5CX_set_lcpl(lcpl_id);
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

// Creates a scalar int32 dataset at `path` inside `loc_id`.
ScopedDataset CreateScalarDataset(hid_t loc_id, const char* path,
                                  int32_t value = 0,
                                  hid_t lcpl_id = H5P_DEFAULT) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) {
    return ScopedDataset();
  }
  ScopedDataset dset(H5Dcreate2(loc_id, path, H5T_NATIVE_INT32, space.get(),
                                lcpl_id, H5P_DEFAULT, H5P_DEFAULT));
  if (dset.valid() && value != 0) {
    if (H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 &value) < 0) {
      dset.reset();
    }
  }
  return dset;
}

TEST(H5LintTest, PackageInitAndTermCycleRestoresExternalLinkClass) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup ensure_open = [] { H5open(); };

  // Calling H5L_init when already initialized succeeds cleanly.
  EXPECT_GE(H5L_init(), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);

  // H5close invokes H5L_term_package in two passes (freeing H5L_table_g, then
  // resetting H5_PKG_INIT_VAR); calling it a third time verifies the no-op branch.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5L_term_package(), 0);

  // Re-opening restores built-in external link registration via H5L__init_package.
  ASSERT_GE(H5open(), 0);
  EXPECT_GT(H5Lis_registered(H5L_TYPE_EXTERNAL), 0);
  EXPECT_NE(H5L_find_class(H5L_TYPE_EXTERNAL), nullptr);
}

TEST(H5LintTest, UdClassRegistryGrowsOverwritesAndUnregistersCleanly) {
  ASSERT_GE(H5open(), 0);

  // Register > H5L_MIN_TABLE_SIZE (32) classes to exercise table doubling.
  constexpr int kNumClasses = 36;
  std::vector<H5L_type_t> registered_ids;
  registered_ids.reserve(kNumClasses);
  absl::Cleanup cleanup_classes = [&registered_ids] {
    ScopedErrorSilence silence;
    for (const H5L_type_t id : registered_ids) {
      H5Lunregister(id);
    }
  };

  for (int i = 0; i < kNumClasses; ++i) {
    const H5L_type_t id = static_cast<H5L_type_t>(kUdBaseTableId + i);
    const H5L_class_t cls = {
        H5L_LINK_CLASS_T_VERS,
        id,
        "test-table-growth",
        nullptr,
        nullptr,
        nullptr,
        UdTravOk,
        nullptr,
        nullptr,
    };
    ASSERT_GE(H5Lregister(&cls), 0);
    registered_ids.push_back(id);
    EXPECT_GT(H5Lis_registered(id), 0);
  }

  // Re-registering an existing ID overwrites its table entry in place.
  const H5L_class_t updated_cls = {
      H5L_LINK_CLASS_T_VERS,
      kUdBaseTableId,
      "updated-comment",
      UdCreateOk,
      nullptr,
      nullptr,
      UdTravOk,
      nullptr,
      UdQueryOk,
  };
  ASSERT_GE(H5Lregister(&updated_cls), 0);
  const H5L_class_t* found = H5L_find_class(kUdBaseTableId);
  ASSERT_NE(found, nullptr);
  EXPECT_STREQ(found->comment, "updated-comment");
  EXPECT_EQ(found->query_func, UdQueryOk);

  // Check unregistered ID detection via both public and internal APIs.
  constexpr H5L_type_t kUnregisteredId = static_cast<H5L_type_t>(220);
  EXPECT_EQ(H5Lis_registered(kUnregisteredId), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5L_find_class(kUnregisteredId), nullptr);
    EXPECT_LT(H5Lunregister(kUnregisteredId), 0);
  }

  // Unregister an interior element (triggers memmove of tail) and the last element.
  const H5L_type_t mid_id = static_cast<H5L_type_t>(kUdBaseTableId + 5);
  EXPECT_GE(H5Lunregister(mid_id), 0);
  EXPECT_EQ(H5Lis_registered(mid_id), 0);
  EXPECT_GT(H5Lis_registered(static_cast<H5L_type_t>(kUdBaseTableId + 35)), 0);
  EXPECT_GE(H5Lunregister(static_cast<H5L_type_t>(kUdBaseTableId + 35)), 0);
}

TEST(H5LintTest, CreateHardSoftAnonAndIntermediateGroupLinks) {
  ScopedFile file1 = CreateInMemoryFile("links1.h5");
  ASSERT_TRUE(file1.valid());
  ScopedFile file2 = CreateInMemoryFile("links2.h5");
  ASSERT_TRUE(file2.valid());

  // 1. Create a dataset with intermediate group creation and UTF-8 encoding.
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);

  ScopedDataset dset =
      CreateScalarDataset(file1.get(), "auto_g1/auto_g2/ds", 42, lcpl.get());
  ASSERT_TRUE(dset.valid());

  // Verify intermediate group and link properties persisted.
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(file1.get(), "auto_g1/auto_g2/ds", &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);
  EXPECT_EQ(linfo.cset, H5T_CSET_UTF8);

  // Also test intermediate group creation via H5Gcreate2 (H5L_get_ocrt_info
  // group branch in H5Gtraverse.c).
  ScopedGroup nested_grp(H5Gcreate2(file1.get(), "auto_grp/sub/leaf_grp",
                                    lcpl.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(nested_grp.valid());

  // 2. Link an anonymous object via H5Olink (exercises udata->path->user_path_r == NULL).
  ScopedGroup anon_grp(H5Gcreate_anon(file1.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(anon_grp.valid());
  ASSERT_GE(H5Olink(anon_grp.get(), file1.get(), "linked_anon_grp", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(file1.get(), "linked_anon_grp", H5P_DEFAULT), 0);

  ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_space.valid());
  ScopedDataset anon_ds(H5Dcreate_anon(file1.get(), H5T_NATIVE_INT32,
                                       scalar_space.get(), H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(anon_ds.valid());
  ASSERT_GE(H5Olink(anon_ds.get(), file1.get(), "auto_anon/ds", lcpl.get(),
                    H5P_DEFAULT),
            0);

  // 3. Create hard and soft links (including paths requiring normalization).
  ASSERT_GE(H5Lcreate_hard(file1.get(), "//auto_g1/auto_g2/ds", file1.get(),
                           "hard_alias", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("//auto_g1/auto_g2/ds", file1.get(), "soft_alias",
                           lcpl.get(), H5P_DEFAULT),
            0);

  // Direct internal H5L_link with literal H5P_DEFAULT exercises the
  // `udata->lc_plist == NULL` default charset path in H5L__link_cb.
  {
    ScopedApiContext default_ctx;
    ASSERT_TRUE(default_ctx.valid());
    H5G_loc_t file_loc{};
    H5G_loc_t dset_loc{};
    ASSERT_GE(H5G_loc(file1.get(), &file_loc), 0);
    ASSERT_GE(H5G_loc(dset.get(), &dset_loc), 0);
    EXPECT_GE(
        H5L_link(&file_loc, "direct_default_lcpl", &dset_loc, H5P_DEFAULT), 0);
  }
  EXPECT_GT(H5Lexists(file1.get(), "direct_default_lcpl", H5P_DEFAULT), 0);

  // 4. Negative paths: existing destination, missing source, H5L_link_object
  // failures (existing name, missing parent, oversized compact layout),
  // cross-file hard link, and direct internal invalid lcpl_id.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lcreate_hard(file1.get(), "auto_g1/auto_g2/ds", file1.get(),
                           "hard_alias", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_soft("/somewhere", file1.get(), "soft_alias", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcreate_hard(file1.get(), "no_such_source", file1.get(),
                           "bad_hard", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(
      CreateScalarDataset(file1.get(), "hard_alias", 1, H5P_DEFAULT).get(),
      H5I_INVALID_HID);
  EXPECT_EQ(
      CreateScalarDataset(file1.get(), "no_parent/ds", 1, H5P_DEFAULT).get(),
      H5I_INVALID_HID);
  {
    constexpr hsize_t kOversizedCompact = 20000;
    ScopedSpace big_space(H5Screate_simple(1, &kOversizedCompact, nullptr));
    ScopedPlist compact_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    if (big_space.valid() && compact_dcpl.valid() &&
        H5Pset_layout(compact_dcpl.get(), H5D_COMPACT) >= 0) {
      EXPECT_EQ(H5Dcreate2(file1.get(), "oversized_compact", H5T_NATIVE_INT32,
                           big_space.get(), H5P_DEFAULT, compact_dcpl.get(),
                           H5P_DEFAULT),
                H5I_INVALID_HID);
    }
  }
  EXPECT_LT(H5Olink(dset.get(), file2.get(), "cross_file_hard", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  // Direct internal call with a closed (non-H5P_DEFAULT) lcpl_id triggers
  // H5I_object(lcpl_id) == NULL -> H5E_BADTYPE.
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5G_loc_t file_loc{};
  H5G_loc_t dset_loc{};
  ASSERT_GE(H5G_loc(file1.get(), &file_loc), 0);
  ASSERT_GE(H5G_loc(dset.get(), &dset_loc), 0);
  EXPECT_LT(H5L_link(&file_loc, "invalid_lcpl_link", &dset_loc, H5I_INVALID_HID),
            0);
}

TEST(H5LintTest, UserDefinedLinkCallbacksAndUnregisteredFallback) {
  ResetUdLog();

  const H5L_class_t cls_ok = {
      H5L_LINK_CLASS_T_VERS,
      kUdSuccessId,
      "ud-ok",
      UdCreateOk,
      UdMoveOk,
      UdCopyOk,
      UdTravOk,
      UdDelOk,
      UdQueryOk,
  };
  const H5L_class_t cls_fail = {
      H5L_LINK_CLASS_T_VERS,
      kUdFailingId,
      "ud-fail",
      UdCreateFail,
      UdMoveFail,
      UdCopyFail,
      UdTravOk,
      nullptr,
      UdQueryFail,
  };
  const H5L_class_t cls_null_ops = {
      H5L_LINK_CLASS_T_VERS,
      kUdNullOpsId,
      "ud-null-ops",
      nullptr,
      nullptr,
      nullptr,
      UdTravOk,
      nullptr,
      nullptr,
  };

  ASSERT_GE(H5Lregister(&cls_ok), 0);
  ASSERT_GE(H5Lregister(&cls_fail), 0);
  ASSERT_GE(H5Lregister(&cls_null_ops), 0);
  absl::Cleanup unregister_all = [] {
    ScopedErrorSilence silence;
    H5Lunregister(kUdSuccessId);
    H5Lunregister(kUdFailingId);
    H5Lunregister(kUdNullOpsId);
  };

  ScopedFile file = CreateInMemoryFile("ud_links.h5");
  ASSERT_TRUE(file.valid());
  ScopedDataset target = CreateScalarDataset(file.get(), "/target", 7);
  ASSERT_TRUE(target.valid());

  // 1. Create UD link with payload -> triggers create_func and stores udata.
  constexpr char kPayload[] = "/target";
  ASSERT_GE(H5Lcreate_ud(file.get(), "ud_link", kUdSuccessId, kPayload,
                         sizeof(kPayload) - 1, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetUdLog().create_calls, 1);
  EXPECT_EQ(GetUdLog().last_create_name, "ud_link");
  EXPECT_EQ(GetUdLog().last_payload, kPayload);

  // Create UD link with zero udata size and null optional callbacks.
  ASSERT_GE(H5Lcreate_ud(file.get(), "ud_empty", kUdNullOpsId, nullptr, 0,
                         H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Query UD link with query_func != nullptr and query_func == nullptr.
  std::array<char, 32> buf{};
  buf.fill('Z');
  ASSERT_GE(H5Lget_val(file.get(), "ud_link", buf.data(), buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_STREQ(buf.data(), kPayload);

  buf.fill('Z');
  ASSERT_GE(H5Lget_val(file.get(), "ud_empty", buf.data(), buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(buf[0], '\0');

  // 2. Copy and move UD link -> triggers copy_func and move_func (and
  // removing the old link during move invokes del_func on the source name).
  ASSERT_GE(H5Lcopy(file.get(), "ud_link", file.get(), "ud_copied",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetUdLog().copy_calls, 1);
  EXPECT_EQ(GetUdLog().last_copy_name, "ud_copied");

  ASSERT_GE(H5Lmove(file.get(), "ud_copied", file.get(), "ud_moved",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(GetUdLog().move_calls, 1);
  EXPECT_EQ(GetUdLog().last_move_name, "ud_moved");
  EXPECT_EQ(GetUdLog().del_calls, 1);
  EXPECT_EQ(GetUdLog().last_del_name, "ud_copied");

  // Copy and move null-ops UD link succeeds without callbacks.
  ASSERT_GE(H5Lcopy(file.get(), "ud_empty", file.get(), "ud_empty_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lmove(file.get(), "ud_empty_copy", file.get(), "ud_empty_moved",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 3. Failing create/query/move/copy callbacks return callback errors.
  {
    ScopedErrorSilence silence;
    constexpr H5L_type_t kMissingClass = static_cast<H5L_type_t>(95);
    EXPECT_LT(H5Lcreate_ud(file.get(), "unreg_ud", kMissingClass, kPayload,
                           sizeof(kPayload) - 1, H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lcreate_ud(file.get(), "fail_create", kUdFailingId, kPayload,
                           sizeof(kPayload) - 1, H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Register temporarily with null create_func so we can create a kUdFailingId
  // link to test failing query_func, move_func, and copy_func.
  const H5L_class_t cls_fail_later = {
      H5L_LINK_CLASS_T_VERS,
      kUdFailingId,
      "ud-fail-after-create",
      nullptr,
      UdMoveFail,
      UdCopyFail,
      UdTravOk,
      nullptr,
      UdQueryFail,
  };
  ASSERT_GE(H5Lregister(&cls_fail_later), 0);
  ASSERT_GE(H5Lcreate_ud(file.get(), "ud_fail_ops", kUdFailingId, kPayload,
                         sizeof(kPayload) - 1, H5P_DEFAULT, H5P_DEFAULT),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lget_val(file.get(), "ud_fail_ops", buf.data(), buf.size(),
                         H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lcopy(file.get(), "ud_fail_ops", file.get(), "fail_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5Lmove(file.get(), "ud_fail_ops", file.get(), "fail_move",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // 4. Unregistering kUdSuccessId leaves existing link queryable as '\0', while
  // move/copy fail with H5E_NOTREGISTERED.
  ASSERT_GE(H5Lunregister(kUdSuccessId), 0);
  buf.fill('Z');
  EXPECT_GE(H5Lget_val(file.get(), "ud_link", buf.data(), buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(buf[0], '\0');

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lcopy(file.get(), "ud_link", file.get(), "ud_after_unreg",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5LintTest, GetValAndGetValByIdxCoverSoftExternalTruncationAndErrors) {
  ScopedFile file = CreateInMemoryFile("get_val.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedDataset hard_ds = CreateScalarDataset(grp.get(), "b_hard", 1);
  ASSERT_TRUE(hard_ds.valid());
  ASSERT_GE(H5Lcreate_soft("/g/b_hard", grp.get(), "a_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_external("other_file.h5", "/ext/obj", grp.get(), "c_ext",
                               H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Full soft-link read, truncated read (buffer smaller than target path),
  // and zero-size/null read.
  std::array<char, 32> full_buf{};
  full_buf.fill('X');
  ASSERT_GE(H5Lget_val(grp.get(), "a_soft", full_buf.data(), full_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_STREQ(full_buf.data(), "/g/b_hard");

  std::array<char, 4> short_buf{};
  short_buf.fill('X');
  ASSERT_GE(H5Lget_val(grp.get(), "a_soft", short_buf.data(), short_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(short_buf, ElementsAre('/', 'g', '/', '\0'));

  EXPECT_GE(H5Lget_val(grp.get(), "a_soft", nullptr, 0, H5P_DEFAULT), 0);

  // External link value query and unpack.
  H5L_info2_t ext_info{};
  ASSERT_GE(H5Lget_info2(grp.get(), "c_ext", &ext_info, H5P_DEFAULT), 0);
  ASSERT_GT(ext_info.u.val_size, 0u);
  std::vector<char> ext_raw(ext_info.u.val_size, '\0');
  ASSERT_GE(H5Lget_val(grp.get(), "c_ext", ext_raw.data(), ext_raw.size(),
                       H5P_DEFAULT),
            0);
  unsigned ext_flags = 99;
  const char* ext_file_name = nullptr;
  const char* ext_obj_name = nullptr;
  ASSERT_GE(H5Lunpack_elink_val(ext_raw.data(), ext_raw.size(), &ext_flags,
                                &ext_file_name, &ext_obj_name),
            0);
  EXPECT_STREQ(ext_file_name, "other_file.h5");
  EXPECT_STREQ(ext_obj_name, "/ext/obj");

  // Query by index (NAME INC/DEC and CRT_ORDER).
  full_buf.fill('X');
  ASSERT_GE(H5Lget_val_by_idx(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC, 0,
                              full_buf.data(), full_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_STREQ(full_buf.data(), "/g/b_hard");

  full_buf.fill('X');
  ASSERT_GE(H5Lget_val_by_idx(file.get(), "/g", H5_INDEX_CRT_ORDER, H5_ITER_INC,
                              1, full_buf.data(), full_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_STREQ(full_buf.data(), "/g/b_hard");

  // Error paths: hard link value query, missing link, missing group, OOB index.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lget_val(grp.get(), "b_hard", full_buf.data(), full_buf.size(),
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val(grp.get(), "missing_link", full_buf.data(),
                       full_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val(grp.get(), "missing_grp/leaf", full_buf.data(),
                       full_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC, 1,
                              full_buf.data(), full_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), "/no_such_grp", H5_INDEX_NAME,
                              H5_ITER_INC, 0, full_buf.data(), full_buf.size(),
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_val_by_idx(file.get(), "/g", H5_INDEX_NAME, H5_ITER_INC, 99,
                              full_buf.data(), full_buf.size(), H5P_DEFAULT),
            0);
}

TEST(H5LintTest, DeleteAndDeleteByIdxCoverNormalDotMissingAndOutOfRange) {
  ScopedFile file = CreateInMemoryFile("delete_links.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(H5Gcreate2(file.get(), "/dir", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedDataset ds = CreateScalarDataset(grp.get(), "ds", 99);
  ASSERT_TRUE(ds.valid());
  ASSERT_GE(H5Lcreate_hard(grp.get(), "ds", grp.get(), "ds_hard2", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/dir/ds", grp.get(), "ds_soft", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_soft("/dir/ds", grp.get(), "ds_soft2", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // Delete by name and verify remaining hard link keeps dataset alive.
  ASSERT_GE(H5Ldelete(grp.get(), "ds_soft", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(grp.get(), "ds_soft", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(grp.get(), "ds", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(grp.get(), "ds_hard2", H5P_DEFAULT), 0);

  // Remaining links in /dir sorted by name: ["ds_hard2", "ds_soft2"].
  ASSERT_GE(H5Ldelete_by_idx(file.get(), "/dir", H5_INDEX_NAME, H5_ITER_INC, 1,
                             H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(grp.get(), "ds_soft2", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete_by_idx(file.get(), "/dir", H5_INDEX_CRT_ORDER,
                             H5_ITER_INC, 0, H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(grp.get(), "ds_hard2", H5P_DEFAULT), 0);

  // Negative paths: '.' (grp_loc == NULL), missing leaf (lnk == NULL),
  // missing group, and out-of-range index.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Ldelete(file.get(), ".", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete(grp.get(), "does_not_exist", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete(file.get(), "/no_such_dir/leaf", H5P_DEFAULT), 0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), "/no_such_dir", H5_INDEX_NAME,
                             H5_ITER_INC, 0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ldelete_by_idx(file.get(), "/dir", H5_INDEX_NAME, H5_ITER_INC, 5,
                             H5P_DEFAULT),
            0);
}

TEST(H5LintTest, MoveAndCopyCoverRelativeAbsoluteLcplCrossFileAndErrors) {
  ScopedFile file1 = CreateInMemoryFile("move1.h5");
  ASSERT_TRUE(file1.valid());
  ScopedFile file2 = CreateInMemoryFile("move2.h5");
  ASSERT_TRUE(file2.valid());

  ScopedGroup sub_g(H5Gcreate2(file1.get(), "/open_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub_g.valid());
  ScopedDataset ds = CreateScalarDataset(sub_g.get(), "inner_ds", 123);
  ASSERT_TRUE(ds.valid());
  ASSERT_GE(H5Lcreate_soft("/open_grp/inner_ds", file1.get(), "slink",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 1. Non-default LCPL with intermediate group creation and UTF-8 charset.
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);

  // Move hard link using an absolute destination path ('/' branch in H5L__move_cb).
  ASSERT_GE(H5Lmove(file1.get(), "/open_grp/inner_ds", H5L_SAME_LOC,
                    "/dst_tree/abs_moved_ds", lcpl.get(), H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Lexists(file1.get(), "/open_grp/inner_ds", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(file1.get(), "/dst_tree/abs_moved_ds", H5P_DEFAULT), 0);

  // Check UTF-8 encoding applied to moved link.
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(file1.get(), "/dst_tree/abs_moved_ds", &linfo,
                         H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.cset, H5T_CSET_UTF8);

  // Open handle `ds` has updated name via H5G_name_replace.
  std::array<char, 64> name_buf{};
  ASSERT_GT(H5Iget_name(ds.get(), name_buf.data(), name_buf.size()), 0);
  EXPECT_STREQ(name_buf.data(), "/dst_tree/abs_moved_ds");

  // 2. Move with a relative destination path (build_fullpath_refstr branch).
  ScopedGroup target_grp(H5Gopen2(file1.get(), "/dst_tree", H5P_DEFAULT));
  ASSERT_TRUE(target_grp.valid());
  ASSERT_GE(H5Lmove(file1.get(), "/slink", target_grp.get(), "rel_slink",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(file1.get(), "/dst_tree/rel_slink", H5P_DEFAULT), 0);

  // 3. Copy hard link within same file and soft link across different files.
  ASSERT_GE(H5Lcopy(file1.get(), "/dst_tree/abs_moved_ds", file1.get(),
                    "copied_hard", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcopy(file1.get(), "/dst_tree/rel_slink", file2.get(),
                    "cross_soft", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Lexists(file2.get(), "cross_soft", H5P_DEFAULT), 0);

  // 4. Negative paths: missing src, '.', existing dest, cross-file hard link,
  // and internal H5L__move with non-plist lcpl_id.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lmove(file1.get(), "/missing_src", file1.get(), "new_name",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lmove(file1.get(), ".", file1.get(), "new_name", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcopy(file1.get(), "/dst_tree/abs_moved_ds", file1.get(),
                    "copied_hard", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lcopy(file1.get(), "/dst_tree/abs_moved_ds", file2.get(),
                    "cross_hard", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lmove(file1.get(), "/dst_tree/abs_moved_ds", file2.get(),
                    "cross_hard", H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5G_loc_t loc1{};
  ASSERT_GE(H5G_loc(file1.get(), &loc1), 0);
  EXPECT_LT(H5L__move(&loc1, "copied_hard", &loc1, "bad_lcpl_move", true,
                      H5I_INVALID_HID),
            0);
}

TEST(H5LintTest, ExistsAndExistsTolerantHandleNestedMissingAndSlashes) {
  ScopedFile file = CreateInMemoryFile("exists.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);
  ScopedDataset ds =
      CreateScalarDataset(file.get(), "/g1/g2/leaf_ds", 55, lcpl.get());
  ASSERT_TRUE(ds.valid());

  // Public H5Lexists checks.
  EXPECT_GT(H5Lexists(file.get(), "/", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(file.get(), "/g1/g2/leaf_ds", H5P_DEFAULT), 0);
  EXPECT_EQ(H5Lexists(file.get(), "/g1/g2/missing_leaf", H5P_DEFAULT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Lexists(file.get(), "/g1/no_such_inter/leaf_ds", H5P_DEFAULT),
              0);
  }

  // Internal H5L_exists_tolerant checks (including slashes and missing intermediates).
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5G_loc_t root_loc{};
  ASSERT_GE(H5G_loc(file.get(), &root_loc), 0);

  bool exists = false;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "/", &exists), 0);
  EXPECT_TRUE(exists);

  exists = false;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "///", &exists), 0);
  EXPECT_TRUE(exists);

  exists = false;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "g1", &exists), 0);
  EXPECT_TRUE(exists);

  exists = true;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "no_top_level", &exists), 0);
  EXPECT_FALSE(exists);

  exists = false;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "///g1//g2///leaf_ds", &exists), 0);
  EXPECT_TRUE(exists);

  exists = true;
  EXPECT_GE(H5L_exists_tolerant(&root_loc, "g1//missing_inter//leaf_ds", &exists),
            0);
  EXPECT_FALSE(exists);

  // Traversing through a non-group dataset fails in H5L__exists_inter_cb.
  ScopedErrorSilence silence;
  EXPECT_LT(H5L_exists_tolerant(&root_loc, "g1/g2/leaf_ds/child", &exists), 0);
}

TEST(H5LintTest, GetInfoGetNameByIdxAndIterate) {
  ScopedFile file = CreateInMemoryFile("info_iter.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ScopedGroup grp(H5Gcreate2(file.get(), "/items", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ASSERT_TRUE(CreateScalarDataset(grp.get(), "z_first", 1).valid());
  ASSERT_TRUE(CreateScalarDataset(grp.get(), "m_second", 2).valid());
  ASSERT_GE(H5Lcreate_soft("/items/z_first", grp.get(), "a_third_soft",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);

  // H5Lget_info2 and H5Lget_info_by_idx2.
  H5L_info2_t linfo{};
  ASSERT_GE(H5Lget_info2(grp.get(), "z_first", &linfo, H5P_DEFAULT), 0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);

  ASSERT_GE(H5Lget_info_by_idx2(file.get(), "/items", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_SOFT);

  ASSERT_GE(H5Lget_info_by_idx2(file.get(), "/items", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, 1, &linfo, H5P_DEFAULT),
            0);
  EXPECT_EQ(linfo.type, H5L_TYPE_HARD);

  // H5Lget_name_by_idx: length query, full buffer, truncated buffer.
  const ssize_t len = H5Lget_name_by_idx(
      file.get(), "/items", H5_INDEX_NAME, H5_ITER_INC, 0, nullptr, 0,
      H5P_DEFAULT);
  ASSERT_EQ(len, static_cast<ssize_t>(std::strlen("a_third_soft")));

  std::array<char, 32> name_buf{};
  ASSERT_EQ(H5Lget_name_by_idx(file.get(), "/items", H5_INDEX_NAME,
                               H5_ITER_INC, 0, name_buf.data(), name_buf.size(),
                               H5P_DEFAULT),
            len);
  EXPECT_STREQ(name_buf.data(), "a_third_soft");

  std::array<char, 5> trunc_buf{};
  ASSERT_EQ(H5Lget_name_by_idx(file.get(), "/items", H5_INDEX_CRT_ORDER,
                               H5_ITER_INC, 1, trunc_buf.data(),
                               trunc_buf.size(), H5P_DEFAULT),
            static_cast<ssize_t>(std::strlen("m_second")));
  EXPECT_STREQ(trunc_buf.data(), "m_se");

  // Iteration: full, paused + resumed via idx_p, and negative abort.
  std::vector<std::string> visited;
  auto collect_all = [](hid_t /*group*/, const char* name,
                        const H5L_info2_t* /*info*/, void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return H5_ITER_CONT;
  };
  ASSERT_EQ(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        collect_all, &visited),
            0);
  EXPECT_THAT(visited, ElementsAre("a_third_soft", "m_second", "z_first"));

  visited.clear();
  auto stop_after_one = [](hid_t /*group*/, const char* name,
                           const H5L_info2_t* /*info*/,
                           void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(name);
    return 1;
  };
  hsize_t idx = 0;
  ASSERT_EQ(H5Literate_by_name2(file.get(), "/items", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, &idx, stop_after_one, &visited,
                                H5P_DEFAULT),
            1);
  EXPECT_EQ(idx, 1u);
  ASSERT_EQ(H5Literate_by_name2(file.get(), "/items", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, &idx, collect_all, &visited,
                                H5P_DEFAULT),
            0);
  EXPECT_THAT(visited, ElementsAre("z_first", "m_second", "a_third_soft"));

  auto abort_iter = [](hid_t /*group*/, const char* /*name*/,
                       const H5L_info2_t* /*info*/, void* /*op_data*/) -> herr_t {
    return -2;
  };

  // Error paths.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Lget_info2(grp.get(), "missing_leaf", &linfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info2(file.get(), "/no_such/leaf", &linfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), "/no_such", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &linfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_info_by_idx2(file.get(), "/items", H5_INDEX_NAME,
                                H5_ITER_INC, 99, &linfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), "/no_such", H5_INDEX_NAME,
                               H5_ITER_INC, 0, name_buf.data(), name_buf.size(),
                               H5P_DEFAULT),
            0);
  EXPECT_LT(H5Lget_name_by_idx(file.get(), "/items", H5_INDEX_NAME,
                               H5_ITER_INC, 99, name_buf.data(),
                               name_buf.size(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Literate2(grp.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        abort_iter, nullptr),
            0);
  EXPECT_LT(H5Literate_by_name2(file.get(), "/no_such", H5_INDEX_NAME,
                                H5_ITER_INC, nullptr, collect_all, &visited,
                                H5P_DEFAULT),
            0);
}

TEST(H5LintTest, CopyGroupAcrossFilesExpandsSoftAndExternalLinks) {
  // External link resolution opens target files via default OS POSIX VFD,
  // so use TempDir under ::testing::TempDir() for this test.
  TempDir temp_dir("h5lint_copy_expand");
  const std::string ext_path = (temp_dir.path() / "ext_target.h5").string();
  const std::string src_path = (temp_dir.path() / "src.h5").string();
  const std::string dst_no_expand_path =
      (temp_dir.path() / "dst_no_expand.h5").string();
  const std::string dst_expand_path =
      (temp_dir.path() / "dst_expand.h5").string();

  // 1. Create external target file with a dataset.
  {
    ScopedFile ext_file(
        H5Fcreate(ext_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ext_file.valid());
    ASSERT_TRUE(
        CreateScalarDataset(ext_file.get(), "/target_val", 777).valid());
  }

  // 2. Create source file containing a group with hard, valid/dangling soft,
  // and valid/dangling external links.
  {
    ScopedFile src_file(
        H5Fcreate(src_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(src_file.valid());
    ScopedGroup bundle(H5Gcreate2(src_file.get(), "/bundle", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(bundle.valid());
    ASSERT_TRUE(CreateScalarDataset(bundle.get(), "hard_ds", 111).valid());
    ASSERT_GE(H5Lcreate_soft("/bundle/hard_ds", bundle.get(), "soft_valid",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_soft("/bundle/missing_target", bundle.get(),
                             "soft_dangling", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/target_val", bundle.get(),
                                 "ext_valid", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external("nonexistent_ext_file.h5", "/missing",
                                 bundle.get(), "ext_dangling", H5P_DEFAULT,
                                 H5P_DEFAULT),
              0);
  }

  ScopedFile src_file(
      H5Fopen(src_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(src_file.valid());

  // 3. Copy without expansion preserves soft and external link types.
  {
    ScopedFile dst_file(H5Fcreate(dst_no_expand_path.c_str(), H5F_ACC_TRUNC,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dst_file.valid());
    ASSERT_GE(H5Ocopy(src_file.get(), "/bundle", dst_file.get(), "/copied",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    H5L_info2_t info{};
    ASSERT_GE(
        H5Lget_info2(dst_file.get(), "/copied/soft_valid", &info, H5P_DEFAULT),
        0);
    EXPECT_EQ(info.type, H5L_TYPE_SOFT);
    ASSERT_GE(
        H5Lget_info2(dst_file.get(), "/copied/ext_valid", &info, H5P_DEFAULT),
        0);
    EXPECT_EQ(info.type, H5L_TYPE_EXTERNAL);
  }

  // 4. Copy with EXPAND_SOFT_LINK | EXPAND_EXT_LINK converts resolvable links
  // to hard links while preserving dangling ones.
  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(
                ocpypl.get(),
                H5O_COPY_EXPAND_SOFT_LINK_FLAG | H5O_COPY_EXPAND_EXT_LINK_FLAG),
            0);

  ScopedFile dst_expand(H5Fcreate(dst_expand_path.c_str(), H5F_ACC_TRUNC,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dst_expand.valid());
  ASSERT_GE(H5Ocopy(src_file.get(), "/bundle", dst_expand.get(), "/expanded",
                    ocpypl.get(), H5P_DEFAULT),
            0);

  H5L_info2_t info{};
  ASSERT_GE(H5Lget_info2(dst_expand.get(), "/expanded/soft_valid", &info,
                         H5P_DEFAULT),
            0);
  EXPECT_EQ(info.type, H5L_TYPE_HARD);
  ASSERT_GE(H5Lget_info2(dst_expand.get(), "/expanded/ext_valid", &info,
                         H5P_DEFAULT),
            0);
  EXPECT_EQ(info.type, H5L_TYPE_HARD);

  // Verify the expanded external link dataset holds the original value.
  ScopedDataset copied_ext_ds(
      H5Dopen2(dst_expand.get(), "/expanded/ext_valid", H5P_DEFAULT));
  ASSERT_TRUE(copied_ext_ds.valid());
  int32_t read_val = 0;
  ASSERT_GE(H5Dread(copied_ext_ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &read_val),
            0);
  EXPECT_EQ(read_val, 777);

  // Dangling soft and external links were not resolvable so they remain links.
  ASSERT_GE(H5Lget_info2(dst_expand.get(), "/expanded/soft_dangling", &info,
                         H5P_DEFAULT),
            0);
  EXPECT_EQ(info.type, H5L_TYPE_SOFT);
  ASSERT_GE(H5Lget_info2(dst_expand.get(), "/expanded/ext_dangling", &info,
                         H5P_DEFAULT),
            0);
  EXPECT_EQ(info.type, H5L_TYPE_EXTERNAL);

  // Attempting to copy again onto existing destination hits H5L_exists_tolerant -> exists,
  // and aborting during committed-datatype copy exercises H5L__link_copy_file cleanup.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Ocopy(src_file.get(), "/bundle", dst_expand.get(), "/expanded",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedFile mem_src = CreateInMemoryFile("cdt_src.h5");
  ASSERT_TRUE(mem_src.valid());
  ScopedFile mem_dst = CreateInMemoryFile("cdt_dst.h5");
  ASSERT_TRUE(mem_dst.valid());
  ScopedGroup cdt_grp(H5Gcreate2(mem_src.get(), "/cdt_grp", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(cdt_grp.valid());
  ScopedType cdt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(cdt.valid());
  ASSERT_GE(H5Tcommit2(cdt_grp.get(), "named_i32", cdt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedPlist abort_ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(abort_ocpypl.valid());
  ASSERT_GE(
      H5Pset_copy_object(abort_ocpypl.get(), H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
      0);
  auto abort_mcdt = [](void* /*op_data*/) -> H5O_mcdt_search_ret_t {
    return H5O_MCDT_SEARCH_ERROR;
  };
  ASSERT_GE(H5Pset_mcdt_search_cb(abort_ocpypl.get(), abort_mcdt, nullptr), 0);
  EXPECT_LT(H5Ocopy(mem_src.get(), "/cdt_grp", mem_dst.get(), "/aborted_grp",
                    abort_ocpypl.get(), H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
