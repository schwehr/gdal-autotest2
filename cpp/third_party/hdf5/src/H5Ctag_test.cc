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
// Tests metadata cache entry tagging, untagging, iteration, eviction, flushing,
// retagging, and type-filtered expunging (third_party/hdf5/src/H5Ctag.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL, so H5C_DO_TAGGING_SANITY_CHECKS / H5C_verify_tag are
// compiled out. Internal headers are wrapped with matching NDEBUG so C++20
// tests observe the exact H5C_t / H5C_cache_entry_t struct layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ctag.c

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

#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::UnorderedElementsAre;

struct SyntheticEntry {
  H5C_cache_entry_t header;
  size_t entry_size;
  bool fail_serialize;
  bool fail_before_evict;
  bool destroy_fd_on_before_evict;
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
                          void* thing) {
  const auto* entry = static_cast<const SyntheticEntry*>(thing);
  if (entry->fail_serialize) {
    return -1;
  }
  std::memset(image_ptr, 0, len);
  return 0;
}

herr_t SyntheticNotify(H5C_notify_action_t action, void* thing) {
  auto* entry = static_cast<SyntheticEntry*>(thing);
  if (action == H5C_NOTIFY_ACTION_BEFORE_EVICT) {
    if (entry->fail_before_evict) {
      return -1;
    }
    if (entry->destroy_fd_on_before_evict &&
        entry->header.flush_dep_nparents > 0) {
      for (unsigned u = entry->header.flush_dep_nparents - 1;
           u < entry->header.flush_dep_nparents; --u) {
        if (H5C_destroy_flush_dependency(entry->header.flush_dep_parent[u],
                                         &entry->header) < 0) {
          return -1;
        }
      }
    }
  }
  return 0;
}

herr_t SyntheticFreeIcr(void* thing) {
  std::free(thing);
  return 0;
}

const H5C_class_t kPrimaryClass = {
    /*id=*/0,
    /*name=*/"synthetic_tag_primary",
    /*mem_type=*/H5FD_MEM_OHDR,
    /*flags=*/H5C__CLASS_SKIP_READS | H5C__CLASS_SKIP_WRITES,
    /*get_initial_load_size=*/SyntheticGetInitialLoadSize,
    /*get_final_load_size=*/nullptr,
    /*verify_chksum=*/nullptr,
    /*deserialize=*/SyntheticDeserialize,
    /*image_len=*/SyntheticImageLen,
    /*pre_serialize=*/SyntheticPreSerialize,
    /*serialize=*/SyntheticSerialize,
    /*notify=*/SyntheticNotify,
    /*free_icr=*/SyntheticFreeIcr,
    /*fsf_size=*/nullptr,
};

const H5C_class_t kSecondaryClass = {
    /*id=*/1,
    /*name=*/"synthetic_tag_secondary",
    /*mem_type=*/H5FD_MEM_OHDR,
    /*flags=*/H5C__CLASS_SKIP_READS | H5C__CLASS_SKIP_WRITES,
    /*get_initial_load_size=*/SyntheticGetInitialLoadSize,
    /*get_final_load_size=*/nullptr,
    /*verify_chksum=*/nullptr,
    /*deserialize=*/SyntheticDeserialize,
    /*image_len=*/SyntheticImageLen,
    /*pre_serialize=*/SyntheticPreSerialize,
    /*serialize=*/SyntheticSerialize,
    /*notify=*/SyntheticNotify,
    /*free_icr=*/SyntheticFreeIcr,
    /*fsf_size=*/nullptr,
};

const H5C_class_t* const* GetSyntheticClassTable() {
  static const H5C_class_t* kTable[H5AC_NTYPES] = {
      &kPrimaryClass, &kSecondaryClass, &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, &kPrimaryClass,
      &kPrimaryClass, &kPrimaryClass,   &kPrimaryClass, H5AC_EPOCH_MARKER,
      &kPrimaryClass, &kPrimaryClass,
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
  explicit CustomCacheHarness(hid_t file_id) : f_(GetFileStruct(file_id)) {
    if (f_ == nullptr || f_->shared == nullptr) {
      return;
    }
    orig_cache_ = f_->shared->cache;
    cache_ = H5C_create(64 * 1024, 16 * 1024, H5AC_NTYPES - 1,
                        GetSyntheticClassTable(), nullptr, true, TestLogFlush,
                        nullptr);
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
        if (cur->type == &kPrimaryClass || cur->type == &kSecondaryClass) {
          auto* syn = reinterpret_cast<SyntheticEntry*>(cur);
          syn->fail_serialize = false;
          syn->fail_before_evict = false;
        }
        cur->prefetched_dirty = false;
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

SyntheticEntry* InsertSynthetic(CustomCacheHarness& harness, haddr_t addr,
                                haddr_t tag,
                                const H5C_class_t* cls = &kPrimaryClass,
                                unsigned flags = H5C__NO_FLAGS_SET) {
  haddr_t prev = HADDR_UNDEF;
  H5AC_tag(tag, &prev);
  auto restore = absl::MakeCleanup([&]() {
    haddr_t unused = HADDR_UNDEF;
    H5AC_tag(prev, &unused);
  });

  auto* entry =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  if (entry == nullptr) {
    return nullptr;
  }
  entry->entry_size = kDefaultEntrySize;
  if (H5C_insert_entry(harness.file(), cls, addr, entry, flags) < 0) {
    std::free(entry);
    return nullptr;
  }
  return entry;
}

struct IterCollectCtx {
  std::vector<haddr_t> addrs;
  haddr_t fail_addr = HADDR_UNDEF;
};

int CollectTaggedAddrCb(H5C_cache_entry_t* entry, void* raw_ctx) {
  auto* ctx = static_cast<IterCollectCtx*>(raw_ctx);
  if (H5_addr_defined(ctx->fail_addr) && entry->addr == ctx->fail_addr) {
    return H5_ITER_ERROR;
  }
  ctx->addrs.push_back(entry->addr);
  return H5_ITER_CONT;
}

TEST(H5CtagTest, IgnoreTagsAndNumObjsCorkedTrackCacheAndPublicState) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  {
    ScopedApiContext ctx;
    CustomCacheHarness harness(file.get());
    ASSERT_TRUE(harness.valid());
    H5C_t* cache = harness.cache();

    // 1. Toggle ignore_tags and verify getter.
    EXPECT_FALSE(H5C_get_ignore_tags(cache));
    EXPECT_GE(H5C_ignore_tags(cache), 0);
    EXPECT_TRUE(H5C_get_ignore_tags(cache));

    // 2. Track corked object counter across multiple H5C_cork operations.
    EXPECT_EQ(0u, H5C_get_num_objs_corked(cache));
    ASSERT_GE(H5C_cork(cache, 0x2000, H5C__SET_CORK, nullptr), 0);
    EXPECT_EQ(1u, H5C_get_num_objs_corked(cache));
    ASSERT_GE(H5C_cork(cache, 0x3000, H5C__SET_CORK, nullptr), 0);
    EXPECT_EQ(2u, H5C_get_num_objs_corked(cache));
    ASSERT_GE(H5C_cork(cache, 0x2000, H5C__UNCORK, nullptr), 0);
    EXPECT_EQ(1u, H5C_get_num_objs_corked(cache));
    ASSERT_GE(H5C_cork(cache, 0x3000, H5C__UNCORK, nullptr), 0);
    EXPECT_EQ(0u, H5C_get_num_objs_corked(cache));
  }

  // 3. Verify public H5Odisable_mdc_flushes / H5Oenable_mdc_flushes after
  //    harness restores the real file metadata cache.
  ScopedGroup grp(H5Gcreate2(file.get(), "/corked_grp", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  H5F_t* real_f = GetFileStruct(file.get());
  ASSERT_NE(nullptr, real_f);
  EXPECT_EQ(0u, H5C_get_num_objs_corked(real_f->shared->cache));
  ASSERT_GE(H5Odisable_mdc_flushes(grp.get()), 0);
  bool disabled = false;
  EXPECT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_TRUE(disabled);
  EXPECT_EQ(1u, H5C_get_num_objs_corked(real_f->shared->cache));
  ASSERT_GE(H5Oenable_mdc_flushes(grp.get()), 0);
  EXPECT_EQ(0u, H5C_get_num_objs_corked(real_f->shared->cache));
}

TEST(H5CtagTest, TagAndUntagEntriesExerciseListLinksCorkAndIgnoreTags) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx(0x5000);
  CustomCacheHarness harness(file.get());
  ASSERT_TRUE(harness.valid());
  H5C_t* cache = harness.cache();

  H5C_cache_entry_t e1{};
  H5C_cache_entry_t e2{};
  H5C_cache_entry_t e3{};
  H5C_cache_entry_t untagged{};
  H5C_cache_entry_t corked_entry{};
  H5C_cache_entry_t ignored_undef{};
  H5C_cache_entry_t ignored_def{};

  auto untag_all = absl::MakeCleanup([&]() {
    H5C__untag_entry(cache, &e1);
    H5C__untag_entry(cache, &e2);
    H5C__untag_entry(cache, &e3);
    H5C__untag_entry(cache, &corked_entry);
    H5C__untag_entry(cache, &ignored_undef);
    H5C__untag_entry(cache, &ignored_def);
    bool is_corked = false;
    if (H5C_cork(cache, 0x6000, H5C__GET_CORKED, &is_corked) >= 0 && is_corked) {
      H5C_cork(cache, 0x6000, H5C__UNCORK, nullptr);
    }
  });

  // 1. Untagging an untagged entry is a clean no-op.
  EXPECT_GE(H5C__untag_entry(cache, &untagged), 0);

  // 2. Tag three entries under 0x5000: tag_info->head == &e3 -> &e2 -> &e1.
  ASSERT_GE(H5C__tag_entry(cache, &e1), 0);
  ASSERT_GE(H5C__tag_entry(cache, &e2), 0);
  ASSERT_GE(H5C__tag_entry(cache, &e3), 0);

  haddr_t read_tag = HADDR_UNDEF;
  ASSERT_GE(H5C_get_tag(&e2, &read_tag), 0);
  EXPECT_EQ(0x5000u, read_tag);

  // Remove middle (&e2), then head (&e3 while &e1 remains), then final (&e1).
  EXPECT_GE(H5C__untag_entry(cache, &e2), 0);
  EXPECT_EQ(nullptr, e2.tag_info);
  EXPECT_GE(H5C__untag_entry(cache, &e3), 0);
  EXPECT_EQ(nullptr, e3.tag_info);
  EXPECT_GE(H5C__untag_entry(cache, &e1), 0);
  EXPECT_EQ(nullptr, e1.tag_info);

  // Repeat tail removal case: head -> tail, remove tail (&e1) then head (&e2).
  ASSERT_GE(H5C__tag_entry(cache, &e1), 0);
  ASSERT_GE(H5C__tag_entry(cache, &e2), 0);
  EXPECT_GE(H5C__untag_entry(cache, &e1), 0);
  EXPECT_GE(H5C__untag_entry(cache, &e2), 0);

  // 3. Tagging an entry whose tag was already corked (head == NULL, corked),
  //    then untagging the last entry preserves corked H5C_tag_info_t.
  ASSERT_GE(H5C_cork(cache, 0x6000, H5C__SET_CORK, nullptr), 0);
  haddr_t prev = HADDR_UNDEF;
  H5AC_tag(0x6000, &prev);
  ASSERT_GE(H5C__tag_entry(cache, &corked_entry), 0);
  ASSERT_GE(H5C_get_tag(&corked_entry, &read_tag), 0);
  EXPECT_EQ(0x6000u, read_tag);
  EXPECT_GE(H5C__untag_entry(cache, &corked_entry), 0);
  bool still_corked = false;
  EXPECT_GE(H5C_cork(cache, 0x6000, H5C__GET_CORKED, &still_corked), 0);
  EXPECT_TRUE(still_corked);
  ASSERT_GE(H5C_cork(cache, 0x6000, H5C__UNCORK, nullptr), 0);

  // 4. With ignore_tags enabled: HADDR_UNDEF maps to H5AC__IGNORE_TAG while
  //    defined tags are preserved verbatim.
  ASSERT_GE(H5C_ignore_tags(cache), 0);
  H5AC_tag(HADDR_UNDEF, &prev);
  ASSERT_GE(H5C__tag_entry(cache, &ignored_undef), 0);
  ASSERT_GE(H5C_get_tag(&ignored_undef, &read_tag), 0);
  EXPECT_EQ(H5AC__IGNORE_TAG, read_tag);

  H5AC_tag(0x7000, &prev);
  ASSERT_GE(H5C__tag_entry(cache, &ignored_def), 0);
  ASSERT_GE(H5C_get_tag(&ignored_def, &read_tag), 0);
  EXPECT_EQ(0x7000u, read_tag);
}

TEST(H5CtagTest, IterTaggedEntriesVisitsGlobalTagsAndHandlesCallbackErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get());
  ASSERT_TRUE(harness.valid());
  H5C_t* cache = harness.cache();

  constexpr haddr_t kTagA = 0x5100;
  ASSERT_NE(nullptr, InsertSynthetic(harness, kBaseAddr, kTagA));
  ASSERT_NE(nullptr, InsertSynthetic(harness, kBaseAddr + 0x1000, kTagA));
  ASSERT_NE(nullptr,
            InsertSynthetic(harness, kBaseAddr + 0x2000, H5AC__SOHM_TAG));
  ASSERT_NE(nullptr,
            InsertSynthetic(harness, kBaseAddr + 0x3000, H5AC__GLOBALHEAP_TAG));

  // 1. Unknown tag visits zero entries cleanly.
  IterCollectCtx collect;
  EXPECT_GE(H5C__iter_tagged_entries(cache, 0x9999, false, CollectTaggedAddrCb,
                                     &collect),
            0);
  EXPECT_TRUE(collect.addrs.empty());

  // 2. match_global=false visits only kTagA entries; match_global=true includes
  //    SOHM and global heap entries.
  collect.addrs.clear();
  EXPECT_GE(H5C__iter_tagged_entries(cache, kTagA, false, CollectTaggedAddrCb,
                                     &collect),
            0);
  EXPECT_THAT(collect.addrs,
              UnorderedElementsAre(kBaseAddr, kBaseAddr + 0x1000));

  collect.addrs.clear();
  EXPECT_GE(H5C__iter_tagged_entries(cache, kTagA, true, CollectTaggedAddrCb,
                                     &collect),
            0);
  EXPECT_THAT(collect.addrs,
              UnorderedElementsAre(kBaseAddr, kBaseAddr + 0x1000,
                                   kBaseAddr + 0x2000, kBaseAddr + 0x3000));

  // 3. Callback errors propagate from primary tag, SOHM tag, and GLOBALHEAP tag.
  ScopedErrorSilence silence;
  for (haddr_t fail_addr : {kBaseAddr, kBaseAddr + 0x2000, kBaseAddr + 0x3000}) {
    IterCollectCtx err_ctx;
    err_ctx.fail_addr = fail_addr;
    EXPECT_LT(H5C__iter_tagged_entries(cache, kTagA, true, CollectTaggedAddrCb,
                                       &err_ctx),
              0);
  }
}

TEST(H5CtagTest, EvictTaggedEntriesHandlesMultiPassGlobalPfDirtyAndErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get());
  ASSERT_TRUE(harness.valid());
  H5F_t* f = harness.file();
  H5C_t* cache = harness.cache();

  constexpr haddr_t kTagA = 0x5200;
  constexpr haddr_t kTagB = 0x5300;
  constexpr haddr_t kTagMulti = 0x5400;
  constexpr haddr_t kTagPfDirty = 0x5500;
  constexpr haddr_t kTagFail = 0x5600;

  // 1. Clean entries under kTagA, global tags, and kTagB. Flush cache so they
  //    become clean and evictable.
  ASSERT_NE(nullptr, InsertSynthetic(harness, kBaseAddr, kTagA));
  ASSERT_NE(nullptr,
            InsertSynthetic(harness, kBaseAddr + 0x1000, H5AC__SOHM_TAG));
  ASSERT_NE(nullptr, InsertSynthetic(harness, kBaseAddr + 0x2000, kTagB));
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

  // Evict kTagA only (match_global = false); SOHM and kTagB remain.
  ASSERT_GE(H5C_evict_tagged_entries(f, kTagA, false), 0);
  bool in_cache = false;
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr, nullptr, &in_cache, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr),
            0);
  EXPECT_FALSE(in_cache);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x1000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_TRUE(in_cache);

  // Evict with match_global = true clears remaining global SOHM entry while
  // keeping kTagB.
  ASSERT_GE(H5C_evict_tagged_entries(f, kTagA, true), 0);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x1000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_FALSE(in_cache);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x2000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_TRUE(in_cache);

  // 2. Dirty entry fails eviction until flushed, then protected and pinned fail.
  SyntheticEntry* dirty_ent = InsertSynthetic(harness, kBaseAddr + 0x3000, kTagA);
  ASSERT_NE(nullptr, dirty_ent);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_evict_tagged_entries(f, kTagA, false), 0);
  }
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

  void* prot_ent = H5C_protect(f, &kPrimaryClass, kBaseAddr + 0x3000, nullptr,
                               H5C__NO_FLAGS_SET);
  ASSERT_EQ(dirty_ent, prot_ent);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_evict_tagged_entries(f, kTagA, false), 0);
  }
  ASSERT_GE(H5C_pin_protected_entry(prot_ent), 0);
  ASSERT_GE(H5C_unprotect(f, kBaseAddr + 0x3000, prot_ent, H5C__NO_FLAGS_SET),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_evict_tagged_entries(f, kTagA, false), 0);
  }
  ASSERT_GE(H5C_unpin_entry(dirty_ent), 0);
  ASSERT_GE(H5C_evict_tagged_entries(f, kTagA, false), 0);

  // 3. Multi-pass eviction: parent is pinned via flush dependency on child;
  //    pass 1 skips parent and evicts child (which destroys FD via notify),
  //    then pass 2 evicts the newly unpinned parent.
  SyntheticEntry* parent =
      InsertSynthetic(harness, kBaseAddr + 0x4000, kTagMulti);
  SyntheticEntry* child =
      InsertSynthetic(harness, kBaseAddr + 0x5000, kTagMulti);
  ASSERT_NE(nullptr, parent);
  ASSERT_NE(nullptr, child);
  child->destroy_fd_on_before_evict = true;
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

  void* prot_parent = H5C_protect(f, &kPrimaryClass, kBaseAddr + 0x4000,
                                  nullptr, H5C__NO_FLAGS_SET);
  ASSERT_EQ(parent, prot_parent);
  ASSERT_GE(H5C_create_flush_dependency(&parent->header, &child->header), 0);
  ASSERT_GE(H5C_unprotect(f, kBaseAddr + 0x4000, prot_parent, H5C__NO_FLAGS_SET),
            0);

  ASSERT_GE(H5C_evict_tagged_entries(f, kTagMulti, false), 0);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x4000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_FALSE(in_cache);

  // 4. prefetched_dirty entries are skipped and suppress the leftover-pinned
  //    error check at the end of H5C_evict_tagged_entries.
  SyntheticEntry* pf_dirty =
      InsertSynthetic(harness, kBaseAddr + 0x6000, kTagPfDirty);
  SyntheticEntry* pinned_clean =
      InsertSynthetic(harness, kBaseAddr + 0x7000, kTagPfDirty, &kPrimaryClass,
                      H5C__PIN_ENTRY_FLAG);
  ASSERT_NE(nullptr, pf_dirty);
  ASSERT_NE(nullptr, pinned_clean);
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

  pf_dirty->header.prefetched_dirty = true;
  EXPECT_GE(H5C_evict_tagged_entries(f, kTagPfDirty, false), 0);
  EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x6000, nullptr, &in_cache,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr),
            0);
  EXPECT_TRUE(in_cache);
  pf_dirty->header.prefetched_dirty = false;
  ASSERT_GE(H5C_unpin_entry(pinned_clean), 0);
  EXPECT_GE(H5C_evict_tagged_entries(f, kTagPfDirty, false), 0);

  // 5. Propagate H5C__flush_single_entry failure during BEFORE_EVICT notify.
  SyntheticEntry* bad_evict =
      InsertSynthetic(harness, kBaseAddr + 0x8000, kTagFail);
  ASSERT_NE(nullptr, bad_evict);
  ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
  ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  bad_evict->fail_before_evict = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_evict_tagged_entries(f, kTagFail, false), 0);
  }
  bad_evict->fail_before_evict = false;
}

TEST(H5CtagTest, FlushTaggedEntriesFlushesTagAndGlobalAndCoversErrorPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  {
    ScopedApiContext ctx;
    CustomCacheHarness harness(file.get());
    ASSERT_TRUE(harness.valid());
    H5F_t* f = harness.file();
    H5C_t* cache = harness.cache();

    constexpr haddr_t kTagA = 0x5700;
    constexpr haddr_t kTagB = 0x5800;

    // 1. Mix dirty & clean entries in kTagA, dirty in GLOBALHEAP, and dirty in
    //    kTagB. H5C_flush_tagged_entries(kTagA) must flush kTagA + global only.
    SyntheticEntry* a_clean = InsertSynthetic(harness, kBaseAddr, kTagA);
    ASSERT_NE(nullptr, a_clean);
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

    SyntheticEntry* a_dirty =
        InsertSynthetic(harness, kBaseAddr + 0x1000, kTagA);
    SyntheticEntry* gh_dirty =
        InsertSynthetic(harness, kBaseAddr + 0x2000, H5AC__GLOBALHEAP_TAG);
    SyntheticEntry* b_dirty =
        InsertSynthetic(harness, kBaseAddr + 0x3000, kTagB);
    ASSERT_NE(nullptr, a_dirty);
    ASSERT_NE(nullptr, gh_dirty);
    ASSERT_NE(nullptr, b_dirty);

    ASSERT_GE(H5C_flush_tagged_entries(f, kTagA), 0);
    EXPECT_FALSE(a_dirty->header.is_dirty);
    EXPECT_FALSE(gh_dirty->header.is_dirty);
    EXPECT_TRUE(b_dirty->header.is_dirty);

    // 2. Error when slist is already enabled before H5C_flush_tagged_entries.
    ASSERT_GE(H5C_set_slist_enabled(cache, true, true), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_flush_tagged_entries(f, kTagB), 0);
    }
    ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

    // 3. Error when serialize fails inside H5C_flush_cache.
    SyntheticEntry* fail_flush =
        InsertSynthetic(harness, kBaseAddr + 0x4000, kTagA);
    ASSERT_NE(nullptr, fail_flush);
    fail_flush->fail_serialize = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_flush_tagged_entries(f, kTagA), 0);
    }
    fail_flush->fail_serialize = false;
    ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);

    // 4. Error when disabling slist fails because a dirty protected entry
    //    stays in slist after H5C_flush_cache(...,
    //    H5C__FLUSH_IGNORE_PROTECTED_FLAG).
    SyntheticEntry* prot_dirty =
        InsertSynthetic(harness, kBaseAddr + 0x5000, kTagA);
    ASSERT_NE(nullptr, prot_dirty);
    void* prot = H5C_protect(f, &kPrimaryClass, kBaseAddr + 0x5000, nullptr,
                             H5C__NO_FLAGS_SET);
    ASSERT_EQ(prot_dirty, prot);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_flush_tagged_entries(f, kTagA), 0);
    }
    ASSERT_GE(H5C_unprotect(f, kBaseAddr + 0x5000, prot, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_set_slist_enabled(cache, false, false), 0);
  }

  // 5. Public H5Oflush exercises tagged metadata flush on an HDF5 group after
  //    harness restores the real file metadata cache.
  ScopedGroup grp(H5Gcreate2(file.get(), "/flush_grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  EXPECT_GE(H5Oflush(grp.get()), 0);
}

TEST(H5CtagTest, RetagAndExpungeTagTypeMetadataUpdateAndRemoveByType) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  {
    ScopedApiContext ctx;
    CustomCacheHarness harness(file.get());
    ASSERT_TRUE(harness.valid());
    H5F_t* f = harness.file();
    H5C_t* cache = harness.cache();

    constexpr haddr_t kSrcTag = 0x5900;
    constexpr haddr_t kDestTag = 0x5A00;
    constexpr haddr_t kOtherTag = 0x5B00;

    // 1. Retag missing src_tag is a no-op; retag existing entries moves all of
    //    them to kDestTag.
    EXPECT_GE(H5C_retag_entries(cache, 0x9999, kDestTag), 0);

    SyntheticEntry* e1 =
        InsertSynthetic(harness, kBaseAddr, kSrcTag, &kPrimaryClass);
    SyntheticEntry* e2 =
        InsertSynthetic(harness, kBaseAddr + 0x1000, kSrcTag, &kSecondaryClass);
    SyntheticEntry* e_other =
        InsertSynthetic(harness, kBaseAddr + 0x2000, kOtherTag, &kPrimaryClass);
    ASSERT_NE(nullptr, e1);
    ASSERT_NE(nullptr, e2);
    ASSERT_NE(nullptr, e_other);

    ASSERT_GE(H5C_retag_entries(cache, kSrcTag, kDestTag), 0);
    haddr_t tag_out = HADDR_UNDEF;
    EXPECT_GE(H5C_get_tag(e1, &tag_out), 0);
    EXPECT_EQ(kDestTag, tag_out);
    EXPECT_GE(H5C_get_tag(e2, &tag_out), 0);
    EXPECT_EQ(kDestTag, tag_out);

    IterCollectCtx collect;
    EXPECT_GE(H5C__iter_tagged_entries(cache, kSrcTag, false,
                                       CollectTaggedAddrCb, &collect),
              0);
    EXPECT_TRUE(collect.addrs.empty());
    EXPECT_GE(H5C__iter_tagged_entries(cache, kDestTag, false,
                                       CollectTaggedAddrCb, &collect),
              0);
    EXPECT_THAT(collect.addrs,
                UnorderedElementsAre(kBaseAddr, kBaseAddr + 0x1000));

    // 2. Expunge only type id 0 under kDestTag; id 1 under kDestTag and id 0
    //    under kOtherTag remain.
    ASSERT_GE(H5C_expunge_tag_type_metadata(f, kDestTag, kPrimaryClass.id,
                                            H5C__NO_FLAGS_SET),
              0);
    bool in_cache = true;
    EXPECT_GE(H5C_get_entry_status(f, kBaseAddr, nullptr, &in_cache, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr),
              0);
    EXPECT_FALSE(in_cache);
    EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x1000, nullptr, &in_cache,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr),
              0);
    EXPECT_TRUE(in_cache);
    EXPECT_GE(H5C_get_entry_status(f, kBaseAddr + 0x2000, nullptr, &in_cache,
                                   nullptr, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr),
              0);
    EXPECT_TRUE(in_cache);

    // 3. Protected matching entry causes H5C_expunge_tag_type_metadata
    //    failure.
    void* prot = H5C_protect(f, &kSecondaryClass, kBaseAddr + 0x1000, nullptr,
                             H5C__NO_FLAGS_SET);
    ASSERT_EQ(e2, prot);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_expunge_tag_type_metadata(f, kDestTag, kSecondaryClass.id,
                                              H5C__NO_FLAGS_SET),
                0);
    }
    ASSERT_GE(H5C_unprotect(f, kBaseAddr + 0x1000, prot, H5C__NO_FLAGS_SET), 0);
  }

  // 4. Exercise H5Ocopy -> H5AC_retag_copied_metadata public path after
  //    harness restores the real file metadata cache.
  ScopedGroup src(H5Gcreate2(file.get(), "/src_copy", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(src.valid());
  EXPECT_GE(
      H5Ocopy(file.get(), "/src_copy", file.get(), "/dst_copy", H5P_DEFAULT,
              H5P_DEFAULT),
      0);
}

}  // namespace
}  // namespace hdf5
