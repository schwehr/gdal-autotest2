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
// Tests internal metadata cache algorithms: auto-resize, age-out eviction,
// epoch marker management, flash cache increases, flush/invalidate rings,
// make-space eviction, and multi-ring serialization
// (third_party/hdf5/src/H5Cint.c). Note: the default google3 non-MPI Linux
// build compiles with `-DNDEBUG` and without `H5_HAVE_PARALLEL`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cint.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` and `H5C_t`. Define NDEBUG around both the prelude and
// internal headers so C++ test callers match `:hdf5` struct layouts.
#ifndef NDEBUG
#define H5CINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACpkg.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#pragma push_macro("try")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5CINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

H5F_t* GetInternalFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// RAII wrapper pushing/popping H5CX context and restoring tag/ring.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000,
                            H5AC_ring_t ring = H5C_RING_USER) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      H5AC_set_ring(ring, &prev_ring_);
      restored_ = true;
    }
  }

  ~ScopedApiContext() {
    if (restored_) {
      H5AC_set_ring(prev_ring_, nullptr);
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  H5AC_ring_t prev_ring_ = H5AC_RING_INV;
  bool pushed_ = false;
  bool restored_ = false;
};

struct TestCacheEntry;

enum class PreSerializeAction {
  kNone,
  kFail,
  kMarkTargetDirty,
  kProtectAndDirtyTarget,
  kInsertExtra,
  kLoadExtra,
  kMoveSelf,
  kExpungeTargets,
  kInsertOversizedForMsic,
};

struct TestCacheEntry {
  H5C_cache_entry_t cache_info;  // Must be first member for H5C casts.
  size_t entry_size;
  int* live_count;
  PreSerializeAction pre_action;
  bool fail_serialize;
  bool fail_before_evict;
  TestCacheEntry* target_entry;
  TestCacheEntry* fd_parent_to_unpin;
  H5F_t* notify_file;
  haddr_t notify_expunge_addr;
  haddr_t extra_addr_1;
  haddr_t extra_addr_2;
  haddr_t move_to_addr;
  size_t extra_size;
  int serialize_calls;
  int pre_serialize_calls;
  int before_evict_calls;
};

const H5C_class_t* TestEntryClass();

TestCacheEntry* MakeEntry(size_t size, int* live_count) {
  auto* entry =
      static_cast<TestCacheEntry*>(std::calloc(1, sizeof(TestCacheEntry)));
  EXPECT_NE(entry, nullptr);
  if (entry != nullptr) {
    entry->entry_size = size;
    entry->live_count = live_count;
    if (live_count != nullptr) {
      ++(*live_count);
    }
  }
  return entry;
}

herr_t TestGetInitialLoadSize(void* /*udata_ptr*/, size_t* image_len_ptr) {
  *image_len_ptr = 64;
  return SUCCEED;
}

void* TestDeserialize(const void* /*image_ptr*/, size_t len, void* udata_ptr,
                      bool* dirty_ptr) {
  auto* live_count = static_cast<int*>(udata_ptr);
  *dirty_ptr = false;
  return MakeEntry(len, live_count);
}

herr_t TestImageLen(const void* thing, size_t* image_len_ptr) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  *image_len_ptr = entry->entry_size;
  return SUCCEED;
}

herr_t TestPreSerialize(H5F_t* f, void* thing, haddr_t /*addr*/,
                        size_t /*len*/, haddr_t* new_addr_ptr,
                        size_t* /*new_len_ptr*/, unsigned* flags_ptr) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  ++entry->pre_serialize_calls;
  *flags_ptr = 0;

  switch (entry->pre_action) {
    case PreSerializeAction::kNone:
      return SUCCEED;

    case PreSerializeAction::kFail:
      entry->pre_action = PreSerializeAction::kNone;
      return FAIL;

    case PreSerializeAction::kMarkTargetDirty: {
      entry->pre_action = PreSerializeAction::kNone;
      if (entry->target_entry != nullptr) {
        return H5C_mark_entry_dirty(entry->target_entry);
      }
      return SUCCEED;
    }

    case PreSerializeAction::kProtectAndDirtyTarget: {
      entry->pre_action = PreSerializeAction::kNone;
      if (entry->extra_addr_1 != HADDR_UNDEF) {
        void* tgt = H5C_protect(f, TestEntryClass(), entry->extra_addr_1,
                                entry->live_count, H5C__NO_FLAGS_SET);
        if (tgt == nullptr) {
          return FAIL;
        }
        return H5C_unprotect(f, entry->extra_addr_1, tgt, H5C__DIRTIED_FLAG);
      }
      return SUCCEED;
    }

    case PreSerializeAction::kInsertExtra: {
      entry->pre_action = PreSerializeAction::kNone;
      TestCacheEntry* extra =
          MakeEntry(entry->extra_size ? entry->extra_size : 128,
                    entry->live_count);
      if (extra == nullptr) {
        return FAIL;
      }
      if (H5C_insert_entry(f, TestEntryClass(), entry->extra_addr_1, extra,
                           H5C__NO_FLAGS_SET) < 0) {
        if (entry->live_count != nullptr) {
          --(*entry->live_count);
        }
        std::free(extra);
        return FAIL;
      }
      return SUCCEED;
    }

    case PreSerializeAction::kLoadExtra: {
      entry->pre_action = PreSerializeAction::kNone;
      if (entry->extra_addr_1 != HADDR_UNDEF) {
        void* loaded = H5C_protect(f, TestEntryClass(), entry->extra_addr_1,
                                   entry->live_count, H5C__NO_FLAGS_SET);
        if (loaded == nullptr) {
          return FAIL;
        }
        return H5C_unprotect(f, entry->extra_addr_1, loaded, H5C__NO_FLAGS_SET);
      }
      return SUCCEED;
    }

    case PreSerializeAction::kInsertOversizedForMsic: {
      entry->pre_action = PreSerializeAction::kNone;
      TestCacheEntry* extra =
          MakeEntry(entry->extra_size ? entry->extra_size : 4096,
                    entry->live_count);
      if (extra == nullptr) {
        return FAIL;
      }
      if (H5C_insert_entry(f, TestEntryClass(), entry->extra_addr_1, extra,
                           H5C__NO_FLAGS_SET) < 0) {
        if (entry->live_count != nullptr) {
          --(*entry->live_count);
        }
        std::free(extra);
        return FAIL;
      }
      return SUCCEED;
    }

    case PreSerializeAction::kMoveSelf: {
      entry->pre_action = PreSerializeAction::kNone;
      *new_addr_ptr = entry->move_to_addr;
      *flags_ptr = H5C__SERIALIZE_MOVED_FLAG;
      return SUCCEED;
    }

    case PreSerializeAction::kExpungeTargets: {
      entry->pre_action = PreSerializeAction::kNone;
      if (entry->extra_addr_1 != HADDR_UNDEF) {
        if (H5C_expunge_entry(f, TestEntryClass(), entry->extra_addr_1,
                              H5C__NO_FLAGS_SET) < 0) {
          return FAIL;
        }
      }
      if (entry->extra_addr_2 != HADDR_UNDEF) {
        if (H5C_expunge_entry(f, TestEntryClass(), entry->extra_addr_2,
                              H5C__NO_FLAGS_SET) < 0) {
          return FAIL;
        }
      }
      return SUCCEED;
    }
  }
  return SUCCEED;
}

herr_t TestSerialize(const H5F_t* /*f*/, void* image_ptr, size_t len,
                     void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  ++entry->serialize_calls;
  if (entry->fail_serialize) {
    entry->fail_serialize = false;
    return FAIL;
  }
  std::memset(image_ptr, 0x5A, len);
  return SUCCEED;
}

herr_t TestNotify(H5C_notify_action_t action, void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  if (action == H5C_NOTIFY_ACTION_BEFORE_EVICT) {
    ++entry->before_evict_calls;
    if (entry->fail_before_evict) {
      entry->fail_before_evict = false;
      return FAIL;
    }
    if (entry->fd_parent_to_unpin != nullptr) {
      TestCacheEntry* parent = entry->fd_parent_to_unpin;
      entry->fd_parent_to_unpin = nullptr;
      if (parent->cache_info.flush_dep_nchildren > 0) {
        if (H5C_destroy_flush_dependency(parent, entry) < 0) {
          return FAIL;
        }
      }
      if (parent->cache_info.is_pinned) {
        if (H5C_unpin_entry(parent) < 0) {
          return FAIL;
        }
      }
    }
    if (entry->notify_file != nullptr && entry->notify_expunge_addr != 0) {
      H5F_t* nf = entry->notify_file;
      haddr_t addr = entry->notify_expunge_addr;
      entry->notify_file = nullptr;
      entry->notify_expunge_addr = 0;
      if (H5C_expunge_entry(nf, TestEntryClass(), addr, H5C__NO_FLAGS_SET) <
          0) {
        return FAIL;
      }
    }
  }
  return SUCCEED;
}

herr_t TestFreeIcr(void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  if (entry->live_count != nullptr) {
    --(*entry->live_count);
  }
  std::free(entry);
  return SUCCEED;
}

herr_t TestFsfSize(const void* thing, hsize_t* fsf_size_ptr) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  *fsf_size_ptr = static_cast<hsize_t>(entry->entry_size);
  return SUCCEED;
}

const H5C_class_t* TestEntryClass() {
  static const H5C_class_t kClass = {
      /*id=*/H5AC_BT_ID,
      /*name=*/"test_cint_entry",
      /*mem_type=*/H5FD_MEM_BTREE,
      /*flags=*/H5C__CLASS_SKIP_READS | H5C__CLASS_SKIP_WRITES,
      /*get_initial_load_size=*/TestGetInitialLoadSize,
      /*get_final_load_size=*/nullptr,
      /*verify_chksum=*/nullptr,
      /*deserialize=*/TestDeserialize,
      /*image_len=*/TestImageLen,
      /*pre_serialize=*/TestPreSerialize,
      /*serialize=*/TestSerialize,
      /*notify=*/TestNotify,
      /*free_icr=*/TestFreeIcr,
      /*fsf_size=*/TestFsfSize,
  };
  return &kClass;
}

// Standalone zero-I/O H5C cache harness backed by minimal H5F_t/H5F_shared_t.
struct StandaloneCache {
  explicit StandaloneCache(size_t max_size = 64 * 1024,
                           size_t min_clean_size = 16 * 1024) {
    class_table[0] = TestEntryClass();
    cache = H5C_create(max_size, min_clean_size, /*max_type_id=*/0, class_table,
                       /*check_write_permitted=*/nullptr,
                       /*write_permitted=*/true, /*log_flush=*/nullptr,
                       /*aux_ptr=*/nullptr);
    shared.cache = cache;
    file.shared = &shared;
  }

  ~StandaloneCache() {
    if (cache != nullptr) {
      cache->close_warning_received = true;
      if (cache->slist_enabled) {
        H5C_set_slist_enabled(cache, false, false);
      }
      H5C_dest(&file);
      cache = nullptr;
    }
  }

  StandaloneCache(const StandaloneCache&) = delete;
  StandaloneCache& operator=(const StandaloneCache&) = delete;

  H5F_t* f() { return &file; }

  int live_entries = 0;
  const H5C_class_t* class_table[1]{};
  H5F_shared_t shared{};
  H5F_t file{};
  H5C_t* cache = nullptr;
};

H5C_auto_size_ctl_t MakeResizeConfig(size_t init_size = 16 * 1024,
                                     size_t min_size = 2048,
                                     size_t max_size = 128 * 1024) {
  H5C_auto_size_ctl_t cfg{};
  cfg.version = H5C__CURR_AUTO_SIZE_CTL_VER;
  cfg.rpt_fcn = nullptr;
  cfg.set_initial_size = true;
  cfg.initial_size = init_size;
  cfg.min_clean_fraction = 0.25;
  cfg.max_size = max_size;
  cfg.min_size = min_size;
  cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  cfg.incr_mode = H5C_incr__threshold;
  cfg.lower_hr_threshold = 0.8;
  cfg.increment = 2.0;
  cfg.apply_max_increment = true;
  cfg.max_increment = 32 * 1024;
  cfg.flash_incr_mode = H5C_flash_incr__add_space;
  cfg.flash_multiple = 2.0;
  cfg.flash_threshold = 0.25;
  cfg.decr_mode = H5C_decr__threshold;
  cfg.upper_hr_threshold = 0.9;
  cfg.decrement = 0.5;
  cfg.apply_max_decrement = true;
  cfg.max_decrement = 16 * 1024;
  cfg.epochs_before_eviction = 2;
  cfg.apply_empty_reserve = true;
  cfg.empty_reserve = 0.1;
  return cfg;
}

struct RptCapture {
  int calls = 0;
  enum H5C_resize_status last_status = in_spec;
  size_t last_old_max = 0;
  size_t last_new_max = 0;
  H5F_t* reenter_file = nullptr;
};

RptCapture* g_rpt_capture = nullptr;

void CaptureResizeRpt(H5C_t* /*cache_ptr*/, int32_t version,
                      double /*hit_rate*/, enum H5C_resize_status status,
                      size_t old_max_cache_size, size_t new_max_cache_size,
                      size_t /*old_min_clean_size*/,
                      size_t /*new_min_clean_size*/) {
  EXPECT_EQ(version, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER);
  if (g_rpt_capture != nullptr) {
    ++g_rpt_capture->calls;
    g_rpt_capture->last_status = status;
    g_rpt_capture->last_old_max = old_max_cache_size;
    g_rpt_capture->last_new_max = new_max_cache_size;
    if (g_rpt_capture->reenter_file != nullptr) {
      H5F_t* reenter_f = g_rpt_capture->reenter_file;
      g_rpt_capture->reenter_file = nullptr;
      EXPECT_GE(H5C__auto_adjust_cache_size(reenter_f, true), 0);
    }
  }
}

void FlushCleanEntryByEnablingSlist(H5F_t* f, H5C_t* cache) {
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  EXPECT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
}

TEST(H5CintAutoAdjustTest, ThresholdResizeModesStatusesAndErrorBranches) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;

  RptCapture rpt{};
  g_rpt_capture = &rpt;
  absl::Cleanup clear_rpt = []() { g_rpt_capture = nullptr; };

  // Disabled resize must fail.
  {
    ScopedErrorSilence silence;
    cache->cache_accesses = H5C__MIN_AR_EPOCH_LENGTH;
    cache->cache_hits = 0;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
  }

  H5C_auto_size_ctl_t cfg = MakeResizeConfig(/*init_size=*/16 * 1024,
                                             /*min_size=*/2048,
                                             /*max_size=*/64 * 1024);
  cfg.rpt_fcn = CaptureResizeRpt;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  // Hit rate < lower_hr_threshold, cache not full -> not_full (and re-entry).
  cache->cache_full = false;
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  rpt.reenter_file = env.f();
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, not_full);

  // Cache full -> increase, clipped by max_increment (32K) and max_size.
  cache->cache_full = true;
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  cfg.increment = 10.0;  // 16K * 10 = 160K -> clip to max_size=64K, then max_incr -> 48K.
  cfg.max_increment = 32 * 1024;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_full = true;
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, increase);
  EXPECT_EQ(rpt.last_new_max, 48 * 1024);
  EXPECT_FALSE(cache->cache_full);

  // Second increase clips to max_size (64K).
  cache->cache_full = true;
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, increase);
  EXPECT_EQ(rpt.last_new_max, 64 * 1024);

  // Already at max_size -> at_max_size.
  cache->cache_full = true;
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, at_max_size);

  // size_increase_possible == false with low hit rate -> increase_disabled.
  cfg.increment = 1.0;  // disables increase while keeping mode threshold
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 20;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, increase_disabled);

  // Threshold decrease: clipped to min_size and max_decrement.
  cfg = MakeResizeConfig(/*init_size=*/64 * 1024, /*min_size=*/4096,
                         /*max_size=*/128 * 1024);
  cfg.rpt_fcn = CaptureResizeRpt;
  cfg.decrement = 0.01;
  cfg.max_decrement = 16 * 1024;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 195;  // 0.975 > 0.9
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, decrease);
  EXPECT_EQ(rpt.last_new_max, 48 * 1024);
  EXPECT_TRUE(cache->size_decreased);

  // Disable max_decrement to hit clip to min_size and then at_min_size.
  cfg.apply_max_decrement = false;
  cfg.set_initial_size = false;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 195;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, decrease);
  EXPECT_EQ(rpt.last_new_max, 4096);

  cache->cache_accesses = 200;
  cache->cache_hits = 195;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, at_min_size);

  // size_decrease_possible == false with high hit rate -> decrease_disabled.
  cfg.decrement = 1.0;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 195;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, decrease_disabled);

  // H5C_decr__off with in_spec hit rate.
  cfg = MakeResizeConfig();
  cfg.decr_mode = H5C_decr__off;
  cfg.rpt_fcn = CaptureResizeRpt;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 180;  // 0.9 >= lower(0.8) -> in_spec
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, in_spec);

  // Internal invariant error branches (saved and restored cleanly).
  {
    ScopedErrorSilence silence;
    cfg = MakeResizeConfig();
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

    const bool saved_inc_poss = cache->size_increase_possible;
    const enum H5C_cache_incr_mode saved_incr_mode = cache->resize_ctl.incr_mode;
    const enum H5C_cache_decr_mode saved_decr_mode = cache->resize_ctl.decr_mode;
    const bool saved_flash_poss = cache->flash_size_increase_possible;
    const enum H5C_cache_flash_incr_mode saved_flash_mode =
        cache->resize_ctl.flash_incr_mode;
    absl::Cleanup restore_cfg = [&]() {
      cache->size_increase_possible = saved_inc_poss;
      cache->resize_ctl.incr_mode = saved_incr_mode;
      cache->resize_ctl.decr_mode = saved_decr_mode;
      cache->flash_size_increase_possible = saved_flash_poss;
      cache->resize_ctl.flash_incr_mode = saved_flash_mode;
    };

    // H5C_incr__off while size_increase_possible == true.
    cache->resize_ctl.incr_mode = H5C_incr__off;
    cache->size_increase_possible = true;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);

    // Invalid incr_mode enum.
    cache->resize_ctl.incr_mode = static_cast<enum H5C_cache_incr_mode>(99);
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
    cache->resize_ctl.incr_mode = saved_incr_mode;
    cache->size_increase_possible = saved_inc_poss;

    // Invalid decr_mode enum.
    cache->resize_ctl.decr_mode = static_cast<enum H5C_cache_decr_mode>(99);
    cache->cache_accesses = 200;
    cache->cache_hits = 170;  // stays in_spec during incr
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
    cache->resize_ctl.decr_mode = saved_decr_mode;

    // Flash mode off / invalid during size increase.
    cache->cache_full = true;
    cache->flash_size_increase_possible = true;
    cache->resize_ctl.flash_incr_mode = H5C_flash_incr__off;
    cache->cache_accesses = 200;
    cache->cache_hits = 20;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);

    cache->cache_full = true;
    cache->resize_ctl.flash_incr_mode =
        static_cast<enum H5C_cache_flash_incr_mode>(99);
    cache->cache_accesses = 200;
    cache->cache_hits = 20;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
  }
}

TEST(H5CintAgeoutTest, AgeoutEvictsFlushesSkipsAndRestartsLruScans) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;

  ScopedApiContext ctx;
  RptCapture rpt{};
  g_rpt_capture = &rpt;
  absl::Cleanup clear_rpt = []() { g_rpt_capture = nullptr; };

  // Start with age_out_with_threshold where size_increase_possible == true and
  // size_decrease_possible == false (keeps resize_enabled == true!).
  H5C_auto_size_ctl_t cfg = MakeResizeConfig(/*init_size=*/64 * 1024,
                                             /*min_size=*/4096,
                                             /*max_size=*/128 * 1024);
  cfg.rpt_fcn = CaptureResizeRpt;
  cfg.incr_mode = H5C_incr__threshold;
  cfg.lower_hr_threshold = 0.8;
  cfg.decr_mode = H5C_decr__age_out_with_threshold;
  cfg.upper_hr_threshold = 1.0;  // size_decrease_possible = false
  cfg.epochs_before_eviction = 2;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  // Epoch 1 inserts marker 0.
  cache->cache_accesses = 200;
  cache->cache_hits = 190;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(cache->epoch_markers_active, 1);

  // Epoch 2 inserts marker 1.
  cache->cache_accesses = 200;
  cache->cache_hits = 190;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(cache->epoch_markers_active, 2);

  // Epoch 3 reaches full markers -> decrease_disabled.
  cache->cache_accesses = 200;
  cache->cache_hits = 190;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(rpt.last_status, decrease_disabled);

  // Enable age_out with threshold cleanly (keeping incr off).
  cfg.incr_mode = H5C_incr__off;
  cfg.upper_hr_threshold = 0.85;
  cfg.epochs_before_eviction = 2;
  cfg.set_initial_size = false;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  // Insert aged entries BEFORE cycling markers:
  // Order inserted -> tail gets oldest entries.
  TestCacheEntry* corked_dirty = MakeEntry(512, &live_entries);
  TestCacheEntry* prefetched_dirty_entry = MakeEntry(512, &live_entries);
  TestCacheEntry* clean_to_evict = MakeEntry(512, &live_entries);
  TestCacheEntry* dirty_to_flush = MakeEntry(512, &live_entries);
  TestCacheEntry* victim_1 = MakeEntry(256, &live_entries);
  TestCacheEntry* victim_2 = MakeEntry(256, &live_entries);
  TestCacheEntry* dirty_trigger_restart = MakeEntry(512, &live_entries);
  dirty_trigger_restart->pre_action = PreSerializeAction::kExpungeTargets;
  dirty_trigger_restart->extra_addr_1 = 0x1050;
  dirty_trigger_restart->extra_addr_2 = 0x1060;

  // Put corked object on tag 0xCAFE.
  ASSERT_GE(H5C_cork(cache, 0xCAFE, H5C__SET_CORK, nullptr), 0);
  {
    ScopedApiContext cork_ctx(0xCAFE, H5C_RING_USER);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1010, corked_dirty,
                               H5C__NO_FLAGS_SET),
              0);
  }
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1020,
                             prefetched_dirty_entry, H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1030, clean_to_evict,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1040, dirty_to_flush,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1050, victim_1,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1060, victim_2,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1070,
                             dirty_trigger_restart, H5C__NO_FLAGS_SET),
            0);

  // Flush cache once so prefetched_dirty_entry, clean_to_evict, and victims are clean.
  FlushCleanEntryByEnablingSlist(env.f(), cache);

  // Re-dirty corked_dirty, dirty_to_flush, and dirty_trigger_restart while leaving
  // clean entries clean behind the next cycled epoch marker.
  void* p = H5C_protect(env.f(), TestEntryClass(), 0x1010, &live_entries,
                        H5C__NO_FLAGS_SET);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x1010, p, H5C__DIRTIED_FLAG), 0);

  p = H5C_protect(env.f(), TestEntryClass(), 0x1040, &live_entries,
                  H5C__NO_FLAGS_SET);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x1040, p, H5C__DIRTIED_FLAG), 0);

  // Protect/unprotect in reverse order so walking from tail (`prev`) hits
  // 0x1070 BEFORE 0x1060 (`prev_ptr`) and 0x1050:
  // LRU [head: 0x1050 -> 0x1060 -> 0x1070 -> 0x1040 -> 0x1010 ... tail].
  p = H5C_protect(env.f(), TestEntryClass(), 0x1070, &live_entries,
                  H5C__NO_FLAGS_SET);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x1070, p, H5C__DIRTIED_FLAG), 0);
  p = H5C_protect(env.f(), TestEntryClass(), 0x1060, &live_entries,
                  H5C__NO_FLAGS_SET);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x1060, p, H5C__NO_FLAGS_SET), 0);
  p = H5C_protect(env.f(), TestEntryClass(), 0x1050, &live_entries,
                  H5C__NO_FLAGS_SET);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x1050, p, H5C__NO_FLAGS_SET), 0);

  // Temporarily mark clean entry as prefetched_dirty.
  prefetched_dirty_entry->cache_info.prefetched_dirty = true;
  absl::Cleanup clear_pf_dirty = [&]() {
    if (prefetched_dirty_entry != nullptr) {
      prefetched_dirty_entry->cache_info.prefetched_dirty = false;
    }
  };

  // Cycle markers by running epochs_before_eviction+1 epochs (with low hit rate
  // first so threshold skips ageout while cycling all markers to LRU head!).
  for (int i = 0; i < 3; ++i) {
    cache->cache_accesses = 200;
    cache->cache_hits = 50;  // < upper_hr_threshold (0.85) -> cycles markers only
    ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  }

  // Trigger remove_excess_markers inside H5C__autoadjust__ageout by lowering
  // epochs_before_eviction directly from 2 to 1.
  cache->resize_ctl.epochs_before_eviction = 1;
  cache->resize_ctl.apply_max_decrement = false;
  cache->resize_ctl.apply_empty_reserve = false;
  cache->cache_accesses = 200;
  cache->cache_hits = 190;  // >= 0.85 -> runs ageout!
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(cache->epoch_markers_active, 1);
  EXPECT_EQ(rpt.last_status, decrease);
  // corked_dirty (0x1010) and prefetched_dirty_entry (0x1020) stayed in cache;
  // clean_to_evict (0x1030) and victims (0x1050, 0x1060) were evicted/expunged;
  // dirty_to_flush (0x1040) and dirty_trigger_restart (0x1070) remain as clean.
  EXPECT_EQ(live_entries, 4);

  // Uncork and flush remaining, restore prefetched_dirty=false.
  prefetched_dirty_entry->cache_info.prefetched_dirty = false;
  prefetched_dirty_entry = nullptr;
  ASSERT_GE(H5C_cork(cache, 0xCAFE, H5C__UNCORK, nullptr), 0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);

  // Test write_permitted == false path (clean evicted, dirty skipped), plus
  // apply_empty_reserve == true and min_size clipping.
  cfg = MakeResizeConfig(/*init_size=*/32 * 1024, /*min_size=*/8192,
                         /*max_size=*/64 * 1024);
  cfg.rpt_fcn = CaptureResizeRpt;
  cfg.incr_mode = H5C_incr__off;
  cfg.decr_mode = H5C_decr__age_out;
  cfg.epochs_before_eviction = 1;
  cfg.apply_max_decrement = true;
  cfg.max_decrement = 4096;
  cfg.apply_empty_reserve = true;
  cfg.empty_reserve = 0.2;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  TestCacheEntry* ro_dirty = MakeEntry(512, &live_entries);
  TestCacheEntry* ro_clean = MakeEntry(512, &live_entries);
  TestCacheEntry* ro_evict_limit_1 = MakeEntry(4096, &live_entries);
  TestCacheEntry* ro_evict_limit_2 = MakeEntry(512, &live_entries);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2010, ro_dirty,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2020, ro_clean,
                             H5C__NO_FLAGS_SET),
            0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);
  // Make ro_dirty dirty again at tail alongside clean entry ro_clean.
  p = H5C_protect(env.f(), TestEntryClass(), 0x2010, &live_entries,
                  H5C__NO_FLAGS_SET);
  ASSERT_GE(H5C_unprotect(env.f(), 0x2010, p, H5C__DIRTIED_FLAG), 0);

  // Insert epoch marker 0.
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), /*write_permitted=*/false), 0);

  // Run ageout with write_permitted == false: evicts ro_clean, skips ro_dirty!
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), /*write_permitted=*/false), 0);

  // Flush ro_dirty cleanly and check apply_max_decrement limit + min_size clip.
  FlushCleanEntryByEnablingSlist(env.f(), cache);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2030,
                             ro_evict_limit_1, H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2040,
                             ro_evict_limit_2, H5C__NO_FLAGS_SET),
            0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);
  // Place marker in front of clean entries by resetting markers to 0 and running 2 epochs.
  ASSERT_GE(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
  // Move entries behind a fresh marker via cycling:
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);  // inserts marker at head
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);  // evicts up to max_decrement!

  // Now shrink until max_cache_size == min_size -> at_min_size branch in ageout.
  cfg.apply_max_decrement = false;
  cfg.set_initial_size = false;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  for (int i = 0; i < 6; ++i) {
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  }
  EXPECT_EQ(rpt.last_status, at_min_size);

  // Test error propagation from H5C__autoadjust__ageout__evict_aged_out_entries
  // when serialize fails, clean BEFORE_EVICT notify fails (rw and ro), and
  // dirty prev_ptr mutation restarts LRU scan.
  {
    ScopedErrorSilence silence;
    cfg.initial_size = 32 * 1024;
    cfg.set_initial_size = true;
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

    TestCacheEntry* fail_clean = MakeEntry(512, &live_entries);
    TestCacheEntry* fail_dirty = MakeEntry(512, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x3010, fail_clean,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x3020, fail_dirty,
                               H5C__NO_FLAGS_SET),
              0);
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    // Marker inserted at head (in front of fail_clean & fail_dirty).
    ASSERT_GE(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);

    // 1. Clean BEFORE_EVICT fail under write_permitted = false.
    fail_clean->fail_before_evict = true;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), false), 0);
    ASSERT_GE(H5C_reset_cache_hit_rate_stats(cache), 0);

    // Reset marker & clean fail_clean/fail_dirty -> test under write_permitted = true.
    ASSERT_GE(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);

    fail_clean->fail_before_evict = true;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
    ASSERT_GE(H5C_reset_cache_hit_rate_stats(cache), 0);

    // Dirty fail_dirty (at tail after reload) -> serialize fails under rw.
    p = H5C_protect(env.f(), TestEntryClass(), 0x3020, &live_entries,
                    H5C__NO_FLAGS_SET);
    ASSERT_NE(p, nullptr);
    ASSERT_GE(H5C_unprotect(env.f(), 0x3020, p, H5C__DIRTIED_FLAG), 0);
    // Keep fail_dirty behind marker by removing all markers and re-inserting one.
    ASSERT_GE(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);

    fail_dirty->fail_serialize = true;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
    ASSERT_GE(H5C_reset_cache_hit_rate_stats(cache), 0);
    // Note: H5C__auto_adjust_cache_size sets resize_in_progress=false on done.
  }

  ASSERT_GE(H5C_evict(env.f()), 0);
  EXPECT_EQ(live_entries, 0);
}

TEST(H5CintEpochMarkerErrorTest, CorruptMarkerStateAndRemoveMarkerErrors) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;

  H5C_auto_size_ctl_t cfg = MakeResizeConfig();
  cfg.decr_mode = H5C_decr__age_out;
  cfg.epochs_before_eviction = 2;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  ScopedErrorSilence silence;

  // remove_excess_markers when no excess markers exist.
  EXPECT_LT(H5C__autoadjust__ageout__remove_excess_markers(cache), 0);

  // Insert 1 marker cleanly.
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  ASSERT_EQ(cache->epoch_markers_active, 1);

  // Test cycle_epoch_marker errors via H5C__auto_adjust_cache_size.
  {
    const int saved_eb = cache->resize_ctl.epochs_before_eviction;
    const int32_t saved_active = cache->epoch_markers_active;
    const int32_t saved_rb_first = cache->epoch_marker_ringbuf_first;
    const int32_t saved_rb_last = cache->epoch_marker_ringbuf_last;
    const int32_t saved_rb_size = cache->epoch_marker_ringbuf_size;
    int saved_rb[H5C__MAX_EPOCH_MARKERS + 1];
    bool saved_marker_active[H5C__MAX_EPOCH_MARKERS];
    std::memcpy(saved_rb, cache->epoch_marker_ringbuf, sizeof(saved_rb));
    std::memcpy(saved_marker_active, cache->epoch_marker_active,
                sizeof(saved_marker_active));

    absl::Cleanup restore_markers = [&]() {
      cache->resize_ctl.epochs_before_eviction = saved_eb;
      cache->epoch_markers_active = saved_active;
      cache->epoch_marker_ringbuf_first = saved_rb_first;
      cache->epoch_marker_ringbuf_last = saved_rb_last;
      cache->epoch_marker_ringbuf_size = saved_rb_size;
      std::memcpy(cache->epoch_marker_ringbuf, saved_rb, sizeof(saved_rb));
      std::memcpy(cache->epoch_marker_active, saved_marker_active,
                  sizeof(saved_marker_active));
    };

    // 1. cycle_epoch_marker: epoch_markers_active <= 0.
    cache->resize_ctl.epochs_before_eviction = 0;
    cache->epoch_markers_active = 0;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);

    // Restore marker 0 state, test ringbuf underflow in cycle_epoch_marker.
    cache->epoch_markers_active = saved_active;
    cache->resize_ctl.epochs_before_eviction = 1;
    cache->epoch_marker_ringbuf_size = 0;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);

    // Test unused marker in LRU during cycle_epoch_marker.
    cache->epoch_marker_ringbuf_first = saved_rb_first;
    cache->epoch_marker_ringbuf_size = 1;
    const int idx = cache->epoch_marker_ringbuf[saved_rb_first];
    cache->epoch_marker_active[idx] = false;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);
    cache->epoch_marker_active[idx] = true;

    // Test remove_all_markers ringbuf underflow and unused marker error.
    cache->epoch_marker_ringbuf_first = saved_rb_first;
    cache->epoch_marker_ringbuf_size = 0;
    EXPECT_LT(H5C__autoadjust__ageout__remove_all_markers(cache), 0);

    cache->epoch_marker_ringbuf_first = saved_rb_first;
    cache->epoch_marker_ringbuf_size = 1;
    cache->epoch_marker_active[idx] = false;
    EXPECT_LT(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
    cache->epoch_marker_active[idx] = true;

    // Test remove_excess_markers ringbuf underflow and unused marker error.
    cache->resize_ctl.epochs_before_eviction = 0;
    cache->epoch_markers_active = 1;
    cache->epoch_marker_ringbuf_first = saved_rb_first;
    cache->epoch_marker_ringbuf_size = 0;
    EXPECT_LT(H5C__autoadjust__ageout__remove_excess_markers(cache), 0);

    cache->epoch_marker_ringbuf_first = saved_rb_first;
    cache->epoch_marker_ringbuf_size = 1;
    cache->epoch_marker_active[idx] = false;
    EXPECT_LT(H5C__autoadjust__ageout__remove_excess_markers(cache), 0);
    cache->epoch_marker_active[idx] = true;

    // Test insert_new_marker: ringbuf_size >= MAX overflow error. Note: we
    // intentionally do NOT set all H5C__MAX_EPOCH_MARKERS elements of
    // epoch_marker_active to true because upstream H5Cint.c checks
    // `epoch_marker_active[i]` before `i < H5C__MAX_EPOCH_MARKERS` (UBSan OOB).
    cache->resize_ctl.epochs_before_eviction = H5C__MAX_EPOCH_MARKERS + 1;
    std::memcpy(cache->epoch_marker_active, saved_marker_active,
                sizeof(saved_marker_active));
    cache->epoch_markers_active = 1;
    cache->epoch_marker_ringbuf_size = H5C__MAX_EPOCH_MARKERS;
    cache->cache_accesses = 200;
    cache->cache_hits = 100;
    EXPECT_LT(H5C__auto_adjust_cache_size(env.f(), true), 0);

    // Note: restore_markers also restores clean LRU state so let's not leave
    // an extra half-unlinked marker. Re-run clean restore -> remove_all_markers!
  }

  ASSERT_GE(H5C__autoadjust__ageout__remove_all_markers(cache), 0);
}

TEST(H5CintFlashIncreaseTest, FlashIncreaseSizingAndErrorBranches) {
  StandaloneCache env(/*max_size=*/16 * 1024, /*min_clean_size=*/4096);
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;

  RptCapture rpt{};
  g_rpt_capture = &rpt;
  absl::Cleanup clear_rpt = []() { g_rpt_capture = nullptr; };

  H5C_auto_size_ctl_t cfg = MakeResizeConfig(/*init_size=*/16 * 1024,
                                             /*min_size=*/4096,
                                             /*max_size=*/40 * 1024);
  cfg.rpt_fcn = CaptureResizeRpt;
  cfg.flash_incr_mode = H5C_flash_incr__add_space;
  cfg.flash_multiple = 2.0;
  cfg.flash_threshold = 0.25;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

  // Add 8K of entries so index_size (8K) < max_cache_size (16K).
  ScopedApiContext ctx;
  TestCacheEntry* e1 = MakeEntry(8 * 1024, &live_entries);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x4000, e1,
                             H5C__NO_FLAGS_SET),
            0);

  // old=0, new=12K -> space_needed=12K; (8K+12K)>16K -> free space=8K subtracted
  // -> added space = 4K * 2.0 = 8K -> new_max = 24K.
  ASSERT_GE(H5C__flash_increase_cache_size(cache, 0, 12 * 1024), 0);
  EXPECT_EQ(rpt.last_status, flash_increase);
  EXPECT_EQ(rpt.last_new_max, 24 * 1024);

  // Request huge entry that clips new_max_cache_size to resize_ctl.max_size (40K).
  ASSERT_GE(H5C__flash_increase_cache_size(cache, 1024, 32 * 1024), 0);
  EXPECT_EQ(cache->max_cache_size, 40 * 1024);

  // No-op when cache already at max_size or enough space.
  const int prev_calls = rpt.calls;
  ASSERT_GE(H5C__flash_increase_cache_size(cache, 1024, 64 * 1024), 0);
  EXPECT_EQ(rpt.calls, prev_calls);

  // Error branches.
  {
    ScopedErrorSilence silence;
    // old_entry_size >= new_entry_size fails.
    EXPECT_LT(H5C__flash_increase_cache_size(cache, 4096, 4096), 0);
    EXPECT_LT(H5C__flash_increase_cache_size(cache, 8192, 4096), 0);

    // flash_incr_mode == off or invalid while flash_size_increase_possible == true.
    cache->max_cache_size = 16 * 1024;
    const enum H5C_cache_flash_incr_mode saved_mode =
        cache->resize_ctl.flash_incr_mode;
    const bool saved_poss = cache->flash_size_increase_possible;
    absl::Cleanup restore = [&]() {
      cache->resize_ctl.flash_incr_mode = saved_mode;
      cache->flash_size_increase_possible = saved_poss;
    };

    cache->flash_size_increase_possible = true;
    cache->resize_ctl.flash_incr_mode = H5C_flash_incr__off;
    EXPECT_LT(H5C__flash_increase_cache_size(cache, 0, 20 * 1024), 0);

    cache->resize_ctl.flash_incr_mode =
        static_cast<enum H5C_cache_flash_incr_mode>(99);
    EXPECT_LT(H5C__flash_increase_cache_size(cache, 0, 20 * 1024), 0);
  }

  ASSERT_GE(H5C_evict(env.f()), 0);
  EXPECT_EQ(live_entries, 0);
}

TEST(H5CintFlushInvalidateTest, MultiRingDependenciesPinnedLastAndRestarts) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;

  // Enable ageout so epoch_markers_active > 0 is also exercised during close/invalidate.
  H5C_auto_size_ctl_t cfg = MakeResizeConfig();
  cfg.decr_mode = H5C_decr__age_out;
  cfg.epochs_before_eviction = 1;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  cache->cache_accesses = 200;
  cache->cache_hits = 100;
  ASSERT_GE(H5C__auto_adjust_cache_size(env.f(), true), 0);
  EXPECT_EQ(cache->epoch_markers_active, 1);

  {
    ScopedApiContext user_ctx(0x1000, H5C_RING_USER);

    // Inner-ring entries (SBE and pinned SB) inserted and flushed clean first so
    // an outer-ring FLUSH_LAST entry sees num_last_entries >= slist_len.
    TestCacheEntry* sbe_entry = MakeEntry(128, &live_entries);
    {
      ScopedApiContext sbe_ctx(0x2000, H5C_RING_SBE);
      ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x3000, sbe_entry,
                                 H5C__NO_FLAGS_SET),
                0);
    }
    TestCacheEntry* sb_pinned = MakeEntry(128, &live_entries);
    {
      ScopedApiContext sb_ctx(0x3000, H5C_RING_SB);
      ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x4000, sb_pinned,
                                 H5C__PIN_ENTRY_FLAG),
                0);
    }
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    // Flush-last dirty entry at low address + regular dirty entry + pinned
    // parent/child + clean pinned entry dirtied via pre_serialize.
    TestCacheEntry* last_entry = MakeEntry(256, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x0500, last_entry,
                               H5C__FLUSH_LAST_FLAG),
              0);
    TestCacheEntry* parent = MakeEntry(256, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1000, parent,
                               H5C__PIN_ENTRY_FLAG),
              0);
    TestCacheEntry* dirty_on_slist = MakeEntry(256, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1500,
                               dirty_on_slist, H5C__NO_FLAGS_SET),
              0);
    TestCacheEntry* clean_pin_target = MakeEntry(256, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1600,
                               clean_pin_target, H5C__PIN_ENTRY_FLAG),
              0);
    TestCacheEntry* child = MakeEntry(256, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2000, child,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_create_flush_dependency(parent, child), 0);

    FlushCleanEntryByEnablingSlist(env.f(), cache);

    // Exercise H5C_flush_cache slist restarts via kMarkTargetDirty & kInsertExtra.
    auto re_dirty = [&](haddr_t addr) {
      void* ptr = H5C_protect(env.f(), TestEntryClass(), addr, &live_entries,
                              H5C__NO_FLAGS_SET);
      ASSERT_NE(ptr, nullptr);
      ASSERT_GE(H5C_unprotect(env.f(), addr, ptr, H5C__DIRTIED_FLAG), 0);
    };
    re_dirty(0x0500);
    ASSERT_GE(H5C_mark_entry_dirty(parent), 0);
    re_dirty(0x1500);
    re_dirty(0x2000);
    dirty_on_slist->pre_action = PreSerializeAction::kMarkTargetDirty;
    dirty_on_slist->target_entry = clean_pin_target;
    child->pre_action = PreSerializeAction::kInsertExtra;
    child->extra_addr_1 = 0x2600;
    child->extra_size = 64;

    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    ASSERT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_unpin_entry(clean_pin_target), 0);
    ASSERT_GE(H5C_destroy_flush_dependency(parent, child), 0);

    // Exercise H5C_evict across multi-pass pinned unpins, dirty pinned slist
    // restart, dirty unpinned slist restart, il restart (watched_for_removal),
    // epoch marker removal, SBE destruction, and ALLOW_LAST_PINS on SB ring:
    //  1) Dirty pinned `parent` (0x1000, no FD children) flushes in slist loop
    //     at `is_pinned` and triggers slist_changed via 0x2700.
    //  2) Dirty unpinned `child` (0x2000) inserts 0x2800 (`slist_changed` in
    //     unpinned slist branch) and unpins `parent` during BEFORE_EVICT.
    //  3) 2-level clean FD chain (fd_parent <- fd_mid <- fd_leaf) reduces
    //     cur_ring_pel_len from 2 -> 1 -> 0 across passes.
    //  4) `il_trigger` (0x2400) expunges `il_victim` (0x2500) during BEFORE_EVICT
    //     so `entry_watched_for_removal` resets to NULL and restarts `il` scan.
    TestCacheEntry* fd_parent = MakeEntry(128, &live_entries);
    TestCacheEntry* fd_mid = MakeEntry(128, &live_entries);
    TestCacheEntry* fd_leaf = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1100, fd_parent,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1200, fd_mid,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x1300, fd_leaf,
                               H5C__NO_FLAGS_SET),
              0);
    TestCacheEntry* il_victim = MakeEntry(128, &live_entries);
    TestCacheEntry* il_trigger = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2500, il_victim,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x2400, il_trigger,
                               H5C__NO_FLAGS_SET),
              0);
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    ASSERT_GE(H5C_create_flush_dependency(fd_parent, fd_mid), 0);
    ASSERT_GE(H5C_create_flush_dependency(fd_mid, fd_leaf), 0);
    fd_leaf->fd_parent_to_unpin = fd_mid;
    fd_mid->fd_parent_to_unpin = fd_parent;
    il_trigger->notify_file = env.f();
    il_trigger->notify_expunge_addr = 0x2500;

    re_dirty(0x0500);
    ASSERT_GE(H5C_mark_entry_dirty(parent), 0);
    parent->pre_action = PreSerializeAction::kInsertExtra;
    parent->extra_addr_1 = 0x2700;
    parent->extra_size = 64;

    re_dirty(0x2000);
    child->fd_parent_to_unpin = parent;
    child->pre_action = PreSerializeAction::kInsertExtra;
    child->extra_addr_1 = 0x2800;
    child->extra_size = 64;

    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(cache->epoch_markers_active, 0);
    EXPECT_EQ(cache->index_ring_len[H5C_RING_USER], 0);
    EXPECT_EQ(live_entries, 1);  // Only sb_pinned remains in SB ring!

    ASSERT_GE(H5C_unpin_entry(sb_pinned), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);
  }
}

TEST(H5CintFlushInvalidateTest, FlushInvalidateClearOnlyAndErrorPaths) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;
  ScopedApiContext ctx(0x1000, H5C_RING_USER);

  // 1. H5C__FLUSH_CLEAR_ONLY_FLAG discards dirty entry without calling serialize.
  TestCacheEntry* dirty_cleared = MakeEntry(128, &live_entries);
  dirty_cleared->fail_serialize = true;
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5010, dirty_cleared,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(
                env.f(), H5C__FLUSH_INVALIDATE_FLAG | H5C__FLUSH_CLEAR_ONLY_FLAG),
            0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  EXPECT_EQ(live_entries, 0);

  // 2. Pinned entry count not decreasing fails when ALLOW_LAST_PINS is unset.
  {
    ScopedErrorSilence silence;
    TestCacheEntry* stuck_pin = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5020, stuck_pin,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_unpin_entry(stuck_pin), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);
  }

  // 3. Single clean protected entry fails in index scan (protected == index_len).
  {
    ScopedErrorSilence silence;
    TestCacheEntry* clean_prot = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5030, clean_prot,
                               H5C__NO_FLAGS_SET),
              0);
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    void* prot = H5C_protect(env.f(), TestEntryClass(), 0x5030, &live_entries,
                             H5C__NO_FLAGS_SET);
    ASSERT_NE(prot, nullptr);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);

    ASSERT_GE(H5C_unprotect(env.f(), 0x5030, prot, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);
  }

  // 4. Dirty protected entry in USER + pinned entry in SB hits slist protected
  //    count and terminates cleanly with protected_entries == index_len.
  {
    ScopedErrorSilence silence;
    TestCacheEntry* dirty_prot = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5040, dirty_prot,
                               H5C__NO_FLAGS_SET),
              0);
    TestCacheEntry* sb_pin = MakeEntry(128, &live_entries);
    {
      ScopedApiContext sb_ctx(0x2000, H5C_RING_SB);
      ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5050, sb_pin,
                                 H5C__PIN_ENTRY_FLAG),
                0);
    }
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    void* prot = H5C_protect(env.f(), TestEntryClass(), 0x5040, &live_entries,
                             H5C__NO_FLAGS_SET);
    ASSERT_NE(prot, nullptr);
    ASSERT_GE(H5C_unprotect(env.f(), 0x5040, prot, H5C__DIRTIED_FLAG), 0);
    prot = H5C_protect(env.f(), TestEntryClass(), 0x5040, &live_entries,
                       H5C__NO_FLAGS_SET);
    ASSERT_NE(prot, nullptr);

    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_unprotect(env.f(), 0x5040, prot, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_unpin_entry(sb_pin), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);
  }

  // 5. Serialize / BEFORE_EVICT failure in pinned slist, unpinned slist, and il scan.
  {
    ScopedErrorSilence silence;

    // 5a: dirty pinned entry fails serialize in slist loop.
    TestCacheEntry* pin_fail = MakeEntry(128, &live_entries);
    pin_fail->fail_serialize = true;
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5060, pin_fail,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_unpin_entry(pin_fail), 0);
    EXPECT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);

    // 5b: dirty unpinned entry fails serialize in slist loop.
    TestCacheEntry* unpin_fail = MakeEntry(128, &live_entries);
    unpin_fail->fail_serialize = true;
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5070, unpin_fail,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    // Reset failure and flush cleanly.
    EXPECT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);

    // 5c: clean unpinned entry fails BEFORE_EVICT in index scan loop + il restart
    //     when entry_watched_for_removal is expunged.
    TestCacheEntry* next_clean = MakeEntry(128, &live_entries);
    TestCacheEntry* evict_fail_clean = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5080,
                               evict_fail_clean, H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5090, next_clean,
                               H5C__NO_FLAGS_SET),
              0);
    FlushCleanEntryByEnablingSlist(env.f(), cache);

    evict_fail_clean->fail_before_evict = true;
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);
  }
}

TEST(H5CintFlushRingTest, MultiPassDepsFlushMeLastProtectedIgnoreAndRestarts) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;
  ScopedApiContext ctx(0x1000, H5C_RING_USER);

  // Dirty pinned parent (0x6000) <- dirty child (0x6100), plus flush_me_last
  // (0x5900), plus clean pinned entry (0x6200) dirtied during child's pre_serialize.
  TestCacheEntry* last_dirty = MakeEntry(128, &live_entries);
  TestCacheEntry* parent = MakeEntry(128, &live_entries);
  TestCacheEntry* child = MakeEntry(128, &live_entries);
  TestCacheEntry* clean_pin = MakeEntry(128, &live_entries);

  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x5900, last_dirty,
                             H5C__FLUSH_LAST_FLAG),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x6000, parent,
                             H5C__PIN_ENTRY_FLAG),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x6100, child,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x6200, clean_pin,
                             H5C__PIN_ENTRY_FLAG),
            0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);

  auto re_dirty = [&](haddr_t addr) {
    void* p =
        H5C_protect(env.f(), TestEntryClass(), addr, &live_entries, 0);
    ASSERT_NE(p, nullptr);
    ASSERT_GE(H5C_unprotect(env.f(), addr, p, H5C__DIRTIED_FLAG), 0);
  };
  re_dirty(0x5900);
  re_dirty(0x6000);
  re_dirty(0x6100);
  ASSERT_GE(H5C_create_flush_dependency(parent, child), 0);

  // Trigger slist_changed when child flushes by marking clean_pin dirty.
  child->pre_action = PreSerializeAction::kMarkTargetDirty;
  child->target_entry = clean_pin;

  // Protected clean entry with H5C__FLUSH_IGNORE_PROTECTED_FLAG -> SUCCEED.
  TestCacheEntry* clean_prot = MakeEntry(128, &live_entries);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x6300, clean_prot,
                             H5C__NO_FLAGS_SET),
            0);
  {
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    ASSERT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    EXPECT_FALSE(parent->cache_info.is_dirty);
    EXPECT_FALSE(child->cache_info.is_dirty);
    EXPECT_FALSE(last_dirty->cache_info.is_dirty);
    EXPECT_FALSE(clean_pin->cache_info.is_dirty);
  }

  ASSERT_GE(H5C_destroy_flush_dependency(parent, child), 0);
  ASSERT_GE(H5C_unpin_entry(parent), 0);
  ASSERT_GE(H5C_unpin_entry(clean_pin), 0);

  // Protect clean 0x6300 while another entry (0x6000) is dirty.
  re_dirty(0x6000);
  void* prot_clean_ptr =
      H5C_protect(env.f(), TestEntryClass(), 0x6300, &live_entries, 0);
  ASSERT_NE(prot_clean_ptr, nullptr);

  {
    ScopedErrorSilence silence;
    // Without IGNORE_PROTECTED_FLAG -> fails due to pl_len > 0!
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  }

  // With H5C__FLUSH_IGNORE_PROTECTED_FLAG -> SUCCEEDS!
  re_dirty(0x6000);
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  EXPECT_GE(H5C_flush_cache(env.f(), H5C__FLUSH_IGNORE_PROTECTED_FLAG), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  ASSERT_GE(H5C_unprotect(env.f(), 0x6300, prot_clean_ptr, H5C__NO_FLAGS_SET),
            0);

  // Dirty protected entry ALWAYS fails (even with IGNORE_PROTECTED_FLAG), and
  // serialize failure fails flush_ring.
  {
    ScopedErrorSilence silence;
    TestCacheEntry* dirty_prot = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x6400, dirty_prot,
                               H5C__NO_FLAGS_SET),
              0);
    void* dp =
        H5C_protect(env.f(), TestEntryClass(), 0x6400, &live_entries, 0);
    ASSERT_NE(dp, nullptr);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__FLUSH_IGNORE_PROTECTED_FLAG), 0);
    ASSERT_GE(H5C_unprotect(env.f(), 0x6400, dp, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

    // Serialize failure propagates from H5C__flush_ring.
    re_dirty(0x6400);
    dirty_prot->fail_serialize = true;
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    EXPECT_LT(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    EXPECT_GE(H5C_flush_cache(env.f(), H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  }

  ASSERT_GE(H5C_evict(env.f()), 0);
  EXPECT_EQ(live_entries, 0);
}

TEST(H5CintMakeSpaceTest, EvictFlushSkipRestartReentrantAndReadOnly) {
  StandaloneCache env(/*max_size=*/2048, /*min_clean_size=*/1024);
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;
  ScopedApiContext ctx(0x1000, H5C_RING_USER);

  // Populate clean + dirty entries and make space via direct H5C__make_space_in_cache.
  ASSERT_GE(H5C_cork(cache, 0xBEEF, H5C__SET_CORK, nullptr), 0);

  TestCacheEntry* e_corked = MakeEntry(256, &live_entries);
  TestCacheEntry* e_pf_dirty = MakeEntry(256, &live_entries);
  TestCacheEntry* e_clean_skip = MakeEntry(256, &live_entries);
  TestCacheEntry* e_dirty_flush = MakeEntry(256, &live_entries);
  TestCacheEntry* e_reenter = MakeEntry(256, &live_entries);

  {
    ScopedApiContext cctx(0xBEEF, H5C_RING_USER);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7010, e_corked,
                               H5C__NO_FLAGS_SET),
              0);
  }
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7020, e_pf_dirty,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7030, e_clean_skip,
                             H5C__NO_FLAGS_SET),
            0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);

  // Keep e_corked dirty; mark e_pf_dirty as prefetched_dirty.
  void* p = H5C_protect(env.f(), TestEntryClass(), 0x7010, &live_entries, 0);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x7010, p, H5C__DIRTIED_FLAG), 0);
  e_pf_dirty->cache_info.prefetched_dirty = true;

  // Insert e_dirty_flush (whose pre_serialize re-enters msic via insert of 0x7050).
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7040, e_dirty_flush,
                             H5C__NO_FLAGS_SET),
            0);
  e_dirty_flush->pre_action = PreSerializeAction::kInsertOversizedForMsic;
  e_dirty_flush->extra_addr_1 = 0x7050;
  e_dirty_flush->extra_size = 512;

  // empty_space (1024) + clean_index_size (512) = 1536. Raise min_clean_size to
  // 1792 so msic must flush e_dirty_flush while skipping e_pf_dirty, e_clean_skip,
  // and corked e_corked.
  cache->min_clean_size = 1792;
  ASSERT_GE(H5C__make_space_in_cache(env.f(), 0, /*write_permitted=*/true), 0);
  EXPECT_FALSE(e_dirty_flush->cache_info.is_dirty);
  e_pf_dirty->cache_info.prefetched_dirty = false;
  ASSERT_GE(H5C_cork(cache, 0xBEEF, H5C__UNCORK, nullptr), 0);
  cache->min_clean_size = 512;

  // Clean everything up, add a clean tail entry, and insert dirty e_reenter
  // immediately behind 0x7030 at LRU head so kProtectAndDirtyTarget mutates
  // prev_ptr->is_dirty and restarts the msic LRU scan.
  FlushCleanEntryByEnablingSlist(env.f(), cache);
  TestCacheEntry* clean_tail = MakeEntry(128, &live_entries);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7060, clean_tail,
                             H5C__NO_FLAGS_SET),
            0);
  FlushCleanEntryByEnablingSlist(env.f(), cache);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7070, e_reenter,
                             H5C__NO_FLAGS_SET),
            0);
  // Touch clean 0x7030 so it moves to LRU_head_ptr (just in front of 0x7070!).
  p = H5C_protect(env.f(), TestEntryClass(), 0x7030, &live_entries, 0);
  ASSERT_NE(p, nullptr);
  ASSERT_GE(H5C_unprotect(env.f(), 0x7030, p, H5C__NO_FLAGS_SET), 0);

  e_reenter->pre_action = PreSerializeAction::kProtectAndDirtyTarget;
  e_reenter->extra_addr_1 = 0x7030;
  ASSERT_GE(H5C__make_space_in_cache(env.f(), 1600, /*write_permitted=*/true), 0);

  // Test write_permitted == false branch cleanly.
  ASSERT_GE(H5C__make_space_in_cache(env.f(), 1024, /*write_permitted=*/false),
            0);

  // Test H5C__make_space_in_cache error paths (dirty flush fail & clean evict fail).
  {
    ScopedErrorSilence silence;
    ASSERT_GE(H5C_evict(env.f()), 0);
    EXPECT_EQ(live_entries, 0);

    TestCacheEntry* fail_d = MakeEntry(1536, &live_entries);
    fail_d->fail_serialize = true;
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7100, fail_d,
                               H5C__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5C__make_space_in_cache(env.f(), 1024, /*write_permitted=*/true),
              0);
    ASSERT_GE(H5C_evict(env.f()), 0);

    TestCacheEntry* fail_c = MakeEntry(1536, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x7110, fail_c,
                               H5C__NO_FLAGS_SET),
              0);
    FlushCleanEntryByEnablingSlist(env.f(), cache);
    fail_c->fail_before_evict = true;
    EXPECT_LT(H5C__make_space_in_cache(env.f(), 1024, /*write_permitted=*/true),
              0);
  }

  ASSERT_GE(H5C_evict(env.f()), 0);
  EXPECT_EQ(live_entries, 0);
}

TEST(H5CintSerializeTest, StandaloneMultiRingDepsAndRestarts) {
  StandaloneCache env;
  ASSERT_NE(env.cache, nullptr);
  H5C_t* cache = env.cache;
  int& live_entries = env.live_entries;

  cache->close_warning_received = true;
  cache->rdfsm_settled = true;
  cache->mdfsm_settled = true;

  ScopedApiContext user_ctx(0x1000, H5C_RING_USER);
  TestCacheEntry* parent = MakeEntry(128, &live_entries);
  TestCacheEntry* child = MakeEntry(128, &live_entries);

  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x8100, parent,
                             H5C__PIN_ENTRY_FLAG),
            0);
  ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x8200, child,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_create_flush_dependency(parent, child), 0);

  // Trigger index scan restart (entries_loaded_counter > 0) during child
  // serialization, and validate multi-ring + flush_me_last ordering.
  child->pre_action = PreSerializeAction::kLoadExtra;
  child->extra_addr_1 = 0x8300;

  TestCacheEntry* last_entry = MakeEntry(128, &live_entries);
  {
    // Place the FLUSH_LAST entry in the innermost ring (H5C_RING_SB, matching
    // HDF5's superblock invariant) so H5C_evict can flush USER entries first.
    ScopedApiContext sb_ctx(0x2000, H5C_RING_SB);
    TestCacheEntry* sb_entry = MakeEntry(128, &live_entries);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x8400, sb_entry,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_insert_entry(env.f(), TestEntryClass(), 0x8000, last_entry,
                               H5C__FLUSH_LAST_FLAG),
              0);
  }

  ASSERT_GE(H5C__serialize_cache(env.f()), 0);
  EXPECT_TRUE(child->cache_info.image_up_to_date);
  EXPECT_TRUE(parent->cache_info.image_up_to_date);
  EXPECT_TRUE(last_entry->cache_info.image_up_to_date);
  EXPECT_FALSE(cache->serialization_in_progress);

  ASSERT_GE(H5C_destroy_flush_dependency(parent, child), 0);
  ASSERT_GE(H5C_unpin_entry(parent), 0);

  // Re-mark entries unserialized to test error branches in serialize_ring:
  // (a) main loop serialize failure, (b) flush_me_last triggering restart
  // via entries_loaded_counter, (c) flush_me_last serialize failure.
  {
    ScopedErrorSilence silence;
    auto reset_unserialized = [&](haddr_t addr) {
      void* p =
          H5C_protect(env.f(), TestEntryClass(), addr, &live_entries, 0);
      ASSERT_NE(p, nullptr);
      ASSERT_GE(H5C_mark_entry_dirty(p), 0);
      ASSERT_GE(H5C_unprotect(env.f(), addr, p, H5C__DIRTIED_FLAG), 0);
    };

    reset_unserialized(0x8200);
    child->fail_serialize = true;
    EXPECT_LT(H5C__serialize_cache(env.f()), 0);
    EXPECT_FALSE(cache->serialization_in_progress);

    // Next call serializes 0x8200 cleanly; 0x8000 triggers restart error!
    {
      ScopedApiContext sb_ctx(0x2000, H5C_RING_SB);
      reset_unserialized(0x8000);
      last_entry->pre_action = PreSerializeAction::kLoadExtra;
      last_entry->extra_addr_1 = 0x8500;
      EXPECT_LT(H5C__serialize_cache(env.f()), 0);

      // Now test flush_me_last serialization failure.
      reset_unserialized(0x8000);
      last_entry->fail_serialize = true;
      EXPECT_LT(H5C__serialize_cache(env.f()), 0);
    }
  }

  ASSERT_GE(H5C_evict(env.f()), 0);
  EXPECT_EQ(live_entries, 0);
}

TEST(H5CintSerializeTest, RealFileCacheImageAndFsmSettle) {
  // Real Core VFD file with persistent FSMs + MDC cache image enabled:
  // closing cleanly invokes H5C_prep_for_file_close -> H5C__serialize_cache
  // and exercises H5MF_settle_raw_data_fsm & H5MF_settle_meta_data_fsm.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_core(fapl.get(), /*increment=*/65536,
                             /*backing_store=*/false),
            0);
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                 H5F_LIBVER_LATEST),
            0);
  H5AC_cache_image_config_t img_cfg = {
      H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION,
      true,
      false,
      H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE,
  };
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                       H5F_FSPACE_STRATEGY_FSM_AGGR,
                                       /*persist=*/true, /*threshold=*/1),
            0);

  ScopedFile file(H5Fcreate("cint_serialize_real.h5", H5F_ACC_TRUNC,
                            fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  grp.reset();
  ASSERT_GE(H5Ldelete(file.get(), "/g", H5P_DEFAULT), 0);

  ASSERT_NE(GetInternalFile(file.get()), nullptr);
  EXPECT_GE(H5Fclose(file.release()), 0);  // Triggers H5C__serialize_cache.
}

}  // namespace
}  // namespace hdf5
