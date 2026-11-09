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
// Tests metadata cache proxy entry lifecycle, parent/child flush dependency
// management, dirty/serialization propagation notifications, error handling,
// and real-world HDF5 SWMR/extensible-array callers
// (third_party/hdf5/src/H5ACproxy_entry.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5ACproxy_entry.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t), the first field of H5AC_proxy_entry_t.
// Match NDEBUG around both the internal prelude and internal headers so struct
// offsets match `:hdf5`.
#ifndef NDEBUG
#define H5ACPROXY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#include "third_party/hdf5/src/H5ACpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5ACPROXY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ACPROXY_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII wrapper pushing an API context and tag for internal H5AC/H5MF calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SUPERBLOCK_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
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

// Lightweight custom cache client entry to serve as parents and children of
// H5AC_proxy_entry_t in cache flush-dependency tests.
struct TestCacheEntry {
  H5AC_info_t cache_info{};
  bool evicted = false;
  int entry_dirtied_calls = 0;
  int entry_cleaned_calls = 0;
  int child_dirtied_calls = 0;
  int child_cleaned_calls = 0;
  int child_unserialized_calls = 0;
  int child_serialized_calls = 0;
};
static_assert(std::is_standard_layout_v<TestCacheEntry>);
static_assert(offsetof(TestCacheEntry, cache_info) == 0);

herr_t TestEntryImageLen(const void* /*thing*/, size_t* image_len) {
  *image_len = 64;
  return 0;
}

herr_t TestEntrySerialize(const H5F_t* /*f*/, void* image, size_t len,
                          void* /*thing*/) {
  std::memset(image, 0, len);
  return 0;
}

herr_t TestEntryNotify(H5AC_notify_action_t action, void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  switch (action) {
    case H5AC_NOTIFY_ACTION_ENTRY_DIRTIED:
      ++entry->entry_dirtied_calls;
      break;
    case H5AC_NOTIFY_ACTION_ENTRY_CLEANED:
      ++entry->entry_cleaned_calls;
      break;
    case H5AC_NOTIFY_ACTION_CHILD_DIRTIED:
      ++entry->child_dirtied_calls;
      break;
    case H5AC_NOTIFY_ACTION_CHILD_CLEANED:
      ++entry->child_cleaned_calls;
      break;
    case H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED:
      ++entry->child_unserialized_calls;
      break;
    case H5AC_NOTIFY_ACTION_CHILD_SERIALIZED:
      ++entry->child_serialized_calls;
      break;
    default:
      break;
  }
  return 0;
}

herr_t TestEntryFreeIcr(void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  entry->evicted = true;
  return 0;
}

constexpr H5AC_class_t kTestEntryClass = {
    H5AC_BT_ID,          // id
    "Test cache entry",  // name
    H5FD_MEM_BTREE,      // mem_type
    0,                   // flags
    nullptr,             // get_initial_load_size
    nullptr,             // get_final_load_size
    nullptr,             // verify_chksum
    nullptr,             // deserialize
    TestEntryImageLen,   // image_len
    nullptr,             // pre_serialize
    TestEntrySerialize,  // serialize
    TestEntryNotify,     // notify
    TestEntryFreeIcr,    // free_icr
    nullptr,             // fsf_size
};

// Owns a TestCacheEntry inserted into an HDF5 file cache and cleanly detaches
// and removes it before file teardown.
class ScopedCacheEntry {
 public:
  ScopedCacheEntry() = default;
  ~ScopedCacheEntry() { Reset(); }

  ScopedCacheEntry(const ScopedCacheEntry&) = delete;
  ScopedCacheEntry& operator=(const ScopedCacheEntry&) = delete;

  bool Insert(H5F_t* f, unsigned flags = H5AC__PIN_ENTRY_FLAG) {
    auto candidate = std::make_unique<TestCacheEntry>();
    const haddr_t addr = H5MF_alloc_tmp(f, 64);
    if (!H5_addr_defined(addr)) {
      return false;
    }
    if (H5AC_insert_entry(f, &kTestEntryClass, addr, candidate.get(), flags) <
        0) {
      return false;
    }
    entry_ = std::move(candidate);
    return true;
  }

  bool CleanAndSerialize() {
    if (!entry_) return false;
    if (entry_->cache_info.is_dirty &&
        H5AC_mark_entry_clean(entry_.get()) < 0) {
      return false;
    }
    if (!entry_->cache_info.image_up_to_date &&
        H5AC_mark_entry_serialized(entry_.get()) < 0) {
      return false;
    }
    return true;
  }

  void Reset() {
    if (entry_ && !entry_->evicted && entry_->cache_info.cache_ptr != nullptr) {
      if (entry_->cache_info.is_dirty) {
        H5AC_mark_entry_clean(entry_.get());
      }
      if (entry_->cache_info.is_pinned) {
        H5AC_unpin_entry(entry_.get());
      }
      H5AC_remove_entry(entry_.get());
    }
    entry_.reset();
  }

  TestCacheEntry* get() const { return entry_.get(); }

 private:
  std::unique_ptr<TestCacheEntry> entry_;
};

// Owns an H5AC_proxy_entry_t not currently registered in a cache (or cleaned up
// beforehand).
class ScopedProxyEntry {
 public:
  ScopedProxyEntry() : pentry_(H5AC_proxy_entry_create()) {}
  ~ScopedProxyEntry() { Reset(); }

  ScopedProxyEntry(const ScopedProxyEntry&) = delete;
  ScopedProxyEntry& operator=(const ScopedProxyEntry&) = delete;

  H5AC_proxy_entry_t* get() const { return pentry_; }
  H5AC_proxy_entry_t* release() {
    H5AC_proxy_entry_t* out = pentry_;
    pentry_ = nullptr;
    return out;
  }

  void Reset() {
    if (pentry_ != nullptr) {
      if (pentry_->cache_info.cache_ptr != nullptr) {
        if (pentry_->cache_info.is_dirty) {
          H5AC_mark_entry_clean(pentry_);
        }
        if (pentry_->cache_info.is_pinned) {
          H5AC_unpin_entry(pentry_);
        }
        H5AC_remove_entry(pentry_);
      }
      H5AC_proxy_entry_dest(pentry_);
      pentry_ = nullptr;
    }
  }

 private:
  H5AC_proxy_entry_t* pentry_ = nullptr;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

unsigned GetStatus(const H5F_t* f, haddr_t addr) {
  unsigned status = 0;
  EXPECT_GE(H5AC_get_entry_status(f, addr, &status), 0);
  return status;
}

TEST(H5ACproxyEntryTest, ClassDescriptorAndDirectCallbacksBehaveAsExpected) {
  ScopedFile file = CreateInMemoryFile("proxy_class.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(H5AC_PROXY_ENTRY[0].id, H5AC_PROXY_ENTRY_ID);
  EXPECT_STREQ(H5AC_PROXY_ENTRY[0].name, "Proxy entry");
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].mem_type, H5FD_MEM_SUPER);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].flags, 0U);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].get_initial_load_size, nullptr);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].get_final_load_size, nullptr);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].verify_chksum, nullptr);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].deserialize, nullptr);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].pre_serialize, nullptr);
  EXPECT_EQ(H5AC_PROXY_ENTRY[0].fsf_size, nullptr);
  ASSERT_NE(H5AC_PROXY_ENTRY[0].image_len, nullptr);
  ASSERT_NE(H5AC_PROXY_ENTRY[0].serialize, nullptr);
  ASSERT_NE(H5AC_PROXY_ENTRY[0].notify, nullptr);
  ASSERT_NE(H5AC_PROXY_ENTRY[0].free_icr, nullptr);

  ScopedProxyEntry proxy;
  ASSERT_NE(proxy.get(), nullptr);
  EXPECT_FALSE(H5_addr_defined(proxy.get()->addr));
  EXPECT_EQ(proxy.get()->parents, nullptr);
  EXPECT_EQ(proxy.get()->nchildren, 0U);
  EXPECT_EQ(proxy.get()->ndirty_children, 0U);
  EXPECT_EQ(proxy.get()->nunser_children, 0U);

  size_t image_len = 0;
  EXPECT_GE(H5AC_PROXY_ENTRY[0].image_len(proxy.get(), &image_len), 0);
  EXPECT_EQ(image_len, 1U);

  {
    ScopedErrorSilence silence;
    uint8_t buf[1] = {0};
    EXPECT_LT(H5AC_PROXY_ENTRY[0].serialize(f, buf, 1, proxy.get()), 0);

    // Valid notify actions that make no state assumptions succeed.
    EXPECT_GE(H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT,
                                         proxy.get()),
              0);
    EXPECT_GE(H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT,
                                         proxy.get()),
              0);
    EXPECT_GE(H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_ENTRY_CLEANED,
                                         proxy.get()),
              0);

    // Notify actions unreachable for proxy entries fail under -DNDEBUG.
    EXPECT_LT(
        H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD, proxy.get()),
        0);
    EXPECT_LT(
        H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_AFTER_FLUSH, proxy.get()),
        0);
    EXPECT_LT(H5AC_PROXY_ENTRY[0].notify(static_cast<H5AC_notify_action_t>(999),
                                         proxy.get()),
              0);
  }

  // Release the proxy through its free_icr callback.
  EXPECT_GE(H5AC_PROXY_ENTRY[0].free_icr(proxy.release()), 0);
}

TEST(H5ACproxyEntryTest, ParentAndChildLifecycleBeforeDuringAndAfterCaching) {
  ScopedFile file = CreateInMemoryFile("proxy_lifecycle.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedCacheEntry parent1;
  ScopedCacheEntry parent2;
  ScopedCacheEntry parent3;
  ScopedCacheEntry child1;
  ScopedCacheEntry child2;
  ASSERT_TRUE(parent1.Insert(f));
  ASSERT_TRUE(parent2.Insert(f));
  ASSERT_TRUE(parent3.Insert(f));
  ASSERT_TRUE(child1.Insert(f));
  ASSERT_TRUE(child2.Insert(f));
  ASSERT_TRUE(parent1.CleanAndSerialize());
  ASSERT_TRUE(parent2.CleanAndSerialize());
  ASSERT_TRUE(parent3.CleanAndSerialize());
  ASSERT_TRUE(child1.CleanAndSerialize());
  ASSERT_TRUE(child2.CleanAndSerialize());

  ScopedProxyEntry proxy;
  H5AC_proxy_entry_t* pentry = proxy.get();
  ASSERT_NE(pentry, nullptr);

  // Add two parents before any children are attached; proxy stays uncached.
  ASSERT_GE(H5AC_proxy_entry_add_parent(pentry, parent1.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_add_parent(pentry, parent2.get()), 0);
  EXPECT_NE(pentry->parents, nullptr);
  EXPECT_EQ(pentry->nchildren, 0U);
  EXPECT_FALSE(H5_addr_defined(pentry->addr));

  // Adding the first child allocates a temp address, inserts the proxy pinned,
  // clean, and serialized, and establishes parent->proxy->child dependencies.
  ASSERT_GE(H5AC_proxy_entry_add_child(pentry, f, child1.get()), 0);
  const haddr_t assigned_addr = pentry->addr;
  EXPECT_TRUE(H5_addr_defined(assigned_addr));
  EXPECT_EQ(pentry->nchildren, 1U);
  const unsigned status = GetStatus(f, assigned_addr);
  EXPECT_NE(status & H5AC_ES__IN_CACHE, 0U);
  EXPECT_NE(status & H5AC_ES__IS_PINNED, 0U);
  EXPECT_NE(status & H5AC_ES__IS_FLUSH_DEP_PARENT, 0U);
  EXPECT_NE(status & H5AC_ES__IS_FLUSH_DEP_CHILD, 0U);
  EXPECT_NE(status & H5AC_ES__IMAGE_IS_UP_TO_DATE, 0U);
  EXPECT_EQ(status & H5AC_ES__IS_DIRTY, 0U);

  // Adding another parent and child while cached updates flush dependencies.
  ASSERT_GE(H5AC_proxy_entry_add_parent(pentry, parent3.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_add_child(pentry, f, child2.get()), 0);
  EXPECT_EQ(pentry->nchildren, 2U);

  // Removing a parent while children exist destroys its flush dependency.
  ASSERT_GE(H5AC_proxy_entry_remove_parent(pentry, parent2.get()), 0);

  // Removing one of two children keeps the proxy cached.
  ASSERT_GE(H5AC_proxy_entry_remove_child(pentry, child2.get()), 0);
  EXPECT_EQ(pentry->nchildren, 1U);
  EXPECT_NE(GetStatus(f, assigned_addr) & H5AC_ES__IN_CACHE, 0U);

  // Removing the last child removes parent flush dependencies, unpins, and
  // removes the proxy from cache.
  ASSERT_GE(H5AC_proxy_entry_remove_child(pentry, child1.get()), 0);
  EXPECT_EQ(pentry->nchildren, 0U);
  EXPECT_EQ(GetStatus(f, assigned_addr), 0U);

  // Re-adding a child reuses the existing defined pentry->addr instead of
  // allocating a new temporary address.
  ASSERT_GE(H5AC_proxy_entry_add_child(pentry, f, child1.get()), 0);
  EXPECT_EQ(pentry->addr, assigned_addr);
  EXPECT_EQ(pentry->nchildren, 1U);
  EXPECT_NE(GetStatus(f, assigned_addr) & H5AC_ES__IN_CACHE, 0U);

  // Tear down remaining child and all parents (closes skip list -> nullptr).
  ASSERT_GE(H5AC_proxy_entry_remove_child(pentry, child1.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_remove_parent(pentry, parent1.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_remove_parent(pentry, parent3.get()), 0);
  EXPECT_EQ(pentry->parents, nullptr);
  EXPECT_EQ(pentry->nchildren, 0U);
}

TEST(H5ACproxyEntryTest,
     DirtyAndSerializationPropagateThroughMultipleChildren) {
  ScopedFile file = CreateInMemoryFile("proxy_propagate.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedCacheEntry parent;
  ScopedCacheEntry child1;
  ScopedCacheEntry child2;
  ASSERT_TRUE(parent.Insert(f));
  ASSERT_TRUE(child1.Insert(f));
  ASSERT_TRUE(child2.Insert(f));
  ASSERT_TRUE(parent.CleanAndSerialize());
  ASSERT_TRUE(child1.CleanAndSerialize());
  ASSERT_TRUE(child2.CleanAndSerialize());

  ScopedProxyEntry proxy;
  H5AC_proxy_entry_t* pentry = proxy.get();
  ASSERT_NE(pentry, nullptr);

  ASSERT_GE(H5AC_proxy_entry_add_parent(pentry, parent.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_add_child(pentry, f, child1.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_add_child(pentry, f, child2.get()), 0);

  // Dirtying the first child marks the proxy and parent dirty.
  const int initial_parent_dirtied = parent.get()->child_dirtied_calls;
  ASSERT_GE(H5AC_mark_entry_dirty(child1.get()), 0);
  EXPECT_EQ(pentry->ndirty_children, 1U);
  EXPECT_EQ(pentry->nunser_children, 1U);
  EXPECT_EQ(parent.get()->child_dirtied_calls, initial_parent_dirtied + 1);
  EXPECT_NE(GetStatus(f, pentry->addr) & H5AC_ES__IS_DIRTY, 0U);

  // Dirtying the second child increments counts without re-dirtying parent.
  ASSERT_GE(H5AC_mark_entry_dirty(child2.get()), 0);
  EXPECT_EQ(pentry->ndirty_children, 2U);
  EXPECT_EQ(pentry->nunser_children, 2U);
  EXPECT_EQ(parent.get()->child_dirtied_calls, initial_parent_dirtied + 1);

  // Marking one child clean and serialized leaves proxy dirty/unserialized.
  const int initial_parent_cleaned = parent.get()->child_cleaned_calls;
  const int initial_parent_ser = parent.get()->child_serialized_calls;
  ASSERT_GE(H5AC_mark_entry_clean(child1.get()), 0);
  ASSERT_GE(H5AC_mark_entry_serialized(child1.get()), 0);
  EXPECT_EQ(pentry->ndirty_children, 1U);
  EXPECT_EQ(pentry->nunser_children, 1U);
  EXPECT_EQ(parent.get()->child_cleaned_calls, initial_parent_cleaned);
  EXPECT_EQ(parent.get()->child_serialized_calls, initial_parent_ser);

  // Cleaning and serializing the second child propagates clean/serialized up.
  ASSERT_GE(H5AC_mark_entry_clean(child2.get()), 0);
  ASSERT_GE(H5AC_mark_entry_serialized(child2.get()), 0);
  EXPECT_EQ(pentry->ndirty_children, 0U);
  EXPECT_EQ(pentry->nunser_children, 0U);
  EXPECT_EQ(parent.get()->child_cleaned_calls, initial_parent_cleaned + 1);
  EXPECT_EQ(parent.get()->child_serialized_calls, initial_parent_ser + 1);
  EXPECT_EQ(GetStatus(f, pentry->addr) & H5AC_ES__IS_DIRTY, 0U);
  EXPECT_NE(GetStatus(f, pentry->addr) & H5AC_ES__IMAGE_IS_UP_TO_DATE, 0U);

  // Test standalone CHILD_UNSERIALIZED transition while children remain clean.
  const int before_unser = parent.get()->child_unserialized_calls;
  ASSERT_GE(H5AC_mark_entry_unserialized(child1.get()), 0);
  ASSERT_GE(H5AC_mark_entry_unserialized(child2.get()), 0);
  EXPECT_EQ(pentry->nunser_children, 2U);
  EXPECT_EQ(parent.get()->child_unserialized_calls, before_unser + 1);
  ASSERT_GE(H5AC_mark_entry_serialized(child1.get()), 0);
  ASSERT_GE(H5AC_mark_entry_serialized(child2.get()), 0);
  EXPECT_EQ(pentry->nunser_children, 0U);

  ASSERT_GE(H5AC_proxy_entry_remove_child(pentry, child1.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_remove_child(pentry, child2.get()), 0);
  ASSERT_GE(H5AC_proxy_entry_remove_parent(pentry, parent.get()), 0);
}

TEST(H5ACproxyEntryTest, HandlesErrorBranchesAndFailingCacheCallsCleanly) {
  ScopedFile file = CreateInMemoryFile("proxy_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedCacheEntry pinned_parent;
  ScopedCacheEntry unpinned_parent;
  ScopedCacheEntry missing_parent;
  ScopedCacheEntry child;
  ScopedCacheEntry unrelated;
  ASSERT_TRUE(pinned_parent.Insert(f, H5AC__PIN_ENTRY_FLAG));
  ASSERT_TRUE(unpinned_parent.Insert(f, H5AC__PIN_ENTRY_FLAG));
  ASSERT_TRUE(missing_parent.Insert(f, H5AC__PIN_ENTRY_FLAG));
  ASSERT_TRUE(child.Insert(f, H5AC__PIN_ENTRY_FLAG));
  ASSERT_TRUE(unrelated.Insert(f, H5AC__PIN_ENTRY_FLAG));
  ASSERT_TRUE(pinned_parent.CleanAndSerialize());
  ASSERT_TRUE(unpinned_parent.CleanAndSerialize());
  ASSERT_GE(H5AC_unpin_entry(unpinned_parent.get()), 0);
  ASSERT_TRUE(missing_parent.CleanAndSerialize());
  ASSERT_TRUE(child.CleanAndSerialize());
  ASSERT_TRUE(unrelated.CleanAndSerialize());

  ScopedErrorSilence silence;

  // 1. H5MF_alloc_tmp failure branch when temporary space is exhausted to EOA.
  {
    ScopedFile tmp_file = CreateInMemoryFile("proxy_alloc_fail.h5");
    ASSERT_TRUE(tmp_file.valid());
    ASSERT_GE(H5Fflush(tmp_file.get(), H5F_SCOPE_GLOBAL), 0);
    H5F_t* af = GetH5File(tmp_file.get());
    ASSERT_NE(af, nullptr);

    ScopedCacheEntry local_child;
    ASSERT_TRUE(local_child.Insert(af, H5AC__PIN_ENTRY_FLAG));
    ASSERT_TRUE(local_child.CleanAndSerialize());

    const haddr_t current_tmp = local_child.get()->cache_info.addr;
    const haddr_t eoa = H5F_get_eoa(af, H5FD_MEM_DEFAULT);
    ASSERT_TRUE(H5_addr_defined(eoa));
    ASSERT_GT(current_tmp, eoa + 1);
    ASSERT_TRUE(H5_addr_defined(H5MF_alloc_tmp(af, current_tmp - eoa - 1)));

    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    EXPECT_LT(H5AC_proxy_entry_add_child(p.get(), af, local_child.get()), 0);
    EXPECT_FALSE(H5_addr_defined(p.get()->addr));
  }

  // 2. H5AC_insert_entry failure when proxy's address collides in cache.
  {
    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    p.get()->addr = pinned_parent.get()->cache_info.addr;
    EXPECT_LT(H5AC_proxy_entry_add_child(p.get(), f, child.get()), 0);
    p.get()->addr = HADDR_UNDEF;
  }

  // 3. H5SL_iterate failure in add_child when a queued parent isn't pinned.
  {
    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    ASSERT_GE(H5AC_proxy_entry_add_parent(p.get(), unpinned_parent.get()), 0);
    EXPECT_LT(H5AC_proxy_entry_add_child(p.get(), f, child.get()), 0);
    EXPECT_EQ(p.get()->nchildren, 0U);
    // Clean up inserted pinned proxy and unpinned parent skip-list entry.
    ASSERT_GE(H5AC_proxy_entry_remove_parent(p.get(), unpinned_parent.get()),
              0);
    ASSERT_GE(H5AC_unpin_entry(p.get()), 0);
    ASSERT_GE(H5AC_remove_entry(p.get()), 0);
  }

  // 4. Duplicate parent insertion, missing parent removal, unpinned parent
  // while nchildren > 0, failed create_flush_dependency on add_child, and
  // failed remove_parent flush dependency destruction.
  {
    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    ASSERT_GE(H5AC_proxy_entry_add_parent(p.get(), pinned_parent.get()), 0);
    // Duplicate key in skip list -> H5SL_insert fails.
    EXPECT_LT(H5AC_proxy_entry_add_parent(p.get(), pinned_parent.get()), 0);
    // Missing parent removal -> H5SL_remove returns NULL.
    EXPECT_LT(H5AC_proxy_entry_remove_parent(p.get(), missing_parent.get()), 0);

    // Attach valid child so p->nchildren == 1.
    ASSERT_GE(H5AC_proxy_entry_add_child(p.get(), f, child.get()), 0);
    EXPECT_EQ(p.get()->nchildren, 1U);

    // Passing proxy as its own child fails H5AC_create_flush_dependency.
    EXPECT_LT(H5AC_proxy_entry_add_child(p.get(), f, p.get()), 0);
    EXPECT_EQ(p.get()->nchildren, 1U);

    // Removing non-child fails H5AC_destroy_flush_dependency.
    EXPECT_LT(H5AC_proxy_entry_remove_child(p.get(), unrelated.get()), 0);
    EXPECT_EQ(p.get()->nchildren, 1U);

    // Adding unpinned parent while nchildren > 0 inserts into skip list, then
    // fails flush dependency creation; removing it while pinned_parent remains
    // keeps skip list valid and fails flush dependency destruction.
    EXPECT_LT(H5AC_proxy_entry_add_parent(p.get(), unpinned_parent.get()), 0);
    EXPECT_LT(H5AC_proxy_entry_remove_parent(p.get(), unpinned_parent.get()),
              0);

    // Clean teardown.
    ASSERT_GE(H5AC_proxy_entry_remove_child(p.get(), child.get()), 0);
    ASSERT_GE(H5AC_proxy_entry_remove_parent(p.get(), pinned_parent.get()), 0);
  }

  // 5. H5SL_iterate failure in remove_child when parent dep already removed.
  {
    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    ASSERT_GE(H5AC_proxy_entry_add_parent(p.get(), pinned_parent.get()), 0);
    ASSERT_GE(H5AC_proxy_entry_add_child(p.get(), f, child.get()), 0);
    ASSERT_GE(H5AC_destroy_flush_dependency(pinned_parent.get(), p.get()), 0);
    EXPECT_LT(H5AC_proxy_entry_remove_child(p.get(), child.get()), 0);
    EXPECT_EQ(p.get()->nchildren, 0U);
    ASSERT_GE(H5AC_proxy_entry_remove_parent(p.get(), pinned_parent.get()), 0);
    ASSERT_GE(H5AC_unpin_entry(p.get()), 0);
    ASSERT_GE(H5AC_remove_entry(p.get()), 0);
  }

  // 6. H5AC__proxy_entry_notify error branches on CHILD_DIRTIED / CLEANED /
  // UNSERIALIZED / SERIALIZED when proxy is neither pinned nor protected.
  {
    ScopedProxyEntry p;
    ASSERT_NE(p.get(), nullptr);
    p.get()->cache_info.addr = 0x2000;
    EXPECT_LT(
        H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_CHILD_DIRTIED, p.get()),
        0);
    EXPECT_EQ(p.get()->ndirty_children, 1U);

    EXPECT_LT(
        H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_CHILD_CLEANED, p.get()),
        0);
    EXPECT_EQ(p.get()->ndirty_children, 0U);

    EXPECT_LT(H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
                                         p.get()),
              0);
    EXPECT_EQ(p.get()->nunser_children, 1U);

    EXPECT_LT(H5AC_PROXY_ENTRY[0].notify(H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
                                         p.get()),
              0);
    EXPECT_EQ(p.get()->nunser_children, 0U);
  }
}

TEST(H5ACproxyEntryTest, PublicChunkedAndSwmrOperationsExerciseProxyCallers) {
  // In-memory extensible-array chunked dataset exercises H5EAhdr proxy entries.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile file(
        H5Fcreate("ea_proxy.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> dims = {4};
    const std::array<hsize_t, 1> maxdims = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> chunk = {2};
    ScopedSpace space(H5Screate_simple(1, dims.data(), maxdims.data()));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk.data()), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "/ea_dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const std::vector<int> values = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, values.data()),
              0);
    const std::array<hsize_t, 1> new_dims = {8};
    ASSERT_GE(H5Dset_extent(dset.get(), new_dims.data()), 0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // SWMR write file exercises H5O header proxies alongside dataset flushes.
  {
    TempFile tmp("swmr_proxy.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
    ScopedFile file(H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                              H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> dims = {2};
    const std::array<hsize_t, 1> maxdims = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> chunk = {2};
    ScopedSpace space(H5Screate_simple(1, dims.data(), maxdims.data()));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk.data()), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "/swmr_dset", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const std::vector<int> values = {1, 2};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, values.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);
  }
}

}  // namespace
}  // namespace hdf5
