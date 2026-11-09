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
// Unit tests for HDF5 metadata cache entry operations
// (third_party/hdf5/src/H5Centry.c): insertion, protect/unprotect, loading,
// EOA checks, read retries, prefetched entry deserialization, serialization and
// image generation, flushing, discarding, pin/unpin, dirty/clean/serialize
// state transitions, move, resize, ring unsettling, flush dependencies, expunge,
// and removal.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Centry.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t and H5C_t. Wrap internal HDF5 headers with NDEBUG=1 so
// C++ struct offsets and sizes match `:hdf5` exactly.
#ifndef NDEBUG
#define H5C_ENTRY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
// `H5Fpkg.h` uses `try` as a parameter name (`bool try`), which collides with
// both the C++ keyword and libcxx's `#define try if (1)` when exceptions are
// disabled. Rename it temporarily for the C header declaration.
#pragma push_macro("try")
#undef try
#define try h5f_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
H5FL_EXTERN(H5C_cache_entry_t);
}  // extern "C"
#ifdef H5C_ENTRY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5C_ENTRY_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

static_assert(H5AC_PREFETCHED_ENTRY_ID == 29);

constexpr int kTypeOhdrLike = 5;
constexpr int kTypeRegular = 10;
constexpr int kTypeSpeculative = 11;
constexpr int kTypeSkipReadsWrites = 12;
constexpr int kTypeNoVerify = 13;
constexpr int kTypeGheap = 14;
constexpr int kMaxTypeId = H5AC_PREFETCHED_ENTRY_ID;

// RAII API context wrapper pushing/popping H5CX state and setting the active
// metadata cache ring for cache entry calls.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(H5C_ring_t ring = H5C_RING_USER,
                            haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5CX_set_ring(ring);
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

struct CallbackState;

struct TestEntry {
  H5C_cache_entry_t header;
  CallbackState* ctrl;
  uint64_t tag_value;
  size_t len;
  size_t pre_ser_new_len;
  haddr_t pre_ser_new_addr;
  unsigned pre_ser_flags;
  bool pre_ser_call_move;
  hsize_t fsf_override;
  bool use_fsf_override;
  uint8_t fill_byte;
};

static_assert(std::is_standard_layout_v<TestEntry>);
static_assert(offsetof(TestEntry, header) == 0);

struct LoadUdata {
  CallbackState* ctrl = nullptr;
  H5F_t* f = nullptr;
  size_t initial_len = 64;
  size_t final_len = 64;
  bool force_dirty_on_deserialize = false;
  uint8_t fill_byte = 0x5A;
};

struct CallbackState {
  // Callback call counters.
  int get_initial_calls = 0;
  int get_final_calls = 0;
  int verify_chksum_calls = 0;
  int deserialize_calls = 0;
  int image_len_calls = 0;
  int pre_serialize_calls = 0;
  int serialize_calls = 0;
  int free_icr_calls = 0;
  int fsf_size_calls = 0;
  int log_flush_calls = 0;
  int write_permitted_calls = 0;
  int auto_resize_rpt_calls = 0;
  std::vector<H5C_notify_action_t> notify_actions;

  // Failure & behavior toggles (reset individually in tests).
  bool fail_get_initial = false;
  int fail_get_final_remaining = 0;
  bool return_bad_final_len_on_fail = false;
  size_t bad_final_len = 128;
  bool fail_get_final_zero = false;
  bool fail_deserialize = false;
  bool fail_image_len = false;
  bool fail_pre_serialize = false;
  bool fail_serialize = false;
  bool fail_free_icr = false;
  bool defer_free_icr = false;
  bool fail_fsf_size = false;
  bool fail_log_flush = false;
  bool fail_write_permitted = false;
  bool write_permitted_value = true;
  bool fail_verify_chksum = false;
  int verify_chksum_false_remaining = 0;
  bool fail_notify = false;
  bool filter_notify_action = false;
  H5C_notify_action_t fail_notify_action = H5C_NOTIFY_ACTION_AFTER_INSERT;

  // Tracks last allocated entry returned by deserialize() in case an error
  // happens after load before insertion into the index.
  TestEntry* last_deserialized = nullptr;
  haddr_t last_log_flush_addr = HADDR_UNDEF;
  bool last_log_flush_was_dirty = false;
  unsigned last_log_flush_flags = 0;
  H5C_t* cache_for_move = nullptr;
};

TestEntry* MakeTestEntry(CallbackState* ctrl, size_t len, uint8_t fill = 0x2A,
                         uint64_t tag = 1) {
  auto* e = new TestEntry{};
  e->header.type = nullptr;
  e->ctrl = ctrl;
  e->tag_value = tag;
  e->len = len;
  e->fill_byte = fill;
  return e;
}

bool ShouldFailNotify(CallbackState* ctrl, H5C_notify_action_t action) {
  if (ctrl == nullptr || !ctrl->fail_notify) return false;
  if (!ctrl->filter_notify_action) return true;
  return ctrl->fail_notify_action == action;
}

herr_t CbGetInitialLoadSize(void* udata_ptr, size_t* image_len_ptr) {
  auto* ud = static_cast<LoadUdata*>(udata_ptr);
  ++ud->ctrl->get_initial_calls;
  if (ud->ctrl->fail_get_initial) return -1;
  *image_len_ptr = ud->initial_len;
  return 0;
}

herr_t CbGetFinalLoadSize(const void* /*image_ptr*/, size_t image_len,
                          void* udata_ptr, size_t* actual_len_ptr) {
  auto* ud = static_cast<LoadUdata*>(udata_ptr);
  ++ud->ctrl->get_final_calls;
  if (ud->ctrl->fail_get_final_remaining > 0) {
    --ud->ctrl->fail_get_final_remaining;
    if (ud->ctrl->return_bad_final_len_on_fail) {
      *actual_len_ptr = ud->ctrl->bad_final_len;
    }
    return -1;
  }
  if (ud->ctrl->fail_get_final_zero) {
    *actual_len_ptr = 0;
    return 0;
  }
  *actual_len_ptr =
      (ud->final_len == 0) ? image_len : ud->final_len;
  return 0;
}

htri_t CbVerifyChksum(const void* /*image_ptr*/, size_t /*len*/,
                      void* udata_ptr) {
  auto* ud = static_cast<LoadUdata*>(udata_ptr);
  ++ud->ctrl->verify_chksum_calls;
  if (ud->ctrl->fail_verify_chksum) return -1;
  if (ud->ctrl->verify_chksum_false_remaining > 0) {
    --ud->ctrl->verify_chksum_false_remaining;
    return 0;
  }
  return 1;
}

void* CbDeserialize(const void* image_ptr, size_t len, void* udata_ptr,
                    bool* dirty_ptr) {
  auto* ud = static_cast<LoadUdata*>(udata_ptr);
  ++ud->ctrl->deserialize_calls;
  if (ud->ctrl->fail_deserialize) return nullptr;
  auto* e = MakeTestEntry(ud->ctrl, len, ud->fill_byte);
  if (image_ptr != nullptr && len > 0) {
    e->fill_byte = static_cast<const uint8_t*>(image_ptr)[0];
  }
  if (ud->force_dirty_on_deserialize && dirty_ptr != nullptr) {
    *dirty_ptr = true;
  }
  ud->ctrl->last_deserialized = e;
  return e;
}

herr_t CbImageLen(const void* thing, size_t* image_len_ptr) {
  const auto* e = static_cast<const TestEntry*>(thing);
  if (e->ctrl != nullptr) {
    ++e->ctrl->image_len_calls;
    if (e->ctrl->fail_image_len) return -1;
  }
  *image_len_ptr = e->len;
  return 0;
}

herr_t CbPreSerialize(H5F_t* /*f*/, void* thing, haddr_t addr, size_t /*len*/,
                      haddr_t* new_addr_ptr, size_t* new_len_ptr,
                      unsigned* flags_ptr) {
  auto* e = static_cast<TestEntry*>(thing);
  ++e->ctrl->pre_serialize_calls;
  if (e->ctrl->fail_pre_serialize) return -1;
  *flags_ptr = e->pre_ser_flags;
  if ((e->pre_ser_flags & H5C__SERIALIZE_RESIZED_FLAG) != 0) {
    e->len = e->pre_ser_new_len;
    *new_len_ptr = e->pre_ser_new_len;
  }
  if ((e->pre_ser_flags & H5C__SERIALIZE_MOVED_FLAG) != 0) {
    if (e->pre_ser_call_move && e->ctrl->cache_for_move != nullptr) {
      if (H5C_move_entry(e->ctrl->cache_for_move, e->header.type, addr,
                         e->pre_ser_new_addr) < 0) {
        return -1;
      }
    }
    *new_addr_ptr = e->pre_ser_new_addr;
  }
  return 0;
}

herr_t CbSerialize(const H5F_t* /*f*/, void* image_ptr, size_t len,
                   void* thing) {
  const auto* e = static_cast<const TestEntry*>(thing);
  ++e->ctrl->serialize_calls;
  if (e->ctrl->fail_serialize) return -1;
  std::memset(image_ptr, e->fill_byte, len);
  return 0;
}

herr_t CbNotify(H5C_notify_action_t action, void* thing) {
  auto* e = static_cast<TestEntry*>(thing);
  if (e->ctrl != nullptr) {
    e->ctrl->notify_actions.push_back(action);
    if (ShouldFailNotify(e->ctrl, action)) return -1;
  }
  return 0;
}

herr_t CbFreeIcr(void* thing) {
  auto* e = static_cast<TestEntry*>(thing);
  CallbackState* ctrl = e->ctrl;
  if (ctrl != nullptr) {
    ++ctrl->free_icr_calls;
    if (ctrl->defer_free_icr) return 0;
    if (ctrl->fail_free_icr) return -1;
  }
  delete e;
  return 0;
}

herr_t CbFsfSize(const void* thing, hsize_t* fsf_size_ptr) {
  const auto* e = static_cast<const TestEntry*>(thing);
  ++e->ctrl->fsf_size_calls;
  if (e->ctrl->fail_fsf_size) return -1;
  *fsf_size_ptr = e->use_fsf_override ? e->fsf_override : e->len;
  return 0;
}

herr_t CbCheckWritePermitted(const H5F_t* f, bool* write_permitted_ptr) {
  if (f != nullptr && f->shared != nullptr && f->shared->cache != nullptr &&
      f->shared->cache->aux_ptr != nullptr) {
    auto* ctrl = static_cast<CallbackState*>(f->shared->cache->aux_ptr);
    ++ctrl->write_permitted_calls;
    if (ctrl->fail_write_permitted) return -1;
    *write_permitted_ptr = ctrl->write_permitted_value;
    return 0;
  }
  *write_permitted_ptr = true;
  return 0;
}

herr_t CbLogFlush(H5C_t* cache_ptr, haddr_t addr, bool was_dirty,
                  unsigned flags) {
  if (cache_ptr != nullptr && cache_ptr->aux_ptr != nullptr) {
    auto* ctrl = static_cast<CallbackState*>(cache_ptr->aux_ptr);
    ++ctrl->log_flush_calls;
    ctrl->last_log_flush_addr = addr;
    ctrl->last_log_flush_was_dirty = was_dirty;
    ctrl->last_log_flush_flags = flags;
    if (ctrl->fail_log_flush) return -1;
  }
  return 0;
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct ClassBundle {
  H5C_class_t ohdr_like{};
  H5C_class_t regular{};
  H5C_class_t speculative{};
  H5C_class_t skip_rw{};
  H5C_class_t no_verify{};
  H5C_class_t gheap{};
  std::array<const H5C_class_t*, kMaxTypeId + 1> table{};

  ClassBundle() {
    ohdr_like = {
        /*id=*/kTypeOhdrLike,
        /*name=*/"ohdr_like",
        /*mem_type=*/H5FD_MEM_OHDR,
        /*flags=*/H5C__CLASS_NO_FLAGS_SET,
        /*get_initial_load_size=*/CbGetInitialLoadSize,
        /*get_final_load_size=*/nullptr,
        /*verify_chksum=*/CbVerifyChksum,
        /*deserialize=*/CbDeserialize,
        /*image_len=*/CbImageLen,
        /*pre_serialize=*/CbPreSerialize,
        /*serialize=*/CbSerialize,
        /*notify=*/CbNotify,
        /*free_icr=*/CbFreeIcr,
        /*fsf_size=*/CbFsfSize,
    };
    regular = ohdr_like;
    regular.id = kTypeRegular;
    regular.name = "regular";
    regular.mem_type = H5FD_MEM_BTREE;

    speculative = regular;
    speculative.id = kTypeSpeculative;
    speculative.name = "speculative";
    speculative.flags = H5C__CLASS_SPECULATIVE_LOAD_FLAG;
    speculative.get_final_load_size = CbGetFinalLoadSize;

    skip_rw = {
        /*id=*/kTypeSkipReadsWrites,
        /*name=*/"skip_rw",
        /*mem_type=*/H5FD_MEM_BTREE,
        /*flags=*/H5C__CLASS_SKIP_READS | H5C__CLASS_SKIP_WRITES,
        /*get_initial_load_size=*/CbGetInitialLoadSize,
        /*get_final_load_size=*/nullptr,
        /*verify_chksum=*/nullptr,
        /*deserialize=*/CbDeserialize,
        /*image_len=*/CbImageLen,
        /*pre_serialize=*/nullptr,
        /*serialize=*/CbSerialize,
        /*notify=*/nullptr,
        /*free_icr=*/CbFreeIcr,
        /*fsf_size=*/nullptr,
    };

    no_verify = {
        /*id=*/kTypeNoVerify,
        /*name=*/"no_verify",
        /*mem_type=*/H5FD_MEM_BTREE,
        /*flags=*/H5C__CLASS_NO_FLAGS_SET,
        /*get_initial_load_size=*/CbGetInitialLoadSize,
        /*get_final_load_size=*/nullptr,
        /*verify_chksum=*/nullptr,
        /*deserialize=*/CbDeserialize,
        /*image_len=*/CbImageLen,
        /*pre_serialize=*/nullptr,
        /*serialize=*/CbSerialize,
        /*notify=*/nullptr,
        /*free_icr=*/CbFreeIcr,
        /*fsf_size=*/nullptr,
    };

    gheap = speculative;
    gheap.id = kTypeGheap;
    gheap.name = "gheap";
    gheap.mem_type = H5FD_MEM_GHEAP;

    table[kTypeOhdrLike] = &ohdr_like;
    table[kTypeRegular] = &regular;
    table[kTypeSpeculative] = &speculative;
    table[kTypeSkipReadsWrites] = &skip_rw;
    table[kTypeNoVerify] = &no_verify;
    table[kTypeGheap] = &gheap;
    table[H5AC_PREFETCHED_ENTRY_ID] = H5AC_PREFETCHED_ENTRY;
  }
};

// Fixture swapping `f->shared->cache` with an isolated `H5C_t*` backed by
// synthetic test classes while preserving file/EOA/FSM/PB support from HDF5.
struct CacheFixture {
  CallbackState ctrl;
  ClassBundle classes;
  ScopedFile file;
  H5F_t* f = nullptr;
  H5C_t* orig_cache = nullptr;
  H5C_t* tc = nullptr;

  ~CacheFixture() { Teardown(); }

  bool InitCore(size_t max_cache_size = 64 * 1024,
                size_t min_clean_size = 16 * 1024,
                bool with_write_cb = true, bool with_log_flush = true,
                H5F_fspace_strategy_t fs_strategy = H5F_FSPACE_STRATEGY_AGGR,
                hsize_t page_size = 4096, size_t page_buf_size = 0) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    if (!fcpl.valid()) return false;
    if (H5Pset_file_space_strategy(fcpl.get(), fs_strategy, false, 1) < 0) {
      return false;
    }
    if (fs_strategy == H5F_FSPACE_STRATEGY_PAGE &&
        H5Pset_file_space_page_size(fcpl.get(), page_size) < 0) {
      return false;
    }
    ScopedPlist fapl = MakeCoreFapl();
    if (!fapl.valid()) return false;
    if (page_buf_size > 0 &&
        H5Pset_page_buffer_size(fapl.get(), page_buf_size, 50, 50) < 0) {
      return false;
    }
    file = ScopedFile(
        H5Fcreate("h5centry_test.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    if (!file.valid()) return false;
    return AttachTestCache(max_cache_size, min_clean_size, with_write_cb,
                           with_log_flush);
  }

  bool InitSwmrTempFile(const std::string& path, unsigned read_attempts,
                        size_t max_cache_size = 64 * 1024,
                        size_t min_clean_size = 16 * 1024) {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    if (!fapl.valid()) return false;
    if (H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
        0) {
      return false;
    }
    if (H5Pset_metadata_read_attempts(fapl.get(), read_attempts) < 0) {
      return false;
    }
    file = ScopedFile(H5Fcreate(path.c_str(),
                                H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                                H5P_DEFAULT, fapl.get()));
    if (!file.valid()) return false;
    return AttachTestCache(max_cache_size, min_clean_size,
                           /*with_write_cb=*/true, /*with_log_flush=*/true);
  }

  bool AttachTestCache(size_t max_cache_size, size_t min_clean_size,
                       bool with_write_cb, bool with_log_flush) {
    f = GetH5File(file.get());
    if (f == nullptr || f->shared == nullptr) return false;
    orig_cache = f->shared->cache;
    tc = H5C_create(
        max_cache_size, min_clean_size, kMaxTypeId, classes.table.data(),
        with_write_cb ? CbCheckWritePermitted : nullptr, true,
        with_log_flush ? CbLogFlush : nullptr, &ctrl);
    if (tc == nullptr) return false;
    H5C_ignore_tags(tc);
    f->shared->cache = tc;
    ctrl.cache_for_move = tc;
    return true;
  }

  haddr_t AllocSpace(size_t size, H5FD_mem_t mem_type = H5FD_MEM_BTREE) {
    ScopedApiContext ctx;
    if (!ctx.valid() || f == nullptr) return HADDR_UNDEF;
    H5C_t* saved = f->shared->cache;
    f->shared->cache = orig_cache;
    haddr_t addr = H5MF_alloc(f, mem_type, size);
    f->shared->cache = saved;
    return addr;
  }

  void Teardown() {
    ctrl.fail_notify = false;
    ctrl.fail_pre_serialize = false;
    ctrl.fail_serialize = false;
    ctrl.fail_free_icr = false;
    ctrl.defer_free_icr = false;
    ctrl.fail_fsf_size = false;
    ctrl.fail_log_flush = false;
    ctrl.fail_write_permitted = false;
    ctrl.write_permitted_value = true;
    if (f != nullptr && f->shared != nullptr && tc != nullptr) {
      ScopedApiContext ctx;
      f->shared->cache = tc;
      tc->close_warning_received = true;
      tc->write_permitted = true;
      if (tc->slist_enabled) {
        EXPECT_GE(H5C_flush_cache(f, H5C__NO_FLAGS_SET), 0);
        EXPECT_GE(H5C_set_slist_enabled(tc, false, false), 0);
      }
      EXPECT_GE(H5C_dest(f), 0);
      tc = nullptr;
      f->shared->cache = orig_cache;
    }
    file.reset();
    f = nullptr;
    orig_cache = nullptr;
  }
};

// Builds a synthetic prefetched entry and inserts it into `cache_ptr` using the
// exact index/slist/LRU invariant sequence used by H5C__load_cache_image.
H5C_cache_entry_t* InsertSyntheticPrefetched(
    H5C_t* cache_ptr, haddr_t addr, size_t size, int prefetch_type_id,
    bool is_dirty, bool prefetched_dirty, uint8_t fill_byte,
    const std::vector<H5C_cache_entry_t*>& parents = {}) {
  H5C_cache_entry_t* pf = H5FL_CALLOC(H5C_cache_entry_t);
  if (pf == nullptr) return nullptr;
  pf->image_ptr = H5MM_malloc(size);
  if (pf->image_ptr == nullptr) {
    H5FL_FREE(H5C_cache_entry_t, pf);
    return nullptr;
  }
  std::memset(pf->image_ptr, fill_byte, size);
  pf->cache_ptr = cache_ptr;
  pf->addr = addr;
  pf->size = size;
  pf->image_up_to_date = true;
  pf->type = H5AC_PREFETCHED_ENTRY;
  pf->is_dirty = is_dirty;
  pf->ring = H5C_RING_USER;
  pf->prefetched = true;
  pf->prefetch_type_id = prefetch_type_id;
  pf->prefetched_dirty = prefetched_dirty;

  if (!parents.empty()) {
    pf->fd_parent_addrs = static_cast<haddr_t*>(
        H5MM_malloc(parents.size() * sizeof(haddr_t)));
    if (pf->fd_parent_addrs == nullptr) {
      H5MM_xfree(pf->image_ptr);
      H5FL_FREE(H5C_cache_entry_t, pf);
      return nullptr;
    }
    pf->fd_parent_count = parents.size();
    for (size_t i = 0; i < parents.size(); ++i) {
      pf->fd_parent_addrs[i] = parents[i]->addr;
    }
  }

  H5C__INSERT_IN_INDEX(cache_ptr, pf, FAIL);
  if (pf->is_dirty && cache_ptr->slist_enabled) {
    if (H5SL_insert(cache_ptr->slist_ptr, pf, &(pf->addr)) >= 0) {
      pf->in_slist = true;
      cache_ptr->slist_changed = true;
      cache_ptr->slist_len++;
      cache_ptr->slist_size += pf->size;
      cache_ptr->slist_ring_len[pf->ring]++;
      cache_ptr->slist_ring_size[pf->ring] += pf->size;
    }
  }
  H5C__DLL_APPEND(pf, cache_ptr->LRU_head_ptr, cache_ptr->LRU_tail_ptr,
                  cache_ptr->LRU_list_len, cache_ptr->LRU_list_size, FAIL);

  for (H5C_cache_entry_t* parent : parents) {
    if (!parent->is_protected) {
      if (!parent->is_pinned) {
        H5C__DLL_REMOVE(parent, cache_ptr->LRU_head_ptr, cache_ptr->LRU_tail_ptr,
                        cache_ptr->LRU_list_len, cache_ptr->LRU_list_size, FAIL);
      } else {
        H5C__DLL_REMOVE(parent, cache_ptr->pel_head_ptr, cache_ptr->pel_tail_ptr,
                        cache_ptr->pel_len, cache_ptr->pel_size, FAIL);
      }
      parent->is_protected = true;
      H5C__DLL_APPEND(parent, cache_ptr->pl_head_ptr, cache_ptr->pl_tail_ptr,
                      cache_ptr->pl_len, cache_ptr->pl_size, FAIL);
      H5C_create_flush_dependency(parent, pf);
      H5C__DLL_REMOVE(parent, cache_ptr->pl_head_ptr, cache_ptr->pl_tail_ptr,
                      cache_ptr->pl_len, cache_ptr->pl_size, FAIL);
      parent->is_protected = false;
      H5C__DLL_PREPEND(parent, cache_ptr->pel_head_ptr, cache_ptr->pel_tail_ptr,
                       cache_ptr->pel_len, cache_ptr->pel_size, FAIL);
    } else {
      H5C_create_flush_dependency(parent, pf);
    }
    if (parent->prefetched) {
      parent->fd_child_count++;
    }
  }
  return pf;
}

TEST(H5CentryTest, InsertPinUnpinAndInsertErrors) {
  CacheFixture fix;
  ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/4096, /*min_clean_size=*/1024));
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  haddr_t a1 = fix.AllocSpace(64);
  haddr_t a_badlen = fix.AllocSpace(64);
  haddr_t a_after_fail = fix.AllocSpace(64);
  haddr_t a_unpinned = fix.AllocSpace(64);
  haddr_t a_child = fix.AllocSpace(64);
  haddr_t a_flash = fix.AllocSpace(4000);
  haddr_t a_big = fix.AllocSpace(64);
  ASSERT_NE(a_big, HADDR_UNDEF);

  // 1. Insert regular pinned + flush-last entry, then duplicate-insert errors.
  TestEntry* e1 = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a1, e1,
                             H5C__PIN_ENTRY_FLAG | H5C__FLUSH_LAST_FLAG),
            0);
  EXPECT_TRUE(e1->header.is_pinned);
  EXPECT_TRUE(e1->header.pinned_from_client);
  EXPECT_TRUE(e1->header.flush_me_last);
  EXPECT_TRUE(e1->header.is_dirty);
  EXPECT_EQ(fix.ctrl.notify_actions.back(), H5C_NOTIFY_ACTION_AFTER_INSERT);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a1, e1,
                               H5C__NO_FLAGS_SET),
              0);
    TestEntry* dup = MakeTestEntry(&fix.ctrl, 64);
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a1, dup,
                               H5C__NO_FLAGS_SET),
              0);
    delete dup;
  }

  // 2. image_len error during insert does not leave entry in cache.
  TestEntry* e_badlen = MakeTestEntry(&fix.ctrl, 64);
  fix.ctrl.fail_image_len = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a_badlen, e_badlen,
                               H5C__NO_FLAGS_SET),
              0);
  }
  fix.ctrl.fail_image_len = false;
  delete e_badlen;

  // 3. Notify AFTER_INSERT failure untags entry but leaves it in cache index;
  // clean up with expunge once failure toggle is cleared.
  TestEntry* e_after_fail = MakeTestEntry(&fix.ctrl, 64);
  fix.ctrl.fail_notify = true;
  fix.ctrl.filter_notify_action = true;
  fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_AFTER_INSERT;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a_after_fail,
                               e_after_fail, H5C__NO_FLAGS_SET),
              0);
  }
  fix.ctrl.fail_notify = false;
  fix.ctrl.filter_notify_action = false;
  EXPECT_EQ(e_after_fail->header.tag_info, nullptr);
  ASSERT_GE(H5C_expunge_entry(fix.f, &fix.classes.regular, a_after_fail,
                              H5C__NO_FLAGS_SET),
            0);

  // 4. Pin/unpin errors on unprotected / unpinned entries.
  TestEntry* e_unpinned = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_unpinned,
                             e_unpinned, H5C__NO_FLAGS_SET),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_pin_protected_entry(e_unpinned), 0);
    EXPECT_LT(H5C_unpin_entry(e_unpinned), 0);
  }

  // Protect e1 (already pinned_from_client) -> double pin fails; protect
  // e_unpinned -> pin succeeds -> unpin while protected -> unpin unprotected.
  void* prot1 = H5C_protect(fix.f, &fix.classes.regular, a1, nullptr,
                            H5C__NO_FLAGS_SET);
  ASSERT_EQ(prot1, e1);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_pin_protected_entry(e1), 0);
  }
  ASSERT_GE(H5C_unprotect(fix.f, a1, e1, H5C__NO_FLAGS_SET), 0);

  void* prot2 = H5C_protect(fix.f, &fix.classes.regular, a_unpinned, nullptr,
                            H5C__NO_FLAGS_SET);
  ASSERT_EQ(prot2, e_unpinned);
  ASSERT_GE(H5C_pin_protected_entry(e_unpinned), 0);
  EXPECT_TRUE(e_unpinned->header.is_pinned);
  ASSERT_GE(H5C_unpin_entry(e_unpinned), 0);
  EXPECT_FALSE(e_unpinned->header.is_pinned);
  ASSERT_GE(H5C_unprotect(fix.f, a_unpinned, e_unpinned, H5C__PIN_ENTRY_FLAG),
            0);
  EXPECT_TRUE(e_unpinned->header.is_pinned);
  ASSERT_GE(H5C_unpin_entry(e_unpinned), 0);
  EXPECT_FALSE(e_unpinned->header.is_pinned);

  // Create flush dependency so e_unpinned becomes pinned_from_cache only, then
  // verify client unpin fails, explicit client pin upgrades it, and client
  // unpin leaves pinned_from_cache intact.
  TestEntry* e_child = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_child, e_child,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_unpinned, nullptr,
                        H5C__NO_FLAGS_SET),
            e_unpinned);
  ASSERT_GE(H5C_create_flush_dependency(e_unpinned, e_child), 0);
  EXPECT_TRUE(e_unpinned->header.is_pinned);
  EXPECT_TRUE(e_unpinned->header.pinned_from_cache);
  EXPECT_FALSE(e_unpinned->header.pinned_from_client);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unpin_entry(e_unpinned), 0);
  }
  ASSERT_GE(H5C_pin_protected_entry(e_unpinned), 0);
  EXPECT_TRUE(e_unpinned->header.pinned_from_client);
  ASSERT_GE(H5C_unprotect(fix.f, a_unpinned, e_unpinned, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_unpin_entry(e_unpinned), 0);
  EXPECT_TRUE(e_unpinned->header.is_pinned);
  ASSERT_GE(H5C_destroy_flush_dependency(e_unpinned, e_child), 0);
  EXPECT_FALSE(e_unpinned->header.is_pinned);

  // Flash resize during H5C_insert_entry and make_space failure on insert.
  H5C_auto_size_ctl_t resize_ctl{};
  resize_ctl.version = H5C__CURR_AUTO_SIZE_CTL_VER;
  resize_ctl.rpt_fcn = nullptr;
  resize_ctl.set_initial_size = true;
  resize_ctl.initial_size = 4096;
  resize_ctl.min_clean_fraction = 0.25;
  resize_ctl.max_size = 65536;
  resize_ctl.min_size = H5C__MIN_MAX_CACHE_SIZE;
  resize_ctl.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  resize_ctl.incr_mode = H5C_incr__threshold;
  resize_ctl.lower_hr_threshold = 0.9;
  resize_ctl.increment = 2.0;
  resize_ctl.flash_incr_mode = H5C_flash_incr__add_space;
  resize_ctl.flash_multiple = 2.0;
  resize_ctl.flash_threshold = 0.25;
  resize_ctl.decr_mode = H5C_decr__off;
  ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);
  EXPECT_TRUE(fix.tc->flash_size_increase_possible);

  // Insert entry larger than both flash threshold (1024) and remaining free
  // space (index_size + 4000 > 4096) to trigger H5C__flash_increase_cache_size.
  TestEntry* e_flash = MakeTestEntry(&fix.ctrl, 4000);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_flash, e_flash,
                             H5C__NO_FLAGS_SET),
            0);
  EXPECT_GT(fix.tc->max_cache_size, 4096U);

  // Disable flash resize before testing H5C_insert_entry make_space failures.
  resize_ctl.set_initial_size = false;
  resize_ctl.incr_mode = H5C_incr__off;
  resize_ctl.flash_incr_mode = H5C_flash_incr__off;
  ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);
  EXPECT_FALSE(fix.tc->flash_size_increase_possible);

  // Trigger check_write_permitted failure inside H5C_insert_entry when cache
  // needs space (index_size + new_size > max_cache_size).
  TestEntry* e_big = MakeTestEntry(&fix.ctrl, fix.tc->max_cache_size + 512);
  fix.ctrl.fail_write_permitted = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a_big, e_big,
                               H5C__NO_FLAGS_SET),
              0);
  }
  fix.ctrl.fail_write_permitted = false;

  // Trigger H5C__make_space_in_cache failure on insert via serialize error
  // while dirty entries occupy the LRU.
  fix.ctrl.write_permitted_value = true;
  fix.ctrl.fail_serialize = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_insert_entry(fix.f, &fix.classes.regular, a_big, e_big,
                               H5C__NO_FLAGS_SET),
              0);
  }
  fix.ctrl.fail_serialize = false;
  delete e_big;

  // Unpin e1 so Teardown can flush cleanly.
  ASSERT_GE(H5C_unpin_entry(e1), 0);
}

TEST(H5CentryTest, ProtectUnprotectMultiRoDeletedAndAutoResize) {
  CacheFixture fix;
  ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/8192, /*min_clean_size=*/2048,
                           /*with_write_cb=*/true, /*with_log_flush=*/true,
                           H5F_FSPACE_STRATEGY_AGGR));
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);

  // 1. Insert clean/serialized entry by flushing it, then test RO refcounting.
  haddr_t addr1 = fix.AllocSpace(128);
  ASSERT_NE(addr1, HADDR_UNDEF);
  TestEntry* e1 = MakeTestEntry(&fix.ctrl, 128, 0x11);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, addr1, e1,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_flush_cache(fix.f, H5C__NO_FLAGS_SET), 0);
  EXPECT_FALSE(e1->header.is_dirty);

  // Wrong type and ring mismatch on H5C_protect.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.skip_rw, addr1, nullptr,
                          H5C__READ_ONLY_FLAG),
              nullptr);
    ScopedApiContext mdfsm_ctx(H5C_RING_MDFSM);
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                          H5C__READ_ONLY_FLAG),
              nullptr);
  }

  // Concurrent RO protects: ro_ref_count 1 -> 2 -> 3 -> 4.
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                        H5C__READ_ONLY_FLAG),
            e1);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                        H5C__READ_ONLY_FLAG),
            e1);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                        H5C__READ_ONLY_FLAG),
            e1);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                        H5C__READ_ONLY_FLAG),
            e1);
  EXPECT_EQ(e1->header.ro_ref_count, 4);

  // R/W protect while protected R/O fails.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                          H5C__NO_FLAGS_SET),
              nullptr);
    // Modifying RO entry (ro_ref_count > 1) fails before decrementing ref.
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__DIRTIED_FLAG), 0);
    EXPECT_EQ(e1->header.ro_ref_count, 4);
    // Unpin while not pinned at ro_ref_count > 1 decrements ref (4 -> 3) then
    // fails in H5C__unpin_entry_from_client.
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__UNPIN_ENTRY_FLAG), 0);
  }
  EXPECT_EQ(e1->header.ro_ref_count, 3);
  // Pin during ro_ref_count > 1 unprotect (3 -> 2), double pin error (2 -> 1),
  // then at ro_ref_count == 1: dirtied fails (keeps 1), then unpin+unprotect
  // succeeds (1 -> 0).
  ASSERT_GE(H5C_unprotect(fix.f, addr1, e1, H5C__PIN_ENTRY_FLAG), 0);
  EXPECT_EQ(e1->header.ro_ref_count, 2);
  EXPECT_TRUE(e1->header.is_pinned);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__PIN_ENTRY_FLAG), 0);
  }
  EXPECT_EQ(e1->header.ro_ref_count, 1);

  // At ro_ref_count == 1: dirtied fails, then normal unpin+unprotect succeeds,
  // then duplicate unprotect fails.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__DIRTIED_FLAG), 0);
  }
  ASSERT_GE(H5C_unprotect(fix.f, addr1, e1, H5C__UNPIN_ENTRY_FLAG), 0);
  EXPECT_FALSE(e1->header.is_pinned);
  EXPECT_FALSE(e1->header.is_protected);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__NO_FLAGS_SET), 0);
  }

  // 2. R/W protect, dirty -> notify error, flush dep propagation, then
  // unprotect with DIRTIED + DELETED + FREE_FILE_SPACE, and DELETED +
  // TAKE_OWNERSHIP.
  haddr_t addr_parent = fix.AllocSpace(64);
  ASSERT_NE(addr_parent, HADDR_UNDEF);
  TestEntry* e_parent = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, addr_parent, e_parent,
                             H5C__PIN_ENTRY_FLAG),
            0);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr1, nullptr,
                        H5C__NO_FLAGS_SET),
            e1);
  ASSERT_GE(H5C_create_flush_dependency(e_parent, e1), 0);
  // Dirty->unprotect notify fail path (entry_ptr stays dirty).
  fix.ctrl.fail_notify = true;
  fix.ctrl.filter_notify_action = true;
  fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_DIRTIED;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unprotect(fix.f, addr1, e1, H5C__DIRTIED_FLAG), 0);
  }
  fix.ctrl.fail_notify = false;
  fix.ctrl.filter_notify_action = false;
  EXPECT_TRUE(e1->header.is_protected);
  ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e1), 0);
  // Now e1 was marked dirty and clean->dirty updated index/slist on failure;
  // unprotect with DELETED + TAKE_OWNERSHIP (caller receives ownership).
  // Note: unprotect removes e1 from cache without freeing it.
  // First mark clean directly so we also cover !was_clean && !is_dirty? No,
  // e1 is already dirty and indexed as dirty, so unprotect with
  // H5C__DELETED_FLAG | H5C__TAKE_OWNERSHIP_FLAG tests take_ownership!
  ASSERT_GE(H5C_unprotect(fix.f, addr1, e1,
                          H5C__DELETED_FLAG | H5C__TAKE_OWNERSHIP_FLAG),
            0);
  delete e1;

  // Test H5C_unprotect clean->dirty via `entry_ptr->dirtied = true` (without
  // H5C__DIRTIED_FLAG), triggering CHILD_DIRTIED notify error on parent, then
  // clean H5C_unprotect with H5C__DELETED_FLAG | H5C__FREE_FILE_SPACE_FLAG.
  haddr_t addr2 = fix.AllocSpace(128);
  ASSERT_NE(addr2, HADDR_UNDEF);
  TestEntry* e2 = MakeTestEntry(&fix.ctrl, 128, 0x22);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, addr2, e2,
                             H5C__PIN_ENTRY_FLAG),
            0);
  ASSERT_GE(H5C_mark_entry_clean(e2), 0);
  ASSERT_GE(H5C_unpin_entry(e2), 0);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr2, nullptr,
                        H5C__NO_FLAGS_SET),
            e2);
  ASSERT_GE(H5C_create_flush_dependency(e_parent, e2), 0);
  e2->header.dirtied = true;
  fix.ctrl.fail_notify = true;
  fix.ctrl.filter_notify_action = true;
  fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_DIRTIED;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_unprotect(fix.f, addr2, e2, H5C__NO_FLAGS_SET), 0);
  }
  fix.ctrl.fail_notify = false;
  fix.ctrl.filter_notify_action = false;
  ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e2), 0);
  ASSERT_GE(H5C_unprotect(fix.f, addr2, e2,
                          H5C__DELETED_FLAG | H5C__FREE_FILE_SPACE_FLAG),
            0);

  // Also test DELETED+FREE_FILE_SPACE path cleanly on a fresh entry.
  // Re-test protect AFTER_LOAD notify failure + auto_resize + size_decreased.

  // 3. Auto-resize epoch and size_decreased on H5C_protect, plus
  // H5C_protect(miss) -> H5C__FLUSH_LAST_FLAG + AFTER_LOAD fail +
  // check_write_permitted fail + make_space fail.
  H5C_auto_size_ctl_t resize_ctl{};
  resize_ctl.version = H5C__CURR_AUTO_SIZE_CTL_VER;
  resize_ctl.rpt_fcn = nullptr;
  resize_ctl.set_initial_size = true;
  resize_ctl.initial_size = 2048;
  resize_ctl.min_clean_fraction = 0.25;
  resize_ctl.max_size = 65536;
  resize_ctl.min_size = H5C__MIN_MAX_CACHE_SIZE;
  resize_ctl.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  resize_ctl.incr_mode = H5C_incr__threshold;
  resize_ctl.lower_hr_threshold = 0.9;
  resize_ctl.increment = 2.0;
  resize_ctl.flash_incr_mode = H5C_flash_incr__add_space;
  resize_ctl.flash_multiple = 2.0;
  resize_ctl.flash_threshold = 0.25;
  resize_ctl.decr_mode = H5C_decr__threshold;
  resize_ctl.upper_hr_threshold = 0.99;
  resize_ctl.decrement = 0.9;
  ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);

  // Trigger size_decreased eviction on H5C_protect.
  haddr_t addr3 = fix.AllocSpace(512);
  ASSERT_NE(addr3, HADDR_UNDEF);
  TestEntry* e_hit = MakeTestEntry(&fix.ctrl, 512);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, addr3, e_hit,
                             H5C__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5C_flush_cache(fix.f, H5C__NO_FLAGS_SET), 0);
  fix.tc->size_decreased = true;
  fix.tc->max_cache_size = 512;
  for (int i = 0; i < H5C__MIN_AR_EPOCH_LENGTH; ++i) {
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr3, nullptr,
                          H5C__NO_FLAGS_SET),
              e_hit);
    ASSERT_GE(H5C_unprotect(fix.f, addr3, e_hit, H5C__NO_FLAGS_SET), 0);
  }
  EXPECT_FALSE(fix.tc->size_decreased);

  // Miss protect with H5C__FLUSH_LAST_FLAG + write disk image first.
  haddr_t addr_disk = fix.AllocSpace(64);
  ASSERT_NE(addr_disk, HADDR_UNDEF);
  std::vector<uint8_t> disk_bytes(64, 0x42);
  ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_BTREE, addr_disk, 64,
                            disk_bytes.data()),
            0);
  LoadUdata ud{&fix.ctrl, fix.f, 64, 64, false, 0x42};

  // check_write_permitted fail during protect miss -> H5C__make_space_in_cache
  // never even called; loaded entry was NOT inserted in index -> clean up.
  fix.tc->max_cache_size = 64;
  fix.ctrl.fail_write_permitted = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr_disk, &ud,
                          H5C__FLUSH_LAST_FLAG),
              nullptr);
  }
  fix.ctrl.fail_write_permitted = false;
  ASSERT_NE(fix.ctrl.last_deserialized, nullptr);
  H5C__untag_entry(fix.tc, &fix.ctrl.last_deserialized->header);
  H5MM_xfree(fix.ctrl.last_deserialized->header.image_ptr);
  delete fix.ctrl.last_deserialized;
  fix.ctrl.last_deserialized = nullptr;

  // Fail make_space during protect miss (serialize failure while evicting
  // e_sac).
  haddr_t addr_sac = fix.AllocSpace(128);
  ASSERT_NE(addr_sac, HADDR_UNDEF);
  TestEntry* e_sac = MakeTestEntry(&fix.ctrl, 128);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, addr_sac, e_sac,
                             H5C__NO_FLAGS_SET),
            0);
  fix.ctrl.fail_serialize = true;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr_disk, &ud,
                          H5C__NO_FLAGS_SET),
              nullptr);
  }
  fix.ctrl.fail_serialize = false;
  ASSERT_NE(fix.ctrl.last_deserialized, nullptr);
  H5C__untag_entry(fix.tc, &fix.ctrl.last_deserialized->header);
  H5MM_xfree(fix.ctrl.last_deserialized->header.image_ptr);
  delete fix.ctrl.last_deserialized;
  fix.ctrl.last_deserialized = nullptr;

  fix.tc->max_cache_size = 8192;

  // Fail AFTER_LOAD notify after protect miss -> entry IS in index & protected;
  // unprotect with DELETED once failure toggle is cleared.
  fix.ctrl.fail_notify = true;
  fix.ctrl.filter_notify_action = true;
  fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_AFTER_LOAD;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr_disk, &ud,
                          H5C__FLUSH_LAST_FLAG),
              nullptr);
  }
  fix.ctrl.fail_notify = false;
  fix.ctrl.filter_notify_action = false;
  ASSERT_NE(fix.ctrl.last_deserialized, nullptr);
  EXPECT_TRUE(fix.ctrl.last_deserialized->header.is_protected);
  EXPECT_TRUE(fix.ctrl.last_deserialized->header.flush_me_last);
  ASSERT_GE(H5C_unprotect(fix.f, addr_disk, fix.ctrl.last_deserialized,
                          H5C__DELETED_FLAG),
            0);
  fix.ctrl.last_deserialized = nullptr;

  ASSERT_GE(H5C_unpin_entry(e_parent), 0);
}

TEST(H5CentryTest, LoadEntrySpeculativeEoaGheapSkipReadsAndSwmrRetries) {
  // Part A: Core VFD tests for EOA clamping, speculative grow/shrink, GHEAP,
  // SKIP_READS, NO_VERIFY, and OHDR dirty-on-deserialize.
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                             /*min_clean_size=*/16 * 1024));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    haddr_t addr_reg = fix.AllocSpace(128);
    haddr_t a_skip = fix.AllocSpace(64);
    haddr_t addr_spec_grow = fix.AllocSpace(128);
    haddr_t addr_spec_shrink = fix.AllocSpace(128);
    haddr_t addr_ohdr = fix.AllocSpace(64, H5FD_MEM_OHDR);
    haddr_t addr_gheap = fix.AllocSpace(64, H5FD_MEM_DRAW);
    ASSERT_NE(addr_reg, HADDR_UNDEF);
    ASSERT_NE(a_skip, HADDR_UNDEF);
    ASSERT_NE(addr_spec_grow, HADDR_UNDEF);
    ASSERT_NE(addr_spec_shrink, HADDR_UNDEF);
    ASSERT_NE(addr_ohdr, HADDR_UNDEF);
    ASSERT_NE(addr_gheap, HADDR_UNDEF);

    std::vector<uint8_t> b64(64, 0x33);
    std::vector<uint8_t> b128(128, 0x44);
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_BTREE, addr_reg, 128, b128.data()), 0);
    ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_BTREE, addr_spec_grow, 128,
                              b128.data()),
              0);
    ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_BTREE, addr_spec_shrink, 128,
                              b128.data()),
              0);
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_OHDR, addr_ohdr, 64, b64.data()), 0);
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_DRAW, addr_gheap, 64, b64.data()), 0);

    // 1. SKIP_READS + SKIP_WRITES entry load (never calls block_read).
    LoadUdata ud_skip{&fix.ctrl, fix.f, 48, 48, false, 0x77};
    auto* e_skip = static_cast<TestEntry*>(
        H5C_protect(fix.f, &fix.classes.skip_rw, a_skip, &ud_skip,
                    H5C__NO_FLAGS_SET));
    ASSERT_NE(e_skip, nullptr);
    EXPECT_EQ(e_skip->len, 48U);
    ASSERT_GE(H5C_unprotect(fix.f, a_skip, e_skip, H5C__DIRTIED_FLAG), 0);

    // 2. NO_VERIFY entry load (verify_chksum == nullptr).
    LoadUdata ud_noverify{&fix.ctrl, fix.f, 64, 64, false, 0x44};
    auto* e_nv = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.no_verify, addr_reg, &ud_noverify,
        H5C__NO_FLAGS_SET));
    ASSERT_NE(e_nv, nullptr);
    ASSERT_GE(H5C_unprotect(fix.f, addr_reg, e_nv, H5C__NO_FLAGS_SET), 0);

    // 3. OHDR type (id=5) dirty on deserialize!
    LoadUdata ud_ohdr{&fix.ctrl, fix.f, 64, 64, /*force_dirty=*/true, 0x33};
    auto* e_ohdr = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.ohdr_like, addr_ohdr, &ud_ohdr,
        H5C__NO_FLAGS_SET));
    ASSERT_NE(e_ohdr, nullptr);
    EXPECT_TRUE(e_ohdr->header.is_dirty);
    EXPECT_FALSE(e_ohdr->header.image_up_to_date);
    ASSERT_GE(H5C_unprotect(fix.f, addr_ohdr, e_ohdr, H5C__NO_FLAGS_SET), 0);

    // 4. Speculative grow (initial=64 -> final=128) & speculative shrink
    // (initial=128 -> final=64).
    LoadUdata ud_grow{&fix.ctrl, fix.f, 64, 128, false, 0x44};
    auto* e_grow = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.speculative, addr_spec_grow, &ud_grow,
        H5C__NO_FLAGS_SET));
    ASSERT_NE(e_grow, nullptr);
    EXPECT_EQ(e_grow->len, 128U);
    ASSERT_GE(
        H5C_unprotect(fix.f, addr_spec_grow, e_grow, H5C__NO_FLAGS_SET), 0);

    LoadUdata ud_shrink{&fix.ctrl, fix.f, 128, 64, false, 0x44};
    auto* e_shrink = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.speculative, addr_spec_shrink, &ud_shrink,
        H5C__NO_FLAGS_SET));
    ASSERT_NE(e_shrink, nullptr);
    EXPECT_EQ(e_shrink->len, 64U);
    ASSERT_GE(
        H5C_unprotect(fix.f, addr_spec_shrink, e_shrink, H5C__NO_FLAGS_SET), 0);

    // 5. GHEAP type maps EOA lookup to H5FD_MEM_DRAW + clamps initial_len past
    // EOA down to (eoa - addr_last).
    haddr_t draw_eoa = H5F_get_eoa(fix.f, H5FD_MEM_DRAW);
    ASSERT_NE(draw_eoa, HADDR_UNDEF);
    ASSERT_GT(draw_eoa, addr_gheap + 32);
    LoadUdata ud_gheap{&fix.ctrl, fix.f, 64, 64, false, 0x33};
    auto* e_gheap = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.gheap, addr_gheap, &ud_gheap, H5C__NO_FLAGS_SET));
    ASSERT_NE(e_gheap, nullptr);
    ASSERT_GE(H5C_unprotect(fix.f, addr_gheap, e_gheap, H5C__NO_FLAGS_SET), 0);

    // Use address near draw_eoa so initial_len=256 exceeds draw_eoa and
    // H5C__verify_len_eoa clamps it cleanly down to 32 bytes.
    haddr_t near_eoa = draw_eoa - 32;
    ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_DRAW, near_eoa, 32, b64.data()),
              0);
    LoadUdata ud_clamp{&fix.ctrl, fix.f, 256, /*final=*/0, false, 0x33};
    // Make final == clamped size by setting final_len = 0 (CbGetFinalLoadSize
    // returns image_len when final_len == 0).
    auto* e_clamp = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.gheap, near_eoa, &ud_clamp, H5C__NO_FLAGS_SET));
    ASSERT_NE(e_clamp, nullptr);
    EXPECT_EQ(e_clamp->len, 32U);
    ASSERT_GE(H5C_unprotect(fix.f, near_eoa, e_clamp, H5C__NO_FLAGS_SET), 0);

    // 6. Error branches in H5C__load_entry & H5C__verify_len_eoa:
    haddr_t btree_eoa = H5F_get_eoa(fix.f, H5FD_MEM_BTREE);
    haddr_t near_bt_eoa = btree_eoa - 32;
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_BTREE, near_bt_eoa, 32, b64.data()), 0);
    {
      ScopedErrorSilence silence;
      // get_initial_load_size fails.
      fix.ctrl.fail_get_initial = true;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, addr_reg, &ud_grow,
                            H5C__NO_FLAGS_SET),
                nullptr);
      fix.ctrl.fail_get_initial = false;

      // addr > eoa -> verify_len_eoa error (actual=false).
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, btree_eoa + 64,
                            &ud_grow, H5C__NO_FLAGS_SET),
                nullptr);
      // addr == eoa -> clamped len == 0 -> error.
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, btree_eoa,
                            &ud_grow, H5C__NO_FLAGS_SET),
                nullptr);

      // actual_len exceeds EOA -> verify_len_eoa error (actual=true).
      LoadUdata ud_too_big{&fix.ctrl, fix.f, 16, 4096, false, 0x33};
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, near_bt_eoa,
                            &ud_too_big, H5C__NO_FLAGS_SET),
                nullptr);

      // actual_len == 0 -> error.
      fix.ctrl.fail_get_final_zero = true;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, addr_reg + 16,
                            &ud_grow, H5C__NO_FLAGS_SET),
                nullptr);
      fix.ctrl.fail_get_final_zero = false;

      // wrap-around addr + actual_len < addr in verify_len_eoa (actual=true).
      LoadUdata ud_wrap{&fix.ctrl, fix.f, 16, static_cast<size_t>(-1), false,
                        0x44};
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.speculative, addr_reg + 16,
                            &ud_wrap, H5C__NO_FLAGS_SET),
                nullptr);

      // H5F_block_read failure on non-speculative address past EOA.
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, btree_eoa + 128,
                            &ud_noverify, H5C__NO_FLAGS_SET),
                nullptr);

      // verify_chksum returns < 0.
      fix.ctrl.fail_verify_chksum = true;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr_reg + 32,
                            &ud_noverify, H5C__NO_FLAGS_SET),
                nullptr);
      fix.ctrl.fail_verify_chksum = false;

      // deserialize returns nullptr.
      fix.ctrl.fail_deserialize = true;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr_reg + 32,
                            &ud_noverify, H5C__NO_FLAGS_SET),
                nullptr);
      fix.ctrl.fail_deserialize = false;
    }
  }

  // Part B: SWMR TempFile for metadata read retry loop (get_final_load_size
  // retry with len change, verify_chksum retry + H5F_track_metadata_read_retries,
  // and tries == 0 exhaustion).
  {
    TempFile tmp("h5centry_swmr_retry");
    CacheFixture fix;
    ASSERT_TRUE(fix.InitSwmrTempFile(tmp.path(), /*read_attempts=*/4));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    haddr_t addr1 = fix.AllocSpace(128);
    haddr_t addr2 = fix.AllocSpace(128);
    ASSERT_NE(addr1, HADDR_UNDEF);
    ASSERT_NE(addr2, HADDR_UNDEF);
    std::vector<uint8_t> bytes(128, 0x55);
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_BTREE, addr1, 128, bytes.data()), 0);
    ASSERT_GE(
        H5F_block_write(fix.f, H5FD_MEM_BTREE, addr2, 128, bytes.data()), 0);

    // Retry get_final_load_size once (changing actual_len so realloc runs at
    // top of retry iteration!) AND verify_chksum returning false once, then
    // succeeding on retry -> tracks retries in H5F_track_metadata_read_retries.
    LoadUdata ud_retry{&fix.ctrl, fix.f, 64, 128, false, 0x55};
    fix.ctrl.fail_get_final_remaining = 1;
    fix.ctrl.return_bad_final_len_on_fail = true;
    fix.ctrl.bad_final_len = 96;
    fix.ctrl.verify_chksum_false_remaining = 1;
    auto* e_retry = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.speculative, addr1, &ud_retry, H5C__NO_FLAGS_SET));
    ASSERT_NE(e_retry, nullptr);
    EXPECT_EQ(e_retry->len, 128U);
    ASSERT_GE(H5C_unprotect(fix.f, addr1, e_retry, H5C__NO_FLAGS_SET), 0);

    // Exhaust all 4 read attempts on checksum failure -> tries == 0 error.
    fix.ctrl.verify_chksum_false_remaining = 10;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, addr2, &ud_retry,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }
    fix.ctrl.verify_chksum_false_remaining = 0;
  }
}

TEST(H5CentryTest, PrefetchedDeserializeFlushDepsAndPublicCacheImage) {
  // Part A: Synthetic prefetched hierarchy covering
  // H5C__deserialize_prefetched_entry, H5C__destroy_pf_entry_child_flush_deps,
  // dirty prefetched entry flush, and error cleanup branches.
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                             /*min_clean_size=*/16 * 1024));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    haddr_t a_grandparent = fix.AllocSpace(64);
    haddr_t a_other_parent = fix.AllocSpace(64);
    haddr_t a_unrelated_pf = fix.AllocSpace(64);
    haddr_t a_reg = fix.AllocSpace(64);
    haddr_t a_target = fix.AllocSpace(64, H5FD_MEM_OHDR);
    haddr_t a_child1 = fix.AllocSpace(64);
    haddr_t a_child2 = fix.AllocSpace(64);
    haddr_t a_dirty_pf = fix.AllocSpace(64);
    haddr_t a_err_pf = fix.AllocSpace(64);
    ASSERT_NE(a_err_pf, HADDR_UNDEF);

    // Also insert one non-prefetched entry and one unrelated prefetched child so
    // H5C__destroy_pf_entry_child_flush_deps scans non-pf, 0-parent pf, and
    // unrelated-parent pf entries.
    TestEntry* e_reg = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_reg, e_reg,
                               H5C__NO_FLAGS_SET),
              0);

    H5C_cache_entry_t* pf_gp = InsertSyntheticPrefetched(
        fix.tc, a_grandparent, 64, kTypeRegular, /*is_dirty=*/false, false,
        0x11);
    H5C_cache_entry_t* pf_op = InsertSyntheticPrefetched(
        fix.tc, a_other_parent, 64, kTypeRegular, /*is_dirty=*/false, false,
        0x12);
    H5C_cache_entry_t* pf_unrelated = InsertSyntheticPrefetched(
        fix.tc, a_unrelated_pf, 64, kTypeRegular, /*is_dirty=*/false, false,
        0x13, {pf_op});
    H5C_cache_entry_t* pf_target = InsertSyntheticPrefetched(
        fix.tc, a_target, 64, kTypeOhdrLike, /*is_dirty=*/true, false, 0x20,
        {pf_gp, pf_op});
    H5C_cache_entry_t* pf_child1 = InsertSyntheticPrefetched(
        fix.tc, a_child1, 64, kTypeRegular, /*is_dirty=*/false, false, 0x21,
        {pf_target});
    H5C_cache_entry_t* pf_child2 = InsertSyntheticPrefetched(
        fix.tc, a_child2, 64, kTypeRegular, /*is_dirty=*/true, false, 0x22,
        {pf_target, pf_op});
    H5C_cache_entry_t* pf_dirty_flush = InsertSyntheticPrefetched(
        fix.tc, a_dirty_pf, 64, kTypeRegular, /*is_dirty=*/true, false, 0x30);
    H5C_cache_entry_t* pf_err = InsertSyntheticPrefetched(
        fix.tc, a_err_pf, 64, kTypeRegular, /*is_dirty=*/false, false, 0x31);
    ASSERT_NE(pf_unrelated, nullptr);
    ASSERT_NE(pf_child1, nullptr);
    ASSERT_NE(pf_child2, nullptr);
    ASSERT_NE(pf_dirty_flush, nullptr);
    ASSERT_NE(pf_err, nullptr);

    // Successful protect of dirty prefetched OHDR entry with 2 parents and 2
    // prefetched children -> destroys parent deps, destroys child deps,
    // deserializes (dirty=true), expunges pf_target, inserts ds_target,
    // restores child deps onto ds_target!
    LoadUdata ud{&fix.ctrl, fix.f, 64, 64, /*force_dirty=*/true, 0x20};
    auto* ds_target = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.ohdr_like, a_target, &ud, H5C__NO_FLAGS_SET));
    ASSERT_NE(ds_target, nullptr);
    EXPECT_FALSE(ds_target->header.prefetched);
    EXPECT_TRUE(ds_target->header.is_dirty);
    EXPECT_EQ(ds_target->header.flush_dep_nchildren, 2U);
    EXPECT_EQ(pf_gp->flush_dep_nchildren, 0U);
    EXPECT_EQ(pf_op->flush_dep_nchildren, 2U);  // pf_unrelated + pf_child2
    EXPECT_TRUE(pf_child1->flush_dep_parent[0] == &ds_target->header);
    ASSERT_GE(H5C_unprotect(fix.f, a_target, ds_target, H5C__NO_FLAGS_SET), 0);

    // Wrong-type hit check after deserialization (`entry_ptr->type != type`).
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_target, &ud,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }

    // Cleanly tear down remaining flush dependencies via expunging children
    // first (`H5C__prefetched_entry_notify` updates parent counts cleanly).
    ASSERT_GE(H5C_expunge_entry(fix.f, H5AC_PREFETCHED_ENTRY, a_child1,
                                H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_expunge_entry(fix.f, H5AC_PREFETCHED_ENTRY, a_child2,
                                H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_expunge_entry(fix.f, H5AC_PREFETCHED_ENTRY, a_unrelated_pf,
                                H5C__NO_FLAGS_SET),
              0);

    // Now test H5C__deserialize_prefetched_entry error paths on `pf_err`
    // (0 parents, 0 children):
    // 1) deserialize returns nullptr.
    fix.ctrl.fail_deserialize = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_err_pf, &ud,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }
    fix.ctrl.fail_deserialize = false;

    // 2) AFTER_LOAD notify fails after pf_err is replaced in cache; defer
    // free_icr on that single failing call to mirror cache state and then
    // expunge cleanly so Teardown leaks nothing.
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_AFTER_LOAD;
    fix.ctrl.defer_free_icr = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_err_pf, &ud,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }
    fix.ctrl.defer_free_icr = false;
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    ASSERT_GE(H5C_expunge_entry(fix.f, &fix.classes.regular, a_err_pf,
                                H5C__NO_FLAGS_SET),
              0);

    // Flush remaining dirty prefetched entry (pf_dirty_flush) and regular
    // entries via H5C_flush_cache -> covers prefetched flush mem_type lookup.
    ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);
    ASSERT_GE(H5C_flush_cache(fix.f, H5C__NO_FLAGS_SET), 0);
    EXPECT_FALSE(pf_dirty_flush->is_dirty);
  }

  // Part B: End-to-end public file round-trip with metadata cache image enabled
  // so HDF5 itself creates and reloads a real cache image on TempFile.
  TempFile img_file("h5centry_mdc_image");
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    H5AC_cache_image_config_t img_cfg{};
    img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    img_cfg.generate_image = true;
    img_cfg.save_resize_status = false;
    img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

    ScopedFile file(H5Fcreate(img_file.path().c_str(), H5F_ACC_TRUNC,
                              H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    for (int i = 0; i < 4; ++i) {
      std::string gname = "grp_" + std::to_string(i);
      ScopedGroup grp(H5Gcreate2(file.get(), gname.c_str(), H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
      hsize_t dim = 8;
      ScopedSpace sp(H5Screate_simple(1, &dim, nullptr));
      ScopedDataset dset(H5Dcreate2(grp.get(), "ds", H5T_NATIVE_INT, sp.get(),
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      int vals[8] = {i, 1, 2, 3, 4, 5, 6, 7};
      ASSERT_GE(
          H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   vals),
          0);
    }
  }
  {
    ScopedFile reopened(H5Fopen(img_file.path().c_str(), H5F_ACC_RDONLY,
                                H5P_DEFAULT));
    ASSERT_TRUE(reopened.valid());
    ScopedDataset dset(H5Dopen2(reopened.get(), "/grp_2/ds", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    int read_vals[8]{};
    ASSERT_GE(
        H5Dread(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                read_vals),
        0);
    EXPECT_EQ(read_vals[0], 2);
  }
}

TEST(H5CentryTest, GenerateImageSerializeFlushSingleAndDiscardPaths) {
  // Part A: Serialization, pre_serialize resize+move, flush dependency
  // propagation, image suppression, fsf_size, log_flush, and error paths.
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                             /*min_clean_size=*/16 * 1024,
                             /*with_write_cb=*/true, /*with_log_flush=*/true,
                             H5F_FSPACE_STRATEGY_AGGR));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    haddr_t a_parent = fix.AllocSpace(64);
    haddr_t a1 = fix.AllocSpace(64);
    haddr_t a1_moved = fix.AllocSpace(128);
    haddr_t a2 = fix.AllocSpace(128);
    haddr_t a2_moved = fix.AllocSpace(128);
    haddr_t a_skip = fix.AllocSpace(64);
    ASSERT_NE(a_skip, HADDR_UNDEF);

    TestEntry* e_parent = MakeTestEntry(&fix.ctrl, 64, 0x01);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_parent, e_parent,
                               H5C__PIN_ENTRY_FLAG),
              0);

    // 1. Entry resized AND moved via pre_serialize (cache performs move).
    TestEntry* e1 = MakeTestEntry(&fix.ctrl, 64, 0x1A);
    e1->pre_ser_flags = H5C__SERIALIZE_RESIZED_FLAG | H5C__SERIALIZE_MOVED_FLAG;
    e1->pre_ser_new_len = 128;
    e1->pre_ser_new_addr = a1_moved;
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a1, e1,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_create_flush_dependency(e_parent, e1), 0);

    ASSERT_GE(H5C__serialize_single_entry(fix.f, fix.tc, &e1->header), 0);
    EXPECT_EQ(e1->header.addr, a1_moved);
    EXPECT_EQ(e1->header.size, 128U);
    EXPECT_TRUE(e1->header.image_up_to_date);
    EXPECT_EQ(e_parent->header.flush_dep_nunser_children, 0U);

    // 2. Entry resized AND already moved inside pre_serialize (`else` branch).
    TestEntry* e2 = MakeTestEntry(&fix.ctrl, 64, 0x2B);
    e2->pre_ser_flags = H5C__SERIALIZE_RESIZED_FLAG | H5C__SERIALIZE_MOVED_FLAG;
    e2->pre_ser_new_len = 96;
    e2->pre_ser_new_addr = a2_moved;
    e2->pre_ser_call_move = true;
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a2, e2,
                               H5C__NO_FLAGS_SET),
              0);
    // Also test H5C_set_slist_enabled(true) so resize/move inside
    // H5C__generate_image hits the slist-enabled branches!
    ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);
    ASSERT_GE(H5C__serialize_single_entry(fix.f, fix.tc, &e2->header), 0);
    EXPECT_EQ(e2->header.addr, a2_moved);
    EXPECT_EQ(e2->header.size, 96U);

    // 3. Serialize & generate_image error branches:
    TestEntry* e_err = MakeTestEntry(&fix.ctrl, 64, 0x3C);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a1, e_err,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_create_flush_dependency(e_parent, e_err), 0);

    {
      ScopedErrorSilence silence;
      // pre_serialize failure.
      fix.ctrl.fail_pre_serialize = true;
      EXPECT_LT(H5C__serialize_single_entry(fix.f, fix.tc, &e_err->header), 0);
      e_err->header.flush_in_progress = false;
      fix.ctrl.fail_pre_serialize = false;

      // Unknown pre_serialize flag.
      e_err->pre_ser_flags = 0x8000U;
      EXPECT_LT(H5C__serialize_single_entry(fix.f, fix.tc, &e_err->header), 0);
      e_err->header.flush_in_progress = false;
      e_err->pre_ser_flags = 0;

      // serialize callback failure.
      fix.ctrl.fail_serialize = true;
      EXPECT_LT(H5C__serialize_single_entry(fix.f, fix.tc, &e_err->header), 0);
      e_err->header.flush_in_progress = false;
      fix.ctrl.fail_serialize = false;

      // CHILD_SERIALIZED notify failure inside H5C__mark_flush_dep_serialized.
      fix.ctrl.fail_notify = true;
      fix.ctrl.filter_notify_action = true;
      fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_SERIALIZED;
      EXPECT_LT(H5C__serialize_single_entry(fix.f, fix.tc, &e_err->header), 0);
      e_err->header.flush_in_progress = false;
      fix.ctrl.fail_notify = false;
      fix.ctrl.filter_notify_action = false;
    }
    ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e_err), 0);
    ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e1), 0);

    // 4. H5C__flush_single_entry branches:
    // SKIP_WRITES dirty entry flush; H5C__GENERATE_IMAGE_FLAG; clear-only
    // dirty; AFTER_FLUSH fail; BEFORE_EVICT fail; ENTRY_CLEANED fail;
    // CHILD_CLEANED fail; log_flush fail; free_icr fail; fsf_size success &
    // fail.
    TestEntry* e_skip = MakeTestEntry(&fix.ctrl, 64, 0x4D);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.skip_rw, a_skip, e_skip,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C__flush_single_entry(fix.f, &e_skip->header,
                                      H5C__NO_FLAGS_SET),
              0);
    EXPECT_FALSE(e_skip->header.is_dirty);

    // Clear-only on dirty entry updates clear stats without disk write.
    ASSERT_GE(
        H5C_expunge_entry(fix.f, &fix.classes.regular, a1, H5C__NO_FLAGS_SET),
        0);
    e1->header.image_up_to_date = false;
    ASSERT_GE(H5C_create_flush_dependency(e_parent, e1), 0);
    // Test H5C__FLUSH_CLEAR_ONLY_FLAG on clean e_parent vs dirty e1: first dirty
    // e1 via protect+unprotect, then flush clear_only!
    ASSERT_EQ(
        H5C_protect(fix.f, &fix.classes.regular, a1_moved, nullptr,
                    H5C__NO_FLAGS_SET),
        e1);
    ASSERT_GE(H5C_unprotect(fix.f, a1_moved, e1, H5C__DIRTIED_FLAG), 0);
    ASSERT_GE(H5C__flush_single_entry(fix.f, &e1->header,
                                      H5C__FLUSH_CLEAR_ONLY_FLAG),
              0);
    EXPECT_FALSE(e1->header.is_dirty);

    // Re-dirty e1 and flush with H5C__GENERATE_IMAGE_FLAG.
    ASSERT_EQ(
        H5C_protect(fix.f, &fix.classes.regular, a1_moved, nullptr,
                    H5C__NO_FLAGS_SET),
        e1);
    ASSERT_GE(H5C_unprotect(fix.f, a1_moved, e1, H5C__DIRTIED_FLAG), 0);
    ASSERT_GE(H5C__flush_single_entry(fix.f, &e1->header,
                                      H5C__GENERATE_IMAGE_FLAG),
              0);
    EXPECT_TRUE(e1->header.image_up_to_date);
    EXPECT_GT(fix.ctrl.log_flush_calls, 0);

    // Error paths inside H5C__flush_single_entry:
    {
      ScopedErrorSilence silence;
      // Protected entry flush error.
      ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a1_moved, nullptr,
                            H5C__NO_FLAGS_SET),
                e1);
      EXPECT_LT(H5C__flush_single_entry(fix.f, &e1->header,
                                        H5C__NO_FLAGS_SET),
                0);
      ASSERT_GE(H5C_unprotect(fix.f, a1_moved, e1, H5C__DIRTIED_FLAG), 0);

      // AFTER_FLUSH notify failure.
      fix.ctrl.fail_notify = true;
      fix.ctrl.filter_notify_action = true;
      fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_AFTER_FLUSH;
      EXPECT_LT(H5C__flush_single_entry(fix.f, &e1->header,
                                        H5C__NO_FLAGS_SET),
                0);
      e1->header.flush_in_progress = false;

      // BEFORE_EVICT notify failure on flush+invalidate.
      fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_BEFORE_EVICT;
      EXPECT_LT(
          H5C__flush_single_entry(
              fix.f, &e2->header,
              H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG),
          0);
      e2->header.flush_in_progress = false;

      // ENTRY_CLEANED notify failure (e2 stays protected from error -> reset
      // flush_in_progress; note RP/slist/is_dirty already updated, so test
      // CHILD_CLEANED notify failure BEFORE resetting).
      fix.ctrl.fail_notify = false;
      fix.ctrl.filter_notify_action = false;
    }

    // Re-dirty e1 and test ENTRY_CLEANED + CHILD_CLEANED notify failures in
    // flush (notice we reset flush_in_progress and delete/expunge e1 cleanly).
    ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e1), 0);
    ASSERT_GE(H5C_create_flush_dependency(e_parent, e1), 0);
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a1_moved, nullptr,
                          H5C__NO_FLAGS_SET),
              e1);
    ASSERT_GE(H5C_unprotect(fix.f, a1_moved, e1, H5C__DIRTIED_FLAG), 0);
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_CLEANED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C__flush_single_entry(fix.f, &e1->header,
                                        H5C__NO_FLAGS_SET),
                0);
    }
    e1->header.flush_in_progress = false;
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e1), 0);

    // log_flush callback failure.
    fix.ctrl.fail_log_flush = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C__flush_single_entry(fix.f, &e1->header,
                                        H5C__FLUSH_INVALIDATE_FLAG),
                0);
    }
    fix.ctrl.fail_log_flush = false;
    // Note: H5C__discard_single_entry ALREADY freed e1 above before log_flush
    // ran, so do NOT touch e1 again!

    // Test fsf_size success & failure and free_icr failure +
    // suppress_image_entry_writes/frees!
    haddr_t a_fsf = fix.AllocSpace(64);
    TestEntry* e_fsf = MakeTestEntry(&fix.ctrl, 64);
    e_fsf->fsf_override = 64;
    e_fsf->use_fsf_override = true;
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_fsf, e_fsf,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_expunge_entry(fix.f, &fix.classes.regular, a_fsf,
                                H5C__FREE_FILE_SPACE_FLAG),
              0);
    EXPECT_GT(fix.ctrl.fsf_size_calls, 0);

    // fsf_size failure + dirty ENTRY_CLEANED failure during discard + free_icr
    // failure (must delete e_err2 manually when free_icr failed!).
    haddr_t a_fail_disc = fix.AllocSpace(64);
    TestEntry* e_fail_disc = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_fail_disc,
                               e_fail_disc, H5C__NO_FLAGS_SET),
              0);
    fix.ctrl.fail_fsf_size = true;
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_CLEANED;
    fix.ctrl.fail_free_icr = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_expunge_entry(fix.f, &fix.classes.regular, a_fail_disc,
                                  H5C__FREE_FILE_SPACE_FLAG),
                0);
    }
    fix.ctrl.fail_fsf_size = false;
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    fix.ctrl.fail_free_icr = false;
    delete e_fail_disc;

    // suppress_image_entry_writes & suppress_image_entry_frees during
    // flush-invalidate!
    haddr_t a_supp = fix.AllocSpace(64);
    TestEntry* e_supp = MakeTestEntry(&fix.ctrl, 64, 0x6E);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_supp, e_supp,
                               H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C__serialize_single_entry(fix.f, fix.tc, &e_supp->header), 0);
    void* saved_img = e_supp->header.image_ptr;
    ASSERT_NE(saved_img, nullptr);
    e_supp->header.include_in_image = true;
    H5C_image_entry_t dummy_ie{};
    fix.tc->close_warning_received = true;
    fix.tc->image_ctl.generate_image = true;
    fix.tc->image_ctl.flags = H5C_CI__ALL_FLAGS;
    fix.tc->num_entries_in_image = 1;
    fix.tc->image_entries = &dummy_ie;
    ASSERT_GE(
        H5C__flush_single_entry(
            fix.f, &e_supp->header,
            H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG),
        0);
    H5MM_xfree(saved_img);
    fix.tc->close_warning_received = false;
    fix.tc->image_ctl.generate_image = false;
    fix.tc->num_entries_in_image = 0;
    fix.tc->image_entries = nullptr;

    ASSERT_GE(H5C_unpin_entry(e_parent), 0);
  }

  // Part B: Paged file with page buffer + H5MF_xfree failure via settled
  // rings & H5PB_update_entry!
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                             /*min_clean_size=*/16 * 1024,
                             /*with_write_cb=*/true, /*with_log_flush=*/true,
                             H5F_FSPACE_STRATEGY_PAGE, /*page_size=*/4096,
                             /*page_buf_size=*/8192));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Allocate metadata entry within a page and warm the page in H5PB via
    // H5PB_read (temporarily using orig_cache if needed).
    haddr_t a_pb = fix.AllocSpace(128, H5FD_MEM_BTREE);
    ASSERT_NE(a_pb, HADDR_UNDEF);
    std::vector<uint8_t> init_page(128, 0x3A);
    ASSERT_GE(
        H5PB_write(fix.f->shared, H5FD_MEM_BTREE, a_pb, 128, init_page.data()),
        0);
    std::vector<uint8_t> tmp_read(128);
    ASSERT_GE(
        H5PB_read(fix.f->shared, H5FD_MEM_BTREE, a_pb, 128, tmp_read.data()),
        0);

    TestEntry* e_pb = MakeTestEntry(&fix.ctrl, 128, 0x5B);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_pb, e_pb,
                               H5C__NO_FLAGS_SET),
              0);
    // Flush without destroy with H5C__UPDATE_PAGE_BUFFER_FLAG -> calls
    // H5PB_update_entry!
    ASSERT_GE(H5C__flush_single_entry(fix.f, &e_pb->header,
                                      H5C__UPDATE_PAGE_BUFFER_FLAG),
              0);

    // Verify page buffer now reflects 0x5B from e_pb!
    std::vector<uint8_t> check_read(128);
    ASSERT_GE(
        H5PB_read(fix.f->shared, H5FD_MEM_BTREE, a_pb, 128, check_read.data()),
        0);
    EXPECT_EQ(check_read[0], 0x5B);

    // Now mark rings settled + close_warning_received on tc so H5MF_xfree
    // fails via H5AC_unsettle_ring inside H5C__discard_single_entry (`line
    // 873`).
    fix.tc->rdfsm_settled = true;
    fix.tc->mdfsm_settled = true;
    fix.tc->close_warning_received = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_expunge_entry(fix.f, &fix.classes.regular, a_pb,
                                  H5C__FREE_FILE_SPACE_FLAG),
                0);
    }
    fix.tc->rdfsm_settled = false;
    fix.tc->mdfsm_settled = false;
    fix.tc->close_warning_received = false;
  }
}

TEST(H5CentryTest, MarkDirtyCleanSerializedUnserializedMoveResizeAndUnsettle) {
  CacheFixture fix;
  ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                           /*min_clean_size=*/16 * 1024));
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  haddr_t a_parent = fix.AllocSpace(64);
  haddr_t a_e = fix.AllocSpace(64);
  haddr_t a_move1 = fix.AllocSpace(2048);
  haddr_t a_other = fix.AllocSpace(64);
  haddr_t a_same = fix.AllocSpace(64);
  haddr_t a_move2 = fix.AllocSpace(64);
  haddr_t a_tmp = fix.AllocSpace(64);
  haddr_t a_ring = fix.AllocSpace(64);
  haddr_t a_missing = fix.AllocSpace(64);
  haddr_t a_missing2 = fix.AllocSpace(64);
  ASSERT_NE(a_missing2, HADDR_UNDEF);

  TestEntry* e_parent = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_parent, e_parent,
                             H5C__PIN_ENTRY_FLAG),
            0);

  // 1. Mark dirty/clean/unserialized/serialized state transitions & errors.
  TestEntry* e = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_e, e,
                             H5C__NO_FLAGS_SET),
            0);
  // Neither pinned nor protected -> error on all 4 mark_* calls.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_mark_entry_dirty(e), 0);
    EXPECT_LT(H5C_mark_entry_clean(e), 0);
    EXPECT_LT(H5C_mark_entry_unserialized(e), 0);
    EXPECT_LT(H5C_mark_entry_serialized(e), 0);
  }

  // Protected entry: mark_clean and mark_serialized fail when protected;
  // mark_dirty and mark_unserialized succeed (including unserialize child
  // propagation error).
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_e, nullptr,
                        H5C__NO_FLAGS_SET),
            e);
  ASSERT_GE(H5C_create_flush_dependency(e_parent, e), 0);
  e->header.image_up_to_date = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_mark_entry_clean(e), 0);
    EXPECT_LT(H5C_mark_entry_serialized(e), 0);

    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED;
    // Trigger CHILD_UNSERIALIZED error inside H5C_mark_entry_dirty:
    EXPECT_LT(H5C_mark_entry_dirty(e), 0);
    e->header.image_up_to_date = true;
    // Trigger CHILD_UNSERIALIZED error inside H5C_mark_entry_unserialized:
    EXPECT_LT(H5C_mark_entry_unserialized(e), 0);
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
  }
  // Undo half-incremented nunser_children from the simulated fail calls before
  // continuing (each failed CHILD_UNSERIALIZED incremented it once):
  e_parent->header.flush_dep_nunser_children = 0;
  e->header.image_up_to_date = true;
  ASSERT_GE(H5C_mark_entry_dirty(e), 0);
  EXPECT_TRUE(e->header.dirtied);
  EXPECT_FALSE(e->header.image_up_to_date);
  EXPECT_EQ(e_parent->header.flush_dep_nunser_children, 1U);
  // Pin while protected, then unprotect.
  ASSERT_GE(H5C_unprotect(fix.f, a_e, e, H5C__PIN_ENTRY_FLAG), 0);

  // Pinned entry: mark_serialized & mark_clean & mark_dirty & mark_unserialized.
  ASSERT_GE(H5C_mark_entry_serialized(e), 0);
  EXPECT_TRUE(e->header.image_up_to_date);
  ASSERT_GE(H5C_mark_entry_clean(e), 0);
  EXPECT_FALSE(e->header.is_dirty);
  // Clean->clean no-op & serialized->serialized no-op:
  ASSERT_GE(H5C_mark_entry_clean(e), 0);
  ASSERT_GE(H5C_mark_entry_serialized(e), 0);
  // Pinned clean->dirty:
  ASSERT_GE(H5C_mark_entry_dirty(e), 0);
  EXPECT_TRUE(e->header.is_dirty);
  EXPECT_FALSE(e->header.image_up_to_date);
  // Pinned already-dirty mark_entry_dirty + mark_entry_unserialized:
  e->header.image_up_to_date = true;
  e_parent->header.flush_dep_nunser_children = 0;
  ASSERT_GE(H5C_mark_entry_dirty(e), 0);
  EXPECT_FALSE(e->header.image_up_to_date);
  ASSERT_GE(H5C_mark_entry_unserialized(e), 0);

  // Notify errors while pinned:
  {
    ScopedErrorSilence silence;
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;

    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_SERIALIZED;
    EXPECT_LT(H5C_mark_entry_serialized(e), 0);

    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_CLEANED;
    EXPECT_LT(H5C_mark_entry_clean(e), 0);
    // e was marked clean before CHILD_CLEANED notify; reset count and test
    // ENTRY_DIRTIED fail next!
    e_parent->header.flush_dep_ndirty_children = 0;

    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_DIRTIED;
    EXPECT_LT(H5C_mark_entry_dirty(e), 0);

    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
  }

  // Re-sync parent dirty child count via mark clean -> dirty cleanly.
  e_parent->header.flush_dep_ndirty_children = 1;
  e_parent->header.flush_dep_nunser_children = 1;
  ASSERT_GE(H5C_mark_entry_serialized(e), 0);
  ASSERT_GE(H5C_mark_entry_clean(e), 0);

  // 2. H5C_move_entry: missing, wrong type, R/O, collisions, clean->dirty with
  // slist enabled, image_up_to_date -> false, flush_in_progress,
  // destroy_in_progress, and notify failure.
  ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);
  // Missing and wrong-type no-ops succeed cleanly.
  EXPECT_GE(H5C_move_entry(fix.tc, &fix.classes.regular, a_missing, a_missing2),
            0);
  EXPECT_GE(H5C_move_entry(fix.tc, &fix.classes.skip_rw, a_e, a_move1), 0);

  // Move clean, serialized pinned entry -> dirties, unserializes, notifies.
  ASSERT_GE(H5C_move_entry(fix.tc, &fix.classes.regular, a_e, a_move1), 0);
  EXPECT_EQ(e->header.addr, a_move1);
  EXPECT_TRUE(e->header.is_dirty);
  EXPECT_FALSE(e->header.image_up_to_date);

  // Collisions & R/O error.
  TestEntry* e_other = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.skip_rw, a_other, e_other,
                             H5C__NO_FLAGS_SET),
            0);
  TestEntry* e_same = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_same, e_same,
                             H5C__NO_FLAGS_SET),
            0);
  {
    ScopedErrorSilence silence;
    // Collide different type & same type:
    EXPECT_LT(H5C_move_entry(fix.tc, &fix.classes.regular, a_move1, a_other),
              0);
    EXPECT_LT(H5C_move_entry(fix.tc, &fix.classes.regular, a_move1, a_same), 0);

    // R/O error:
    ASSERT_GE(H5C_flush_cache(fix.f, H5C__NO_FLAGS_SET), 0);
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_same, nullptr,
                          H5C__READ_ONLY_FLAG),
              e_same);
    EXPECT_LT(H5C_move_entry(fix.tc, &fix.classes.regular, a_same, a_move2), 0);
    ASSERT_GE(H5C_unprotect(fix.f, a_same, e_same, H5C__NO_FLAGS_SET), 0);
  }

  // Move while flush_in_progress = true skips dirty notify/RP update.
  e_same->header.flush_in_progress = true;
  EXPECT_GE(H5C_move_entry(fix.tc, &fix.classes.regular, a_same, a_move2), 0);
  e_same->header.flush_in_progress = false;

  // Move while destroy_in_progress = true only updates entry->addr (restore addr
  // afterwards so index stays consistent before teardown).
  e_same->header.destroy_in_progress = true;
  EXPECT_GE(H5C_move_entry(fix.tc, &fix.classes.regular, a_move2, a_tmp), 0);
  EXPECT_EQ(e_same->header.addr, a_tmp);
  e_same->header.addr = a_move2;
  e_same->header.destroy_in_progress = false;

  // 3. H5C_resize_entry: usage errors, same size, grow/shrink while pinned,
  // grow/shrink while protected, flash increase trigger + notify fail.
  ASSERT_GE(H5C_unpin_entry(e), 0);
  ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_resize_entry(e, 0), 0);
    EXPECT_LT(H5C_resize_entry(e, 128), 0);  // Neither pinned nor protected!
  }

  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_move1, nullptr,
                        H5C__NO_FLAGS_SET),
            e);
  ASSERT_GE(H5C_pin_protected_entry(e), 0);
  ASSERT_GE(H5C_create_flush_dependency(e_parent, e), 0);
  // Same size is a no-op:
  ASSERT_GE(H5C_resize_entry(e, e->header.size), 0);
  // Allocate image_ptr if not already serialized and mark image_up_to_date to
  // test freeing image_ptr + CHILD_UNSERIALIZED inside H5C_resize_entry:
  if (e->header.image_ptr == nullptr) {
    e->header.image_ptr = H5MM_malloc(e->header.size);
  }
  e->header.image_up_to_date = true;
  e_parent->header.flush_dep_nunser_children = 0;

  H5C_auto_size_ctl_t resize_ctl{};
  resize_ctl.version = H5C__CURR_AUTO_SIZE_CTL_VER;
  resize_ctl.rpt_fcn = nullptr;
  resize_ctl.set_initial_size = true;
  resize_ctl.initial_size = 4096;
  resize_ctl.min_clean_fraction = 0.25;
  resize_ctl.max_size = 65536;
  resize_ctl.min_size = H5C__MIN_MAX_CACHE_SIZE;
  resize_ctl.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  resize_ctl.incr_mode = H5C_incr__off;
  resize_ctl.flash_incr_mode = H5C_flash_incr__add_space;
  resize_ctl.flash_multiple = 2.0;
  resize_ctl.flash_threshold = 0.25;
  resize_ctl.decr_mode = H5C_decr__off;
  ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);

  // Clean->dirty resize large enough to trigger flash increase (must make
  // index_size + new_size > 4096 when max_cache_size == 4096):
  ASSERT_GE(H5C_unprotect(fix.f, a_move1, e, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_mark_entry_clean(e), 0);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_move1, nullptr,
                        H5C__NO_FLAGS_SET),
            e);
  e->len = 4096;
  ASSERT_GE(H5C_resize_entry(e, 4096), 0);
  EXPECT_EQ(e->header.image_ptr, nullptr);
  EXPECT_TRUE(e->header.is_dirty);
  EXPECT_GT(fix.tc->max_cache_size, 4096U);

  // Shrink while pinned & protected:
  e->len = 64;
  ASSERT_GE(H5C_resize_entry(e, 64), 0);
  ASSERT_GE(H5C_unprotect(fix.f, a_move1, e, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_unpin_entry(e), 0);
  ASSERT_GE(H5C_destroy_flush_dependency(e_parent, e), 0);

  // 4. H5C_unsettle_entry_ring: USER, RDFSM, MDFSM (already unsettled,
  // settled -> unsettled, settled during flush/close error).
  TestEntry* e_ring = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_ring, e_ring,
                             H5C__NO_FLAGS_SET),
            0);
  e_ring->header.ring = H5C_RING_USER;
  EXPECT_GE(H5C_unsettle_entry_ring(e_ring), 0);

  for (H5C_ring_t ring : {H5C_RING_RDFSM, H5C_RING_MDFSM}) {
    e_ring->header.ring = ring;
    fix.tc->rdfsm_settled = false;
    fix.tc->mdfsm_settled = false;
    EXPECT_GE(H5C_unsettle_entry_ring(e_ring), 0);

    fix.tc->rdfsm_settled = true;
    fix.tc->mdfsm_settled = true;
    EXPECT_GE(H5C_unsettle_entry_ring(e_ring), 0);

    fix.tc->rdfsm_settled = true;
    fix.tc->mdfsm_settled = true;
    fix.tc->close_warning_received = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_unsettle_entry_ring(e_ring), 0);
    }
    fix.tc->close_warning_received = false;
  }
  e_ring->header.ring = H5C_RING_USER;
  // Reconcile ring index sizes for e_ring? e_ring was inserted under USER ring
  // and restored to USER ring, so ring sizes match identically!

  ASSERT_GE(H5C_unpin_entry(e_parent), 0);
}

TEST(H5CentryTest, FlushDependenciesExpungeAndRemoveEntry) {
  CacheFixture fix;
  ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                           /*min_clean_size=*/16 * 1024));
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  haddr_t a_child = fix.AllocSpace(64);
  std::array<haddr_t, 9> a_parents{};
  for (int i = 0; i < 9; ++i) {
    a_parents[i] = fix.AllocSpace(64);
    ASSERT_NE(a_parents[i], HADDR_UNDEF);
  }
  haddr_t a_unpinned_p = fix.AllocSpace(64);
  haddr_t a_outsider = fix.AllocSpace(64);
  haddr_t a_missing = fix.AllocSpace(64);
  ASSERT_NE(a_missing, HADDR_UNDEF);

  // 1. Create child and 9 parents to exercise array allocation (8 -> 16),
  // middle parent removal (memmove), and array shrinking (16 -> 4 -> free).
  TestEntry* child = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_child, child,
                             H5C__PIN_ENTRY_FLAG),
            0);
  child->header.image_up_to_date = false;  // Dirty AND unserialized.

  std::vector<TestEntry*> parents;
  for (int i = 0; i < 9; ++i) {
    TestEntry* p = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_parents[i], p,
                               H5C__PIN_ENTRY_FLAG),
              0);
    parents.push_back(p);
  }

  // Error checks on create & destroy before adding parents.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_create_flush_dependency(parents[0], parents[0]), 0);
    TestEntry* unpinned_p = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_unpinned_p,
                               unpinned_p, H5C__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5C_create_flush_dependency(unpinned_p, child), 0);
    EXPECT_LT(H5C_destroy_flush_dependency(unpinned_p, child), 0);
    EXPECT_LT(H5C_destroy_flush_dependency(parents[0], child), 0);
  }

  for (int i = 0; i < 9; ++i) {
    ASSERT_GE(H5C_create_flush_dependency(parents[i], child), 0);
  }
  EXPECT_EQ(child->header.flush_dep_nparents, 9U);
  EXPECT_GE(child->header.flush_dep_parent_nalloc, 16U);

  // Unrelated parent not in child's array fails destroy.
  TestEntry* outsider = MakeTestEntry(&fix.ctrl, 64);
  ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_outsider, outsider,
                             H5C__PIN_ENTRY_FLAG),
            0);
  ASSERT_GE(H5C_create_flush_dependency(outsider, parents[0]), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_destroy_flush_dependency(outsider, child), 0);
  }
  ASSERT_GE(H5C_destroy_flush_dependency(outsider, parents[0]), 0);
  ASSERT_GE(H5C_unpin_entry(outsider), 0);

  // Destroy parents[0] and parents[4] from middle (memmove), then remaining
  // parents so nalloc shrinks 16 -> 4 -> 0!
  ASSERT_GE(H5C_destroy_flush_dependency(parents[0], child), 0);
  ASSERT_GE(H5C_destroy_flush_dependency(parents[4], child), 0);
  for (int i = 1; i < 9; ++i) {
    if (i == 4) continue;
    ASSERT_GE(H5C_destroy_flush_dependency(parents[i], child), 0);
  }
  EXPECT_EQ(child->header.flush_dep_nparents, 0U);
  EXPECT_EQ(child->header.flush_dep_parent, nullptr);
  EXPECT_EQ(child->header.flush_dep_parent_nalloc, 0U);

  // 2. H5C_expunge_entry & H5C_remove_entry error and success paths.
  // Missing address and wrong type are harmless no-ops.
  EXPECT_GE(H5C_expunge_entry(fix.f, &fix.classes.regular, a_missing,
                              H5C__NO_FLAGS_SET),
            0);
  EXPECT_GE(H5C_expunge_entry(fix.f, &fix.classes.skip_rw, a_child,
                              H5C__NO_FLAGS_SET),
            0);

  // Cannot remove or expunge pinned entry.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_expunge_entry(fix.f, &fix.classes.regular, a_child,
                                H5C__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5C_remove_entry(child), 0);
  }

  // Unpin parents[0] and attach dep so parents[0] has flush_dep_nchildren > 0,
  // while child has flush_dep_nparents > 0 -> verify both H5C_remove_entry
  // checks fail.
  ASSERT_GE(H5C_unpin_entry(child), 0);
  ASSERT_GE(H5C_unpin_entry(parents[0]), 0);
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_parents[0], nullptr,
                        H5C__NO_FLAGS_SET),
            parents[0]);
  ASSERT_GE(H5C_create_flush_dependency(parents[0], child), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_remove_entry(child), 0);  // child is dirty AND has parent.
    EXPECT_LT(H5C_remove_entry(parents[0]), 0);
  }
  ASSERT_GE(H5C_unprotect(fix.f, a_parents[0], parents[0], H5C__NO_FLAGS_SET),
            0);
  // Protect child -> protected checks on expunge and remove.
  ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_child, nullptr,
                        H5C__NO_FLAGS_SET),
            child);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_expunge_entry(fix.f, &fix.classes.regular, a_child,
                                H5C__NO_FLAGS_SET),
              0);
    EXPECT_LT(H5C_remove_entry(child), 0);
  }
  ASSERT_GE(H5C_unprotect(fix.f, a_child, child, H5C__NO_FLAGS_SET), 0);
  ASSERT_GE(H5C_destroy_flush_dependency(parents[0], child), 0);

  // Flush all so parents[0] is clean and unpinned, then test H5C_remove_entry
  // with BEFORE_EVICT fail, then clean H5C_remove_entry + entry_watched_for_removal.
  ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);
  ASSERT_GE(H5C_flush_cache(fix.f, H5C__NO_FLAGS_SET), 0);
  ASSERT_FALSE(parents[0]->header.is_dirty);
  fix.ctrl.fail_notify = true;
  fix.ctrl.filter_notify_action = true;
  fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_BEFORE_EVICT;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_remove_entry(parents[0]), 0);
  }
  fix.ctrl.fail_notify = false;
  fix.ctrl.filter_notify_action = false;

  if (parents[0]->header.image_ptr == nullptr) {
    parents[0]->header.image_ptr = H5MM_malloc(64);
  }
  fix.tc->entry_watched_for_removal = &parents[0]->header;
  ASSERT_GE(H5C_remove_entry(parents[0]), 0);
  EXPECT_EQ(fix.tc->entry_watched_for_removal, nullptr);
  EXPECT_EQ(parents[0]->header.cache_ptr, nullptr);
  delete parents[0];

  // Unpin remaining parents[1..8] so Teardown cleans up.
  for (int i = 1; i < 9; ++i) {
    ASSERT_GE(H5C_unpin_entry(parents[i]), 0);
  }
}

TEST(H5CentryTest, ExtraNotifyAndWritePermittedPaths) {
  // Part A: Null check_write_permitted, oversized protect space_needed clamp,
  // flash increase in H5C_protect, and size_decreased error branches.
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/4096,
                             /*min_clean_size=*/1024,
                             /*with_write_cb=*/false,
                             /*with_log_flush=*/true,
                             H5F_FSPACE_STRATEGY_AGGR));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5C_auto_size_ctl_t resize_ctl{};
    resize_ctl.version = H5C__CURR_AUTO_SIZE_CTL_VER;
    resize_ctl.rpt_fcn = nullptr;
    resize_ctl.set_initial_size = true;
    resize_ctl.initial_size = 4096;
    resize_ctl.min_clean_fraction = 0.25;
    resize_ctl.max_size = 65536;
    resize_ctl.min_size = H5C__MIN_MAX_CACHE_SIZE;
    resize_ctl.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
    resize_ctl.incr_mode = H5C_incr__off;
    resize_ctl.flash_incr_mode = H5C_flash_incr__add_space;
    resize_ctl.flash_multiple = 2.0;
    resize_ctl.flash_threshold = 0.25;
    resize_ctl.decr_mode = H5C_decr__off;
    ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);

    haddr_t a_ins = fix.AllocSpace(5000);
    haddr_t a_flash = fix.AllocSpace(4000);
    haddr_t a_huge = fix.AllocSpace(8192);
    haddr_t a_hit = fix.AllocSpace(64);
    ASSERT_NE(a_hit, HADDR_UNDEF);
    std::vector<uint8_t> b4000(4000, 0x71);
    std::vector<uint8_t> b8192(8192, 0x72);
    ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_BTREE, a_flash, 4000,
                              b4000.data()),
              0);
    ASSERT_GE(H5F_block_write(fix.f, H5FD_MEM_BTREE, a_huge, 8192,
                              b8192.data()),
              0);

    // Insert with check_write_permitted == nullptr and size > max_cache_size.
    TestEntry* e_ins = MakeTestEntry(&fix.ctrl, 5000);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_ins, e_ins,
                               H5C__NO_FLAGS_SET),
              0);

    // Protect miss triggering flash size increase inside H5C_protect (miss).
    LoadUdata ud_flash{&fix.ctrl, fix.f, 4000, 4000, false, 0x71};
    auto* e_flash = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.regular, a_flash, &ud_flash, H5C__NO_FLAGS_SET));
    ASSERT_NE(e_flash, nullptr);
    EXPECT_GT(fix.tc->max_cache_size, 4096U);
    ASSERT_GE(H5C_unprotect(fix.f, a_flash, e_flash, H5C__NO_FLAGS_SET), 0);

    // Disable flash increase and protect an oversized entry with
    // check_write_permitted == nullptr (`space_needed > max_cache_size` clamp).
    resize_ctl.set_initial_size = false;
    resize_ctl.flash_incr_mode = H5C_flash_incr__off;
    ASSERT_GE(H5C_set_cache_auto_resize_config(fix.tc, &resize_ctl), 0);
    fix.tc->max_cache_size = 4096;
    LoadUdata ud_huge{&fix.ctrl, fix.f, 8192, 8192, false, 0x72};
    auto* e_huge = static_cast<TestEntry*>(H5C_protect(
        fix.f, &fix.classes.regular, a_huge, &ud_huge, H5C__NO_FLAGS_SET));
    ASSERT_NE(e_huge, nullptr);
    ASSERT_GE(H5C_unprotect(fix.f, a_huge, e_huge, H5C__NO_FLAGS_SET), 0);

    // Hit protect with size_decreased = true and check_write_permitted ==
    // nullptr.
    TestEntry* e_hit = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_hit, e_hit,
                               H5C__NO_FLAGS_SET),
              0);
    fix.tc->size_decreased = true;
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_hit, nullptr,
                          H5C__NO_FLAGS_SET),
              e_hit);
    EXPECT_FALSE(fix.tc->size_decreased);
    ASSERT_GE(H5C_unprotect(fix.f, a_hit, e_hit, H5C__NO_FLAGS_SET), 0);

    // Attach CbCheckWritePermitted to cover size_decreased error branches.
    fix.tc->check_write_permitted = CbCheckWritePermitted;
    fix.tc->size_decreased = true;
    fix.ctrl.fail_write_permitted = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_hit, nullptr,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }
    fix.ctrl.fail_write_permitted = false;
    fix.tc->size_decreased = false;
    // Previous protect hit already unprotected/protected e_hit before the
    // write_permitted check at line 3304, so e_hit is protected now.
    ASSERT_GE(H5C_unprotect(fix.f, a_hit, e_hit, H5C__NO_FLAGS_SET), 0);

    // Enable slist, expunge clean e_huge so only dirty e_dirty_sac can be
    // evicted, and trigger size_decreased make_space failure via fail_serialize.
    ASSERT_GE(H5C_expunge_entry(fix.f, &fix.classes.regular, a_huge,
                                H5C__NO_FLAGS_SET),
              0);
    ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);
    haddr_t a_dirty_sac = fix.AllocSpace(4096);
    ASSERT_NE(a_dirty_sac, HADDR_UNDEF);
    TestEntry* e_dirty_sac = MakeTestEntry(&fix.ctrl, 4096);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_dirty_sac,
                               e_dirty_sac, H5C__NO_FLAGS_SET),
              0);
    fix.tc->size_decreased = true;
    fix.ctrl.fail_serialize = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_hit, nullptr,
                            H5C__NO_FLAGS_SET),
                nullptr);
    }
    fix.ctrl.fail_serialize = false;
    fix.tc->size_decreased = false;
    ASSERT_GE(H5C_unprotect(fix.f, a_hit, e_hit, H5C__NO_FLAGS_SET), 0);
  }

  // Part B: Notify failure paths in unprotect, mark_dirty/clean, move, resize,
  // create/destroy_flush_dependency, discard without fsf_size, and remove.
  {
    CacheFixture fix;
    ASSERT_TRUE(fix.InitCore(/*max_cache_size=*/64 * 1024,
                             /*min_clean_size=*/16 * 1024,
                             /*with_write_cb=*/true,
                             /*with_log_flush=*/true,
                             H5F_FSPACE_STRATEGY_AGGR));
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5C_set_slist_enabled(fix.tc, true, true), 0);

    haddr_t a_p1 = fix.AllocSpace(64);
    haddr_t a_p2 = fix.AllocSpace(64);
    haddr_t a_c = fix.AllocSpace(64);
    haddr_t a_c_mv = fix.AllocSpace(64);
    haddr_t a_c_mv2 = fix.AllocSpace(64);
    haddr_t a_nofsf = fix.AllocSpace(64);
    haddr_t a_rem = fix.AllocSpace(64);
    ASSERT_NE(a_rem, HADDR_UNDEF);

    TestEntry* p1 = MakeTestEntry(&fix.ctrl, 64);
    TestEntry* p2 = MakeTestEntry(&fix.ctrl, 64);
    TestEntry* c = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_p1, p1,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_p2, p2,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_c, c,
                               H5C__PIN_ENTRY_FLAG),
              0);

    // Clean & serialize c first, unpin so it can be protected/unprotected.
    ASSERT_GE(H5C_mark_entry_clean(c), 0);
    ASSERT_GE(H5C_mark_entry_serialized(c), 0);
    ASSERT_GE(H5C_unpin_entry(c), 0);
    ASSERT_GE(H5C_create_flush_dependency(p1, c), 0);

    // 1) H5C_unprotect at ro_ref_count <= 1 pin/unpin error +
    //    CHILD_UNSERIALIZED notify fail.
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_c, nullptr,
                          H5C__NO_FLAGS_SET),
              c);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_unprotect(fix.f, a_c, c, H5C__UNPIN_ENTRY_FLAG), 0);
    }
    ASSERT_GE(H5C_pin_protected_entry(c), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_unprotect(fix.f, a_c, c, H5C__PIN_ENTRY_FLAG), 0);
    }
    // Fail CHILD_UNSERIALIZED notify inside H5C_unprotect(DIRTIED). Note
    // line 3582 set c->is_dirty = true before H5C__UPDATE_INDEX_FOR_ENTRY_DIRTY
    // ran, so reset c->is_dirty = false to keep clean_index_size consistent
    // before the subsequent clean H5C_unprotect.
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_unprotect(fix.f, a_c, c, H5C__DIRTIED_FLAG), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    c->header.is_dirty = false;
    ASSERT_GE(H5C_unprotect(fix.f, a_c, c, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_mark_entry_serialized(c), 0);

    // 2) H5C_mark_entry_dirty CHILD_DIRTIED & CHILD_UNSERIALIZED notify errors
    //    and H5C_mark_entry_clean ENTRY_CLEANED notify error.
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_mark_entry_dirty(c), 0);
    }
    // c is now dirty and unserialized, p1->ndirty_children == 1,
    // p1->nunser_children == 0. Test CHILD_UNSERIALIZED on already-dirty c:
    c->header.image_up_to_date = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_mark_entry_dirty(c), 0);
    }
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_CLEANED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_mark_entry_clean(c), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    // Sync p1 counts (c is now clean & unserialized).
    p1->header.flush_dep_ndirty_children = 0;

    // 3) H5C_move_entry notify failures: ENTRY_DIRTIED and CHILD_DIRTIED
    //    (keeping c indexed in cache).
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_move_entry(fix.tc, &fix.classes.regular, a_c, a_c_mv), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    p1->header.flush_dep_ndirty_children = 1;
    ASSERT_GE(H5C_mark_entry_clean(c), 0);

    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_move_entry(fix.tc, &fix.classes.regular, a_c_mv, a_c_mv2),
                0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    ASSERT_GE(H5C_mark_entry_clean(c), 0);

    // 4) H5C_resize_entry notify failures: ENTRY_DIRTIED & CHILD_DIRTIED.
    c->len = 96;
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_ENTRY_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_resize_entry(c, 96), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;
    p1->header.flush_dep_ndirty_children = 1;
    ASSERT_GE(H5C_mark_entry_clean(c), 0);

    c->len = 64;
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_resize_entry(c, 64), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;

    // 5) H5C_create_flush_dependency & H5C_destroy_flush_dependency notify
    //    errors while keeping another parent attached so parent array frees
    //    cleanly on the last destroy.
    // c is dirty & unserialized right now.
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_DIRTIED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_create_flush_dependency(p2, c), 0);
    }
    // Destroy p2 dep (syncing nunser_children since error returned early before
    // nunser_children++), clean c (leaving unserialized), then fail
    // CHILD_UNSERIALIZED during create_flush_dependency.
    fix.ctrl.fail_notify = false;
    p2->header.flush_dep_nunser_children = 1;
    ASSERT_GE(H5C_destroy_flush_dependency(p2, c), 0);
    ASSERT_GE(H5C_mark_entry_clean(c), 0);
    fix.ctrl.fail_notify = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_create_flush_dependency(p2, c), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;

    // Now c has 2 parents (p1, p2); make c dirty & unserialized and test
    // H5C_destroy_flush_dependency notify errors on p2 while p1 stays
    // attached.
    ASSERT_GE(H5C_mark_entry_dirty(c), 0);
    fix.ctrl.fail_notify = true;
    fix.ctrl.filter_notify_action = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_CLEANED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_destroy_flush_dependency(p2, c), 0);
    }
    fix.ctrl.fail_notify = false;
    p2->header.flush_dep_nunser_children = 0;
    // Re-add p2, mark c clean (unserialized), and test CHILD_SERIALIZED error.
    ASSERT_GE(H5C_mark_entry_clean(c), 0);
    ASSERT_GE(H5C_create_flush_dependency(p2, c), 0);
    fix.ctrl.fail_notify = true;
    fix.ctrl.fail_notify_action = H5C_NOTIFY_ACTION_CHILD_SERIALIZED;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_destroy_flush_dependency(p2, c), 0);
    }
    fix.ctrl.fail_notify = false;
    fix.ctrl.filter_notify_action = false;

    // Cleanly destroy remaining dep p1->c and unpin p1, p2, c.
    ASSERT_GE(H5C_destroy_flush_dependency(p1, c), 0);
    ASSERT_GE(H5C_unpin_entry(c), 0);
    ASSERT_GE(H5C_unpin_entry(p1), 0);
    ASSERT_GE(H5C_unpin_entry(p2), 0);

    // 6) Discard without fsf_size callback + entry_watched_for_removal.
    TestEntry* e_nofsf = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.no_verify, a_nofsf, e_nofsf,
                               H5C__NO_FLAGS_SET),
              0);
    fix.tc->entry_watched_for_removal = &e_nofsf->header;
    ASSERT_GE(H5C_expunge_entry(fix.f, &fix.classes.no_verify, a_nofsf,
                                H5C__FREE_FILE_SPACE_FLAG),
              0);
    EXPECT_EQ(fix.tc->entry_watched_for_removal, nullptr);

    // 7) H5C_remove_entry on clean pinned and clean protected entries.
    TestEntry* e_rem = MakeTestEntry(&fix.ctrl, 64);
    ASSERT_GE(H5C_insert_entry(fix.f, &fix.classes.regular, a_rem, e_rem,
                               H5C__PIN_ENTRY_FLAG),
              0);
    ASSERT_GE(H5C_mark_entry_clean(e_rem), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_remove_entry(e_rem), 0);  // clean pinned -> fail.
    }
    ASSERT_GE(H5C_unpin_entry(e_rem), 0);
    ASSERT_EQ(H5C_protect(fix.f, &fix.classes.regular, a_rem, nullptr,
                          H5C__NO_FLAGS_SET),
              e_rem);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5C_remove_entry(e_rem), 0);  // clean protected -> fail.
    }
    ASSERT_GE(H5C_unprotect(fix.f, a_rem, e_rem, H5C__NO_FLAGS_SET), 0);
  }
}

}  // namespace
}  // namespace hdf5
