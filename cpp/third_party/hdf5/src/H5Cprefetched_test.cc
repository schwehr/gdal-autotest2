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
// Tests metadata cache prefetched entry callbacks, flush dependency teardown on
// eviction, in-core representation cleanup, and cache-image reconstruction
// (third_party/hdf5/src/H5Cprefetched.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL. Internal headers are wrapped with matching NDEBUG so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t struct layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cprefetched.c

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
#include "third_party/hdf5/src/H5FLprivate.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
H5FL_EXTERN(H5C_cache_entry_t);
}  // extern "C"

#ifdef H5C_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5C_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
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

constexpr size_t kDefaultEntrySize = 512;
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
    /*name=*/"synthetic_prefetched_parent",
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
      &kSyntheticClass, H5AC_PREFETCHED_ENTRY,
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
    cache_ = H5C_create(64 * 1024, 32 * 1024, H5AC_NTYPES - 1,
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
        cur->prefetched = false;
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

TEST(H5CprefetchedTest, ClassMetadataAndUnreachableCallbacksFailCleanly) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  const H5AC_class_t& cls = H5AC_PREFETCHED_ENTRY[0];
  EXPECT_EQ(H5AC_PREFETCHED_ENTRY_ID, cls.id);
  EXPECT_STREQ("prefetched entry", cls.name);
  EXPECT_EQ(H5FD_MEM_DEFAULT, cls.mem_type);
  EXPECT_EQ(H5AC__CLASS_NO_FLAGS_SET, cls.flags);

  ScopedErrorSilence silence;
  size_t len = 123;
  EXPECT_LT(cls.get_initial_load_size(nullptr, &len), 0);
  EXPECT_EQ(123u, len);

  size_t actual_len = 456;
  EXPECT_LT(cls.get_final_load_size(nullptr, 0, nullptr, &actual_len), 0);
  EXPECT_EQ(456u, actual_len);

  EXPECT_LT(cls.verify_chksum(nullptr, 0, nullptr), 0);

  bool dirty = true;
  EXPECT_EQ(nullptr, cls.deserialize(nullptr, 0, nullptr, &dirty));

  size_t image_len = 789;
  EXPECT_LT(cls.image_len(nullptr, &image_len), 0);
  EXPECT_EQ(789u, image_len);

  haddr_t new_addr = HADDR_UNDEF;
  size_t new_len = 0;
  unsigned flags = 0;
  EXPECT_LT(
      cls.pre_serialize(nullptr, nullptr, 0, 0, &new_addr, &new_len, &flags),
      0);

  EXPECT_LT(cls.serialize(nullptr, nullptr, 0, nullptr), 0);

  hsize_t fsf = 0;
  EXPECT_LT(cls.fsf_size(nullptr, &fsf), 0);
}

TEST(H5CprefetchedTest, NotifyIgnoresNonEvictActionsAndRejectsUnknownAction) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  H5C_cache_entry_t* entry = H5FL_CALLOC(H5C_cache_entry_t);
  ASSERT_NE(nullptr, entry);
  auto free_entry = absl::MakeCleanup([&]() {
    if (entry != nullptr) {
      entry = H5FL_FREE(H5C_cache_entry_t, entry);
    }
  });
  entry->prefetched = true;

  // All non-evict notification actions are intentional no-ops.
  constexpr auto kNoOpActions = std::to_array<H5C_notify_action_t>({
      H5C_NOTIFY_ACTION_AFTER_INSERT,
      H5C_NOTIFY_ACTION_AFTER_LOAD,
      H5C_NOTIFY_ACTION_AFTER_FLUSH,
      H5C_NOTIFY_ACTION_ENTRY_DIRTIED,
      H5C_NOTIFY_ACTION_ENTRY_CLEANED,
      H5C_NOTIFY_ACTION_CHILD_DIRTIED,
      H5C_NOTIFY_ACTION_CHILD_CLEANED,
      H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED,
      H5C_NOTIFY_ACTION_CHILD_SERIALIZED,
      H5C_NOTIFY_ACTION_BEFORE_EVICT,
  });
  for (H5C_notify_action_t action : kNoOpActions) {
    EXPECT_GE(H5AC_PREFETCHED_ENTRY[0].notify(action, entry), 0);
  }

  // Value 10 exceeds H5C_NOTIFY_ACTION_CHILD_SERIALIZED (9) while staying inside
  // the 4-bit C++ representation range [0, 15] of H5C_notify_action_t.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_PREFETCHED_ENTRY[0].notify(
                  static_cast<H5C_notify_action_t>(10), entry),
              0);
  }
}

TEST(H5CprefetchedTest, NotifyBeforeEvictDestroysFlushDepsAndHandlesErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  CustomCacheHarness harness(file.get());
  ASSERT_TRUE(harness.valid());
  H5C_t* cache = harness.cache();

  // 1. Insert a regular parent and a prefetched-flag parent in the cache.
  auto* reg_parent =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, reg_parent);
  reg_parent->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(harness.file(), &kSyntheticClass, kBaseAddr,
                             reg_parent, H5C__NO_FLAGS_SET),
            0);
  void* prot_reg = H5C_protect(harness.file(), &kSyntheticClass, kBaseAddr,
                               nullptr, H5C__NO_FLAGS_SET);
  ASSERT_EQ(reg_parent, prot_reg);

  auto* pf_parent =
      static_cast<SyntheticEntry*>(std::calloc(1, sizeof(SyntheticEntry)));
  ASSERT_NE(nullptr, pf_parent);
  pf_parent->entry_size = kDefaultEntrySize;
  ASSERT_GE(H5C_insert_entry(harness.file(), &kSyntheticClass,
                             kBaseAddr + 0x1000, pf_parent,
                             H5C__PIN_ENTRY_FLAG),
            0);

  H5C_cache_entry_t* child = H5FL_CALLOC(H5C_cache_entry_t);
  ASSERT_NE(nullptr, child);
  auto cleanup_child = absl::MakeCleanup([&]() {
    pf_parent->header.prefetched = false;
    while (child != nullptr && child->flush_dep_nparents > 0) {
      H5C_cache_entry_t* p = child->flush_dep_parent[0];
      p->is_pinned = true;
      H5C_destroy_flush_dependency(p, child);
    }
    if (child != nullptr) {
      H5AC_PREFETCHED_ENTRY[0].free_icr(child);
      child = nullptr;
    }
  });

  child->cache_ptr = cache;
  child->addr = kBaseAddr + 0x2000;
  child->prefetched = true;
  child->is_dirty = false;
  child->image_up_to_date = true;

  // Link both parents to child; reg_parent is auto-pinned from cache, while
  // pf_parent carries prefetched=true and fd_child_count=1.
  ASSERT_GE(H5C_create_flush_dependency(&reg_parent->header, child), 0);
  ASSERT_GE(H5C_unprotect(harness.file(), kBaseAddr, prot_reg, H5C__NO_FLAGS_SET),
            0);
  EXPECT_TRUE(reg_parent->header.is_pinned);

  ASSERT_GE(H5C_create_flush_dependency(&pf_parent->header, child), 0);
  pf_parent->header.prefetched = true;
  pf_parent->header.fd_child_count = 1;
  EXPECT_EQ(2u, child->flush_dep_nparents);

  // 2. BEFORE_EVICT destroys both flush dependencies and decrements the
  //    prefetched parent's fd_child_count to zero.
  EXPECT_GE(
      H5AC_PREFETCHED_ENTRY[0].notify(H5C_NOTIFY_ACTION_BEFORE_EVICT, child),
      0);
  EXPECT_EQ(0u, child->flush_dep_nparents);
  EXPECT_EQ(0u, reg_parent->header.flush_dep_nchildren);
  EXPECT_FALSE(reg_parent->header.is_pinned);
  EXPECT_EQ(0u, pf_parent->header.fd_child_count);

  // Clear temporary test-only prefetched flag before step 3; pf_parent stays
  // client-pinned so step 3 can attach failing_child as a flush child.
  pf_parent->header.prefetched = false;
  ASSERT_GE(H5AC_PREFETCHED_ENTRY[0].free_icr(child), 0);
  child = nullptr;

  // 3. Verify BEFORE_EVICT propagates H5C_destroy_flush_dependency failures.
  H5C_cache_entry_t* failing_child = H5FL_CALLOC(H5C_cache_entry_t);
  ASSERT_NE(nullptr, failing_child);
  auto cleanup_failing = absl::MakeCleanup([&]() {
    if (failing_child != nullptr) {
      while (failing_child->flush_dep_nparents > 0) {
        H5C_cache_entry_t* p = failing_child->flush_dep_parent[0];
        p->is_pinned = true;
        H5C_destroy_flush_dependency(p, failing_child);
      }
      H5AC_PREFETCHED_ENTRY[0].free_icr(failing_child);
      failing_child = nullptr;
    }
  });
  failing_child->cache_ptr = cache;
  failing_child->addr = kBaseAddr + 0x3000;
  failing_child->prefetched = true;
  failing_child->is_dirty = false;
  failing_child->image_up_to_date = true;

  ASSERT_GE(H5C_create_flush_dependency(&pf_parent->header, failing_child), 0);

  pf_parent->header.is_pinned = false;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_PREFETCHED_ENTRY[0].notify(H5C_NOTIFY_ACTION_BEFORE_EVICT,
                                              failing_child),
              0);
  }
  pf_parent->header.is_pinned = true;
}

TEST(H5CprefetchedTest, FreeIcrReleasesParentAddrsAndRejectsAttachedImage) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Entry with allocated fd_parent_addrs and null image_ptr succeeds cleanly.
  H5C_cache_entry_t* with_addrs = H5FL_CALLOC(H5C_cache_entry_t);
  ASSERT_NE(nullptr, with_addrs);
  with_addrs->prefetched = true;
  with_addrs->fd_parent_count = 2;
  with_addrs->fd_parent_addrs =
      static_cast<haddr_t*>(H5MM_malloc(2 * sizeof(haddr_t)));
  ASSERT_NE(nullptr, with_addrs->fd_parent_addrs);
  with_addrs->fd_parent_addrs[0] = kBaseAddr;
  with_addrs->fd_parent_addrs[1] = kBaseAddr + 0x100;
  EXPECT_GE(H5AC_PREFETCHED_ENTRY[0].free_icr(with_addrs), 0);

  // 2. Entry with attached image_ptr triggers error without freeing entry_ptr.
  H5C_cache_entry_t* with_image = H5FL_CALLOC(H5C_cache_entry_t);
  ASSERT_NE(nullptr, with_image);
  with_image->prefetched = true;
  with_image->image_ptr = H5MM_malloc(64);
  ASSERT_NE(nullptr, with_image->image_ptr);
  auto cleanup_image_entry = absl::MakeCleanup([&]() {
    if (with_image != nullptr) {
      with_image->image_ptr = H5MM_xfree(with_image->image_ptr);
      with_image = H5FL_FREE(H5C_cache_entry_t, with_image);
    }
  });

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_PREFETCHED_ENTRY[0].free_icr(with_image), 0);
  }
}

TEST(H5CprefetchedTest, CacheImageRoundTripReconstructsAndFreesPrefetched) {
  TempFile temp("h5c_prefetched_cache_image.h5");

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

  // 1. Create a file with simple compact metadata (avoiding dense fractal-heap
  //    flush dependencies) so cache image captures entries on close cleanly
  //    even under Google3's -DNDEBUG build.
  {
    ScopedFile file(H5Fcreate(temp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup grp(H5Gcreate2(file.get(), "/simple_grp", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    const hsize_t dims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const int vals[4] = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals),
              0);
  }

  // 2. Reopen read-only to reconstruct prefetched entries in the cache, touch
  //    one object to force cache-image load, and verify close frees them.
  img_cfg.generate_image = false;
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);
  {
    ScopedFile ro_file(H5Fopen(temp.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro_file.valid());

    haddr_t img_addr = HADDR_UNDEF;
    hsize_t img_size = 0;
    EXPECT_GE(H5Fget_mdc_image_info(ro_file.get(), &img_addr, &img_size), 0);
    EXPECT_TRUE(H5_addr_defined(img_addr));
    EXPECT_GT(img_size, 0u);

    ScopedGroup grp(H5Gopen2(ro_file.get(), "/simple_grp", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  // 3. Reopen read-write, load cache image, and evict clean entries so
  //    H5C__prefetched_entry_notify and free_icr execute during live eviction.
  {
    ScopedFile rw_file(H5Fopen(temp.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(rw_file.valid());
    {
      ScopedGroup grp(H5Gopen2(rw_file.get(), "/simple_grp", H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }
    H5F_t* f = GetFileStruct(rw_file.get());
    ASSERT_NE(nullptr, f);
    EXPECT_GE(H5C_evict(f), 0);
  }
}

}  // namespace
}  // namespace hdf5
