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
// Tests metadata cache query helpers for configuration, size, hit rate, flush
// progress, eviction enablement, entry status/rings, aux pointer, and cache
// image metadata (third_party/hdf5/src/H5Cquery.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL. Internal headers are wrapped with matching NDEBUG so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t struct layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cquery.c

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

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

struct SyntheticEntry {
  H5C_cache_entry_t header;
  size_t entry_size;
};

constexpr size_t kDefaultEntrySize = 1024;
constexpr haddr_t kBaseAddr = 0x10000;

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
                          void* /*thing*/) {
  std::memset(image_ptr, 0, len);
  return 0;
}

herr_t SyntheticFreeIcr(void* thing) {
  std::free(thing);
  return 0;
}

const H5C_class_t kSyntheticClass = {
    /*id=*/0,
    /*name=*/"synthetic_query_entry",
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

herr_t TestLogFlush(H5C_t* /*cache_ptr*/, haddr_t /*addr*/, bool /*was_dirty*/,
                    unsigned /*flags*/) {
  return 0;
}

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__COPIED_TAG)
      : prev_tag_(HADDR_UNDEF) {
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

H5F_t* GetFileStruct(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

class CustomCacheHarness {
 public:
  CustomCacheHarness(hid_t file_id, size_t max_size, size_t min_clean,
                     void* aux_ptr)
      : f_(GetFileStruct(file_id)) {
    if (f_ == nullptr || f_->shared == nullptr) {
      return;
    }
    orig_cache_ = f_->shared->cache;
    cache_ = H5C_create(max_size, min_clean, H5AC_NTYPES - 1,
                        GetSyntheticClassTable(), nullptr, true, TestLogFlush,
                        aux_ptr);
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
      while (cache_->pl_head_ptr != nullptr) {
        H5C_cache_entry_t* prot = cache_->pl_head_ptr;
        H5C_unprotect(f_, prot->addr, prot, H5C__NO_FLAGS_SET);
      }
      for (H5C_cache_entry_t* cur = cache_->il_head; cur != nullptr;
           cur = cur->il_next) {
        while (cur->flush_dep_nparents > 0) {
          H5C_destroy_flush_dependency(cur->flush_dep_parent[0], cur);
        }
      }
      for (H5C_cache_entry_t* cur = cache_->pel_head_ptr; cur != nullptr;) {
        H5C_cache_entry_t* next = cur->next;
        if (cur->pinned_from_client) {
          H5C_unpin_entry(cur);
        }
        cur = next;
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

 private:
  H5F_t* f_ = nullptr;
  H5C_t* orig_cache_ = nullptr;
  H5C_t* cache_ = nullptr;
};

TEST(H5CqueryTest, AutoResizeConfigAndCacheSizeValidateAndReportFields) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get(), 64 * 1024, 16 * 1024, nullptr);
  ASSERT_TRUE(harness.valid());
  H5C_t* cache = harness.cache();

  // 1. Invalid argument checks for resize config and size getters.
  H5C_auto_size_ctl_t cfg{};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_get_cache_auto_resize_config(nullptr, &cfg), 0);
    EXPECT_LT(H5C_get_cache_auto_resize_config(cache, nullptr), 0);
    EXPECT_LT(
        H5C_get_cache_size(nullptr, nullptr, nullptr, nullptr, nullptr), 0);
  }

  // 2. Valid auto-resize config clears set_initial_size and sets initial_size.
  cfg.set_initial_size = true;
  ASSERT_GE(H5C_get_cache_auto_resize_config(cache, &cfg), 0);
  EXPECT_FALSE(cfg.set_initial_size);
  EXPECT_EQ(64u * 1024u, cfg.initial_size);

  H5AC_cache_config_t public_cfg{};
  public_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  EXPECT_GE(H5Fget_mdc_config(file.get(), &public_cfg), 0);
  EXPECT_FALSE(public_cfg.set_initial_size);

  // 3. Query size with all-NULL output pointers, then populated pointers.
  EXPECT_GE(H5C_get_cache_size(cache, nullptr, nullptr, nullptr, nullptr), 0);

  auto* entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, entry);
  entry->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(harness.file(), &kSyntheticClass, kBaseAddr, entry,
                             H5C__NO_FLAGS_SET),
            0);

  size_t max_sz = 0;
  size_t min_clean = 0;
  size_t cur_sz = 0;
  uint32_t cur_entries = 0;
  ASSERT_GE(
      H5C_get_cache_size(cache, &max_sz, &min_clean, &cur_sz, &cur_entries), 0);
  EXPECT_EQ(64u * 1024u, max_sz);
  EXPECT_EQ(16u * 1024u, min_clean);
  EXPECT_EQ(kDefaultEntrySize, cur_sz);
  EXPECT_EQ(1u, cur_entries);

  size_t pub_max = 0;
  size_t pub_min = 0;
  size_t pub_cur = 0;
  int pub_cnt = 0;
  EXPECT_GE(H5Fget_mdc_size(file.get(), &pub_max, &pub_min, &pub_cur, &pub_cnt),
            0);
  EXPECT_EQ(cur_sz, pub_cur);
  EXPECT_EQ(1, pub_cnt);
}

TEST(H5CqueryTest, FlushInProgressEvictionsEnabledAuxPtrAndHitRateWork) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;

  int aux_sentinel = 42;
  H5F_t* real_f = GetFileStruct(file.get());
  ASSERT_NE(nullptr, real_f);
  EXPECT_EQ(nullptr, H5C_get_aux_ptr(real_f->shared->cache));

  CustomCacheHarness harness(file.get(), 64 * 1024, 16 * 1024, &aux_sentinel);
  ASSERT_TRUE(harness.valid());
  H5C_t* cache = harness.cache();
  EXPECT_EQ(&aux_sentinel, H5C_get_aux_ptr(cache));

  // 1. Null-pointer validation across query functions.
  bool flag = false;
  double hit_rate = -1.0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_get_cache_flush_in_progress(nullptr, &flag), 0);
    EXPECT_LT(H5C_get_evictions_enabled(nullptr, &flag), 0);
    EXPECT_LT(H5C_get_evictions_enabled(cache, nullptr), 0);
    EXPECT_LT(H5C_get_cache_hit_rate(nullptr, &hit_rate), 0);
    EXPECT_LT(H5C_get_cache_hit_rate(cache, nullptr), 0);
  }

  // 2. Query flush_in_progress with null output pointer, false, and true.
  EXPECT_GE(H5C_get_cache_flush_in_progress(cache, nullptr), 0);
  flag = true;
  EXPECT_GE(H5C_get_cache_flush_in_progress(cache, &flag), 0);
  EXPECT_FALSE(flag);

  {
    const bool orig_flush_in_progress = cache->flush_in_progress;
    auto restore_fip = absl::MakeCleanup([&]() {
      cache->flush_in_progress = orig_flush_in_progress;
    });
    cache->flush_in_progress = true;
    flag = false;
    EXPECT_GE(H5C_get_cache_flush_in_progress(cache, &flag), 0);
    EXPECT_TRUE(flag);
  }

  // 3. Toggle evictions_enabled and verify query reflects updates.
  EXPECT_GE(H5C_get_evictions_enabled(cache, &flag), 0);
  EXPECT_TRUE(flag);
  ASSERT_GE(H5C_set_evictions_enabled(cache, false), 0);
  EXPECT_GE(H5C_get_evictions_enabled(cache, &flag), 0);
  EXPECT_FALSE(flag);
  ASSERT_GE(H5C_set_evictions_enabled(cache, true), 0);

  // 4. Zero-access hit rate returns 0.0, then 1 miss + 3 hits returns 0.75.
  ASSERT_GE(H5C_get_cache_hit_rate(cache, &hit_rate), 0);
  EXPECT_DOUBLE_EQ(0.0, hit_rate);

  void* first = H5C_protect(harness.file(), &kSyntheticClass, kBaseAddr,
                            nullptr, H5C__NO_FLAGS_SET);
  ASSERT_NE(nullptr, first);
  ASSERT_GE(H5C_unprotect(harness.file(), kBaseAddr, first, H5C__NO_FLAGS_SET),
            0);
  for (int i = 0; i < 3; ++i) {
    void* hit = H5C_protect(harness.file(), &kSyntheticClass, kBaseAddr,
                            nullptr, H5C__NO_FLAGS_SET);
    ASSERT_EQ(first, hit);
    ASSERT_GE(H5C_unprotect(harness.file(), kBaseAddr, hit, H5C__NO_FLAGS_SET),
              0);
  }
  ASSERT_GE(H5C_get_cache_hit_rate(cache, &hit_rate), 0);
  EXPECT_DOUBLE_EQ(0.75, hit_rate);

  double pub_hr = -1.0;
  EXPECT_GE(H5Freset_mdc_hit_rate_stats(file.get()), 0);
  EXPECT_GE(H5Fget_mdc_hit_rate(file.get(), &pub_hr), 0);
  EXPECT_DOUBLE_EQ(0.0, pub_hr);
}

TEST(H5CqueryTest, EntryStatusAndRingCoverMissingHitFlagsAndNullCache) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx(0x7000);
  CustomCacheHarness harness(file.get(), 64 * 1024, 16 * 1024, nullptr);
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  // 1. Defensive cache_ptr == nullptr guard inside H5C_get_entry_status.
  {
    f->shared->cache = nullptr;
    auto restore_cache = absl::MakeCleanup([&]() { f->shared->cache = cache; });
    bool in_cache = false;
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_get_entry_status(f, kBaseAddr, nullptr, &in_cache, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr),
              0);
  }

  // 2. Cache miss reports in_cache = false, and H5C_get_entry_ring keeps the
  //    caller's ring unmodified on failure.
  bool in_cache = true;
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x9999, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_FALSE(in_cache);

  H5C_ring_t ring = H5C_RING_MDFSM;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_get_entry_ring(f, kBaseAddr + 0x9999, &ring), 0);
  }
  EXPECT_EQ(H5C_RING_MDFSM, ring);

  // 3. Insert parent and child with custom ring, flush dependency, cork, and
  //    image_up_to_date to exercise every H5C_get_entry_status output pointer.
  H5AC_ring_t orig_ring = H5C_RING_UNDEFINED;
  H5AC_set_ring(H5C_RING_USER, &orig_ring);
  auto restore_ring = absl::MakeCleanup([&]() {
    H5AC_ring_t unused = H5C_RING_UNDEFINED;
    H5AC_set_ring(orig_ring, &unused);
  });

  auto* parent =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, parent);
  parent->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseAddr, parent,
                             H5C__PIN_ENTRY_FLAG),
            0);

  auto* child =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, child);
  child->entry_size = kDefaultEntrySize / 2;
  ASSERT_GE(H5C_insert_entry(f, &kSyntheticClass, kBaseAddr + 0x1000, child,
                             H5C__NO_FLAGS_SET),
            0);

  ASSERT_GE(H5C_create_flush_dependency(&parent->header, &child->header), 0);
  ASSERT_GE(H5C_cork(cache, 0x7000, H5C__SET_CORK, nullptr), 0);

  // Protect child and toggle image_up_to_date on parent for coverage.
  void* prot_child = H5C_protect(f, &kSyntheticClass, kBaseAddr + 0x1000,
                                 nullptr, H5C__NO_FLAGS_SET);
  ASSERT_EQ(child, prot_child);
  child->header.image_up_to_date = true;

  // Hit with all optional pointers NULL.
  in_cache = false;
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr, nullptr, &in_cache, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr),
            0);
  EXPECT_TRUE(in_cache);

  // Hit on parent: dirty, unprotected, pinned, corked, FD parent, !image_up_to_date.
  size_t size = 0;
  bool dirty = false;
  bool prot = true;
  bool pinned = false;
  bool corked = false;
  bool fd_parent = false;
  bool fd_child = true;
  bool img_utd = true;
  ASSERT_GE(H5C_get_entry_status(f, kBaseAddr, &size, &in_cache, &dirty, &prot,
                                 &pinned, &corked, &fd_parent, &fd_child,
                                 &img_utd),
            0);
  EXPECT_EQ(kDefaultEntrySize, size);
  EXPECT_TRUE(in_cache);
  EXPECT_TRUE(dirty);
  EXPECT_FALSE(prot);
  EXPECT_TRUE(pinned);
  EXPECT_TRUE(corked);
  EXPECT_TRUE(fd_parent);
  EXPECT_FALSE(fd_child);
  EXPECT_FALSE(img_utd);

  // Hit on child: protected, FD child, image_up_to_date = true, then untag to
  // cover tag_info == NULL false branch.
  ASSERT_GE(H5C_get_entry_status(f, kBaseAddr + 0x1000, &size, &in_cache,
                                 &dirty, &prot, &pinned, &corked, &fd_parent,
                                 &fd_child, &img_utd),
            0);
  EXPECT_EQ(kDefaultEntrySize / 2, size);
  EXPECT_TRUE(prot);
  EXPECT_FALSE(pinned);
  EXPECT_TRUE(corked);
  EXPECT_FALSE(fd_parent);
  EXPECT_TRUE(fd_child);
  EXPECT_TRUE(img_utd);

  ASSERT_GE(H5C__untag_entry(cache, &child->header), 0);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x1000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, &corked, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_FALSE(corked);
  ASSERT_GE(H5C__tag_entry(cache, &child->header), 0);

  // Ring lookup on present entry.
  ASSERT_GE(H5C_get_entry_ring(f, kBaseAddr, &ring), 0);
  EXPECT_EQ(H5C_RING_USER, ring);

  // Also verify H5AC_get_entry_status round-trip.
  unsigned status = 0;
  EXPECT_GE(H5AC_get_entry_status(f, kBaseAddr, &status), 0);
  EXPECT_NE(0u, status & H5AC_ES__IN_CACHE);
  EXPECT_NE(0u, status & H5AC_ES__IS_PINNED);
  EXPECT_NE(0u, status & H5AC_ES__IS_CORKED);
  EXPECT_NE(0u, status & H5AC_ES__IS_FLUSH_DEP_PARENT);

  // Clean up test state before harness destruction.
  ASSERT_GE(H5C_cork(cache, 0x7000, H5C__UNCORK, nullptr), 0);
  ASSERT_GE(H5C_unprotect(f, kBaseAddr + 0x1000, prot_child, H5C__NO_FLAGS_SET),
            0);
}

TEST(H5CqueryTest, GetMdcImageInfoHandlesNullsUnsetAndPersistedCacheImage) {
  TempFile temp("h5c_query_image_info.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
            0);

  H5AC_cache_image_config_t img_cfg{};
  img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  img_cfg.generate_image = true;
  img_cfg.save_resize_status = false;
  img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

  // 1. Validate error and NULL-out-param handling on open cache without image.
  {
    ScopedFile file(H5Fcreate(temp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetFileStruct(file.get());
    ASSERT_NE(nullptr, f);
    H5C_t* cache = f->shared->cache;

    haddr_t addr = 0;
    hsize_t len = 123;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_get_mdc_image_info(nullptr, &addr, &len), 0);
    }
    EXPECT_GE(H5C_get_mdc_image_info(cache, nullptr, nullptr), 0);
    EXPECT_GE(H5C_get_mdc_image_info(cache, &addr, &len), 0);
    EXPECT_FALSE(H5_addr_defined(addr));
    EXPECT_EQ(0u, len);

    ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  // 2. Reopen file with persisted cache image and query image info.
  img_cfg.generate_image = false;
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);
  {
    ScopedFile reopened(H5Fopen(temp.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_t* f = GetFileStruct(reopened.get());
    ASSERT_NE(nullptr, f);

    haddr_t addr = HADDR_UNDEF;
    hsize_t len = 0;
    ASSERT_GE(H5C_get_mdc_image_info(f->shared->cache, &addr, &len), 0);
    EXPECT_TRUE(H5_addr_defined(addr));
    EXPECT_GT(len, 0u);

    haddr_t pub_addr = HADDR_UNDEF;
    hsize_t pub_len = 0;
    EXPECT_GE(H5Fget_mdc_image_info(reopened.get(), &pub_addr, &pub_len), 0);
    EXPECT_EQ(addr, pub_addr);
    EXPECT_EQ(len, pub_len);

    // Also verify real superblock ring via H5C_get_entry_ring.
    H5C_ring_t sb_ring = H5C_RING_UNDEFINED;
    EXPECT_GE(H5C_get_entry_ring(f, 0, &sb_ring), 0);
    EXPECT_EQ(H5C_RING_SB, sb_ring);
  }
}

}  // namespace
}  // namespace hdf5
