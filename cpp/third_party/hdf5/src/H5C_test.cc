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
// Tests metadata cache lifecycle, flushing, eviction, auto-resize validation
// and configuration, ring management, and corking (third_party/hdf5/src/H5C.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL. Internal headers are wrapped with matching NDEBUG so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t struct layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5C.c

#ifndef NDEBUG
#define H5C_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5C_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5C_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

struct SyntheticEntry {
  H5C_cache_entry_t header;
  size_t entry_size;
  bool fail_serialize;
};

constexpr size_t kDefaultEntrySize = 1024;
constexpr haddr_t kBaseSyntheticAddr = 0x10000;

herr_t SyntheticGetInitialLoadSize(void* /*udata*/, size_t* image_len_ptr) {
  *image_len_ptr = kDefaultEntrySize;
  return 0;
}

void* SyntheticDeserialize(const void* /*image*/, size_t len, void* /*udata*/,
                           bool* dirty_ptr) {
  auto* entry = static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  if (entry == nullptr) {
    return nullptr;
  }
  entry->entry_size = len;
  entry->fail_serialize = false;
  *dirty_ptr = false;
  return entry;
}

herr_t SyntheticImageLen(const void* thing, size_t* image_len_ptr) {
  const auto* entry = static_cast<const SyntheticEntry*>(thing);
  *image_len_ptr = entry->entry_size;
  return 0;
}

herr_t SyntheticPreSerialize(H5F_t* /*f*/, void* /*thing*/, haddr_t /*addr*/,
                             size_t /*len*/, haddr_t* /*new_addr_ptr*/,
                             size_t* /*new_len_ptr*/, unsigned* flags_ptr) {
  *flags_ptr = H5C__SERIALIZE_NO_FLAGS_SET;
  return 0;
}

herr_t SyntheticSerialize(const H5F_t* /*f*/, void* image_ptr, size_t len,
                          void* thing) {
  const auto* entry = static_cast<const SyntheticEntry*>(thing);
  if (entry->fail_serialize) {
    return -1;
  }
  std::memset(image_ptr, 0, len);
  return 0;
}

herr_t SyntheticFreeIcr(void* thing) {
  std::free(thing);
  return 0;
}

const H5C_class_t kSyntheticClass = {
    /*id=*/0,
    /*name=*/"synthetic_test_entry",
    /*mem_type=*/H5FD_MEM_OHDR,
    /*flags=*/H5C__CLASS_SKIP_READS | H5C__CLASS_SKIP_WRITES,
    /*get_initial_load_size=*/SyntheticGetInitialLoadSize,
    /*get_final_load_size=*/nullptr,
    /*verify_chksum=*/nullptr,
    /*deserialize=*/SyntheticDeserialize,
    /*image_len=*/SyntheticImageLen,
    /*pre_serialize=*/SyntheticPreSerialize,
    /*serialize=*/SyntheticSerialize,
    /*notify=*/nullptr,
    /*free_icr=*/SyntheticFreeIcr,
    /*fsf_size=*/nullptr,
};

struct WritePermittedState {
  herr_t rc = 0;
  bool permitted = true;
};

herr_t CheckWritePermittedFromAux(const H5F_t* f, bool* write_permitted) {
  const auto* state =
      static_cast<const WritePermittedState*>(H5C_get_aux_ptr(f->shared->cache));
  if (state->rc < 0) {
    return -1;
  }
  *write_permitted = state->permitted;
  return 0;
}

herr_t TestLogFlush(H5C_t* /*cache_ptr*/, haddr_t /*addr*/, bool /*was_dirty*/,
                    unsigned /*flags*/) {
  return 0;
}

// Pushes a default HDF5 internal API context with a valid user ring & tag so
// direct H5C_* entry/protect calls satisfy ring and tagging preconditions.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__COPIED_TAG) : prev_tag_(HADDR_UNDEF) {
    std::memset(&node_, 0, sizeof(node_));
    pushed_ = (H5CX_push(&node_) >= 0);
    if (pushed_) {
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      haddr_t unused = HADDR_UNDEF;
      H5AC_tag(prev_tag_, &unused);
    }
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5C_auto_size_ctl_t MakeValidResizeConfig() {
  H5C_auto_size_ctl_t cfg{};
  cfg.version = H5C__CURR_AUTO_SIZE_CTL_VER;
  cfg.rpt_fcn = nullptr;
  cfg.set_initial_size = true;
  cfg.initial_size = 64 * 1024;
  cfg.min_clean_fraction = 0.25;
  cfg.max_size = 128 * 1024;
  cfg.min_size = 16 * 1024;
  cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  cfg.incr_mode = H5C_incr__threshold;
  cfg.lower_hr_threshold = 0.80;
  cfg.increment = 1.5;
  cfg.apply_max_increment = true;
  cfg.max_increment = 32 * 1024;
  cfg.flash_incr_mode = H5C_flash_incr__off;
  cfg.flash_multiple = 1.0;
  cfg.flash_threshold = 0.25;
  cfg.decr_mode = H5C_decr__threshold;
  cfg.upper_hr_threshold = 0.95;
  cfg.decrement = 0.9;
  cfg.apply_max_decrement = true;
  cfg.max_decrement = 16 * 1024;
  cfg.epochs_before_eviction = 3;
  cfg.apply_empty_reserve = true;
  cfg.empty_reserve = 0.1;
  return cfg;
}

const H5C_class_t* const* GetSyntheticClassTable() {
  static const H5C_class_t* kTable[H5AC_NTYPES] = {
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, &kSyntheticClass,
      &kSyntheticClass, &kSyntheticClass, &kSyntheticClass, H5AC_EPOCH_MARKER,
      &kSyntheticClass, &kSyntheticClass,
  };
  return kTable;
}

H5F_t* GetFileStruct(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Temporarily replaces f->shared->cache with an isolated synthetic H5C_t and
// guarantees leak-free teardown (including releasing any non-null entries) and
// restoration of the file's original cache before H5Fclose runs.
class CustomCacheHarness {
 public:
  CustomCacheHarness(hid_t file_id, size_t max_size, size_t min_clean,
                     H5C_write_permitted_func_t write_cb, bool write_permitted,
                     void* aux_ptr)
      : f_(GetFileStruct(file_id)) {
    if (f_ == nullptr || f_->shared == nullptr) {
      return;
    }
    orig_cache_ = f_->shared->cache;
    cache_ = H5C_create(max_size, min_clean, H5AC_NTYPES - 1,
                        GetSyntheticClassTable(), write_cb, write_permitted,
                        TestLogFlush, aux_ptr);
    if (cache_ != nullptr) {
      f_->shared->cache = cache_;
    }
  }

  ~CustomCacheHarness() {
    if (f_ == nullptr || f_->shared == nullptr) {
      return;
    }
    if (cache_ != nullptr) {
      f_->shared->cache = cache_;
      // Clear fail flags and flush/destroy any remaining entries safely.
      while (cache_->pl_head_ptr != nullptr) {
        H5C_cache_entry_t* prot = cache_->pl_head_ptr;
        H5C_unprotect(f_, prot->addr, prot, H5C__NO_FLAGS_SET);
      }
      for (H5C_cache_entry_t* cur = cache_->il_head; cur != nullptr;
           cur = cur->il_next) {
        if (cur->type == &kSyntheticClass) {
          reinterpret_cast<SyntheticEntry*>(cur)->fail_serialize = false;
        }
      }
      cache_->close_warning_received = true;
      if (!cache_->slist_enabled) {
        H5C_set_slist_enabled(cache_, true, true);
      }
      H5C_flush_cache(f_, H5C__FLUSH_INVALIDATE_FLAG);
      if (cache_->slist_enabled) {
        H5C_set_slist_enabled(cache_, false, false);
      }
      H5C_dest(f_);
      cache_ = nullptr;
    }
    f_->shared->cache = orig_cache_;
  }

  CustomCacheHarness(const CustomCacheHarness&) = delete;
  CustomCacheHarness& operator=(const CustomCacheHarness&) = delete;

  bool valid() const { return f_ != nullptr && cache_ != nullptr; }
  H5F_t* file() const { return f_; }
  H5C_t* cache() const { return cache_; }

  // Marks cache_ already destroyed after testing H5C_dest directly.
  void ReleaseAfterDest() {
    cache_ = nullptr;
    if (f_ != nullptr && f_->shared != nullptr) {
      f_->shared->cache = orig_cache_;
    }
  }

 private:
  H5F_t* f_ = nullptr;
  H5C_t* orig_cache_ = nullptr;
  H5C_t* cache_ = nullptr;
};

TEST(H5CTest, CreateAndDestCoverTagListAndFailureRecoveryPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;

  // 1. H5C_dest cleans up corked tag entries and destroys an empty cache.
  {
    CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                               nullptr);
    ASSERT_TRUE(harness.valid());
    H5C_t* cache = harness.cache();
    EXPECT_EQ(nullptr, H5C_get_aux_ptr(cache));

    ASSERT_GE(H5C_cork(cache, 0x2000, H5C__SET_CORK, nullptr), 0);
    cache->close_warning_received = true;
    EXPECT_GE(H5C_dest(harness.file()), 0);
    harness.ReleaseAfterDest();
  }

  // 2. H5C_dest fails at slist enable (slist already enabled with dirty entry)
  //    and also fails to disable non-empty slist in the done cleanup handler.
  {
    CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                               nullptr);
    ASSERT_TRUE(harness.valid());
    H5C_t* cache = harness.cache();

    auto* entry =
        static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
    ASSERT_NE(nullptr, entry);
    entry->entry_size = kDefaultEntrySize;
    ASSERT_GE(H5C_insert_entry(harness.file(), &kSyntheticClass,
                               kBaseSyntheticAddr, entry, H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    cache->close_warning_received = true;

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_dest(harness.file()), 0);
    }

    // Drain dirty entry and destroy normally.
    EXPECT_GE(H5C_flush_cache(harness.file(), H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
    EXPECT_GE(H5C_dest(harness.file()), 0);
    harness.ReleaseAfterDest();
  }

  // 3. H5C_dest fails at flush_invalidate due to a protected entry and
  //    disables slist on exit.
  {
    CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                               nullptr);
    ASSERT_TRUE(harness.valid());
    H5C_t* cache = harness.cache();

    void* thing = H5C_protect(harness.file(), &kSyntheticClass,
                              kBaseSyntheticAddr + 0x1000, nullptr,
                              H5C__NO_FLAGS_SET);
    ASSERT_NE(nullptr, thing);
    cache->close_warning_received = true;

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_dest(harness.file()), 0);
    }
    EXPECT_FALSE(cache->slist_enabled);

    EXPECT_GE(H5C_unprotect(harness.file(), kBaseSyntheticAddr + 0x1000, thing,
                            H5C__NO_FLAGS_SET),
              0);
    EXPECT_GE(H5C_dest(harness.file()), 0);
    harness.ReleaseAfterDest();
  }
}

TEST(H5CTest, CacheImageGenerationOnCloseExecutesDestImagePath) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
            0);

  H5AC_cache_image_config_t img_cfg{};
  img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  img_cfg.generate_image = true;
  img_cfg.save_resize_status = false;
  img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

  ScopedFile file(H5Fcreate("cache_image_in_mem.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  grp.reset();
  // Closing generates the cache image inside H5C_prep_for_file_close / H5C_dest.
  file.reset();
}

TEST(H5CTest, PrepForFileCloseHandlesDuplicateCallsAndImageLoadFailure) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  H5F_t* f = GetFileStruct(file.get());
  ASSERT_NE(nullptr, f);
  H5C_t* cache = f->shared->cache;
  ASSERT_NE(nullptr, cache);

  auto reset_flags = absl::MakeCleanup([&]() {
    cache->close_warning_received = false;
    cache->load_image = false;
    cache->delete_image = false;
    cache->image_addr = HADDR_UNDEF;
    cache->image_len = 0;
  });

  // 1. Failing H5C__prep_image_for_file_close (out-of-range image address).
  ASSERT_GE(H5C_load_cache_image_on_next_protect(f, 0x100000000ULL, 64, false),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_prep_for_file_close(f), 0);
  }
  cache->close_warning_received = false;
  cache->image_addr = HADDR_UNDEF;
  cache->image_len = 0;

  // 2. Normal call followed by idempotent duplicate close warning.
  EXPECT_GE(H5C_prep_for_file_close(f), 0);
  EXPECT_TRUE(cache->close_warning_received);
  EXPECT_GE(H5C_prep_for_file_close(f), 0);
}

TEST(H5CTest, EvictAndFlushCacheExerciseRingsCloseWarningAndProtectedFailures) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Flush an empty/live real file cache with close_warning_received=true to
  //    cover every H5C_RING_* switch arm (including RDFSM and MDFSM settle).
  {
    ScopedGroup grp(H5Gcreate2(file.get(), "/group_for_flush", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }
  H5F_t* real_f = GetFileStruct(file.get());
  ASSERT_NE(nullptr, real_f);
  H5C_t* real_cache = real_f->shared->cache;
  {
    ScopedApiContext flush_ctx;
    auto cleanup_real = absl::MakeCleanup([&]() {
      if (real_cache->slist_enabled) {
        H5C_set_slist_enabled(real_cache, false, false);
      }
      real_cache->close_warning_received = false;
      real_cache->rdfsm_settled = false;
      real_cache->mdfsm_settled = false;
    });
    ASSERT_GE(H5C_set_slist_enabled(real_cache, true, true), 0);
    EXPECT_GE(H5C_flush_cache(real_f, H5C__NO_FLAGS_SET), 0);
    real_cache->close_warning_received = true;
    EXPECT_GE(H5C_flush_cache(real_f, H5C__NO_FLAGS_SET), 0);
    EXPECT_GE(H5C_set_slist_enabled(real_cache, false, false), 0);
  }

  // 2. Evict and flush_cache success and error branches on synthetic cache.
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                             nullptr);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  auto* dirty_entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, dirty_entry);
  dirty_entry->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseSyntheticAddr,
                             dirty_entry, H5C__NO_FLAGS_SET),
            0);
  EXPECT_GE(H5C_evict(f), 0);
  EXPECT_EQ(0u, cache->index_len);

  // Protect a clean entry so evict and flush_cache fail cleanly.
  void* prot_entry = H5C_protect(f, &kSyntheticClass, kBaseSyntheticAddr + 0x2000,
                                 nullptr, H5C__NO_FLAGS_SET);
  ASSERT_NE(nullptr, prot_entry);
  {
    ScopedErrorSilence silence;
    // First H5C_evict enables slist, then fails at H5C__flush_invalidate_cache.
    EXPECT_LT(H5C_evict(f), 0);
    // Second H5C_evict fails immediately because slist is still enabled.
    EXPECT_LT(H5C_evict(f), 0);
    // Flush paths while slist is enabled and entry is protected.
    EXPECT_LT(H5C_flush_cache(f, H5C__FLUSH_INVALIDATE_FLAG), 0);
    EXPECT_LT(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  }
  EXPECT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  EXPECT_GE(H5C_unprotect(f, kBaseSyntheticAddr + 0x2000, prot_entry,
                          H5C__NO_FLAGS_SET),
            0);
}

TEST(H5CTest, FlushToMinCleanCoversPermissionAndMakeSpacePaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;

  // 1. Direct write_permitted boolean when callback is NULL.
  {
    CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, false,
                               nullptr);
    ASSERT_TRUE(harness.valid());
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_flush_to_min_clean(harness.file()), 0);
    }
    harness.cache()->write_permitted = true;
    EXPECT_GE(H5C_flush_to_min_clean(harness.file()), 0);
  }

  // 2. Callback check_write_permitted errors, denials, and serialize failure.
  WritePermittedState wp_state{};
  CustomCacheHarness harness(file.get(), 8192, 8192, CheckWritePermittedFromAux,
                             true, &wp_state);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  wp_state.rc = -1;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_flush_to_min_clean(f), 0);
  }

  wp_state.rc = 0;
  wp_state.permitted = false;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_flush_to_min_clean(f), 0);
  }

  // Disable evictions while inserting a dirty entry with failing serialize so
  // min_clean_size (8192) is violated, then re-enable evictions to trigger failure.
  ASSERT_GE(H5C_set_evictions_enabled(cache, false), 0);
  auto* bad_entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, bad_entry);
  bad_entry->entry_size = kDefaultEntrySize;
  bad_entry->fail_serialize = true;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseSyntheticAddr, bad_entry,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_set_evictions_enabled(cache, true), 0);
  wp_state.permitted = true;

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_flush_to_min_clean(f), 0);
  }

  bad_entry->fail_serialize = false;
  EXPECT_GE(H5C_flush_to_min_clean(f), 0);
}

TEST(H5CTest, ValidateAndSetAutoResizeConfigCoverAllModesAndValidationErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                             nullptr);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  // 1. Null and version checks on both validation and setter routines.
  EXPECT_GE(H5C_reset_cache_hit_rate_stats(cache), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_reset_cache_hit_rate_stats(nullptr), 0);
    EXPECT_LT(H5C_validate_resize_config(nullptr, H5C_RESIZE_CFG__VALIDATE_ALL),
              0);
    EXPECT_LT(H5C_set_cache_auto_resize_config(nullptr, nullptr), 0);
    H5C_auto_size_ctl_t valid = MakeValidResizeConfig();
    EXPECT_LT(H5C_set_cache_auto_resize_config(cache, nullptr), 0);
    valid.version = 999;
    EXPECT_LT(H5C_validate_resize_config(&valid, H5C_RESIZE_CFG__VALIDATE_ALL),
              0);
    EXPECT_LT(H5C_set_cache_auto_resize_config(cache, &valid), 0);
  }

  // 2. Exercise every invalid field rejected by H5C_validate_resize_config and
  //    propagated through H5C_set_cache_auto_resize_config.
  auto expect_invalid = [&](const H5C_auto_size_ctl_t& bad_cfg) {
    H5C_auto_size_ctl_t copy = bad_cfg;
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_validate_resize_config(&copy, H5C_RESIZE_CFG__VALIDATE_ALL),
              0);
    EXPECT_LT(H5C_set_cache_auto_resize_config(cache, &copy), 0);
  };

  // GENERAL checks.
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.max_size = H5C__MAX_MAX_CACHE_SIZE + 1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.min_size = H5C__MIN_MAX_CACHE_SIZE - 1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.min_size = cfg.max_size + 1024;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.set_initial_size = true;
    cfg.initial_size = cfg.min_size - 1;
    expect_invalid(cfg);
    cfg.initial_size = cfg.max_size + 1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.min_clean_fraction = -0.1;
    expect_invalid(cfg);
    cfg.min_clean_fraction = 1.1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH - 1;
    expect_invalid(cfg);
    cfg.epoch_length = H5C__MAX_AR_EPOCH_LENGTH + 1;
    expect_invalid(cfg);
  }

  // INCREMENT checks (including propagation when GENERAL passes).
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.lower_hr_threshold = -0.1;
    expect_invalid(cfg);
    cfg.lower_hr_threshold = 1.1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.increment = 0.5;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.flash_incr_mode = H5C_flash_incr__add_space;
    cfg.flash_multiple = 0.05;
    expect_invalid(cfg);
    cfg.flash_multiple = 10.5;
    expect_invalid(cfg);
    cfg.flash_multiple = 1.0;
    cfg.flash_threshold = 0.05;
    expect_invalid(cfg);
    cfg.flash_threshold = 1.1;
    expect_invalid(cfg);
  }

  // DECREMENT checks.
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.decr_mode = H5C_decr__threshold;
    cfg.upper_hr_threshold = 1.1;
    expect_invalid(cfg);
    cfg.upper_hr_threshold = 0.95;
    cfg.decrement = -0.1;
    expect_invalid(cfg);
    cfg.decrement = 1.1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.decr_mode = H5C_decr__age_out;
    cfg.epochs_before_eviction = 0;
    expect_invalid(cfg);
    cfg.epochs_before_eviction = H5C__MAX_EPOCH_MARKERS + 1;
    expect_invalid(cfg);
    cfg.epochs_before_eviction = 3;
    cfg.apply_empty_reserve = true;
    cfg.empty_reserve = -0.1;
    expect_invalid(cfg);
    cfg.empty_reserve = 1.1;
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.decr_mode = H5C_decr__age_out_with_threshold;
    cfg.upper_hr_threshold = -0.1;
    expect_invalid(cfg);
    cfg.upper_hr_threshold = 1.1;
    expect_invalid(cfg);
  }

  // INTERACTIONS checks.
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.incr_mode = H5C_incr__threshold;
    cfg.decr_mode = H5C_decr__threshold;
    cfg.lower_hr_threshold = 0.90;
    cfg.upper_hr_threshold = 0.90;
    expect_invalid(cfg);
    cfg.decr_mode = H5C_decr__age_out_with_threshold;
    expect_invalid(cfg);
  }

#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.incr_mode = static_cast<enum H5C_cache_incr_mode>(99);
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.flash_incr_mode = static_cast<enum H5C_cache_flash_incr_mode>(99);
    expect_invalid(cfg);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.decr_mode = static_cast<enum H5C_cache_decr_mode>(99);
    expect_invalid(cfg);
  }
#endif

  // 3. Cover valid combinations of incr/decr/flash modes, size clamping when
  //    set_initial_size=false, max_size==min_size, and active epoch markers.
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.incr_mode = H5C_incr__off;
    cfg.decr_mode = H5C_decr__off;
    cfg.flash_incr_mode = H5C_flash_incr__off;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_FALSE(cache->size_increase_possible);
    EXPECT_FALSE(cache->size_decrease_possible);
    EXPECT_FALSE(cache->flash_size_increase_possible);
    EXPECT_FALSE(cache->resize_enabled);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.incr_mode = H5C_incr__threshold;
    cfg.lower_hr_threshold = 0.0;
    cfg.decr_mode = H5C_decr__threshold;
    cfg.upper_hr_threshold = 1.0;
    cfg.flash_incr_mode = H5C_flash_incr__add_space;
    cfg.max_increment = 0;
    cfg.max_decrement = 0;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_FALSE(cache->size_increase_possible);
    EXPECT_FALSE(cache->size_decrease_possible);
    EXPECT_TRUE(cache->flash_size_increase_possible);
  }
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.decr_mode = H5C_decr__age_out;
    cfg.apply_empty_reserve = true;
    cfg.empty_reserve = 1.0;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_FALSE(cache->size_decrease_possible);

    cfg.decr_mode = H5C_decr__age_out_with_threshold;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_FALSE(cache->size_decrease_possible);
  }
  {
    // Clamp max_cache_size down, then up, then test max_size == min_size.
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.set_initial_size = false;
    cfg.min_size = 4096;
    cfg.max_size = 8192;
    cache->max_cache_size = 32768;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_EQ(8192u, cache->max_cache_size);
    EXPECT_TRUE(cache->size_decreased);
    cache->size_decreased = false;

    cfg.min_size = 16384;
    cfg.max_size = 32768;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_EQ(16384u, cache->max_cache_size);

    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_EQ(16384u, cache->max_cache_size);

    cfg.min_size = 16384;
    cfg.max_size = 16384;
    cfg.set_initial_size = true;
    cfg.initial_size = 16384;
    EXPECT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_FALSE(cache->resize_enabled);
    EXPECT_FALSE(cache->flash_size_increase_possible);
  }

  // 4. Populate active ageout epoch markers via protect/unprotect epochs, then
  //    exercise remove_excess_markers and remove_all_markers on reconfig.
  {
    H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
    cfg.set_initial_size = true;
    cfg.min_size = 4096;
    cfg.max_size = 64 * 1024;
    cfg.initial_size = 16 * 1024;
    cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
    cfg.incr_mode = H5C_incr__off;
    cfg.decr_mode = H5C_decr__age_out;
    cfg.epochs_before_eviction = 5;
    cfg.apply_empty_reserve = false;
    cfg.apply_max_decrement = false;
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);

    for (int i = 0; i < 3 * H5C__MIN_AR_EPOCH_LENGTH; ++i) {
      void* p = H5C_protect(f, &kSyntheticClass, kBaseSyntheticAddr, nullptr,
                            H5C__NO_FLAGS_SET);
      ASSERT_NE(nullptr, p);
      ASSERT_GE(H5C_unprotect(f, kBaseSyntheticAddr, p, H5C__NO_FLAGS_SET), 0);
    }
    EXPECT_EQ(3, cache->epoch_markers_active);

    cfg.epochs_before_eviction = 2;
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_EQ(2, cache->epoch_markers_active);

    cfg.decr_mode = H5C_decr__off;
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_EQ(0, cache->epoch_markers_active);
  }
}

TEST(H5CTest, SetEvictionsEnabledSetSlistEnabledAndUnsettleRingPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                             nullptr);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  // 1. Set evictions enabled validation checks.
  H5C_auto_size_ctl_t cfg = MakeValidResizeConfig();
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_set_evictions_enabled(nullptr, true), 0);
    EXPECT_LT(H5C_set_evictions_enabled(cache, false), 0);
    cfg.incr_mode = H5C_incr__off;
    ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
    EXPECT_LT(H5C_set_evictions_enabled(cache, false), 0);
  }
  cfg.decr_mode = H5C_decr__off;
  ASSERT_GE(H5C_set_cache_auto_resize_config(cache, &cfg), 0);
  EXPECT_GE(H5C_set_evictions_enabled(cache, false), 0);
  EXPECT_GE(H5C_set_evictions_enabled(cache, true), 0);

  // 2. Insert one clean entry and one dirty entry, then exercise slist enable
  //    (with populate_slist=true) and error branches.
  void* clean_p = H5C_protect(f, &kSyntheticClass, kBaseSyntheticAddr + 0x100,
                              nullptr, H5C__NO_FLAGS_SET);
  ASSERT_NE(nullptr, clean_p);
  ASSERT_GE(H5C_unprotect(f, kBaseSyntheticAddr + 0x100, clean_p,
                          H5C__NO_FLAGS_SET),
            0);

  auto* dirty_entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, dirty_entry);
  dirty_entry->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseSyntheticAddr + 0x200,
                             dirty_entry, H5C__NO_FLAGS_SET),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_set_slist_enabled(nullptr, true, false), 0);
    EXPECT_LT(H5C_set_slist_enabled(cache, false, false), 0);
  }

  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  EXPECT_EQ(1u, cache->slist_len);
  {
    ScopedErrorSilence silence;
    // Already enabled and non-empty slist checks.
    EXPECT_LT(H5C_set_slist_enabled(cache, true, false), 0);
    EXPECT_LT(H5C_set_slist_enabled(cache, false, false), 0);
  }
  cache->slist_enabled = false;
  cache->slist_len = 0;
  cache->slist_size = 0;
  H5SL_free(cache->slist_ptr, nullptr, nullptr);
  // Restore valid empty slist before hitting remaining error check when
  // slist_len is artificially non-zero while disabled.
  cache->slist_len = 1;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_set_slist_enabled(cache, true, false), 0);
  }
  cache->slist_len = 0;
  dirty_entry->header.in_slist = false;

  // 3. Exercise H5C_unsettle_ring for RDFSM and MDFSM in unsettled, settled,
  //    and unexpected shutdown states.
  auto reset_rings = absl::MakeCleanup([&]() {
    cache->rdfsm_settled = false;
    cache->mdfsm_settled = false;
    cache->close_warning_received = false;
  });

  EXPECT_GE(H5C_unsettle_ring(f, H5C_RING_RDFSM), 0);
  EXPECT_GE(H5C_unsettle_ring(f, H5C_RING_MDFSM), 0);

  cache->rdfsm_settled = true;
  cache->mdfsm_settled = true;
  cache->close_warning_received = false;
  EXPECT_GE(H5C_unsettle_ring(f, H5C_RING_RDFSM), 0);
  EXPECT_FALSE(cache->rdfsm_settled);
  EXPECT_GE(H5C_unsettle_ring(f, H5C_RING_MDFSM), 0);
  EXPECT_FALSE(cache->mdfsm_settled);

  cache->rdfsm_settled = true;
  cache->mdfsm_settled = true;
  cache->close_warning_received = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unsettle_ring(f, H5C_RING_RDFSM), 0);
    EXPECT_LT(H5C_unsettle_ring(f, H5C_RING_MDFSM), 0);
  }
}

TEST(H5CTest, CorkCoversPublicRoundTripAndInternalTagStates) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Public API round-trip exercising H5AC_cork -> H5C_cork on a group.
  ScopedGroup grp(H5Gcreate2(file.get(), "/corked_group", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  bool disabled = true;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);
  ASSERT_GE(H5Odisable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_TRUE(disabled);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Odisable_mdc_flushes(grp.get()), 0);
  }
  ASSERT_GE(H5Oenable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);
  grp.reset();

  // 2. Direct H5C_cork state machine covering empty tags, tagged entries, and
  //    duplicate cork/uncork error paths.
  constexpr haddr_t kObjTag = 0x40000;
  ScopedApiContext ctx(kObjTag);
  CustomCacheHarness harness(file.get(), 64 * 1024, 32 * 1024, nullptr, true,
                             nullptr);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  bool corked = true;
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__GET_CORKED, &corked), 0);
  EXPECT_FALSE(corked);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_cork(cache, kObjTag, H5C__UNCORK, nullptr), 0);
  }

  // Cork an untagged/new tag (allocates H5C_tag_info_t), check status, then uncork
  // with entry_cnt == 0 (frees H5C_tag_info_t).
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__SET_CORK, nullptr), 0);
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__GET_CORKED, &corked), 0);
  EXPECT_TRUE(corked);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_cork(cache, kObjTag, H5C__SET_CORK, nullptr), 0);
  }
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__UNCORK, nullptr), 0);
  EXPECT_EQ(0u, H5C_get_num_objs_corked(cache));

  // Insert an entry under kObjTag so tag_info exists with entry_cnt > 0 and
  // corked == false, then cork and uncork it cleanly.
  auto* tagged_entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, tagged_entry);
  tagged_entry->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseSyntheticAddr,
                             tagged_entry, H5C__NO_FLAGS_SET),
            0);

  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__GET_CORKED, &corked), 0);
  EXPECT_FALSE(corked);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_cork(cache, kObjTag, H5C__UNCORK, nullptr), 0);
  }
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__SET_CORK, nullptr), 0);
  EXPECT_GE(H5C_cork(cache, kObjTag, H5C__UNCORK, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
