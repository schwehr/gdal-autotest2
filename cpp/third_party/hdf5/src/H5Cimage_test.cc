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
// Tests metadata cache image configuration validation, status tracking,
// SWMR incompatibility, superblock-version/empty-cache cancellation,
// Core VFD R/O and R/W cache image generation and loading, flush dependency
// height calculation, and malformed/corrupt cache image error handling
// (third_party/hdf5/src/H5Cimage.c).
//
// Build caveats: `:hdf5` in google3 is compiled with `-DNDEBUG`, without
// `H5_HAVE_PARALLEL`, and with `H5C_COLLECT_CACHE_STATS == 0`. `NDEBUG` is
// temporarily defined around internal HDF5 headers so `H5C_t`, `H5F_t`, and
// `H5C_cache_entry_t` struct layouts match `:hdf5`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cimage.c

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5CIMAGE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define try h5f_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5CIMAGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CIMAGE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII guard for pushing an HDF5 internal API context when invoking internal
// package routines directly.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__INVALID_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__INVALID_TAG) {
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

// In-memory Core VFD file-image capture state so closed HDF5 files (including
// cache images emitted inside H5C_dest during H5Fclose) remain inspectable
// in memory with zero disk I/O.
struct CaptureState {
  std::mutex mu;
  std::unordered_map<void*, size_t> alloc_sizes;
  std::vector<uint8_t> closed_image;
};

struct UdataBox {
  std::shared_ptr<CaptureState> state;
};

void* CaptureImageMalloc(size_t size, H5FD_file_image_op_t /*op*/,
                         void* udata) {
  auto* box = static_cast<UdataBox*>(udata);
  void* ptr = std::calloc(1, size == 0 ? 1 : size);
  if (ptr != nullptr && box != nullptr && box->state != nullptr) {
    std::lock_guard<std::mutex> lock(box->state->mu);
    box->state->alloc_sizes[ptr] = size;
  }
  return ptr;
}

void* CaptureImageMemcpy(void* dest, const void* src, size_t size,
                         H5FD_file_image_op_t /*op*/, void* /*udata*/) {
  return std::memcpy(dest, src, size);
}

void* CaptureImageRealloc(void* ptr, size_t size, H5FD_file_image_op_t /*op*/,
                          void* udata) {
  auto* box = static_cast<UdataBox*>(udata);
  size_t old_size = 0;
  if (ptr != nullptr && box != nullptr && box->state != nullptr) {
    std::lock_guard<std::mutex> lock(box->state->mu);
    auto it = box->state->alloc_sizes.find(ptr);
    if (it != box->state->alloc_sizes.end()) {
      old_size = it->second;
    }
  }
  void* next = std::calloc(1, size == 0 ? 1 : size);
  if (next == nullptr) {
    return nullptr;
  }
  if (ptr != nullptr && old_size > 0) {
    std::memcpy(next, ptr, std::min(old_size, size));
  }
  if (box != nullptr && box->state != nullptr) {
    std::lock_guard<std::mutex> lock(box->state->mu);
    if (ptr != nullptr) {
      box->state->alloc_sizes.erase(ptr);
    }
    box->state->alloc_sizes[next] = size;
  }
  std::free(ptr);
  return next;
}

herr_t CaptureImageFree(void* ptr, H5FD_file_image_op_t op, void* udata) {
  if (ptr == nullptr) {
    return 0;
  }
  auto* box = static_cast<UdataBox*>(udata);
  if (box != nullptr && box->state != nullptr) {
    std::lock_guard<std::mutex> lock(box->state->mu);
    auto it = box->state->alloc_sizes.find(ptr);
    if (it != box->state->alloc_sizes.end()) {
      const size_t sz = it->second;
      if (op == H5FD_FILE_IMAGE_OP_FILE_CLOSE && sz > 0) {
        const auto* bytes = static_cast<const uint8_t*>(ptr);
        box->state->closed_image.assign(bytes, bytes + sz);
      }
      box->state->alloc_sizes.erase(it);
    }
  }
  std::free(ptr);
  return 0;
}

void* CaptureUdataCopy(void* udata) {
  const auto* src = static_cast<const UdataBox*>(udata);
  return new UdataBox{src->state};
}

herr_t CaptureUdataFree(void* udata) {
  delete static_cast<UdataBox*>(udata);
  return 0;
}

ScopedPlist MakeCoreCacheImageFapl(
    std::shared_ptr<CaptureState> capture, bool generate_image,
    H5F_libver_t low = H5F_LIBVER_V110, H5F_libver_t high = H5F_LIBVER_LATEST,
    const std::vector<uint8_t>* initial_image = nullptr) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedPlist();
  }
  if (capture != nullptr) {
    UdataBox box{std::move(capture)};
    H5FD_file_image_callbacks_t cb{};
    cb.image_malloc = CaptureImageMalloc;
    cb.image_memcpy = CaptureImageMemcpy;
    cb.image_realloc = CaptureImageRealloc;
    cb.image_free = CaptureImageFree;
    cb.udata_copy = CaptureUdataCopy;
    cb.udata_free = CaptureUdataFree;
    cb.udata = &box;
    if (H5Pset_file_image_callbacks(fapl.get(), &cb) < 0) {
      return ScopedPlist();
    }
  }
  if (initial_image != nullptr && !initial_image->empty()) {
    if (H5Pset_file_image(fapl.get(),
                          const_cast<uint8_t*>(initial_image->data()),
                          initial_image->size()) < 0) {
      return ScopedPlist();
    }
  }
  if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) {
    return ScopedPlist();
  }
  H5AC_cache_image_config_t img_cfg = H5AC__DEFAULT_CACHE_IMAGE_CONFIG;
  img_cfg.generate_image = generate_image;
  if (H5Pset_mdc_image_config(fapl.get(), &img_cfg) < 0) {
    return ScopedPlist();
  }
  return fapl;
}

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Restores the active API context ring on scope exit.
class ScopedRingSetter {
 public:
  explicit ScopedRingSetter(H5AC_ring_t ring) {
    prev_ = H5CX_get_ring();
    H5CX_set_ring(ring);
  }

  ~ScopedRingSetter() { H5CX_set_ring(prev_); }

  ScopedRingSetter(const ScopedRingSetter&) = delete;
  ScopedRingSetter& operator=(const ScopedRingSetter&) = delete;

 private:
  H5AC_ring_t prev_ = H5C_RING_UNDEFINED;
};

struct TestCacheEntry {
  H5C_cache_entry_t hdr{};
  size_t size = 16;
  bool fail_serialize = false;
};

herr_t TestEntryGetInitialLoadSize(void* /*udata*/, size_t* /*image_len*/) {
  return -1;
}

herr_t TestEntryGetFinalLoadSize(const void* /*image*/, size_t /*image_len*/,
                                 void* /*udata*/, size_t* /*actual_len*/) {
  return -1;
}

htri_t TestEntryVerifyChksum(const void* /*image*/, size_t /*len*/,
                             void* /*udata*/) {
  return -1;
}

void* TestEntryDeserialize(const void* /*image*/, size_t /*len*/,
                           void* /*udata*/, bool* /*dirty*/) {
  return nullptr;
}

herr_t TestEntryImageLen(const void* thing, size_t* image_len_ptr) {
  *image_len_ptr = static_cast<const TestCacheEntry*>(thing)->size;
  return 0;
}

herr_t TestEntryPreSerialize(H5F_t* /*f*/, void* /*thing*/, haddr_t /*addr*/,
                             size_t /*len*/, haddr_t* /*new_addr*/,
                             size_t* /*new_len*/, unsigned* flags_ptr) {
  *flags_ptr = 0;
  return 0;
}

herr_t TestEntrySerialize(const H5F_t* /*f*/, void* image_ptr, size_t len,
                          void* thing) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  if (entry->fail_serialize) {
    return -1;
  }
  std::memset(image_ptr, 0x5A, len);
  return 0;
}

herr_t TestEntryNotify(H5C_notify_action_t action, void* thing) {
  if (action == H5C_NOTIFY_ACTION_BEFORE_EVICT) {
    auto* entry = static_cast<H5C_cache_entry_t*>(thing);
    while (entry->flush_dep_nparents > 0) {
      if (H5C_destroy_flush_dependency(
              entry->flush_dep_parent[entry->flush_dep_nparents - 1], entry) <
          0) {
        return -1;
      }
    }
  }
  return 0;
}

herr_t TestEntryFreeIcr(void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  if (entry->hdr.fd_parent_addrs != nullptr) {
    entry->hdr.fd_parent_addrs =
        static_cast<haddr_t*>(H5MM_xfree(entry->hdr.fd_parent_addrs));
  }
  std::free(entry);
  return 0;
}

const H5C_class_t* TestEntryClass() {
  static const H5C_class_t kClass = {
      /* id = */ H5AC_OHDR_ID,
      /* name = */ "test_ohdr",
      /* mem_type = */ H5FD_MEM_OHDR,
      /* flags = */ H5C__CLASS_SKIP_WRITES,
      /* get_initial_load_size = */ TestEntryGetInitialLoadSize,
      /* get_final_load_size = */ TestEntryGetFinalLoadSize,
      /* verify_chksum = */ TestEntryVerifyChksum,
      /* deserialize = */ TestEntryDeserialize,
      /* image_len = */ TestEntryImageLen,
      /* pre_serialize = */ TestEntryPreSerialize,
      /* serialize = */ TestEntrySerialize,
      /* notify = */ TestEntryNotify,
      /* free_icr = */ TestEntryFreeIcr,
      /* fsf_size = */ nullptr,
  };
  return &kClass;
}

TestCacheEntry* AllocTestEntry(size_t size = 16, bool fail_serialize = false) {
  auto* entry = static_cast<TestCacheEntry*>(std::calloc(1, sizeof(TestCacheEntry)));
  if (entry != nullptr) {
    entry->size = size;
    entry->fail_serialize = fail_serialize;
  }
  return entry;
}

// Per-file VFD hook that clones the open file's `H5FD_class_t` onto itself so
// global `H5FD_core_g` is never mutated and VFD failures remain isolated.
class ScopedVfdHooks {
 public:
  struct State {
    const H5FD_class_t* orig_cls = nullptr;
    int read_calls = 0;
    int fail_read_on_call = 0;
    int write_calls = 0;
    int fail_write_on_call = 0;
    int set_eoa_calls = 0;
    int fail_set_eoa_on_call = 0;
    bool fail_get_eoa_after_set_eoa = false;
    bool saw_set_eoa = false;
  };

  explicit ScopedVfdHooks(H5F_t* f) {
    if (f != nullptr && f->shared != nullptr && f->shared->lf != nullptr &&
        f->shared->lf->cls != nullptr) {
      lf_ = f->shared->lf;
      orig_cls_ = lf_->cls;
      hooked_cls_ = *orig_cls_;
      hooked_cls_.read = HookRead;
      hooked_cls_.write = HookWrite;
      hooked_cls_.set_eoa = HookSetEoa;
      hooked_cls_.get_eoa = HookGetEoa;
      hooked_cls_.close = HookClose;
      state_.orig_cls = orig_cls_;
      {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Registry()[lf_] = &state_;
      }
      lf_->cls = &hooked_cls_;
    }
  }

  ~ScopedVfdHooks() { Release(); }

  void Release() {
    if (lf_ == nullptr) {
      return;
    }
    State* found = nullptr;
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      auto it = Registry().find(lf_);
      if (it != Registry().end()) {
        found = it->second;
        Registry().erase(it);
      }
    }
    if (found != nullptr && lf_->cls == &hooked_cls_) {
      lf_->cls = orig_cls_;
    }
    lf_ = nullptr;
  }

  void set_fail_read_on_call(int call_num) {
    state_.read_calls = 0;
    state_.fail_read_on_call = call_num;
  }

  void set_fail_write_on_call(int call_num) {
    state_.write_calls = 0;
    state_.fail_write_on_call = call_num;
  }

  void set_fail_set_eoa_on_call(int call_num) {
    state_.set_eoa_calls = 0;
    state_.fail_set_eoa_on_call = call_num;
  }

  void set_fail_get_eoa_after_set_eoa(bool enable) {
    state_.saw_set_eoa = false;
    state_.fail_get_eoa_after_set_eoa = enable;
  }

  ScopedVfdHooks(const ScopedVfdHooks&) = delete;
  ScopedVfdHooks& operator=(const ScopedVfdHooks&) = delete;

 private:
  static std::mutex& RegistryMutex() {
    static auto* mu = new std::mutex();
    return *mu;
  }

  static std::unordered_map<const H5FD_t*, State*>& Registry() {
    static auto* map = new std::unordered_map<const H5FD_t*, State*>();
    return *map;
  }

  static State* Lookup(const H5FD_t* file) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto it = Registry().find(file);
    return it == Registry().end() ? nullptr : it->second;
  }

  static herr_t HookRead(H5FD_t* file, H5FD_mem_t type, hid_t dxpl,
                         haddr_t addr, size_t size, void* buf) {
    State* s = Lookup(file);
    if (s == nullptr || s->orig_cls == nullptr) return -1;
    if (++s->read_calls == s->fail_read_on_call) return -1;
    return s->orig_cls->read(file, type, dxpl, addr, size, buf);
  }

  static herr_t HookWrite(H5FD_t* file, H5FD_mem_t type, hid_t dxpl,
                          haddr_t addr, size_t size, const void* buf) {
    State* s = Lookup(file);
    if (s == nullptr || s->orig_cls == nullptr) return -1;
    if (++s->write_calls == s->fail_write_on_call) return -1;
    return s->orig_cls->write(file, type, dxpl, addr, size, buf);
  }

  static herr_t HookSetEoa(H5FD_t* file, H5FD_mem_t type, haddr_t addr) {
    State* s = Lookup(file);
    if (s == nullptr || s->orig_cls == nullptr) return -1;
    if (++s->set_eoa_calls == s->fail_set_eoa_on_call) return -1;
    s->saw_set_eoa = true;
    return s->orig_cls->set_eoa(file, type, addr);
  }

  static haddr_t HookGetEoa(const H5FD_t* file, H5FD_mem_t type) {
    State* s = Lookup(file);
    if (s == nullptr || s->orig_cls == nullptr) return HADDR_UNDEF;
    if (s->fail_get_eoa_after_set_eoa && s->saw_set_eoa) {
      s->saw_set_eoa = false;
      return HADDR_UNDEF;
    }
    return s->orig_cls->get_eoa(file, type);
  }

  static herr_t HookClose(H5FD_t* file) {
    const H5FD_class_t* orig = nullptr;
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      auto it = Registry().find(file);
      if (it != Registry().end()) {
        orig = it->second->orig_cls;
        Registry().erase(it);
      }
    }
    if (orig == nullptr) return -1;
    file->cls = orig;
    return orig->close(file);
  }

  H5FD_t* lf_ = nullptr;
  const H5FD_class_t* orig_cls_ = nullptr;
  H5FD_class_t hooked_cls_{};
  State state_{};
};

void AppendLeU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xffU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
}

void AppendLeI32(std::vector<uint8_t>& out, int32_t sv) {
  const uint32_t v = static_cast<uint32_t>(sv);
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xffU));
  }
}

void AppendLeU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xffU));
  }
}

void AppendLeU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xffU));
  }
}

struct SynthEntrySpec {
  uint8_t type_id = static_cast<uint8_t>(H5AC_OHDR_ID);
  uint8_t flags = 0;
  uint8_t ring = static_cast<uint8_t>(H5C_RING_USER);
  uint8_t age = 0;
  uint16_t fd_child_count = 0;
  uint16_t fd_dirty_child_count = 0;
  uint16_t fd_parent_count = 0;
  int32_t lru_rank = -1;
  uint64_t addr = 0;
  uint64_t size = 16;
  std::vector<uint64_t> fd_parent_addrs;
};

// Encodes an 8-byte-addr / 8-byte-size MDCI cache image block matching
// H5Cimage.c layout so header length, entry records, and trailing checksum slot
// stay self-consistent.
std::vector<uint8_t> EncodeSynthImageBlock(
    const std::vector<SynthEntrySpec>& entries,
    std::string_view signature = "MDCI", uint8_t version = 0,
    uint8_t header_flags = 0, uint64_t override_data_len = UINT64_MAX,
    uint32_t override_num_entries = UINT32_MAX) {
  std::vector<uint8_t> body;
  for (const auto& e : entries) {
    body.push_back(e.type_id);
    body.push_back(e.flags);
    body.push_back(e.ring);
    body.push_back(e.age);
    AppendLeU16(body, e.fd_child_count);
    AppendLeU16(body, e.fd_dirty_child_count);
    AppendLeU16(body, e.fd_parent_count);
    AppendLeI32(body, e.lru_rank);
    AppendLeU64(body, e.addr);
    AppendLeU64(body, e.size);
    for (uint64_t paddr : e.fd_parent_addrs) {
      AppendLeU64(body, paddr);
    }
    body.insert(body.end(), static_cast<size_t>(e.size), 0xA5);
  }
  // Trailing 4-byte checksum slot.
  AppendLeU32(body, 0U);

  constexpr size_t kHeaderSize = 4 + 1 + 1 + 8 + 4;
  const uint64_t total_len =
      override_data_len == UINT64_MAX
          ? static_cast<uint64_t>(kHeaderSize + body.size())
          : override_data_len;
  const uint32_t num_entries =
      override_num_entries == UINT32_MAX
          ? static_cast<uint32_t>(entries.size())
          : override_num_entries;

  std::vector<uint8_t> buf;
  buf.reserve(kHeaderSize + body.size());
  for (size_t i = 0; i < 4; ++i) {
    buf.push_back(i < signature.size() ? static_cast<uint8_t>(signature[i])
                                       : 0);
  }
  buf.push_back(version);
  buf.push_back(header_flags);
  AppendLeU64(buf, total_len);
  AppendLeU32(buf, num_entries);
  buf.insert(buf.end(), body.begin(), body.end());
  return buf;
}

// Creates a populated in-memory file with libver [1.10, latest] and cache image
// enabled, returning its closed file bytes alongside an in-file data offset
// suitable for in-place cache image corruption or synthetic block loading tests.
struct SampleImageFile {
  std::vector<uint8_t> bytes;
  haddr_t image_addr = HADDR_UNDEF;
  hsize_t image_len = 0;
  haddr_t scratch_addr = HADDR_UNDEF;
  hsize_t scratch_len = 4096;
};

SampleImageFile CreatePopulatedCacheImageFile(bool with_dense_attrs,
                                              int extra_groups = 0) {
  SampleImageFile out;
  auto capture = std::make_shared<CaptureState>();
  ScopedPlist fapl = MakeCoreCacheImageFapl(capture, /*generate_image=*/true);
  if (!fapl.valid()) {
    return out;
  }
  ScopedFile file(H5Fcreate("h5cimage_sample.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  if (!file.valid()) {
    return out;
  }

  ScopedGroup g1(
      H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(
      H5Gcreate2(file.get(), "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));

  // Create a 4096-byte contiguous zero-initialized dataset so tests also have a
  // known in-EOA offset (`scratch_addr`) without perturbing HDF5 metadata.
  std::vector<uint8_t> zeros(out.scratch_len, 0);
  const hsize_t dim = zeros.size();
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ScopedDataset dset(H5Dcreate2(file.get(), "/g1/blob", H5T_NATIVE_UINT8,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  if (H5Dwrite(dset.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
               zeros.data()) < 0) {
    return out;
  }
  // Ensure storage is allocated so H5Dget_offset returns a real offset < EOA.
  if (H5Fflush(file.get(), H5F_SCOPE_LOCAL) < 0) {
    return out;
  }
  out.scratch_addr = H5Dget_offset(dset.get());

  // Re-dirty group metadata after flush so the cache image includes dirty and
  // clean entries across multiple objects. When `extra_groups > 0`, chain them
  // linearly so no group exceeds compact link storage (no dense B-tree/FD).
  ScopedGroup g3(
      H5Gcreate2(file.get(), "/g3", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  if (extra_groups > 0) {
    std::string chain_path = "/eg_chain";
    ScopedGroup cur(H5Gcreate2(file.get(), chain_path.c_str(), H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    for (int i = 0; i < extra_groups && cur.valid(); ++i) {
      cur.reset(H5Gcreate2(cur.get(), "next", H5P_DEFAULT, H5P_DEFAULT,
                           H5P_DEFAULT));
    }
  }
  if (with_dense_attrs) {
    const hsize_t a_dim = 4;
    ScopedSpace aspace(H5Screate_simple(1, &a_dim, nullptr));
    const std::array<int32_t, 4> aval = {1, 2, 3, 4};
    // >8 attributes forces dense attribute storage (fractal heap + v2 B-tree)
    // and real HDF5 flush dependencies in the metadata cache.
    for (int i = 0; i < 12; ++i) {
      const std::string aname = "attr_" + std::to_string(i);
      ScopedAttr attr(H5Acreate2(g1.get(), aname.c_str(), H5T_NATIVE_INT32,
                                 aspace.get(), H5P_DEFAULT, H5P_DEFAULT));
      H5Awrite(attr.get(), H5T_NATIVE_INT32, aval.data());
    }
  }

  g3.reset();
  dset.reset();
  space.reset();
  g2.reset();
  g1.reset();
  file.reset();

  out.bytes = std::move(capture->closed_image);

  // Inspect R/O to record exact image_addr and image_len.
  if (!out.bytes.empty()) {
    ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &out.bytes);
    ScopedFile ro_file(H5Fopen("h5cimage_probe_ro.h5", H5F_ACC_RDONLY,
                               ro_fapl.get()));
    if (ro_file.valid()) {
      H5Fget_mdc_image_info(ro_file.get(), &out.image_addr, &out.image_len);
    }
  }
  return out;
}

TEST(H5CimageTest, ValidateCacheImageConfigRejectsInvalidAndAcceptsValid) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  EXPECT_LT(H5C_validate_cache_image_config(nullptr), 0);

  H5C_cache_image_ctl_t bad_ver = H5C__DEFAULT_CACHE_IMAGE_CTL;
  bad_ver.version = H5C__CURR_CACHE_IMAGE_CTL_VER + 1;
  EXPECT_LT(H5C_validate_cache_image_config(&bad_ver), 0);

  H5C_cache_image_ctl_t bad_resize = H5C__DEFAULT_CACHE_IMAGE_CTL;
  bad_resize.save_resize_status = true;
  EXPECT_LT(H5C_validate_cache_image_config(&bad_resize), 0);

  H5C_cache_image_ctl_t bad_ageout = H5C__DEFAULT_CACHE_IMAGE_CTL;
  bad_ageout.entry_ageout = 0;
  EXPECT_LT(H5C_validate_cache_image_config(&bad_ageout), 0);

  H5C_cache_image_ctl_t bad_flags = H5C__DEFAULT_CACHE_IMAGE_CTL;
  bad_flags.flags = H5C_CI__ALL_FLAGS | 0x100U;
  EXPECT_LT(H5C_validate_cache_image_config(&bad_flags), 0);

  H5C_cache_image_ctl_t valid_ctl = H5C__DEFAULT_CACHE_IMAGE_CTL;
  valid_ctl.generate_image = true;
  valid_ctl.flags = 0;
  EXPECT_GE(H5C_validate_cache_image_config(&valid_ctl), 0);

  // Public FAPL wrappers forward validation and store/read config.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  H5AC_cache_image_config_t pub_cfg = H5AC__DEFAULT_CACHE_IMAGE_CONFIG;
  pub_cfg.version = 99;
  EXPECT_LT(H5Pset_mdc_image_config(fapl.get(), &pub_cfg), 0);

  pub_cfg = H5AC__DEFAULT_CACHE_IMAGE_CONFIG;
  pub_cfg.save_resize_status = true;
  EXPECT_LT(H5Pset_mdc_image_config(fapl.get(), &pub_cfg), 0);

  pub_cfg = H5AC__DEFAULT_CACHE_IMAGE_CONFIG;
  pub_cfg.entry_ageout = 5;
  EXPECT_LT(H5Pset_mdc_image_config(fapl.get(), &pub_cfg), 0);

  pub_cfg = H5AC__DEFAULT_CACHE_IMAGE_CONFIG;
  pub_cfg.generate_image = true;
  EXPECT_GE(H5Pset_mdc_image_config(fapl.get(), &pub_cfg), 0);

  H5AC_cache_image_config_t got_cfg{};
  got_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  ASSERT_GE(H5Pget_mdc_image_config(fapl.get(), &got_cfg), 0);
  EXPECT_TRUE(got_cfg.generate_image);
  EXPECT_FALSE(got_cfg.save_resize_status);
  EXPECT_EQ(got_cfg.entry_ageout, H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE);
}

TEST(H5CimageTest, SetAndGetCacheImageConfigAndPendingStatusHelpers) {
  ScopedFile rw_file = CreateInMemoryFile("h5cimage_cfg_rw.h5");
  ASSERT_TRUE(rw_file.valid());
  H5F_t* f = GetH5File(rw_file.get());
  ASSERT_NE(f, nullptr);
  H5C_t* cache_ptr = f->shared->cache;
  ASSERT_NE(cache_ptr, nullptr);

  {
    ScopedErrorSilence silence;
    H5C_cache_image_ctl_t ctl = H5C__DEFAULT_CACHE_IMAGE_CTL;
    EXPECT_LT(H5C__get_cache_image_config(nullptr, &ctl), 0);
    EXPECT_LT(H5C__get_cache_image_config(cache_ptr, nullptr), 0);
    EXPECT_LT(H5C_set_cache_image_config(f, nullptr, &ctl), 0);

    ctl.version = 1234;
    EXPECT_LT(H5C_set_cache_image_config(f, cache_ptr, &ctl), 0);
  }

  // R/W file applies valid config and preserves default/status queries.
  H5C_cache_image_ctl_t enable_ctl = H5C__DEFAULT_CACHE_IMAGE_CTL;
  enable_ctl.generate_image = true;
  ASSERT_GE(H5C_set_cache_image_config(f, cache_ptr, &enable_ctl), 0);

  H5C_cache_image_ctl_t got{};
  ASSERT_GE(H5C__get_cache_image_config(cache_ptr, &got), 0);
  EXPECT_TRUE(got.generate_image);

  bool load_ci = true;
  bool write_ci = false;
  ASSERT_GE(H5C_cache_image_status(f, &load_ci, &write_ci), 0);
  EXPECT_FALSE(load_ci);
  EXPECT_TRUE(write_ci);
  EXPECT_FALSE(H5C_cache_image_pending(cache_ptr));

  // Pending / loaded combinations.
  ASSERT_GE(H5C_load_cache_image_on_next_protect(f, 0x100, 64, false), 0);
  EXPECT_TRUE(H5C_cache_image_pending(cache_ptr));
  ASSERT_GE(H5C_cache_image_status(f, &load_ci, &write_ci), 0);
  EXPECT_TRUE(load_ci);
  EXPECT_TRUE(write_ci);

  cache_ptr->image_loaded = true;
  EXPECT_FALSE(H5C_cache_image_pending(cache_ptr));
  cache_ptr->load_image = false;
  EXPECT_FALSE(H5C_cache_image_pending(cache_ptr));
  ASSERT_GE(H5C_cache_image_status(f, &load_ci, &write_ci), 0);
  EXPECT_TRUE(load_ci);

  // Restore clean state before closing rw_file.
  cache_ptr->image_loaded = false;
  cache_ptr->image_addr = HADDR_UNDEF;
  cache_ptr->image_len = 0;
  H5C_cache_image_ctl_t disable_ctl = H5C__DEFAULT_CACHE_IMAGE_CTL;
  ASSERT_GE(H5C_set_cache_image_config(f, cache_ptr, &disable_ctl), 0);

  // R/O file silently resets generate_image to default (false).
  SampleImageFile empty_sample;
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/false);
    ASSERT_TRUE(fapl.valid());
    ScopedFile tmp(H5Fcreate("h5cimage_cfg_ro_src.h5", H5F_ACC_TRUNC,
                             H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(tmp.valid());
    tmp.reset();
    empty_sample.bytes = std::move(cap->closed_image);
  }

  // Attempting H5F_ACC_RDONLY with generate_image=true silently forces false.
  ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
      nullptr, /*generate_image=*/true, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
      &empty_sample.bytes);
  ASSERT_TRUE(ro_fapl.valid());
  ScopedFile ro_file(
      H5Fopen("h5cimage_cfg_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  H5F_t* ro_f = GetH5File(ro_file.get());
  ASSERT_NE(ro_f, nullptr);
  ASSERT_GE(H5C__get_cache_image_config(ro_f->shared->cache, &got), 0);
  EXPECT_FALSE(got.generate_image);
}

TEST(H5CimageTest, SwmrRejectsCacheImageOnOpenAndStartSwmrWrite) {
  SampleImageFile sample =
      CreatePopulatedCacheImageFile(/*with_dense_attrs=*/false);
  ASSERT_FALSE(sample.bytes.empty());
  ASSERT_NE(sample.image_addr, HADDR_UNDEF);

  ScopedErrorSilence silence;

  // 1. Opening an existing cache-image file with SWMR_WRITE fails.
  ScopedPlist open_fapl = MakeCoreCacheImageFapl(
      nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
      &sample.bytes);
  ASSERT_TRUE(open_fapl.valid());
  EXPECT_EQ(H5Fopen("h5cimage_swmr_open.h5", H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                    open_fapl.get()),
            H5I_INVALID_HID);

  // 2. Enabling generate_image and calling H5Fstart_swmr_write fails.
  ScopedPlist start_fapl = MakeCoreCacheImageFapl(
      nullptr, /*generate_image=*/true, H5F_LIBVER_V110, H5F_LIBVER_LATEST);
  ASSERT_TRUE(start_fapl.valid());
  ScopedFile file(H5Fcreate("h5cimage_swmr_start.h5", H5F_ACC_TRUNC,
                            H5P_DEFAULT, start_fapl.get()));
  ASSERT_TRUE(file.valid());
  EXPECT_LT(H5Fstart_swmr_write(file.get()), 0);
}

TEST(H5CimageTest, EmptyCacheAndOlderSuperblockCancelCacheImageGeneration) {
  // 1. Superblock version < 2 (LIBVER_EARLIEST..V18) cancels image generation.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(
        cap, /*generate_image=*/true, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_old_sb.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    g.reset();
    f.reset();

    ScopedPlist ro_fapl =
        MakeCoreCacheImageFapl(nullptr, false, H5F_LIBVER_EARLIEST,
                               H5F_LIBVER_V18, &cap->closed_image);
    ScopedFile ro(
        H5Fopen("h5cimage_old_sb_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro.valid());
    haddr_t addr = 0;
    hsize_t len = 123;
    ASSERT_GE(H5Fget_mdc_image_info(ro.get(), &addr, &len), 0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(len, 0U);
  }

  // 2. Superblock v2 with high_bound < V110 cancels image generation.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(
        cap, /*generate_image=*/true, H5F_LIBVER_V18, H5F_LIBVER_V18);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_v18.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    g.reset();
    f.reset();

    ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
        nullptr, false, H5F_LIBVER_V18, H5F_LIBVER_V18, &cap->closed_image);
    ScopedFile ro(
        H5Fopen("h5cimage_v18_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro.valid());
    haddr_t addr = 0;
    hsize_t len = 123;
    ASSERT_GE(H5Fget_mdc_image_info(ro.get(), &addr, &len), 0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(len, 0U);
  }

  // 3. Flush and evict all non-pinned user/FS metadata entries before close so
  //    only ring > H5C_MAX_RING_IN_IMAGE entries (superblock/ext) remain,
  //    hitting `num_entries_in_image == 0` cancellation and deleting the
  //    provisional MDCI superblock extension message when GENERATE_BLK is set.
  for (bool keep_blk_flag : {true, false}) {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_empty.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* hf = GetH5File(f.get());
      ASSERT_NE(hf, nullptr);
      ASSERT_GE(H5C_evict(hf), 0);
      if (!keep_blk_flag) {
        hf->shared->cache->image_ctl.flags &= ~H5C_CI__GEN_MDC_IMAGE_BLK;
      }
    }
    f.reset();
    ASSERT_FALSE(cap->closed_image.empty());

    if (keep_blk_flag) {
      ScopedPlist ro_fapl =
          MakeCoreCacheImageFapl(nullptr, false, H5F_LIBVER_V110,
                                 H5F_LIBVER_LATEST, &cap->closed_image);
      ScopedFile ro(
          H5Fopen("h5cimage_empty_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
      ASSERT_TRUE(ro.valid());
      haddr_t addr = 0;
      hsize_t len = 123;
      ASSERT_GE(H5Fget_mdc_image_info(ro.get(), &addr, &len), 0);
      EXPECT_EQ(addr, HADDR_UNDEF);
      EXPECT_EQ(len, 0U);
    }
  }
}

TEST(H5CimageTest, RoundTripPopulatedFileRoAndRwPreservesAndDeletesImage) {
  SampleImageFile sample = CreatePopulatedCacheImageFile(
      /*with_dense_attrs=*/false, /*extra_groups=*/12);
  ASSERT_FALSE(sample.bytes.empty());
  EXPECT_NE(sample.image_addr, HADDR_UNDEF);
  EXPECT_GT(sample.image_len, 0U);

  // 1. Reopen R/O and open objects: verify image stays pending until protect,
  //    reconstructs dirty entries as clean + prefetched_dirty, and retains
  //    the on-disk MDCI message on close.
  {
    ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(ro_fapl.valid());
    ScopedFile ro_file(
        H5Fopen("h5cimage_rt_ro1.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro_file.valid());
    H5F_t* ro_f = GetH5File(ro_file.get());
    ASSERT_NE(ro_f, nullptr);
    EXPECT_TRUE(H5C_cache_image_pending(ro_f->shared->cache));

    haddr_t addr = HADDR_UNDEF;
    hsize_t len = 0;
    ASSERT_GE(H5Fget_mdc_image_info(ro_file.get(), &addr, &len), 0);
    EXPECT_EQ(addr, sample.image_addr);
    EXPECT_EQ(len, sample.image_len);

    // Protect user metadata to force H5C__load_cache_image().
    ScopedGroup g1(H5Gopen2(ro_file.get(), "/g1", H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());
    EXPECT_FALSE(H5C_cache_image_pending(ro_f->shared->cache));
    EXPECT_TRUE(ro_f->shared->cache->image_loaded);
    EXPECT_FALSE(ro_f->shared->cache->delete_image);
  }

  // 2. Reopen R/O and immediately close without touching any group or dataset
  //    to exercise `if (cache_ptr->load_image)` in H5C__prep_image_for_file_close.
  {
    ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(ro_fapl.valid());
    ScopedFile ro_file(
        H5Fopen("h5cimage_rt_ro2.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro_file.valid());
    H5F_t* ro_f = GetH5File(ro_file.get());
    ASSERT_NE(ro_f, nullptr);
    EXPECT_TRUE(H5C_cache_image_pending(ro_f->shared->cache));
    ro_file.reset();
  }

  // 3. Reopen R/W with generate_image=false: loading deletes MDCI message.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist rw_fapl = MakeCoreCacheImageFapl(
        cap, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(rw_fapl.valid());
    ScopedFile rw_file(
        H5Fopen("h5cimage_rt_rw1.h5", H5F_ACC_RDWR, rw_fapl.get()));
    ASSERT_TRUE(rw_file.valid());
    ScopedGroup g1(H5Gopen2(rw_file.get(), "/g1", H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());

    haddr_t addr = 0;
    hsize_t len = 99;
    ASSERT_GE(H5Fget_mdc_image_info(rw_file.get(), &addr, &len), 0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(len, 0U);
    g1.reset();
    rw_file.reset();

    ScopedPlist probe_fapl = MakeCoreCacheImageFapl(
        nullptr, false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &cap->closed_image);
    ScopedFile probe(
        H5Fopen("h5cimage_rt_rw1_chk.h5", H5F_ACC_RDONLY, probe_fapl.get()));
    ASSERT_TRUE(probe.valid());
    ASSERT_GE(H5Fget_mdc_image_info(probe.get(), &addr, &len), 0);
    EXPECT_EQ(addr, HADDR_UNDEF);
    EXPECT_EQ(len, 0U);
  }

  // 4. Reopen R/W with generate_image=true: unaccessed prefetched entries in
  //    the cache are carried forward into the new image with age incremented
  //    (lines 2074-2081), including age clamped at ENTRY_AGEOUT__MAX.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist rw_fapl = MakeCoreCacheImageFapl(
        cap, /*generate_image=*/true, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(rw_fapl.valid());
    ScopedFile rw_file(
        H5Fopen("h5cimage_rt_rw2.h5", H5F_ACC_RDWR, rw_fapl.get()));
    ASSERT_TRUE(rw_file.valid());
    ScopedGroup g1(H5Gopen2(rw_file.get(), "/g1", H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());

    H5F_t* rw_f = GetH5File(rw_file.get());
    ASSERT_NE(rw_f, nullptr);
    int saturated = 0;
    for (H5C_cache_entry_t* e = rw_f->shared->cache->il_head; e != nullptr;
         e = e->il_next) {
      if (e->type == H5AC_PREFETCHED_ENTRY) {
        if (saturated == 0) {
          // Exercise age >= H5AC__CACHE_IMAGE__ENTRY_AGEOUT__MAX clamp branch.
          e->age = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__MAX;
        }
        ++saturated;
      }
    }
    EXPECT_GT(saturated, 0);
    g1.reset();
    rw_file.reset();
    EXPECT_FALSE(cap->closed_image.empty());
  }

  // 5. Reopen R/W with a tiny max_cache_size (min_size == max_size == 1024)
  //    so loaded prefetched entries cause cache_ptr->index_size >= max_cache_size
  //    in H5C__reconstruct_cache_contents (triggering H5C__make_space_in_cache).
  {
    ScopedPlist tiny_fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(tiny_fapl.valid());
    H5AC_cache_config_t tiny_cfg{};
    tiny_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
    ASSERT_GE(H5Pget_mdc_config(tiny_fapl.get(), &tiny_cfg), 0);
    tiny_cfg.set_initial_size = true;
    tiny_cfg.min_size = H5C__MIN_MAX_CACHE_SIZE;
    tiny_cfg.initial_size = H5C__MIN_MAX_CACHE_SIZE;
    tiny_cfg.max_size = H5C__MIN_MAX_CACHE_SIZE;
    tiny_cfg.incr_mode = H5C_incr__off;
    tiny_cfg.flash_incr_mode = H5C_flash_incr__off;
    tiny_cfg.decr_mode = H5C_decr__off;
    tiny_cfg.apply_empty_reserve = false;
    ASSERT_GE(H5Pset_mdc_config(tiny_fapl.get(), &tiny_cfg), 0);

    ScopedFile tiny_file(
        H5Fopen("h5cimage_rt_tiny.h5", H5F_ACC_RDWR, tiny_fapl.get()));
    ASSERT_TRUE(tiny_file.valid());
    ScopedGroup g1(H5Gopen2(tiny_file.get(), "/g1", H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());
    H5F_t* tf = GetH5File(tiny_file.get());
    ASSERT_NE(tf, nullptr);
    EXPECT_TRUE(tf->shared->cache->image_loaded);
  }
}

TEST(H5CimageTest, EpochMarkersAndDenseFlushDependenciesDuringClose) {
  // 1. Dense attributes create flush dependencies (`compute_fd_heights` &
  //    height-sorted `H5C__image_entry_cmp`) across fractal heap / v2 B-tree
  //    entries; epoch age-out config also places H5AC_EPOCH_MARKER entries in
  //    LRU (`scan_entries` line 2372).
  auto cap = std::make_shared<CaptureState>();
  ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/true);
  ASSERT_TRUE(fapl.valid());
  ScopedFile file(
      H5Fcreate("h5cimage_fd_epoch.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                fapl.get()));
  ASSERT_TRUE(file.valid());

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);
  cfg.set_initial_size = true;
  cfg.min_size = 4096;
  cfg.initial_size = 64 * 1024;
  cfg.max_size = 64 * 1024;
  cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  cfg.incr_mode = H5C_incr__off;
  cfg.flash_incr_mode = H5C_flash_incr__off;
  cfg.decr_mode = H5C_decr__age_out;
  cfg.epochs_before_eviction = 3;
  cfg.apply_empty_reserve = false;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  // Create groups and dense attributes, then churn cache accesses > 100
  // times so epoch markers are active in the LRU when the cache image closes.
  ScopedGroup target(
      H5Gcreate2(file.get(), "/dense_grp", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(target.valid());
  const hsize_t adim = 2;
  ScopedSpace aspace(H5Screate_simple(1, &adim, nullptr));
  const std::array<int32_t, 2> aval = {7, 11};
  for (int i = 0; i < 12; ++i) {
    const std::string aname = "a_" + std::to_string(i);
    ScopedAttr a(H5Acreate2(target.get(), aname.c_str(), H5T_NATIVE_INT32,
                            aspace.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, aval.data()), 0);
  }
  for (int i = 0; i < 60; ++i) {
    const std::string gname = "/g_" + std::to_string(i);
    ScopedGroup g(H5Gcreate2(file.get(), gname.c_str(), H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }
  for (int i = 0; i < 60; ++i) {
    const std::string gname = "/g_" + std::to_string(i);
    ScopedGroup g(H5Gopen2(file.get(), gname.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  }

  H5F_t* hf = GetH5File(file.get());
  ASSERT_NE(hf, nullptr);
  EXPECT_GT(hf->shared->cache->epoch_markers_active, 0);

  aspace.reset();
  target.reset();
  file.reset();
  ASSERT_FALSE(cap->closed_image.empty());

  // 2. Reopen R/O to check image creation and document `-DNDEBUG` reload
  //    behavior when FD parents exist in the cache image:
  //    In H5C__reconstruct_cache_entry(), `is_fd_parent` is only set inside
  //    `#ifndef NDEBUG`, so under Google3 `-DNDEBUG`, loading an image entry
  //    with `fd_child_count > 0` hits `"non-parent entry has children"` and
  //    exercises full `reconstruct_cache_contents` error cleanup on close/open.
  ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
      nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
      &cap->closed_image);
  ASSERT_TRUE(ro_fapl.valid());
  ScopedFile ro(
      H5Fopen("h5cimage_fd_epoch_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro.valid());
  haddr_t addr = HADDR_UNDEF;
  hsize_t len = 0;
  ASSERT_GE(H5Fget_mdc_image_info(ro.get(), &addr, &len), 0);
  EXPECT_NE(addr, HADDR_UNDEF);
  EXPECT_GT(len, 0U);

  ScopedErrorSilence silence;
  ScopedGroup open_dense(H5Gopen2(ro.get(), "/dense_grp", H5P_DEFAULT));
  // H5Gopen2 fails to load the dense-FD cache image under `-DNDEBUG` because
  // `is_fd_parent` is guarded by `#ifndef NDEBUG` in H5C__reconstruct_cache_entry.
  EXPECT_FALSE(open_dense.valid());
}

TEST(H5CimageTest, MalformedImageBlocksAndHeaderTruncationsFailCleanly) {
  SampleImageFile sample =
      CreatePopulatedCacheImageFile(/*with_dense_attrs=*/false);
  ASSERT_FALSE(sample.bytes.empty());
  ASSERT_NE(sample.image_addr, HADDR_UNDEF);
  ASSERT_GT(sample.image_len, 18U);
  ASSERT_NE(sample.scratch_addr, HADDR_UNDEF);

  ScopedErrorSilence silence;

  // Helper calling internal H5C__load_cache_image directly against an in-file
  // synthetic image buffer written at sample.scratch_addr.
  auto run_direct_load = [&](const std::vector<uint8_t>& block, size_t load_len,
                             bool rw) -> herr_t {
    std::vector<uint8_t> patched = sample.bytes;
    if (sample.scratch_addr + block.size() > patched.size()) {
      return -1;
    }
    std::memcpy(patched.data() + sample.scratch_addr, block.data(),
                block.size());
    ScopedPlist fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &patched);
    if (!fapl.valid()) return -1;
    ScopedFile f(H5Fopen("h5cimage_direct_load.h5",
                         rw ? H5F_ACC_RDWR : H5F_ACC_RDONLY, fapl.get()));
    if (!f.valid()) return -1;
    H5F_t* hf = GetH5File(f.get());
    if (hf == nullptr) return -1;

    ScopedApiContext ctx;
    if (!ctx.valid()) return -1;
    // Consume/cancel any real pending image before substituting scratch block.
    hf->shared->cache->load_image = false;
    hf->shared->cache->image_addr = HADDR_UNDEF;
    hf->shared->cache->image_len = 0;

    if (H5C_load_cache_image_on_next_protect(hf, sample.scratch_addr, load_len,
                                             rw) < 0) {
      return -1;
    }
    hf->shared->cache->load_image = false;
    const herr_t rc = H5C__load_cache_image(hf);
    hf->shared->cache->image_addr = HADDR_UNDEF;
    hf->shared->cache->image_len = 0;
    return rc;
  };

  // 1. Header buffer overflow checks in H5C__decode_cache_image_header().
  //    Since H5C__load_cache_image passes `image_len + 1` as buf_size, effective
  //    available bytes == image_len + 1. Note image_len=14 has a 14-byte header
  //    prefix with encoded data_len == 14 so length check passes before
  //    overflowing on the trailing 4-byte entry count.
  const std::vector<uint8_t> trunc_header =
      EncodeSynthImageBlock({SynthEntrySpec{.addr = sample.scratch_addr}},
                            "MDCI", 0, 0, /*override_data_len=*/14);
  for (size_t bad_len : {size_t{2}, size_t{6}, size_t{14}}) {
    EXPECT_LT(run_direct_load(trunc_header, bad_len, false), 0)
        << "bad_len=" << bad_len;
  }

  // 2. Header semantic failures (valid size, corrupted header fields) tested via
  //    public H5Fopen -> H5Gopen2 (and H5Fclose immediate load failure).
  auto run_in_place_corrupt = [&](const std::vector<uint8_t>& block,
                                  bool close_without_protect) -> herr_t {
    if (block.size() > sample.image_len ||
        sample.image_addr + sample.image_len > sample.bytes.size()) {
      return -1;
    }
    std::vector<uint8_t> patched = sample.bytes;
    std::memcpy(patched.data() + sample.image_addr, block.data(), block.size());
    ScopedPlist fapl = MakeCoreCacheImageFapl(
        nullptr, false, H5F_LIBVER_V110, H5F_LIBVER_LATEST, &patched);
    if (!fapl.valid()) return -1;
    ScopedFile f(
        H5Fopen("h5cimage_corrupt.h5", H5F_ACC_RDONLY, fapl.get()));
    if (!f.valid()) return -1;
    if (close_without_protect) {
      return H5Fclose(f.release());
    }
    ScopedGroup g(H5Gopen2(f.get(), "/g1", H5P_DEFAULT));
    return g.valid() ? 0 : -1;
  };

  {
    std::vector<uint8_t> blk(sample.image_len, 0);
    // Bad signature.
    auto hdr = EncodeSynthImageBlock(
        {}, "BAD!", 0, 0, sample.image_len, 1);
    std::copy(hdr.begin(), hdr.begin() + 18, blk.begin());
    EXPECT_LT(run_in_place_corrupt(blk, false), 0);

    // Immediate close on corrupt image exercises lines 852-856 error path.
    EXPECT_LT(run_in_place_corrupt(blk, true), 0);

    // Bad version.
    hdr = EncodeSynthImageBlock({}, "MDCI", 1, 0, sample.image_len, 1);
    std::copy(hdr.begin(), hdr.begin() + 18, blk.begin());
    EXPECT_LT(run_in_place_corrupt(blk, false), 0);

    // Unsupported HAVE_RESIZE_STATUS flag.
    hdr = EncodeSynthImageBlock({}, "MDCI", 0, 0x01, sample.image_len, 1);
    std::copy(hdr.begin(), hdr.begin() + 18, blk.begin());
    EXPECT_LT(run_in_place_corrupt(blk, false), 0);

    // Mismatched image_data_len.
    hdr = EncodeSynthImageBlock({}, "MDCI", 0, 0, sample.image_len + 1, 1);
    std::copy(hdr.begin(), hdr.begin() + 18, blk.begin());
    EXPECT_LT(run_in_place_corrupt(blk, false), 0);

    // Zero num_entries_in_image.
    hdr = EncodeSynthImageBlock({}, "MDCI", 0, 0, sample.image_len, 0);
    std::copy(hdr.begin(), hdr.begin() + 18, blk.begin());
    EXPECT_LT(run_in_place_corrupt(blk, false), 0);
  }

  // 3. Entry semantic validation failures & reconstruction error paths.
  const haddr_t a0 = sample.scratch_addr;
  const haddr_t a1 = sample.scratch_addr + 64;
  const haddr_t a2 = sample.scratch_addr + 128;

  struct EntryFailCase {
    std::string label;
    std::vector<SynthEntrySpec> entries;
    bool rw = false;
  };
  const std::vector<EntryFailCase> cases = {
      {"type_id_oob", {SynthEntrySpec{.type_id = H5AC_NTYPES, .addr = a0}}},
      {"ring_oob", {SynthEntrySpec{.ring = H5C_RING_NTYPES, .addr = a0}}},
      {"age_oob",
       {SynthEntrySpec{.age = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__MAX + 1,
                       .addr = a0}}},
      {"dirty_child_gt_child_rw",
       {SynthEntrySpec{.fd_child_count = 0,
                       .fd_dirty_child_count = 1,
                       .addr = a0}},
       /*rw=*/true},
      {"addr_undef", {SynthEntrySpec{.addr = HADDR_UNDEF}}},
      {"addr_past_eoa",
       {SynthEntrySpec{.addr = static_cast<uint64_t>(sample.bytes.size()) + 16}}},
      {"zero_size", {SynthEntrySpec{.addr = a0, .size = 0}}},
      {"fd_parent_undef",
       {SynthEntrySpec{.flags = 0x08,
                       .fd_parent_count = 1,
                       .addr = a0,
                       .fd_parent_addrs = {HADDR_UNDEF}}}},
      {"duplicate_addr",
       {SynthEntrySpec{.addr = a0}, SynthEntrySpec{.addr = a0}}},
      {"duplicate_addr_with_fd_parent",
       {SynthEntrySpec{.addr = a0},
        SynthEntrySpec{.flags = 0x08,
                       .fd_parent_count = 1,
                       .addr = a0,
                       .fd_parent_addrs = {a1}}}},
      {"missing_fd_parent",
       {SynthEntrySpec{.addr = a0},
        SynthEntrySpec{.flags = 0x08,
                       .fd_parent_count = 1,
                       .addr = a1,
                       .fd_parent_addrs = {a2}}}},
  };
  for (const auto& tc : cases) {
    const std::vector<uint8_t> block = EncodeSynthImageBlock(tc.entries);
    EXPECT_LT(run_direct_load(block, block.size(), tc.rw), 0) << tc.label;
  }

  // Entry whose decoded `size` exceeds remaining input buffer triggers overflow
  // cleanup freeing both `image_ptr` and `fd_parent_addrs`.
  {
    std::vector<uint8_t> overflow_block = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x08,
                       .fd_parent_count = 1,
                       .addr = a0,
                       .size = 16,
                       .fd_parent_addrs = {a0}},
    });
    // Header (18) + entry prefix up to `size` (14 + 8) = offset 40..47.
    constexpr size_t kEntrySizeOffset = 18 + 14 + 8;
    const uint64_t oversized = 256;
    for (size_t i = 0; i < 8; ++i) {
      overflow_block[kEntrySizeOffset + i] =
          static_cast<uint8_t>((oversized >> (i * 8)) & 0xffU);
    }
    EXPECT_LT(run_direct_load(overflow_block, overflow_block.size(), false), 0);
  }

  // 4. Valid 2-entry parent->child FD restoration succeeds and cleans up cleanly
  //    on R/O close, while an R/W direct load also deletes the real MDCI msg.
  {
    const std::vector<uint8_t> valid_fd_block = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x02, .lru_rank = -1, .addr = a0, .size = 16},
        SynthEntrySpec{.flags = 0x01 | 0x02 | 0x08,
                       .fd_parent_count = 1,
                       .lru_rank = 1,
                       .addr = a1,
                       .size = 16,
                       .fd_parent_addrs = {a0}},
    });
    // Use lru_rank=-1 (not in LRU) on entry 0 cleanly without setting 0x02.
    std::vector<uint8_t> clean_fd = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x00, .lru_rank = -1, .addr = a0, .size = 16},
        SynthEntrySpec{.flags = 0x01 | 0x02 | 0x08,
                       .fd_parent_count = 1,
                       .lru_rank = 1,
                       .addr = a1,
                       .size = 16,
                       .fd_parent_addrs = {a0}},
    });
    (void)valid_fd_block;
    EXPECT_GE(run_direct_load(clean_fd, clean_fd.size(), /*rw=*/true), 0);
  }

  // 5. H5C__load_cache_image with HADDR_UNDEF and out-of-EOA read failure.
  {
    ScopedPlist fapl = MakeCoreCacheImageFapl(
        nullptr, false, H5F_LIBVER_V110, H5F_LIBVER_LATEST, &sample.bytes);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fopen("h5cimage_undef_addr.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Skip block load via HADDR_UNDEF while rw=true still removes MDCI msg.
    ASSERT_GE(H5C_load_cache_image_on_next_protect(hf, HADDR_UNDEF, 0, true),
              0);
    hf->shared->cache->load_image = false;
    EXPECT_GE(H5C__load_cache_image(hf), 0);
    // Second call with HADDR_UNDEF and rw=false also succeeds no-op.
    ASSERT_GE(H5C_load_cache_image_on_next_protect(hf, HADDR_UNDEF, 0, false),
              0);
    hf->shared->cache->load_image = false;
    EXPECT_GE(H5C__load_cache_image(hf), 0);

    // Out-of-EOA read fails H5C__read_cache_image -> frees image_buffer cleanly.
    ASSERT_GE(H5C_load_cache_image_on_next_protect(
                  hf, hf->shared->tmp_addr + 64, 64, false),
              0);
    hf->shared->cache->load_image = false;
    EXPECT_LT(H5C__load_cache_image(hf), 0);
    hf->shared->cache->image_addr = HADDR_UNDEF;
    hf->shared->cache->image_len = 0;
  }
}

TEST(H5CimageTest, SyntheticCacheEntriesCoverEncodeErrorsAndFdHeightGraph) {
  ScopedErrorSilence silence;
  ScopedFile file = CreateInMemoryFile("h5cimage_synth_encode.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);
  H5C_t* cache_ptr = f->shared->cache;
  ASSERT_NE(cache_ptr, nullptr);

  // Allocate in-EOA space up front for the write-failure subtest below.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  haddr_t waddr = HADDR_UNDEF;
  {
    ScopedApiContext alloc_ctx;
    ASSERT_TRUE(alloc_ctx.valid());
    waddr = H5FD_alloc(f->shared->lf, H5FD_MEM_SUPER, f, 256, nullptr, nullptr);
  }
  ASSERT_NE(waddr, HADDR_UNDEF);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // 1. Exercise H5C__encode_cache_image_entry() range errors and missing
  //    H5C_CI__GEN_MDC_IMAGE_BLK flag inside H5C__generate_cache_image().
  cache_ptr->close_warning_received = true;
  cache_ptr->image_ctl.generate_image = true;
  const uint32_t saved_index_len = cache_ptr->index_len;
  cache_ptr->index_len = 0;

  absl::Cleanup restore_cache_state = [&] {
    cache_ptr->image_buffer = H5MM_xfree(cache_ptr->image_buffer);
    if (cache_ptr->image_entries != nullptr) {
      for (uint32_t i = 0; i < cache_ptr->num_entries_in_image; ++i) {
        cache_ptr->image_entries[i].fd_parent_addrs =
            static_cast<haddr_t*>(H5MM_xfree(
                cache_ptr->image_entries[i].fd_parent_addrs));
        cache_ptr->image_entries[i].image_ptr =
            H5MM_xfree(cache_ptr->image_entries[i].image_ptr);
      }
      cache_ptr->image_entries = static_cast<H5C_image_entry_t*>(
          H5MM_xfree(cache_ptr->image_entries));
    }
    cache_ptr->num_entries_in_image = 0;
    cache_ptr->image_len = 0;
    cache_ptr->image_data_len = 0;
    cache_ptr->image_addr = HADDR_UNDEF;
    cache_ptr->index_len = saved_index_len;
    cache_ptr->image_ctl.generate_image = false;
    cache_ptr->image_ctl.save_resize_status = false;
    cache_ptr->image_ctl.flags = H5C_CI__ALL_FLAGS;
    cache_ptr->close_warning_received = false;
  };

  auto prepare_single_image_entry = [&](H5C_image_entry_t spec) {
    cache_ptr->image_buffer = H5MM_xfree(cache_ptr->image_buffer);
    if (cache_ptr->image_entries != nullptr) {
      for (uint32_t i = 0; i < cache_ptr->num_entries_in_image; ++i) {
        cache_ptr->image_entries[i].image_ptr =
            H5MM_xfree(cache_ptr->image_entries[i].image_ptr);
      }
      cache_ptr->image_entries = static_cast<H5C_image_entry_t*>(
          H5MM_xfree(cache_ptr->image_entries));
    }
    cache_ptr->num_entries_in_image = 1;
    cache_ptr->image_entries = static_cast<H5C_image_entry_t*>(
        H5MM_calloc(sizeof(H5C_image_entry_t) * 2));
    ASSERT_NE(cache_ptr->image_entries, nullptr);
    cache_ptr->image_entries[0] = spec;
    cache_ptr->image_entries[0].image_ptr = H5MM_calloc(spec.size);
    ASSERT_NE(cache_ptr->image_entries[0].image_ptr, nullptr);

    const size_t total =
        18 + (14 + H5F_SIZEOF_ADDR(f) + H5F_SIZEOF_SIZE(f)) + spec.size + 4;
    cache_ptr->image_len = total;
    cache_ptr->image_data_len = total;
    cache_ptr->image_addr = 0x2000;
  };

  H5C_image_entry_t base{};
  base.addr = 0x100;
  base.size = 16;
  base.ring = H5C_RING_USER;
  base.type_id = H5AC_OHDR_ID;
  base.lru_rank = 1;

  // Out-of-range type_id (< 0 and > 255).
  {
    H5C_image_entry_t bad = base;
    bad.type_id = -1;
    prepare_single_image_entry(bad);
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);

    bad.type_id = 256;
    prepare_single_image_entry(bad);
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);
  }

  // Out-of-range fd_child_count, fd_dirty_child_count, fd_parent_count.
  {
    H5C_image_entry_t bad = base;
    bad.fd_child_count = static_cast<uint64_t>(USHRT_MAX) + 1;
    prepare_single_image_entry(bad);
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);
  }
  {
    H5C_image_entry_t bad = base;
    bad.fd_dirty_child_count = static_cast<uint64_t>(USHRT_MAX) + 1;
    prepare_single_image_entry(bad);
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);
  }
  {
    H5C_image_entry_t bad = base;
    bad.fd_parent_count = static_cast<uint64_t>(USHRT_MAX) + 1;
    prepare_single_image_entry(bad);
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);
  }

  // Valid encode with H5C_CI__GEN_MDC_IMAGE_BLK cleared skips H5C__write_cache_image.
  cache_ptr->image_ctl.flags = H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDC_IMAGE_BLK;
  prepare_single_image_entry(base);
  EXPECT_GE(H5C__generate_cache_image(f, cache_ptr), 0);

  // Header resize-status bit branch inside H5C__encode_cache_image_header().
  cache_ptr->image_ctl.save_resize_status = true;
  prepare_single_image_entry(base);
  EXPECT_GE(H5C__generate_cache_image(f, cache_ptr), 0);
  cache_ptr->image_ctl.save_resize_status = false;

  // Enabling H5C_CI__GEN_MDC_IMAGE_BLK and failing VFD write covers
  // H5C__write_cache_image() and H5C__generate_cache_image() write errors.
  {
    ScopedApiContext io_ctx;
    ASSERT_TRUE(io_ctx.valid());
    ASSERT_GE(H5F__accum_reset(f->shared, /*flush=*/true, /*force=*/false), 0);
    const unsigned long saved_feat = f->shared->feature_flags;
    f->shared->feature_flags &= ~H5FD_FEAT_ACCUMULATE_METADATA;
    absl::Cleanup restore_feat = [&] { f->shared->feature_flags = saved_feat; };

    ScopedVfdHooks vfd(f);
    vfd.set_fail_write_on_call(1);
    cache_ptr->image_ctl.flags = H5C_CI__ALL_FLAGS;
    prepare_single_image_entry(base);
    cache_ptr->image_addr = waddr;
    EXPECT_LT(H5C__generate_cache_image(f, cache_ptr), 0);
  }
}

TEST(H5CimageTest, ReconstructOversizedAndCheckWritePermitted) {
  SampleImageFile sample =
      CreatePopulatedCacheImageFile(/*with_dense_attrs=*/false);
  ASSERT_FALSE(sample.bytes.empty());
  ASSERT_NE(sample.scratch_addr, HADDR_UNDEF);
  ASSERT_GE(sample.scratch_len, 512U);

  ScopedErrorSilence silence;

  // 1. Failing check_write_permitted callback during oversized reconstruction.
  {
    const std::vector<uint8_t> block = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x02,
                       .lru_rank = 1,
                       .addr = sample.scratch_addr,
                       .size = 16},
    });
    std::vector<uint8_t> patched = sample.bytes;
    const haddr_t block_addr = sample.scratch_addr + 256;
    ASSERT_LE(block_addr + block.size(), patched.size());
    std::memcpy(patched.data() + block_addr, block.data(), block.size());

    ScopedPlist fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &patched);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fopen("h5cimage_chk_wp.h5", H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    H5C_t* cache_ptr = hf->shared->cache;

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    const size_t saved_max = cache_ptr->max_cache_size;
    const H5C_write_permitted_func_t saved_cb = cache_ptr->check_write_permitted;
    absl::Cleanup restore_wp = [&] {
      cache_ptr->max_cache_size = saved_max;
      cache_ptr->check_write_permitted = saved_cb;
      cache_ptr->image_addr = HADDR_UNDEF;
      cache_ptr->image_len = 0;
    };

    cache_ptr->load_image = false;
    cache_ptr->image_addr = HADDR_UNDEF;
    cache_ptr->image_len = 0;
    ASSERT_GE(H5C_load_cache_image_on_next_protect(hf, block_addr, block.size(),
                                                   /*rw=*/false),
              0);
    cache_ptr->load_image = false;
    cache_ptr->max_cache_size = 1;
    cache_ptr->check_write_permitted = [](const H5F_t* /*f*/,
                                          bool* /*wp*/) -> herr_t { return -1; };
    EXPECT_LT(H5C__load_cache_image(hf), 0);
  }

  // 2. Oversized R/W reconstruction with write_permitted=true triggers eviction
  //    write of a reconstructed dirty entry and propagates write failure.
  {
    const std::vector<uint8_t> dirty_block = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x01 | 0x02,
                       .lru_rank = 1,
                       .addr = sample.scratch_addr,
                       .size = 16},
    });
    std::vector<uint8_t> patched = sample.bytes;
    const haddr_t block_addr = sample.scratch_addr + 256;
    ASSERT_LE(block_addr + dirty_block.size(), patched.size());
    std::memcpy(patched.data() + block_addr, dirty_block.data(),
                dirty_block.size());

    ScopedPlist fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &patched);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fopen("h5cimage_rw_msic.h5", H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    H5C_t* cache_ptr = hf->shared->cache;

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5F__accum_reset(hf->shared, /*flush=*/true, /*force=*/false), 0);
    const unsigned long saved_feat = hf->shared->feature_flags;
    const size_t saved_max = cache_ptr->max_cache_size;
    const bool saved_wp = cache_ptr->write_permitted;
    const H5C_write_permitted_func_t saved_cb = cache_ptr->check_write_permitted;
    absl::Cleanup restore_rw = [&] {
      hf->shared->feature_flags = saved_feat;
      cache_ptr->max_cache_size = saved_max;
      cache_ptr->write_permitted = saved_wp;
      cache_ptr->check_write_permitted = saved_cb;
      cache_ptr->delete_image = false;
      cache_ptr->image_addr = HADDR_UNDEF;
      cache_ptr->image_len = 0;
    };

    cache_ptr->load_image = false;
    cache_ptr->image_addr = HADDR_UNDEF;
    cache_ptr->image_len = 0;
    ASSERT_GE(H5C_load_cache_image_on_next_protect(
                  hf, block_addr, dirty_block.size(), /*rw=*/true),
              0);
    cache_ptr->load_image = false;
    cache_ptr->max_cache_size = 1;
    cache_ptr->write_permitted = true;
    cache_ptr->check_write_permitted = nullptr;

    hf->shared->feature_flags &= ~H5FD_FEAT_ACCUMULATE_METADATA;
    ScopedVfdHooks vfd(hf);
    vfd.set_fail_write_on_call(1);
    EXPECT_LT(H5C__load_cache_image(hf), 0);
  }
}

TEST(H5CimageTest, PrefetchedFlushDepRecloseAndFdHeights) {
  // 1. Reclosing a file that still holds reconstructed prefetched entries with
  //    preserved flush dependencies hits matching and mismatched parent-count
  //    branches in H5C__prep_for_file_close__scan_entries().
  for (bool add_extra_parent : {false, true}) {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_reclose_fd.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);

    haddr_t base_addr = HADDR_UNDEF;
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      base_addr =
          H5MF_alloc(hf, H5FD_MEM_OHDR, 512);
    }
    ASSERT_NE(base_addr, HADDR_UNDEF);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    const haddr_t a0 = base_addr;
    const haddr_t a1 = base_addr + 64;
    const haddr_t a2 = base_addr + 128;
    const haddr_t blk_addr = base_addr + 192;
    const std::vector<uint8_t> blk = EncodeSynthImageBlock({
        SynthEntrySpec{.flags = 0x00,
                       .fd_child_count = 0,
                       .fd_parent_count = 0,
                       .lru_rank = -1,
                       .addr = a0,
                       .size = 16},
        SynthEntrySpec{.flags = 0x08,
                       .fd_child_count = 0,
                       .fd_parent_count = 1,
                       .lru_rank = -1,
                       .addr = a1,
                       .size = 16,
                       .fd_parent_addrs = {a0}},
    });
    {
      ScopedApiContext ctx(a0);
      ASSERT_TRUE(ctx.valid());
      ASSERT_GE(H5F_block_write(hf, H5FD_MEM_DEFAULT, blk_addr, blk.size(),
                                blk.data()),
                0);
      ASSERT_GE(H5C_load_cache_image_on_next_protect(hf, blk_addr, blk.size(),
                                                     /*rw=*/false),
                0);
      hf->shared->cache->load_image = false;
      ASSERT_GE(H5C__load_cache_image(hf), 0);
      hf->shared->cache->image_addr = HADDR_UNDEF;
      hf->shared->cache->image_len = 0;
      hf->shared->cache->image_data_len = 0;

      if (add_extra_parent) {
        H5C_cache_entry_t* e1 = nullptr;
        for (H5C_cache_entry_t* cur = hf->shared->cache->il_head;
             cur != nullptr; cur = cur->il_next) {
          if (cur->addr == a1) {
            e1 = cur;
            break;
          }
        }
        ASSERT_NE(e1, nullptr);
        TestCacheEntry* e2 = AllocTestEntry();
        ASSERT_NE(e2, nullptr);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a2, e2,
                                   H5C__PIN_ENTRY_FLAG),
                  0);
        ASSERT_GE(H5C_create_flush_dependency(e2, e1), 0);
        ASSERT_GE(H5C_unpin_entry(e2), 0);
      }
    }

    f.reset();
    EXPECT_FALSE(cap->closed_image.empty());
  }

  // 2. Tree 1: clean child in RING_USER with clean parent in RING_SBE (outside
  //    image) prunes the external parent reference while keeping the child.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_tree1.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);

    haddr_t base_addr = HADDR_UNDEF;
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      base_addr = H5MF_alloc(hf, H5FD_MEM_OHDR, 256);
    }
    ASSERT_NE(base_addr, HADDR_UNDEF);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    const haddr_t a0 = base_addr;
    const haddr_t a1 = base_addr + 64;
    {
      ScopedApiContext ctx(a0);
      ASSERT_TRUE(ctx.valid());
      TestCacheEntry* p_sbe = AllocTestEntry();
      TestCacheEntry* c_user = AllocTestEntry();
      ASSERT_NE(p_sbe, nullptr);
      ASSERT_NE(c_user, nullptr);
      {
        ScopedRingSetter ring_sbe(H5C_RING_SBE);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a0, p_sbe,
                                   H5C__PIN_ENTRY_FLAG),
                  0);
      }
      {
        ScopedRingSetter ring_user(H5C_RING_USER);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a1, c_user,
                                   H5C__NO_FLAGS_SET),
                  0);
      }
      ASSERT_GE(H5C_create_flush_dependency(p_sbe, c_user), 0);
      ASSERT_GE(H5C_unpin_entry(p_sbe), 0);
      ASSERT_GE(H5C_flush_cache(hf, H5C__NO_FLAGS_SET), 0);
    }

    f.reset();
    EXPECT_FALSE(cap->closed_image.empty());
  }

  // 3. Tree 2: dirty child in RING_USER with both a dirty RING_SBE parent
  //    (excluded) and a dirty RING_USER parent (included) removes the child
  //    from the image and decrements fd_child_count / fd_dirty_child_count on
  //    its in-image RING_USER parent.
  {
    auto cap = std::make_shared<CaptureState>();
    ScopedPlist fapl = MakeCoreCacheImageFapl(cap, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_tree2.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);

    haddr_t base_addr = HADDR_UNDEF;
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      base_addr = H5MF_alloc(hf, H5FD_MEM_OHDR, 256);
    }
    ASSERT_NE(base_addr, HADDR_UNDEF);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    const haddr_t a0 = base_addr;
    const haddr_t a1 = base_addr + 64;
    const haddr_t a2 = base_addr + 128;
    {
      ScopedApiContext ctx(a0);
      ASSERT_TRUE(ctx.valid());
      TestCacheEntry* p_sbe_dirty = AllocTestEntry();
      TestCacheEntry* p_user_top = AllocTestEntry();
      TestCacheEntry* c_user_dirty = AllocTestEntry();
      ASSERT_NE(p_sbe_dirty, nullptr);
      ASSERT_NE(p_user_top, nullptr);
      ASSERT_NE(c_user_dirty, nullptr);
      {
        ScopedRingSetter ring_sbe(H5C_RING_SBE);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a0, p_sbe_dirty,
                                   H5C__PIN_ENTRY_FLAG),
                  0);
      }
      {
        ScopedRingSetter ring_user(H5C_RING_USER);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a1, p_user_top,
                                   H5C__PIN_ENTRY_FLAG),
                  0);
        ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a2, c_user_dirty,
                                   H5C__NO_FLAGS_SET),
                  0);
      }
      ASSERT_GE(H5C_create_flush_dependency(p_sbe_dirty, c_user_dirty), 0);
      ASSERT_GE(H5C_create_flush_dependency(p_user_top, c_user_dirty), 0);
      ASSERT_GE(H5C_unpin_entry(p_sbe_dirty), 0);
      ASSERT_GE(H5C_unpin_entry(p_user_top), 0);
    }

    f.reset();
    EXPECT_FALSE(cap->closed_image.empty());
  }

  // 4. Unpinned FD parent detected during scan_entries() returns failure.
  {
    ScopedErrorSilence silence;
    ScopedPlist fapl = MakeCoreCacheImageFapl(nullptr, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_unpinned_fdp.h5", H5F_ACC_TRUNC,
                           H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    H5C_t* cache_ptr = hf->shared->cache;

    haddr_t base_addr = HADDR_UNDEF;
    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      base_addr = H5MF_alloc(hf, H5FD_MEM_OHDR, 256);
    }
    ASSERT_NE(base_addr, HADDR_UNDEF);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    const haddr_t a0 = base_addr;
    const haddr_t a1 = base_addr + 64;
    ScopedApiContext ctx(a0);
    ASSERT_TRUE(ctx.valid());
    TestCacheEntry* p = AllocTestEntry();
    TestCacheEntry* c = AllocTestEntry();
    ASSERT_NE(p, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_GE(
        H5C_insert_entry(hf, TestEntryClass(), a0, p, H5C__PIN_ENTRY_FLAG), 0);
    ASSERT_GE(
        H5C_insert_entry(hf, TestEntryClass(), a1, c, H5C__NO_FLAGS_SET), 0);
    ASSERT_GE(H5C_create_flush_dependency(p, c), 0);
    ASSERT_GE(H5C_unpin_entry(p), 0);

    const bool saved_cwr = cache_ptr->close_warning_received;
    const unsigned saved_flags = cache_ptr->image_ctl.flags;
    cache_ptr->close_warning_received = true;
    cache_ptr->image_ctl.flags =
        H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDCI_SBE_MESG;
    p->hdr.is_pinned = false;
    bool img_gen = false;
    EXPECT_LT(H5C__prep_image_for_file_close(hf, &img_gen), 0);

    p->hdr.is_pinned = true;
    cache_ptr->close_warning_received = saved_cwr;
    cache_ptr->image_ctl.generate_image = false;
    cache_ptr->image_ctl.flags = saved_flags;
  }
}

TEST(H5CimageTest, VfdAndPrepImageErrorPaths) {
  SampleImageFile sample =
      CreatePopulatedCacheImageFile(/*with_dense_attrs=*/false);
  ASSERT_FALSE(sample.bytes.empty());
  ASSERT_NE(sample.image_addr, HADDR_UNDEF);

  ScopedErrorSilence silence;

  // 1. VFD read failure when loading a cache image covers H5C__read_cache_image()
  //    and H5C__load_cache_image() read-error cleanup.
  {
    ScopedPlist ro_fapl = MakeCoreCacheImageFapl(
        nullptr, /*generate_image=*/false, H5F_LIBVER_V110, H5F_LIBVER_LATEST,
        &sample.bytes);
    ASSERT_TRUE(ro_fapl.valid());
    ScopedFile ro(
        H5Fopen("h5cimage_read_fail.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro.valid());
    H5F_t* hf = GetH5File(ro.get());
    ASSERT_NE(hf, nullptr);

    ASSERT_GE(H5F__accum_reset(hf->shared, /*flush=*/true, /*force=*/false), 0);
    const unsigned long saved_feat = hf->shared->feature_flags;
    hf->shared->feature_flags &= ~H5FD_FEAT_ACCUMULATE_METADATA;
    absl::Cleanup restore_feat = [&] { hf->shared->feature_flags = saved_feat; };

    ScopedVfdHooks vfd(hf);
    vfd.set_fail_read_on_call(1);
    ScopedGroup g(H5Gopen2(ro.get(), "/g1", H5P_DEFAULT));
    EXPECT_FALSE(g.valid());
  }

  // 2. Deleting an MDCI superblock extension message when none exists covers
  //    H5C__load_cache_image() remove-message error.
  {
    ScopedPlist fapl = MakeCoreCacheImageFapl(nullptr, /*generate_image=*/false);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_no_mdci_del.h5", H5F_ACC_TRUNC,
                           H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5C_load_cache_image_on_next_protect(hf, HADDR_UNDEF, 0,
                                                   /*rw=*/true),
              0);
    hf->shared->cache->load_image = false;
    EXPECT_LT(H5C__load_cache_image(hf), 0);
    hf->shared->cache->delete_image = false;
  }

  // 3. Duplicate creation of the MDCI superblock extension message covers
  //    H5C__write_cache_image_superblock_msg() and create failure in
  //    H5C__prep_image_for_file_close().
  {
    ScopedPlist fapl = MakeCoreCacheImageFapl(nullptr, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_dup_mdci.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    g.reset();
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    H5C_t* cache_ptr = hf->shared->cache;

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    cache_ptr->close_warning_received = true;
    bool img_gen = false;
    ASSERT_GE(H5C__prep_image_for_file_close(hf, &img_gen), 0);

    // Clear image_entries metadata while keeping the newly created MDCI message
    // in the superblock extension, then call prep a second time. Note that
    // image_entries[i].image_ptr aliases live entry_ptr->image_ptr (which
    // retains ownership until cache destruction), whereas fd_parent_addrs
    // ownership was transferred to image_entries.
    if (cache_ptr->image_entries != nullptr) {
      for (uint32_t i = 0; i < cache_ptr->num_entries_in_image; ++i) {
        cache_ptr->image_entries[i].fd_parent_addrs =
            static_cast<haddr_t*>(H5MM_xfree(
                cache_ptr->image_entries[i].fd_parent_addrs));
        cache_ptr->image_entries[i].image_ptr = nullptr;
      }
      cache_ptr->image_entries = static_cast<H5C_image_entry_t*>(
          H5MM_xfree(cache_ptr->image_entries));
    }
    cache_ptr->num_entries_in_image = 0;
    cache_ptr->image_len = 0;
    cache_ptr->image_data_len = 0;
    cache_ptr->image_addr = HADDR_UNDEF;
    hf->shared->eoa_post_mdci_fsalloc = HADDR_UNDEF;

    EXPECT_LT(H5C__prep_image_for_file_close(hf, &img_gen), 0);

    cache_ptr->image_ctl.generate_image = false;
    cache_ptr->close_warning_received = false;
  }

  // Helper that opens a fresh V110 file with 1 TestCacheEntry inserted and runs
  // H5C__prep_image_for_file_close() under custom cache/VFD fault conditions.
  auto run_prep_fault = [&](auto setup_fn, herr_t expected_rc) {
    ScopedPlist fapl = MakeCoreCacheImageFapl(nullptr, /*generate_image=*/true);
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(H5Fcreate("h5cimage_prep_fault.h5", H5F_ACC_TRUNC,
                           H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = GetH5File(f.get());
    ASSERT_NE(hf, nullptr);
    H5C_t* cache_ptr = hf->shared->cache;

    haddr_t a0 = HADDR_UNDEF;
    {
      ScopedApiContext alloc_ctx;
      ASSERT_TRUE(alloc_ctx.valid());
      a0 = H5MF_alloc(hf, H5FD_MEM_OHDR, 64);
    }
    ASSERT_NE(a0, HADDR_UNDEF);
    ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedApiContext ctx(a0);
    ASSERT_TRUE(ctx.valid());
    TestCacheEntry* entry = AllocTestEntry();
    ASSERT_NE(entry, nullptr);
    ASSERT_GE(H5C_insert_entry(hf, TestEntryClass(), a0, entry,
                               H5C__NO_FLAGS_SET),
              0);

    ScopedVfdHooks vfd(hf);
    cache_ptr->close_warning_received = true;
    setup_fn(hf, cache_ptr, entry, vfd);

    bool img_gen = false;
    if (expected_rc < 0) {
      EXPECT_LT(H5C__prep_image_for_file_close(hf, &img_gen), 0);
    } else {
      EXPECT_GE(H5C__prep_image_for_file_close(hf, &img_gen), 0);
    }

    vfd.Release();
    entry->fail_serialize = false;
    if (cache_ptr->image_entries != nullptr) {
      for (uint32_t i = 0; i < cache_ptr->num_entries_in_image; ++i) {
        cache_ptr->image_entries[i].fd_parent_addrs =
            static_cast<haddr_t*>(H5MM_xfree(
                cache_ptr->image_entries[i].fd_parent_addrs));
        cache_ptr->image_entries[i].image_ptr = nullptr;
      }
      cache_ptr->image_entries = static_cast<H5C_image_entry_t*>(
          H5MM_xfree(cache_ptr->image_entries));
    }
    cache_ptr->num_entries_in_image = 0;
    cache_ptr->image_len = 0;
    cache_ptr->image_data_len = 0;
    cache_ptr->image_addr = HADDR_UNDEF;
    hf->shared->eoa_post_mdci_fsalloc = HADDR_UNDEF;
    cache_ptr->image_ctl.generate_image = false;
    cache_ptr->image_ctl.flags = H5C_CI__ALL_FLAGS;
    cache_ptr->close_warning_received = false;
  };

  // 4. Cache serialization failure during close prep.
  run_prep_fault(
      [](H5F_t* /*hf*/, H5C_t* cache_ptr, TestCacheEntry* entry,
         ScopedVfdHooks& /*vfd*/) {
        cache_ptr->image_ctl.flags =
            H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDCI_SBE_MESG;
        entry->fail_serialize = true;
      },
      -1);

  // 5. VFD allocation (set_eoa) failure for cache image block.
  run_prep_fault(
      [](H5F_t* /*hf*/, H5C_t* cache_ptr, TestCacheEntry* /*entry*/,
         ScopedVfdHooks& vfd) {
        cache_ptr->image_ctl.flags =
            H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDCI_SBE_MESG;
        vfd.set_fail_set_eoa_on_call(1);
      },
      -1);

  // 6. VFD get_eoa failure immediately after H5FD_alloc during close prep.
  run_prep_fault(
      [](H5F_t* /*hf*/, H5C_t* cache_ptr, TestCacheEntry* /*entry*/,
         ScopedVfdHooks& vfd) {
        cache_ptr->image_ctl.flags =
            H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDCI_SBE_MESG;
        vfd.set_fail_get_eoa_after_set_eoa(true);
      },
      -1);

  // 7. Updating MDCI superblock extension message when create was skipped and no
  //    message exists covers H5C__write_cache_image_superblock_msg(f, false) failure.
  run_prep_fault(
      [](H5F_t* /*hf*/, H5C_t* cache_ptr, TestCacheEntry* /*entry*/,
         ScopedVfdHooks& /*vfd*/) {
        cache_ptr->image_ctl.flags =
            H5C_CI__ALL_FLAGS & ~H5C_CI__GEN_MDCI_SBE_MESG;
      },
      -1);
}

}  // namespace
}  // namespace hdf5
