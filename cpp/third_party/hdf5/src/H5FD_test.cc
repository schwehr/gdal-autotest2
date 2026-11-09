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
// Tests the Virtual File Layer (VFL) dispatch, driver registration, file
// open/close/compare/query/delete, space allocation, scalar/vector/selection
// I/O, flush/truncate/lock/unlock/ctl, superblock driver info, and package
// initialization/termination in third_party/hdf5/src/H5FD.c.
//
// Google3 build caveats: H5_HAVE_PARALLEL, H5_HAVE_DIRECT, H5_HAVE_ROS3_VFD,
// H5_HAVE_MIRROR_VFD, H5_HAVE_SUBFILING_VFD, H5_HAVE_LIBHDFS, and
// H5_HAVE_IOC_VFD are disabled in H5pubconf.h.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FD.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
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
// internal structs (such as H5C_cache_entry_t pulled in via H5CXprivate.h).
// Define NDEBUG around both the prelude and internal headers to match `:hdf5`
// struct layouts.
#ifndef NDEBUG
#define H5FD_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
}  // extern "C"
#ifdef H5FD_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FD_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

// Move-only RAII wrapper for H5FD_t* opened via H5FDopen / H5FD_open.
class ScopedFdFile {
 public:
  ScopedFdFile() = default;
  explicit ScopedFdFile(H5FD_t* file) : file_(file) {}
  ~ScopedFdFile() { reset(); }

  ScopedFdFile(const ScopedFdFile&) = delete;
  ScopedFdFile& operator=(const ScopedFdFile&) = delete;

  ScopedFdFile(ScopedFdFile&& other) noexcept
      : file_(std::exchange(other.file_, nullptr)) {}

  ScopedFdFile& operator=(ScopedFdFile&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.file_, nullptr));
    }
    return *this;
  }

  void reset(H5FD_t* next = nullptr) {
    if (file_ != nullptr) {
      H5FDclose(file_);
    }
    file_ = next;
  }

  H5FD_t* release() { return std::exchange(file_, nullptr); }
  H5FD_t* get() const { return file_; }
  bool valid() const { return file_ != nullptr; }

 private:
  H5FD_t* file_ = nullptr;
};

// Move-only RAII wrapper for custom VFD driver IDs registered with
// H5FDregister / H5FD_register.
class ScopedVfdDriver {
 public:
  ScopedVfdDriver() = default;
  explicit ScopedVfdDriver(hid_t id) : id_(id) {}
  ~ScopedVfdDriver() { reset(); }

  ScopedVfdDriver(const ScopedVfdDriver&) = delete;
  ScopedVfdDriver& operator=(const ScopedVfdDriver&) = delete;

  ScopedVfdDriver(ScopedVfdDriver&& other) noexcept
      : id_(std::exchange(other.id_, H5I_INVALID_HID)) {}

  ScopedVfdDriver& operator=(ScopedVfdDriver&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.id_, H5I_INVALID_HID));
    }
    return *this;
  }

  void reset(hid_t next = H5I_INVALID_HID) {
    if (id_ != H5I_INVALID_HID) {
      H5FDunregister(id_);
    }
    id_ = next;
  }

  hid_t release() { return std::exchange(id_, H5I_INVALID_HID); }
  hid_t get() const { return id_; }
  bool valid() const { return id_ >= 0; }

 private:
  hid_t id_ = H5I_INVALID_HID;
};

// RAII wrapper for pushing/popping an HDF5 API context around internal
// H5FD_* calls that inspect H5CX (e.g., H5FD_flush / H5FD_truncate /
// H5FD_read).
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// Per-test fake VFD controller for exercising every callback and fault path in
// H5FD.c without disk I/O or global state leakage across tests.
struct FakeVfdFile;

struct FakeVfdState {
  // Call counters.
  int terminate_calls = 0;
  int sb_size_calls = 0;
  int sb_encode_calls = 0;
  int sb_decode_calls = 0;
  int fapl_get_calls = 0;
  int fapl_free_calls = 0;
  int open_calls = 0;
  int close_calls = 0;
  int cmp_calls = 0;
  int query_calls = 0;
  int get_type_map_calls = 0;
  int alloc_calls = 0;
  int free_calls = 0;
  int get_eoa_calls = 0;
  int set_eoa_calls = 0;
  int get_eof_calls = 0;
  int get_handle_calls = 0;
  int read_calls = 0;
  int write_calls = 0;
  int read_vector_calls = 0;
  int write_vector_calls = 0;
  int read_selection_calls = 0;
  int write_selection_calls = 0;
  int flush_calls = 0;
  int truncate_calls = 0;
  int lock_calls = 0;
  int unlock_calls = 0;
  int del_calls = 0;
  int ctl_calls = 0;

  // Fault injection switches.
  bool fail_sb_encode = false;
  bool fail_sb_decode = false;
  bool fail_fapl_free = false;
  bool fail_open = false;
  bool fail_close = false;
  bool fail_query_on_null = false;
  bool fail_query_on_file = false;
  bool fail_get_type_map = false;
  bool fail_alloc = false;
  bool fail_free = false;
  bool fail_get_eoa = false;
  bool fail_set_eoa = false;
  bool fail_get_eof = false;
  bool fail_get_handle = false;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_read_vector = false;
  bool fail_write_vector = false;
  bool fail_read_selection = false;
  bool fail_write_selection = false;
  bool fail_flush = false;
  bool fail_truncate = false;
  bool fail_lock = false;
  bool fail_unlock = false;
  bool fail_del = false;
  bool fail_ctl = false;

  // Observed parameters & responses.
  unsigned long query_flags = H5FD_FEAT_AGGREGATE_METADATA;
  hsize_t sb_size_bytes = 8;
  bool last_flush_closing = false;
  bool last_truncate_closing = false;
  bool last_lock_rw = false;
  uint64_t last_ctl_opcode = 0;
  uint64_t last_ctl_flags = 0;
  const void* last_ctl_input = nullptr;
  int ctl_output_payload = 0x4242;
  int handle_token = 0x1357;
  std::string last_deleted_name;
  std::string last_sb_name;
  std::vector<uint8_t> last_sb_buf;
  haddr_t eoa = 0;
  haddr_t eof = 0;
  int cmp_key = 0;
  std::vector<uint8_t> storage;
  std::vector<std::unique_ptr<FakeVfdFile>> orphaned_files;
};

struct FakeVfdFile {
  H5FD_t pub;
  FakeVfdState* state;
  int key;
};
static_assert(std::is_standard_layout_v<FakeVfdFile>);

FakeVfdState* g_fake_state = nullptr;

class ScopedFakeVfdState {
 public:
  explicit ScopedFakeVfdState(FakeVfdState* state)
      : prev_(std::exchange(g_fake_state, state)) {}
  ~ScopedFakeVfdState() { g_fake_state = prev_; }

  ScopedFakeVfdState(const ScopedFakeVfdState&) = delete;
  ScopedFakeVfdState& operator=(const ScopedFakeVfdState&) = delete;

 private:
  FakeVfdState* prev_;
};

FakeVfdFile* AsFake(H5FD_t* f) { return reinterpret_cast<FakeVfdFile*>(f); }
const FakeVfdFile* AsFake(const H5FD_t* f) {
  return reinterpret_cast<const FakeVfdFile*>(f);
}

herr_t FakeTerminate() {
  if (g_fake_state != nullptr) {
    ++g_fake_state->terminate_calls;
  }
  return 0;
}

hsize_t FakeSbSize(H5FD_t* f) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->sb_size_calls;
  return st->sb_size_bytes;
}

herr_t FakeSbEncode(H5FD_t* f, char* name, unsigned char* buf) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->sb_encode_calls;
  if (st->fail_sb_encode) {
    return -1;
  }
  std::memcpy(name, "FAKEVFD0", 9);
  for (size_t i = 0; i < st->sb_size_bytes; ++i) {
    buf[i] = static_cast<unsigned char>(0xa0u + i);
  }
  return 0;
}

herr_t FakeSbDecode(H5FD_t* f, const char* name, const unsigned char* buf) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->sb_decode_calls;
  st->last_sb_name = name ? name : "";
  if (st->fail_sb_decode) {
    return -1;
  }
  st->last_sb_buf.assign(buf, buf + st->sb_size_bytes);
  return 0;
}

void* FakeFaplGet(H5FD_t* f) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->fapl_get_calls;
  int* info = static_cast<int*>(H5MM_malloc(sizeof(int)));
  if (info != nullptr) {
    *info = 77;
  }
  return info;
}

herr_t FakeFaplFree(void* info) {
  if (g_fake_state != nullptr) {
    ++g_fake_state->fapl_free_calls;
    if (g_fake_state->fail_fapl_free) {
      return -1;
    }
  }
  H5MM_xfree(info);
  return 0;
}

H5FD_t* FakeOpen(const char* /*name*/, unsigned /*flags*/, hid_t /*fapl*/,
                 haddr_t /*maxaddr*/) {
  FakeVfdState* st = g_fake_state;
  if (st == nullptr) {
    return nullptr;
  }
  ++st->open_calls;
  if (st->fail_open) {
    return nullptr;
  }
  auto f = std::make_unique<FakeVfdFile>();
  std::memset(&f->pub, 0, sizeof(f->pub));
  f->state = st;
  f->key = st->cmp_key;
  if (st->fail_query_on_file) {
    // H5FD_open does not free `file` if H5FD__query fails after open(); retain
    // ownership in orphaned_files so ASan/LSan sees zero leaks.
    FakeVfdFile* raw = f.get();
    st->orphaned_files.push_back(std::move(f));
    return &raw->pub;
  }
  return &f.release()->pub;
}

herr_t FakeClose(H5FD_t* f) {
  FakeVfdFile* fake = AsFake(f);
  FakeVfdState* st = fake->state;
  ++st->close_calls;
  if (st->fail_close) {
    // Keep ownership so the test remains leak-free under asan-chlor.
    st->orphaned_files.emplace_back(fake);
    return -1;
  }
  delete fake;
  return 0;
}

int FakeCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const FakeVfdFile* a = AsFake(f1);
  const FakeVfdFile* b = AsFake(f2);
  ++a->state->cmp_calls;
  if (a->key < b->key) return -1;
  if (a->key > b->key) return 1;
  return 0;
}

herr_t FakeQuery(const H5FD_t* f, unsigned long* flags) {
  FakeVfdState* st = (f != nullptr) ? AsFake(f)->state : g_fake_state;
  if (st == nullptr) {
    *flags = 0;
    return 0;
  }
  ++st->query_calls;
  if (f == nullptr && st->fail_query_on_null) {
    return -1;
  }
  if (f != nullptr && st->fail_query_on_file) {
    return -1;
  }
  *flags = st->query_flags;
  return 0;
}

herr_t FakeGetTypeMap(const H5FD_t* f, H5FD_mem_t* type_map) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->get_type_map_calls;
  if (st->fail_get_type_map) {
    return -1;
  }
  for (int i = 0; i < H5FD_MEM_NTYPES; ++i) {
    type_map[i] = H5FD_MEM_DRAW;
  }
  return 0;
}

haddr_t FakeAlloc(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                  hsize_t size) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->alloc_calls;
  if (st->fail_alloc) {
    return HADDR_UNDEF;
  }
  haddr_t addr = st->eoa;
  st->eoa += size;
  if (st->eoa > st->storage.size()) {
    st->storage.resize(static_cast<size_t>(st->eoa), 0);
  }
  st->eof = std::max(st->eof, st->eoa);
  return addr;
}

herr_t FakeFree(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t /*addr*/, hsize_t /*size*/) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->free_calls;
  return st->fail_free ? -1 : 0;
}

haddr_t FakeGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->get_eoa_calls;
  return st->fail_get_eoa ? HADDR_UNDEF : st->eoa;
}

herr_t FakeSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->set_eoa_calls;
  if (st->fail_set_eoa) {
    return -1;
  }
  st->eoa = addr;
  if (st->eoa > st->storage.size()) {
    st->storage.resize(static_cast<size_t>(st->eoa), 0);
  }
  st->eof = std::max(st->eof, st->eoa);
  return 0;
}

haddr_t FakeGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->get_eof_calls;
  return st->fail_get_eof ? HADDR_UNDEF : st->eof;
}

herr_t FakeGetHandle(H5FD_t* f, hid_t /*fapl*/, void** file_handle) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->get_handle_calls;
  if (st->fail_get_handle) {
    return -1;
  }
  *file_handle = &st->handle_token;
  return 0;
}

herr_t FakeRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/, haddr_t addr,
                size_t size, void* buf) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->read_calls;
  if (st->fail_read || addr + size > st->storage.size()) {
    return -1;
  }
  if (size > 0) {
    std::memcpy(buf, st->storage.data() + addr, size);
  }
  return 0;
}

herr_t FakeWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/, haddr_t addr,
                 size_t size, const void* buf) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->write_calls;
  if (st->fail_write) {
    return -1;
  }
  if (addr + size > st->storage.size()) {
    st->storage.resize(static_cast<size_t>(addr + size), 0);
  }
  if (size > 0) {
    std::memcpy(st->storage.data() + addr, buf, size);
  }
  st->eof = std::max(st->eof, addr + size);
  return 0;
}

herr_t FakeReadVector(H5FD_t* f, hid_t /*dxpl*/, uint32_t count,
                      H5FD_mem_t /*types*/[], haddr_t addrs[], size_t sizes[],
                      void* bufs[]) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->read_vector_calls;
  if (st->fail_read_vector) {
    return -1;
  }
  size_t cur_size = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (sizes[i] != 0) {
      cur_size = sizes[i];
    }
    if (addrs[i] + cur_size > st->storage.size()) {
      return -1;
    }
    std::memcpy(bufs[i], st->storage.data() + addrs[i], cur_size);
  }
  return 0;
}

herr_t FakeWriteVector(H5FD_t* f, hid_t /*dxpl*/, uint32_t count,
                       H5FD_mem_t /*types*/[], haddr_t addrs[], size_t sizes[],
                       const void* bufs[]) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->write_vector_calls;
  if (st->fail_write_vector) {
    return -1;
  }
  size_t cur_size = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (sizes[i] != 0) {
      cur_size = sizes[i];
    }
    if (addrs[i] + cur_size > st->storage.size()) {
      st->storage.resize(static_cast<size_t>(addrs[i] + cur_size), 0);
    }
    std::memcpy(st->storage.data() + addrs[i], bufs[i], cur_size);
    st->eof = std::max(st->eof, addrs[i] + cur_size);
  }
  return 0;
}

herr_t FakeReadSelection(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                         size_t count, hid_t mem_spaces[], hid_t file_spaces[],
                         haddr_t offsets[], size_t element_sizes[],
                         void* bufs[]) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->read_selection_calls;
  if (st->fail_read_selection) {
    return -1;
  }
  size_t elem_sz = 0;
  for (size_t i = 0; i < count; ++i) {
    if (element_sizes[i] != 0) {
      elem_sz = element_sizes[i];
    }
    const hssize_t n_mem = H5Sget_select_npoints(mem_spaces[i]);
    const hssize_t n_file = H5Sget_select_npoints(file_spaces[i]);
    if (n_mem < 0 || n_mem != n_file) {
      return -1;
    }
    const size_t nbytes = static_cast<size_t>(n_file) * elem_sz;
    if (offsets[i] + nbytes > st->storage.size()) {
      return -1;
    }
    std::memcpy(bufs[i], st->storage.data() + offsets[i], nbytes);
  }
  return 0;
}

herr_t FakeWriteSelection(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                          size_t count, hid_t mem_spaces[], hid_t file_spaces[],
                          haddr_t offsets[], size_t element_sizes[],
                          const void* bufs[]) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->write_selection_calls;
  if (st->fail_write_selection) {
    return -1;
  }
  size_t elem_sz = 0;
  for (size_t i = 0; i < count; ++i) {
    if (element_sizes[i] != 0) {
      elem_sz = element_sizes[i];
    }
    const hssize_t n_mem = H5Sget_select_npoints(mem_spaces[i]);
    const hssize_t n_file = H5Sget_select_npoints(file_spaces[i]);
    if (n_mem < 0 || n_mem != n_file) {
      return -1;
    }
    const size_t nbytes = static_cast<size_t>(n_file) * elem_sz;
    if (offsets[i] + nbytes > st->storage.size()) {
      st->storage.resize(static_cast<size_t>(offsets[i] + nbytes), 0);
    }
    std::memcpy(st->storage.data() + offsets[i], bufs[i], nbytes);
    st->eof = std::max(st->eof, offsets[i] + nbytes);
  }
  return 0;
}

herr_t FakeFlush(H5FD_t* f, hid_t /*dxpl_id*/, bool closing) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->flush_calls;
  st->last_flush_closing = closing;
  return st->fail_flush ? -1 : 0;
}

herr_t FakeTruncate(H5FD_t* f, hid_t /*dxpl_id*/, bool closing) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->truncate_calls;
  st->last_truncate_closing = closing;
  return st->fail_truncate ? -1 : 0;
}

herr_t FakeLock(H5FD_t* f, bool rw) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->lock_calls;
  st->last_lock_rw = rw;
  return st->fail_lock ? -1 : 0;
}

herr_t FakeUnlock(H5FD_t* f) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->unlock_calls;
  return st->fail_unlock ? -1 : 0;
}

herr_t FakeDelete(const char* name, hid_t /*fapl*/) {
  if (g_fake_state == nullptr) {
    return -1;
  }
  ++g_fake_state->del_calls;
  g_fake_state->last_deleted_name = name ? name : "";
  return g_fake_state->fail_del ? -1 : 0;
}

herr_t FakeCtl(H5FD_t* f, uint64_t op_code, uint64_t flags, const void* input,
               void** output) {
  FakeVfdState* st = AsFake(f)->state;
  ++st->ctl_calls;
  st->last_ctl_opcode = op_code;
  st->last_ctl_flags = flags;
  st->last_ctl_input = input;
  if (st->fail_ctl) {
    return -1;
  }
  if (output != nullptr) {
    *output = &st->ctl_output_payload;
  }
  return 0;
}

H5FD_class_t MakeFullFakeClass(const char* name = "fake_full_vfd",
                               H5FD_class_value_t value = 301) {
  return H5FD_class_t{
      .version = H5FD_CLASS_VERSION,
      .value = value,
      .name = name,
      .maxaddr = 1024ULL * 1024ULL,
      .fc_degree = H5F_CLOSE_WEAK,
      .terminate = FakeTerminate,
      .sb_size = FakeSbSize,
      .sb_encode = FakeSbEncode,
      .sb_decode = FakeSbDecode,
      .fapl_size = 0,
      .fapl_get = FakeFaplGet,
      .fapl_copy = nullptr,
      .fapl_free = FakeFaplFree,
      .dxpl_size = 0,
      .dxpl_copy = nullptr,
      .dxpl_free = nullptr,
      .open = FakeOpen,
      .close = FakeClose,
      .cmp = FakeCmp,
      .query = FakeQuery,
      .get_type_map = FakeGetTypeMap,
      .alloc = FakeAlloc,
      .free = FakeFree,
      .get_eoa = FakeGetEoa,
      .set_eoa = FakeSetEoa,
      .get_eof = FakeGetEof,
      .get_handle = FakeGetHandle,
      .read = FakeRead,
      .write = FakeWrite,
      .read_vector = FakeReadVector,
      .write_vector = FakeWriteVector,
      .read_selection = FakeReadSelection,
      .write_selection = FakeWriteSelection,
      .flush = FakeFlush,
      .truncate = FakeTruncate,
      .lock = FakeLock,
      .unlock = FakeUnlock,
      .del = FakeDelete,
      .ctl = FakeCtl,
      .fl_map = H5FD_FLMAP_SINGLE,
  };
}

H5FD_class_t MakeMinimalFakeClass(const char* name = "fake_min_vfd",
                                  H5FD_class_value_t value = 302) {
  return H5FD_class_t{
      .version = H5FD_CLASS_VERSION,
      .value = value,
      .name = name,
      .maxaddr = 65536,
      .fc_degree = H5F_CLOSE_WEAK,
      .terminate = nullptr,
      .sb_size = nullptr,
      .sb_encode = nullptr,
      .sb_decode = nullptr,
      .fapl_size = 0,
      .fapl_get = nullptr,
      .fapl_copy = nullptr,
      .fapl_free = nullptr,
      .dxpl_size = 0,
      .dxpl_copy = nullptr,
      .dxpl_free = nullptr,
      .open = FakeOpen,
      .close = FakeClose,
      .cmp = nullptr,
      .query = nullptr,
      .get_type_map = nullptr,
      .alloc = nullptr,
      .free = nullptr,
      .get_eoa = FakeGetEoa,
      .set_eoa = FakeSetEoa,
      .get_eof = FakeGetEof,
      .get_handle = nullptr,
      .read = FakeRead,
      .write = FakeWrite,
      .read_vector = nullptr,
      .write_vector = nullptr,
      .read_selection = nullptr,
      .write_selection = nullptr,
      .flush = nullptr,
      .truncate = nullptr,
      .lock = nullptr,
      .unlock = nullptr,
      .del = nullptr,
      .ctl = nullptr,
      .fl_map = H5FD_FLMAP_DICHOTOMY,
  };
}

TEST(H5FDTest, RegisterValidationRejectsInvalidClassesAndUnregisterLifecycle) {
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(nullptr));

    H5FD_class_t bad = MakeMinimalFakeClass();
    bad.version = H5FD_CLASS_VERSION + 1;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.open = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.close = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.get_eoa = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.set_eoa = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.get_eof = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.read = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.write = nullptr;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.fl_map[H5FD_MEM_DRAW] = static_cast<H5FD_mem_t>(H5FD_MEM_NOLIST - 1);
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    bad = MakeMinimalFakeClass();
    bad.fl_map[H5FD_MEM_DRAW] = H5FD_MEM_NTYPES;
    EXPECT_EQ(H5I_INVALID_HID, H5FDregister(&bad));

    EXPECT_LT(H5FDunregister(H5I_INVALID_HID), 0);
    EXPECT_LT(H5FDunregister(H5P_DEFAULT), 0);
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_NE(H5I_INVALID_HID, fapl.get());
    EXPECT_LT(H5FDunregister(fapl.get()), 0);
  }

  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_reg_full", 310);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_reg_min", 311);
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  EXPECT_GT(H5FDis_driver_registered_by_name("fake_reg_full"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(310), 0);
  EXPECT_GT(H5FDis_driver_registered_by_name("fake_reg_min"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(311), 0);

  const hid_t released_full = full_drv.release();
  EXPECT_GE(H5FDunregister(released_full), 0);
  EXPECT_EQ(1, state.terminate_calls);
  EXPECT_EQ(0, H5FDis_driver_registered_by_name("fake_reg_full"));
  EXPECT_EQ(0, H5FDis_driver_registered_by_value(310));

  ScopedErrorSilence silence;
  EXPECT_LT(H5FDunregister(released_full), 0);
}

TEST(H5FDTest, BuiltinDriverRegistrationQueriesAndDriverQueryFlags) {
  struct BuiltinDriverCase {
    std::string_view name;
    H5FD_class_value_t value;
  };
  constexpr auto kBuiltins = std::to_array<BuiltinDriverCase>({
      {.name = "core", .value = H5_VFD_CORE},
      {.name = "sec2", .value = H5_VFD_SEC2},
      {.name = "stdio", .value = H5_VFD_STDIO},
      {.name = "family", .value = H5_VFD_FAMILY},
      {.name = "multi", .value = H5_VFD_MULTI},
      {.name = "log", .value = H5_VFD_LOG},
      {.name = "splitter", .value = H5_VFD_SPLITTER},
      {.name = "onion", .value = H5_VFD_ONION},
  });

  for (const auto& entry : kBuiltins) {
    const std::string name_str(entry.name);
    EXPECT_GT(H5FDis_driver_registered_by_name(name_str.c_str()), 0)
        << entry.name;
    EXPECT_GT(H5FDis_driver_registered_by_value(entry.value), 0) << entry.name;
  }
  EXPECT_EQ(0, H5FDis_driver_registered_by_name("nonexistent_driver_name"));
  EXPECT_EQ(0, H5FDis_driver_registered_by_name(""));
  EXPECT_EQ(0, H5FDis_driver_registered_by_value(9999));
  EXPECT_EQ(0, H5FDis_driver_registered_by_value(H5_VFD_INVALID));

  // Query built-in drivers.
  unsigned long flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_CORE, &flags), 0);
  EXPECT_NE(0UL, flags & H5FD_FEAT_ALLOW_FILE_IMAGE);
  ASSERT_GE(H5FDdriver_query(H5FD_SEC2, &flags), 0);
  EXPECT_NE(0UL, flags & H5FD_FEAT_DATA_SIEVE);
  ASSERT_GE(H5FDdriver_query(H5FD_STDIO, &flags), 0);
  ASSERT_GE(H5FDdriver_query(H5FD_FAMILY, &flags), 0);
  ASSERT_GE(H5FDdriver_query(H5FD_MULTI, &flags), 0);
  ASSERT_GE(H5FDdriver_query(H5FD_LOG, &flags), 0);
  ASSERT_GE(H5FDdriver_query(H5FD_SPLITTER, &flags), 0);
  ASSERT_GE(H5FDdriver_query(H5FD_ONION, &flags), 0);

  // Driver with null query callback returns 0 flags; failing query fails.
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_query_min", 312);
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());
  flags = 0xdeadbeefUL;
  EXPECT_GE(H5FDdriver_query(min_drv.get(), &flags), 0);
  EXPECT_EQ(0UL, flags);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_query_full", 313);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  state.fail_query_on_null = true;

  ScopedErrorSilence silence;
  EXPECT_LT(H5FDdriver_query(full_drv.get(), &flags), 0);
  EXPECT_LT(H5FDdriver_query(H5FD_CORE, nullptr), 0);
  EXPECT_LT(H5FDdriver_query(H5I_INVALID_HID, &flags), 0);
  EXPECT_LT(H5FDdriver_query(H5P_DEFAULT, &flags), 0);
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, fapl.get());
  EXPECT_LT(H5FDdriver_query(fapl.get(), &flags), 0);
}

TEST(H5FDTest, OpenClosePublicAndInternalPathsAndErrors) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_open_full", 314);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, fapl.get());
  ASSERT_GE(H5Pset_driver(fapl.get(), full_drv.get(), nullptr), 0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_NE(H5I_INVALID_HID, dxpl.get());

  // 1. Public H5FDopen error checks.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5FDopen("bad", H5F_ACC_RDWR, dxpl.get(), HADDR_UNDEF));
    EXPECT_EQ(nullptr,
              H5FDopen("bad", H5F_ACC_RDWR, H5I_INVALID_HID, HADDR_UNDEF));
    EXPECT_EQ(nullptr, H5FDopen("zero_max", H5F_ACC_RDWR, fapl.get(), 0));
  }

  // 2. Internal H5FD_open error checks and try=true/false paths.
  {
    ScopedErrorSilence silence;
    H5FD_t* out_file = nullptr;

    EXPECT_LT(H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, H5I_INVALID_HID,
                        HADDR_UNDEF),
              0);
    EXPECT_LT(
        H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, dxpl.get(), HADDR_UNDEF),
        0);

    // Internal H5FD_register with open == nullptr hits driver->open == NULL.
    H5FD_class_t no_open_cls = MakeMinimalFakeClass("fake_no_open", 315);
    no_open_cls.open = nullptr;
    ScopedVfdDriver no_open_drv(
        H5FD_register(&no_open_cls, sizeof(no_open_cls), true));
    ASSERT_NE(H5I_INVALID_HID, no_open_drv.get());
    ScopedPlist no_open_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_NE(H5I_INVALID_HID, no_open_fapl.get());
    ASSERT_GE(H5Pset_driver(no_open_fapl.get(), no_open_drv.get(), nullptr), 0);
    EXPECT_LT(H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, no_open_fapl.get(),
                        HADDR_UNDEF),
              0);

    // Driver query fails before open.
    state.fail_query_on_null = true;
    EXPECT_LT(
        H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
        0);
    state.fail_query_on_null = false;

    // File image set on FAPL when driver lacks H5FD_FEAT_ALLOW_FILE_IMAGE.
    ScopedPlist img_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_NE(H5I_INVALID_HID, img_fapl.get());
    ASSERT_GE(H5Pset_driver(img_fapl.get(), full_drv.get(), nullptr), 0);
    std::array<uint8_t, 16> image_bytes{};
    ASSERT_GE(H5Pset_file_image(img_fapl.get(), image_bytes.data(),
                                image_bytes.size()),
              0);
    EXPECT_LT(H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, img_fapl.get(),
                        HADDR_UNDEF),
              0);

    // try=true returns >= 0 with out_file == nullptr when driver->open fails;
    // try=false returns < 0.
    state.fail_open = true;
    out_file = reinterpret_cast<H5FD_t*>(0x1);
    EXPECT_GE(
        H5FD_open(true, &out_file, "x", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
        0);
    EXPECT_EQ(nullptr, out_file);
    EXPECT_LT(
        H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
        0);
    state.fail_open = false;

    // Driver query fails on the newly opened H5FD_t* inside H5FD_open.
    // Register a disposable driver whose internal ref is cleared afterwards.
    const H5FD_class_t qfail_cls = MakeFullFakeClass("fake_qfail", 316);
    hid_t qfail_drv = H5FDregister(&qfail_cls);
    ASSERT_NE(H5I_INVALID_HID, qfail_drv);
    ScopedPlist qfail_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_NE(H5I_INVALID_HID, qfail_fapl.get());
    ASSERT_GE(H5Pset_driver(qfail_fapl.get(), qfail_drv, nullptr), 0);
    state.fail_query_on_file = true;
    EXPECT_LT(H5FD_open(false, &out_file, "x", H5F_ACC_RDWR, qfail_fapl.get(),
                        HADDR_UNDEF),
              0);
    state.fail_query_on_file = false;
    qfail_fapl.reset();
    // H5FD_open incremented qfail_drv's internal ref before H5FD__query failed;
    // decrement app ref then drop the leaked internal ref so registry stays
    // clean.
    EXPECT_GE(H5FDunregister(qfail_drv), 0);
    if (H5I_object(qfail_drv) != nullptr) {
      H5I_dec_ref(qfail_drv);
    }
  }

  // 3. Successful H5FDopen with explicit maxaddr and HADDR_UNDEF, plus close
  // error checks.
  ScopedFdFile fd_file(H5FDopen("inmem", H5F_ACC_RDWR, fapl.get(), 4096));
  ASSERT_TRUE(fd_file.valid());
  EXPECT_EQ(4096ULL, H5FD_get_maxaddr(fd_file.get()));
  fd_file.reset();

  fd_file.reset(H5FDopen("inmem2", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(fd_file.valid());
  EXPECT_EQ(full_cls.maxaddr, H5FD_get_maxaddr(fd_file.get()));

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDclose(nullptr), 0);
    H5FD_t no_cls{};
    EXPECT_LT(H5FDclose(&no_cls), 0);

    state.fail_close = true;
    EXPECT_LT(H5FDclose(fd_file.release()), 0);
    state.fail_close = false;
  }

  // 4. H5FDopen with H5P_DEFAULT uses sec2 on a TempFile path.
  TempFile sec2_tmp("h5fd_default_fapl.h5");
  ScopedFdFile sec2_file(H5FDopen(sec2_tmp.c_str(),
                                  H5F_ACC_CREAT | H5F_ACC_TRUNC | H5F_ACC_RDWR,
                                  H5P_DEFAULT, HADDR_UNDEF));
  ASSERT_TRUE(sec2_file.valid());
  EXPECT_GE(H5FDset_eoa(sec2_file.get(), H5FD_MEM_DEFAULT, 16), 0);
  const std::array<uint8_t, 4> payload = {0x11, 0x22, 0x33, 0x44};
  EXPECT_GE(H5FDwrite(sec2_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                      payload.size(), payload.data()),
            0);
  sec2_file.reset();

  // Opening a non-existent sec2 path without H5F_ACC_CREAT fails.
  TempFile missing_tmp("h5fd_missing.h5");
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5FDopen(missing_tmp.c_str(), H5F_ACC_RDONLY,
                                H5P_DEFAULT, HADDR_UNDEF));
  }
  EXPECT_GE(H5FDdelete(sec2_tmp.c_str(), H5P_DEFAULT), 0);
  EXPECT_FALSE(std::filesystem::exists(sec2_tmp.path()));
}

TEST(H5FDTest, CompareFilesAcrossNullsDifferentDriversAndCmpCallbacks) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_cmp_full", 317);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_cmp_min", 318);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  ScopedPlist full_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist min_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_NE(H5I_INVALID_HID, full_fapl.get());
  ASSERT_NE(H5I_INVALID_HID, min_fapl.get());
  ASSERT_GE(H5Pset_driver(full_fapl.get(), full_drv.get(), nullptr), 0);
  ASSERT_GE(H5Pset_driver(min_fapl.get(), min_drv.get(), nullptr), 0);

  state.cmp_key = 10;
  ScopedFdFile f1(H5FDopen("a", H5F_ACC_RDWR, full_fapl.get(), HADDR_UNDEF));
  state.cmp_key = 20;
  ScopedFdFile f2(H5FDopen("b", H5F_ACC_RDWR, full_fapl.get(), HADDR_UNDEF));
  ScopedFdFile m1(H5FDopen("c", H5F_ACC_RDWR, min_fapl.get(), HADDR_UNDEF));
  ScopedFdFile m2(H5FDopen("d", H5F_ACC_RDWR, min_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(f1.valid());
  ASSERT_TRUE(f2.valid());
  ASSERT_TRUE(m1.valid());
  ASSERT_TRUE(m2.valid());

  H5FD_t no_cls{};
  EXPECT_EQ(0, H5FDcmp(nullptr, nullptr));
  EXPECT_EQ(0, H5FDcmp(&no_cls, nullptr));
  EXPECT_EQ(-1, H5FDcmp(nullptr, f1.get()));
  EXPECT_EQ(-1, H5FDcmp(&no_cls, f1.get()));
  EXPECT_EQ(1, H5FDcmp(f1.get(), nullptr));
  EXPECT_EQ(1, H5FDcmp(f1.get(), &no_cls));

  // Same driver with cmp callback.
  EXPECT_EQ(0, H5FDcmp(f1.get(), f1.get()));
  EXPECT_LT(H5FDcmp(f1.get(), f2.get()), 0);
  EXPECT_GT(H5FDcmp(f2.get(), f1.get()), 0);
  EXPECT_GE(state.cmp_calls, 3);

  // Same driver without cmp callback falls back to pointer comparison.
  EXPECT_EQ(0, H5FDcmp(m1.get(), m1.get()));
  EXPECT_EQ(-H5FDcmp(m2.get(), m1.get()), H5FDcmp(m1.get(), m2.get()));
  EXPECT_NE(0, H5FDcmp(m1.get(), m2.get()));

  // Different drivers order strictly by cls pointer address.
  const int cross = H5FDcmp(f1.get(), m1.get());
  EXPECT_NE(0, cross);
  EXPECT_EQ(-cross, H5FDcmp(m1.get(), f1.get()));
  EXPECT_EQ(-cross, H5FD_cmp(m1.get(), f1.get()));
}

TEST(H5FDTest, QueryFeatureFlagsClassMaxaddrFilenoBaseAddrAndPagedAggr) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_meta_full", 319);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_meta_min", 320);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  ScopedPlist full_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist min_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(full_fapl.get(), full_drv.get(), nullptr), 0);
  ASSERT_GE(H5Pset_driver(min_fapl.get(), min_drv.get(), nullptr), 0);

  // H5FD_get_class lookups.
  const H5FD_class_t* cls_from_drv = H5FD_get_class(full_drv.get());
  const H5FD_class_t* cls_from_fapl = H5FD_get_class(full_fapl.get());
  ASSERT_NE(nullptr, cls_from_drv);
  EXPECT_EQ(cls_from_drv, cls_from_fapl);
  EXPECT_STREQ("fake_meta_full", cls_from_drv->name);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5FD_get_class(H5I_INVALID_HID));
    EXPECT_EQ(nullptr, H5FD_get_class(dxpl.get()));
  }

  ScopedFdFile f1(H5FDopen("m1", H5F_ACC_RDWR, full_fapl.get(), HADDR_UNDEF));
  ScopedFdFile f2(H5FDopen("m2", H5F_ACC_RDWR, min_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(f1.valid());
  ASSERT_TRUE(f2.valid());

  unsigned long flags = 0;
  EXPECT_GE(H5FDquery(f1.get(), &flags), 0);
  EXPECT_EQ(state.query_flags, flags);

  flags = 0x1234UL;
  EXPECT_GE(H5FDquery(f2.get(), &flags), 0);
  EXPECT_EQ(0UL, flags);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDquery(nullptr, &flags), 0);
    H5FD_t no_cls{};
    EXPECT_LT(H5FDquery(&no_cls, &flags), 0);
    EXPECT_LT(H5FDquery(f1.get(), nullptr), 0);
    state.fail_query_on_file = true;
    EXPECT_LT(H5FDquery(f1.get(), &flags), 0);
    state.fail_query_on_file = false;
  }

  unsigned long fflags = 0;
  EXPECT_GE(H5FD_get_feature_flags(f1.get(), &fflags), 0);
  EXPECT_EQ(state.query_flags, fflags);
  EXPECT_GE(H5FD_set_feature_flags(f1.get(), H5FD_FEAT_DATA_SIEVE), 0);
  EXPECT_GE(H5FD_get_feature_flags(f1.get(), &fflags), 0);
  EXPECT_EQ(static_cast<unsigned long>(H5FD_FEAT_DATA_SIEVE), fflags);

  unsigned long fn1 = 0;
  unsigned long fn2 = 0;
  EXPECT_GE(H5FD_get_fileno(f1.get(), &fn1), 0);
  EXPECT_GE(H5FD_get_fileno(f2.get(), &fn2), 0);
  EXPECT_NE(0UL, fn1);
  EXPECT_NE(fn1, fn2);

  EXPECT_EQ(0ULL, H5FD_get_base_addr(f1.get()));
  EXPECT_GE(H5FD_set_base_addr(f1.get(), 1024), 0);
  EXPECT_EQ(1024ULL, H5FD_get_base_addr(f1.get()));

  EXPECT_GE(H5FD_set_paged_aggr(f1.get(), true), 0);
  EXPECT_TRUE(f1.get()->paged_aggr);
  EXPECT_GE(H5FD_set_paged_aggr(f1.get(), false), 0);
  EXPECT_FALSE(f1.get()->paged_aggr);
}

TEST(H5FDTest, AllocFreeEoaEofAndBaseAddressCompensation) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_alloc_full", 321);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(fapl.get(), full_drv.get(), nullptr), 0);

  ScopedFdFile file(H5FDopen("alloc_mem", H5F_ACC_RDWR, fapl.get(), 8192));
  ASSERT_TRUE(file.valid());

  // Error checks on H5FDalloc, H5FDfree, H5FDget_eoa, H5FDset_eoa, H5FDget_eof.
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    const auto bad_type_low = static_cast<H5FD_mem_t>(-1);
    const auto bad_type_high = H5FD_MEM_NTYPES;

    EXPECT_EQ(HADDR_UNDEF, H5FDalloc(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 16));
    EXPECT_EQ(HADDR_UNDEF, H5FDalloc(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 16));
    EXPECT_EQ(HADDR_UNDEF,
              H5FDalloc(file.get(), bad_type_low, H5P_DEFAULT, 16));
    EXPECT_EQ(HADDR_UNDEF,
              H5FDalloc(file.get(), bad_type_high, H5P_DEFAULT, 16));
    EXPECT_EQ(HADDR_UNDEF,
              H5FDalloc(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0));
    EXPECT_EQ(HADDR_UNDEF,
              H5FDalloc(file.get(), H5FD_MEM_DRAW, fapl.get(), 16));

    EXPECT_LT(H5FDfree(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16), 0);
    EXPECT_LT(H5FDfree(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16), 0);
    EXPECT_LT(H5FDfree(file.get(), bad_type_low, H5P_DEFAULT, 0, 16), 0);
    EXPECT_LT(H5FDfree(file.get(), bad_type_high, H5P_DEFAULT, 0, 16), 0);
    EXPECT_LT(H5FDfree(file.get(), H5FD_MEM_DRAW, fapl.get(), 0, 16), 0);

    EXPECT_EQ(HADDR_UNDEF, H5FDget_eoa(nullptr, H5FD_MEM_DRAW));
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eoa(&no_cls, H5FD_MEM_DRAW));
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eoa(file.get(), bad_type_low));
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eoa(file.get(), bad_type_high));

    EXPECT_LT(H5FDset_eoa(nullptr, H5FD_MEM_DRAW, 64), 0);
    EXPECT_LT(H5FDset_eoa(&no_cls, H5FD_MEM_DRAW, 64), 0);
    EXPECT_LT(H5FDset_eoa(file.get(), bad_type_low, 64), 0);
    EXPECT_LT(H5FDset_eoa(file.get(), bad_type_high, 64), 0);
    EXPECT_LT(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, HADDR_UNDEF), 0);
    EXPECT_LT(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 9000), 0);

    EXPECT_EQ(HADDR_UNDEF, H5FDget_eof(nullptr, H5FD_MEM_DRAW));
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eof(&no_cls, H5FD_MEM_DRAW));

    // Driver failure propagation.
    state.fail_alloc = true;
    EXPECT_EQ(HADDR_UNDEF,
              H5FDalloc(file.get(), H5FD_MEM_DRAW, dxpl.get(), 32));
    state.fail_alloc = false;

    state.fail_free = true;
    EXPECT_LT(H5FDfree(file.get(), H5FD_MEM_DRAW, dxpl.get(), 0, 32), 0);
    state.fail_free = false;

    state.fail_get_eoa = true;
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eoa(file.get(), H5FD_MEM_DRAW));
    state.fail_get_eoa = false;

    state.fail_set_eoa = true;
    EXPECT_LT(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 128), 0);
    state.fail_set_eoa = false;

    state.fail_get_eof = true;
    EXPECT_EQ(HADDR_UNDEF, H5FDget_eof(file.get(), H5FD_MEM_DRAW));
    state.fail_get_eof = false;
  }

  // Test base_addr compensation on set_eoa / get_eoa / get_eof / alloc / free.
  EXPECT_GE(H5FD_set_base_addr(file.get(), 512), 0);
  EXPECT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 576), 0);
  // H5FDset_eoa subtracts base_addr; H5FD_set_eoa re-adds it before calling
  // cls->set_eoa.
  EXPECT_EQ(576ULL, state.eoa);
  EXPECT_EQ(64ULL, H5FD_get_eoa(file.get(), H5FD_MEM_DRAW));
  EXPECT_EQ(576ULL, H5FDget_eoa(file.get(), H5FD_MEM_DRAW));
  EXPECT_EQ(64ULL, H5FD_get_eof(file.get(), H5FD_MEM_DRAW));
  EXPECT_EQ(576ULL, H5FDget_eof(file.get(), H5FD_MEM_DRAW));

  const haddr_t a0 = H5FDalloc(file.get(), H5FD_MEM_DRAW, dxpl.get(), 32);
  EXPECT_EQ(576ULL, a0);
  EXPECT_GE(H5FDfree(file.get(), H5FD_MEM_DRAW, dxpl.get(), a0, 32), 0);

  // Reset base_addr and simulated EOA/EOF before unshifted assertions.
  EXPECT_GE(H5FD_set_base_addr(file.get(), 0), 0);
  state.eoa = 0;
  state.eof = 0;

  EXPECT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 128), 0);
  EXPECT_EQ(128ULL, H5FDget_eoa(file.get(), H5FD_MEM_DRAW));
  EXPECT_EQ(128ULL, H5FDget_eof(file.get(), H5FD_MEM_DRAW));

  const haddr_t a1 = H5FDalloc(file.get(), H5FD_MEM_DRAW, dxpl.get(), 64);
  EXPECT_EQ(128ULL, a1);
  EXPECT_EQ(192ULL, H5FDget_eoa(file.get(), H5FD_MEM_DRAW));
  EXPECT_GE(H5FDfree(file.get(), H5FD_MEM_DRAW, dxpl.get(), a1, 64), 0);

  // Core VFD uses internal extend/free-list path (cls->alloc/free == nullptr).
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  ScopedFdFile core_file(H5FDopen("core_alloc", H5F_ACC_CREAT | H5F_ACC_RDWR,
                                  core_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(core_file.valid());
  const haddr_t c1 =
      H5FDalloc(core_file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 64);
  ASSERT_NE(HADDR_UNDEF, c1);
  EXPECT_EQ(64ULL, H5FDget_eoa(core_file.get(), H5FD_MEM_SUPER));
  EXPECT_GE(H5FDfree(core_file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, c1, 64), 0);
  EXPECT_EQ(0ULL, H5FDget_eoa(core_file.get(), H5FD_MEM_SUPER));
}

TEST(H5FDTest, ScalarReadAndWriteValidationAndRoundTrip) {
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_NE(H5I_INVALID_HID, dxpl.get());

  ScopedFdFile core_file(H5FDopen("core_rw", H5F_ACC_CREAT | H5F_ACC_RDWR,
                                  core_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(core_file.valid());

  std::array<uint8_t, 8> wbuf = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<uint8_t, 8> rbuf{};
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDread(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, rbuf.data()),
              0);
    EXPECT_LT(H5FDread(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, rbuf.data()),
              0);
    EXPECT_LT(
        H5FDread(core_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, nullptr),
        0);
    EXPECT_LT(H5FDread(core_file.get(), H5FD_MEM_DRAW, core_fapl.get(), 0, 8,
                       rbuf.data()),
              0);
    // Reading beyond EOA (currently 0) fails inside H5FD_read.
    EXPECT_LT(H5FDread(core_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8,
                       rbuf.data()),
              0);

    EXPECT_LT(H5FDwrite(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, wbuf.data()),
              0);
    EXPECT_LT(H5FDwrite(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, wbuf.data()),
              0);
    EXPECT_LT(
        H5FDwrite(core_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, nullptr),
        0);
    EXPECT_LT(H5FDwrite(core_file.get(), H5FD_MEM_DRAW, core_fapl.get(), 0, 8,
                        wbuf.data()),
              0);
    // Writing beyond EOA fails inside H5FD_write.
    EXPECT_LT(H5FDwrite(core_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8,
                        wbuf.data()),
              0);
  }

  ASSERT_GE(H5FDset_eoa(core_file.get(), H5FD_MEM_DRAW, 64), 0);
  ASSERT_GE(H5FDwrite(core_file.get(), H5FD_MEM_DRAW, dxpl.get(), 16,
                      wbuf.size(), wbuf.data()),
            0);
  rbuf.fill(0xff);
  ASSERT_GE(H5FDread(core_file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16,
                     rbuf.size(), rbuf.data()),
            0);
  EXPECT_EQ(wbuf, rbuf);
}

TEST(H5FDTest, VectorReadAndWriteValidationCallbackAndFallback) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_vec_full", 322);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(fapl.get(), full_drv.get(), nullptr), 0);

  ScopedFdFile file(H5FDopen("vec", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 256), 0);

  std::array<uint8_t, 4> w0 = {10, 20, 30, 40};
  std::array<uint8_t, 4> w1 = {50, 60, 70, 80};
  std::array<uint8_t, 4> r0{};
  std::array<uint8_t, 4> r1{};
  std::array<H5FD_mem_t, 2> types = {H5FD_MEM_DRAW, H5FD_MEM_NOLIST};
  std::array<haddr_t, 2> addrs = {8, 32};
  std::array<size_t, 2> sizes = {4, 0};
  std::array<const void*, 2> wbufs = {w0.data(), w1.data()};
  std::array<void*, 2> rbufs = {r0.data(), r1.data()};

  // Validation error paths.
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDread_vector(nullptr, H5P_DEFAULT, 2, types.data(),
                              addrs.data(), sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(&no_cls, H5P_DEFAULT, 2, types.data(),
                              addrs.data(), sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, nullptr, addrs.data(),
                              sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, types.data(), nullptr,
                              sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                              addrs.data(), nullptr, rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                              addrs.data(), sizes.data(), nullptr),
              0);

    std::array<size_t, 2> zero_sizes = {0, 4};
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                              addrs.data(), zero_sizes.data(), rbufs.data()),
              0);
    std::array<H5FD_mem_t, 2> bad_types = {H5FD_MEM_NOLIST, H5FD_MEM_DRAW};
    EXPECT_LT(H5FDread_vector(file.get(), H5P_DEFAULT, 2, bad_types.data(),
                              addrs.data(), sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(H5FDread_vector(file.get(), fapl.get(), 2, types.data(),
                              addrs.data(), sizes.data(), rbufs.data()),
              0);

    EXPECT_LT(H5FDwrite_vector(nullptr, H5P_DEFAULT, 2, types.data(),
                               addrs.data(), sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(&no_cls, H5P_DEFAULT, 2, types.data(),
                               addrs.data(), sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, nullptr,
                               addrs.data(), sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                               nullptr, sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                               addrs.data(), nullptr, wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                               addrs.data(), sizes.data(), nullptr),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                               addrs.data(), zero_sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), H5P_DEFAULT, 2, bad_types.data(),
                               addrs.data(), sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(H5FDwrite_vector(file.get(), fapl.get(), 2, types.data(),
                               addrs.data(), sizes.data(), wbufs.data()),
              0);

    state.fail_write_vector = true;
    EXPECT_LT(H5FDwrite_vector(file.get(), dxpl.get(), 2, types.data(),
                               addrs.data(), sizes.data(), wbufs.data()),
              0);
    state.fail_write_vector = false;

    state.fail_read_vector = true;
    EXPECT_LT(H5FDread_vector(file.get(), dxpl.get(), 2, types.data(),
                              addrs.data(), sizes.data(), rbufs.data()),
              0);
    state.fail_read_vector = false;
  }

  // count == 0 succeeds as a no-op even with null array pointers.
  EXPECT_GE(H5FDwrite_vector(file.get(), H5P_DEFAULT, 0, nullptr, nullptr,
                             nullptr, nullptr),
            0);
  EXPECT_GE(H5FDread_vector(file.get(), H5P_DEFAULT, 0, nullptr, nullptr,
                            nullptr, nullptr),
            0);

  // Successful vector write and read via driver callbacks.
  state.write_vector_calls = 0;
  state.read_vector_calls = 0;
  ASSERT_GE(H5FDwrite_vector(file.get(), dxpl.get(), 2, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_vector(file.get(), H5P_DEFAULT, 2, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(1, state.write_vector_calls);
  EXPECT_EQ(1, state.read_vector_calls);
  EXPECT_EQ(w0, r0);
  EXPECT_EQ(w1, r1);

  // Core VFD translates vector I/O to scalar read/write calls.
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  ScopedFdFile core_file(H5FDopen("core_vec", H5F_ACC_CREAT | H5F_ACC_RDWR,
                                  core_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(core_file.valid());
  ASSERT_GE(H5FDset_eoa(core_file.get(), H5FD_MEM_DRAW, 128), 0);
  r0.fill(0);
  r1.fill(0);
  ASSERT_GE(H5FDwrite_vector(core_file.get(), H5P_DEFAULT, 2, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_vector(core_file.get(), H5P_DEFAULT, 2, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(w0, r0);
  EXPECT_EQ(w1, r1);
}

TEST(H5FDTest, SelectionReadAndWriteVariantsAndValidation) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_sel_full", 323);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(fapl.get(), full_drv.get(), nullptr), 0);

  ScopedFdFile file(H5FDopen("sel", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 512), 0);

  constexpr hsize_t kDim = 4;
  ScopedSpace mspace0(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace fspace0(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace mspace1(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace fspace1(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(mspace0.valid());
  ASSERT_TRUE(fspace0.valid());
  ASSERT_TRUE(mspace1.valid());
  ASSERT_TRUE(fspace1.valid());

  std::array<hid_t, 2> mem_ids = {mspace0.get(), mspace1.get()};
  std::array<hid_t, 2> file_ids = {fspace0.get(), fspace1.get()};
  std::array<haddr_t, 2> offsets = {0, 64};
  std::array<size_t, 2> elem_sizes = {sizeof(uint32_t), 0};
  std::array<uint32_t, 4> w0 = {101, 102, 103, 104};
  std::array<uint32_t, 4> w1 = {201, 202, 203, 204};
  std::array<uint32_t, 4> r0{};
  std::array<uint32_t, 4> r1{};
  std::array<const void*, 2> wbufs = {w0.data(), w1.data()};
  std::array<void*, 2> rbufs = {r0.data(), r1.data()};

  auto check_read_validation = [&](auto fn) {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    std::array<size_t, 2> zero_elem = {0, 4};
    std::array<void*, 2> null_buf0 = {nullptr, r1.data()};
    EXPECT_LT(
        fn(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), rbufs.data()),
        0);
    EXPECT_LT(
        fn(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), rbufs.data()),
        0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, nullptr, file_ids.data(),
           offsets.data(), elem_sizes.data(), rbufs.data()),
        0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 nullptr, offsets.data(), elem_sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), nullptr, elem_sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), nullptr, rbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), elem_sizes.data(), nullptr),
              0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), zero_elem.data(), rbufs.data()),
        0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), elem_sizes.data(),
                 null_buf0.data()),
              0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, fapl.get(), 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), rbufs.data()),
        0);
    EXPECT_GE(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, nullptr, nullptr,
                 nullptr, nullptr, nullptr),
              0);
  };

  auto check_write_validation = [&](auto fn) {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    std::array<size_t, 2> zero_elem = {0, 4};
    std::array<const void*, 2> null_buf0 = {nullptr, w1.data()};
    EXPECT_LT(
        fn(nullptr, H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), wbufs.data()),
        0);
    EXPECT_LT(
        fn(&no_cls, H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), wbufs.data()),
        0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, nullptr, file_ids.data(),
           offsets.data(), elem_sizes.data(), wbufs.data()),
        0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 nullptr, offsets.data(), elem_sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), nullptr, elem_sizes.data(), wbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), nullptr, wbufs.data()),
              0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), elem_sizes.data(), nullptr),
              0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
           file_ids.data(), offsets.data(), zero_elem.data(), wbufs.data()),
        0);
    EXPECT_LT(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, mem_ids.data(),
                 file_ids.data(), offsets.data(), elem_sizes.data(),
                 null_buf0.data()),
              0);
    EXPECT_LT(
        fn(file.get(), H5FD_MEM_DRAW, fapl.get(), 2, mem_ids.data(),
           file_ids.data(), offsets.data(), elem_sizes.data(), wbufs.data()),
        0);
    EXPECT_GE(fn(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, nullptr, nullptr,
                 nullptr, nullptr, nullptr),
              0);
  };

  check_read_validation(H5FDread_selection);
  check_read_validation(H5FDread_vector_from_selection);
  check_read_validation(H5FDread_from_selection);
  check_write_validation(H5FDwrite_selection);
  check_write_validation(H5FDwrite_vector_from_selection);
  check_write_validation(H5FDwrite_from_selection);

  // 1. Direct selection callbacks: H5FDwrite_selection / H5FDread_selection.
  ASSERT_GE(H5FDwrite_selection(file.get(), H5FD_MEM_DRAW, dxpl.get(), 2,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_selection(file.get(), H5FD_MEM_DRAW, dxpl.get(), 2,
                               mem_ids.data(), file_ids.data(), offsets.data(),
                               elem_sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(1, state.write_selection_calls);
  EXPECT_EQ(1, state.read_selection_calls);
  EXPECT_EQ(w0, r0);
  EXPECT_EQ(w1, r1);

  // 2. Skip selection -> translate to vector callbacks.
  r0.fill(0);
  r1.fill(0);
  ASSERT_GE(H5FDwrite_vector_from_selection(file.get(), H5FD_MEM_DRAW,
                                            dxpl.get(), 2, mem_ids.data(),
                                            file_ids.data(), offsets.data(),
                                            elem_sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_vector_from_selection(file.get(), H5FD_MEM_DRAW,
                                           dxpl.get(), 2, mem_ids.data(),
                                           file_ids.data(), offsets.data(),
                                           elem_sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(1, state.write_vector_calls);
  EXPECT_EQ(1, state.read_vector_calls);
  EXPECT_EQ(w0, r0);
  EXPECT_EQ(w1, r1);

  // 3. Skip selection + vector -> translate to scalar callbacks.
  r0.fill(0);
  r1.fill(0);
  ASSERT_GE(
      H5FDwrite_from_selection(file.get(), H5FD_MEM_DRAW, dxpl.get(), 2,
                               mem_ids.data(), file_ids.data(), offsets.data(),
                               elem_sizes.data(), wbufs.data()),
      0);
  ASSERT_GE(
      H5FDread_from_selection(file.get(), H5FD_MEM_DRAW, dxpl.get(), 2,
                              mem_ids.data(), file_ids.data(), offsets.data(),
                              elem_sizes.data(), rbufs.data()),
      0);
  EXPECT_GE(state.write_calls, 2);
  EXPECT_GE(state.read_calls, 2);
  EXPECT_EQ(w0, r0);
  EXPECT_EQ(w1, r1);

  // 4. Driver failure propagation across all 6 variants.
  {
    ScopedErrorSilence silence;
    state.fail_write_selection = true;
    EXPECT_LT(
        H5FDwrite_selection(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2,
                            mem_ids.data(), file_ids.data(), offsets.data(),
                            elem_sizes.data(), wbufs.data()),
        0);
    state.fail_write_selection = false;

    state.fail_read_selection = true;
    EXPECT_LT(
        H5FDread_selection(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2,
                           mem_ids.data(), file_ids.data(), offsets.data(),
                           elem_sizes.data(), rbufs.data()),
        0);
    state.fail_read_selection = false;

    state.fail_write_vector = true;
    EXPECT_LT(H5FDwrite_vector_from_selection(file.get(), H5FD_MEM_DRAW,
                                              H5P_DEFAULT, 2, mem_ids.data(),
                                              file_ids.data(), offsets.data(),
                                              elem_sizes.data(), wbufs.data()),
              0);
    state.fail_write_vector = false;

    state.fail_read_vector = true;
    EXPECT_LT(H5FDread_vector_from_selection(file.get(), H5FD_MEM_DRAW,
                                             H5P_DEFAULT, 2, mem_ids.data(),
                                             file_ids.data(), offsets.data(),
                                             elem_sizes.data(), rbufs.data()),
              0);
    state.fail_read_vector = false;

    state.fail_write = true;
    EXPECT_LT(H5FDwrite_from_selection(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT,
                                       2, mem_ids.data(), file_ids.data(),
                                       offsets.data(), elem_sizes.data(),
                                       wbufs.data()),
              0);
    state.fail_write = false;

    state.fail_read = true;
    EXPECT_LT(
        H5FDread_from_selection(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), rbufs.data()),
        0);
    state.fail_read = false;
  }
}

TEST(H5FDTest, FlushTruncateLockUnlockCtlAndGetVfdHandle) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_ctrl_full", 324);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_ctrl_min", 325);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  ScopedPlist full_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist min_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(full_fapl.get(), full_drv.get(), nullptr), 0);
  ASSERT_GE(H5Pset_driver(min_fapl.get(), min_drv.get(), nullptr), 0);

  ScopedFdFile f_full(
      H5FDopen("cf", H5F_ACC_RDWR, full_fapl.get(), HADDR_UNDEF));
  ScopedFdFile f_min(H5FDopen("cm", H5F_ACC_RDWR, min_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(f_full.valid());
  ASSERT_TRUE(f_min.valid());

  // Flush & truncate validation + callbacks + null-callback no-ops.
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDflush(nullptr, H5P_DEFAULT, false), 0);
    EXPECT_LT(H5FDflush(&no_cls, H5P_DEFAULT, false), 0);
    EXPECT_LT(H5FDflush(f_full.get(), full_fapl.get(), false), 0);

    EXPECT_LT(H5FDtruncate(nullptr, H5P_DEFAULT, false), 0);
    EXPECT_LT(H5FDtruncate(&no_cls, H5P_DEFAULT, false), 0);
    EXPECT_LT(H5FDtruncate(f_full.get(), full_fapl.get(), false), 0);

    state.fail_flush = true;
    EXPECT_LT(H5FDflush(f_full.get(), dxpl.get(), true), 0);
    state.fail_flush = false;

    state.fail_truncate = true;
    EXPECT_LT(H5FDtruncate(f_full.get(), dxpl.get(), true), 0);
    state.fail_truncate = false;
  }
  EXPECT_GE(H5FDflush(f_full.get(), dxpl.get(), true), 0);
  EXPECT_TRUE(state.last_flush_closing);
  EXPECT_GE(H5FDflush(f_min.get(), H5P_DEFAULT, false), 0);

  EXPECT_GE(H5FDtruncate(f_full.get(), H5P_DEFAULT, true), 0);
  EXPECT_TRUE(state.last_truncate_closing);
  EXPECT_GE(H5FDtruncate(f_min.get(), H5P_DEFAULT, false), 0);

  // Lock & unlock validation + callbacks + null-callback no-ops.
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDlock(nullptr, true), 0);
    EXPECT_LT(H5FDlock(&no_cls, true), 0);
    EXPECT_LT(H5FDunlock(nullptr), 0);
    EXPECT_LT(H5FDunlock(&no_cls), 0);

    state.fail_lock = true;
    EXPECT_LT(H5FDlock(f_full.get(), true), 0);
    state.fail_lock = false;

    state.fail_unlock = true;
    EXPECT_LT(H5FDunlock(f_full.get()), 0);
    state.fail_unlock = false;
  }
  EXPECT_GE(H5FDlock(f_full.get(), true), 0);
  EXPECT_TRUE(state.last_lock_rw);
  EXPECT_GE(H5FDunlock(f_full.get()), 0);
  EXPECT_GE(H5FDlock(f_min.get(), false), 0);
  EXPECT_GE(H5FDunlock(f_min.get()), 0);

  // H5FDctl validation, callback dispatch, and FAIL_IF_UNKNOWN_FLAG behavior.
  int input_val = 99;
  void* output_ptr = nullptr;
  EXPECT_GE(
      H5FDctl(f_full.get(), H5FD_CTL_TEST_OPCODE, 0, &input_val, &output_ptr),
      0);
  EXPECT_EQ(&state.ctl_output_payload, output_ptr);
  EXPECT_EQ(static_cast<uint64_t>(H5FD_CTL_TEST_OPCODE), state.last_ctl_opcode);
  EXPECT_EQ(&input_val, state.last_ctl_input);

  // Driver without ctl succeeds when FAIL_IF_UNKNOWN_FLAG is not set.
  EXPECT_GE(H5FDctl(f_min.get(), H5FD_CTL_TEST_OPCODE, 0, nullptr, nullptr), 0);
  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDctl(nullptr, 1, 0, nullptr, nullptr), 0);
    EXPECT_LT(H5FDctl(&no_cls, 1, 0, nullptr, nullptr), 0);
    EXPECT_LT(H5FDctl(f_min.get(), H5FD_CTL_TEST_OPCODE,
                      H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr, nullptr),
              0);
    state.fail_ctl = true;
    EXPECT_LT(H5FDctl(f_full.get(), H5FD_CTL_TEST_OPCODE, 0, nullptr, nullptr),
              0);
    state.fail_ctl = false;
  }

  // H5FDget_vfd_handle validation + callback + error clears *file_handle.
  void* handle_out = reinterpret_cast<void*>(0xdeadbeef);
  EXPECT_GE(H5FDget_vfd_handle(f_full.get(), full_fapl.get(), &handle_out), 0);
  EXPECT_EQ(&state.handle_token, handle_out);

  // H5FDget_vfd_handle checks `false == H5P_isa_class(fapl_id,
  // H5P_FILE_ACCESS)`; H5P_DEFAULT makes H5P_isa_class return FAIL (-1 !=
  // false), passing 0 through.
  handle_out = nullptr;
  EXPECT_GE(H5FDget_vfd_handle(f_full.get(), H5P_DEFAULT, &handle_out), 0);
  EXPECT_EQ(&state.handle_token, handle_out);

  {
    ScopedErrorSilence silence;
    H5FD_t no_cls{};
    EXPECT_LT(H5FDget_vfd_handle(nullptr, full_fapl.get(), &handle_out), 0);
    EXPECT_LT(H5FDget_vfd_handle(&no_cls, full_fapl.get(), &handle_out), 0);
    EXPECT_LT(H5FDget_vfd_handle(f_full.get(), dxpl.get(), &handle_out), 0);
    EXPECT_LT(H5FDget_vfd_handle(f_full.get(), full_fapl.get(), nullptr), 0);

    handle_out = &input_val;
    EXPECT_LT(H5FDget_vfd_handle(f_min.get(), min_fapl.get(), &handle_out), 0);
    EXPECT_EQ(nullptr, handle_out);

    state.fail_get_handle = true;
    handle_out = &input_val;
    EXPECT_LT(H5FDget_vfd_handle(f_full.get(), full_fapl.get(), &handle_out),
              0);
    EXPECT_EQ(nullptr, handle_out);
    state.fail_get_handle = false;
  }
}

TEST(H5FDTest, SuperblockEncodeLoadFaplGetFreeDriverInfoAndTypeMap) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_sb_full", 326);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_sb_min", 327);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  ScopedPlist full_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist min_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(full_fapl.get(), full_drv.get(), nullptr), 0);
  ASSERT_GE(H5Pset_driver(min_fapl.get(), min_drv.get(), nullptr), 0);

  ScopedFdFile f_full(
      H5FDopen("sbf", H5F_ACC_RDWR, full_fapl.get(), HADDR_UNDEF));
  ScopedFdFile f_min(
      H5FDopen("sbm", H5F_ACC_RDWR, min_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(f_full.valid());
  ASSERT_TRUE(f_min.valid());

  // 1. Superblock size / encode / load round-trip and error checks.
  EXPECT_EQ(0ULL, H5FD_sb_size(f_min.get()));
  EXPECT_EQ(8ULL, H5FD_sb_size(f_full.get()));

  std::array<char, 9> sb_name{};
  std::array<uint8_t, 8> sb_buf{};
  EXPECT_GE(H5FD_sb_encode(f_min.get(), sb_name.data(), sb_buf.data()), 0);
  EXPECT_GE(H5FD_sb_load(f_min.get(), "OTHERVFD", sb_buf.data()), 0);

  EXPECT_GE(H5FD_sb_encode(f_full.get(), sb_name.data(), sb_buf.data()), 0);
  EXPECT_STREQ("FAKEVFD0", sb_name.data());
  EXPECT_GE(H5FD_sb_load(f_full.get(), sb_name.data(), sb_buf.data()), 0);
  EXPECT_EQ("FAKEVFD0", state.last_sb_name);
  EXPECT_THAT(state.last_sb_buf,
              ElementsAre(0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7));

  {
    ScopedErrorSilence silence;
    // Family / multi name mismatches on a non-family / non-multi driver.
    EXPECT_LT(H5FD_sb_load(f_full.get(), "NCSAfami", sb_buf.data()), 0);
    EXPECT_LT(H5FD_sb_load(f_full.get(), "NCSAmult", sb_buf.data()), 0);

    state.fail_sb_encode = true;
    EXPECT_LT(H5FD_sb_encode(f_full.get(), sb_name.data(), sb_buf.data()), 0);
    state.fail_sb_encode = false;

    state.fail_sb_decode = true;
    EXPECT_LT(H5FD_sb_load(f_full.get(), "FAKEVFD0", sb_buf.data()), 0);
    state.fail_sb_decode = false;
  }

  // 2. H5FD_fapl_get and H5FD_free_driver_info.
  EXPECT_EQ(nullptr, H5FD_fapl_get(f_min.get()));
  void* drv_info = H5FD_fapl_get(f_full.get());
  ASSERT_NE(nullptr, drv_info);
  EXPECT_EQ(77, *static_cast<const int*>(drv_info));

  // No-op paths for invalid/null inputs.
  EXPECT_GE(H5FD_free_driver_info(H5I_INVALID_HID, drv_info), 0);
  EXPECT_GE(H5FD_free_driver_info(full_drv.get(), nullptr), 0);

  {
    ScopedErrorSilence silence;
    // H5FD_free_driver_info uses H5I_object() without type verification, so use
    // a non-existent positive hid_t rather than a live non-VFL hid_t to hit the
    // NULL == driver check.
    constexpr hid_t kNonExistentDriverId = 0x7ffffff;
    EXPECT_LT(H5FD_free_driver_info(kNonExistentDriverId, drv_info), 0);

    state.fail_fapl_free = true;
    EXPECT_LT(H5FD_free_driver_info(full_drv.get(), drv_info), 0);
    state.fail_fapl_free = false;
  }
  EXPECT_GE(H5FD_free_driver_info(full_drv.get(), drv_info), 0);

  // Core VFD has fapl_get != nullptr and fapl_free == nullptr, so
  // H5FD_free_driver_info falls back to H5MM_xfree_const.
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  ScopedFdFile core_file(H5FDopen("core_fapl_get", H5F_ACC_CREAT | H5F_ACC_RDWR,
                                  core_fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(core_file.valid());
  void* core_info = H5FD_fapl_get(core_file.get());
  ASSERT_NE(nullptr, core_info);
  EXPECT_GE(H5FD_free_driver_info(H5FD_CORE, core_info), 0);

  // 3. H5FD_get_fs_type_map.
  std::array<H5FD_mem_t, H5FD_MEM_NTYPES> type_map{};
  EXPECT_GE(H5FD_get_fs_type_map(f_full.get(), type_map.data()), 0);
  EXPECT_EQ(H5FD_MEM_DRAW, type_map[H5FD_MEM_SUPER]);

  EXPECT_GE(H5FD_get_fs_type_map(f_min.get(), type_map.data()), 0);
  EXPECT_THAT(type_map, ElementsAreArray(min_cls.fl_map));

  {
    ScopedErrorSilence silence;
    state.fail_get_type_map = true;
    EXPECT_LT(H5FD_get_fs_type_map(f_full.get(), type_map.data()), 0);
    state.fail_get_type_map = false;
  }
}

TEST(H5FDTest, DeleteValidationAndCustomDriverDispatch) {
  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);

  const H5FD_class_t full_cls = MakeFullFakeClass("fake_del_full", 328);
  const H5FD_class_t min_cls = MakeMinimalFakeClass("fake_del_min", 329);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ScopedVfdDriver min_drv(H5FDregister(&min_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());
  ASSERT_NE(H5I_INVALID_HID, min_drv.get());

  ScopedPlist full_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist min_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_GE(H5Pset_driver(full_fapl.get(), full_drv.get(), nullptr), 0);
  ASSERT_GE(H5Pset_driver(min_fapl.get(), min_drv.get(), nullptr), 0);

  EXPECT_GE(H5FDdelete("virtual_path.h5", full_fapl.get()), 0);
  EXPECT_EQ(1, state.del_calls);
  EXPECT_EQ("virtual_path.h5", state.last_deleted_name);

  ScopedErrorSilence silence;
  EXPECT_LT(H5FDdelete(nullptr, full_fapl.get()), 0);
  EXPECT_LT(H5FDdelete("", full_fapl.get()), 0);
  EXPECT_LT(H5FDdelete("x.h5", dxpl.get()), 0);
  EXPECT_LT(H5FDdelete("x.h5", min_fapl.get()), 0);

  state.fail_del = true;
  EXPECT_LT(H5FDdelete("virtual_path.h5", full_fapl.get()), 0);
  state.fail_del = false;
}

TEST(H5FDTest, InitTermPackageAndFileLockingEnvVarBranches) {
  const char* prev_env = std::getenv(HDF5_USE_FILE_LOCKING);
  const bool had_env = (prev_env != nullptr);
  const std::string saved_env = had_env ? prev_env : "";

  absl::Cleanup restore = [&] {
    if (had_env) {
      setenv(HDF5_USE_FILE_LOCKING, saved_env.c_str(), 1);
    } else {
      unsetenv(HDF5_USE_FILE_LOCKING);
    }
    H5close();
    H5open();
  };

  auto reinit_and_check = [](const char* value, htri_t expected_flag) {
    ASSERT_GE(H5close(), 0);
    // Calling H5FD_term_package() when already terminated exercises the
    // !H5_PKG_INIT_VAR no-op branch.
    EXPECT_EQ(0, H5FD_term_package());
    if (value != nullptr) {
      ASSERT_EQ(0, setenv(HDF5_USE_FILE_LOCKING, value, 1));
    } else {
      ASSERT_EQ(0, unsetenv(HDF5_USE_FILE_LOCKING));
    }
    ASSERT_GE(H5open(), 0);
    EXPECT_EQ(expected_flag, H5FD_ignore_disabled_file_locks_p);
  };

  reinit_and_check("BEST_EFFORT", 1);
  reinit_and_check("TRUE", 0);
  reinit_and_check("1", 0);
  reinit_and_check("FALSE", -1);
  reinit_and_check(nullptr, -1);

  // Explicitly invoke H5FD_init() and internal H5FD_flush/truncate/lock/unlock/
  // ctl/read/write inside a pushed API context to verify NOAPI wrappers.
  EXPECT_GE(H5FD_init(), 0);

  FakeVfdState state;
  ScopedFakeVfdState state_guard(&state);
  const H5FD_class_t full_cls = MakeFullFakeClass("fake_internal_ctx", 330);
  ScopedVfdDriver full_drv(H5FDregister(&full_cls));
  ASSERT_NE(H5I_INVALID_HID, full_drv.get());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(fapl.get(), full_drv.get(), nullptr), 0);
  ScopedFdFile file(H5FDopen("ctx", H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 32), 0);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  EXPECT_GE(H5FD_flush(file.get(), false), 0);
  EXPECT_GE(H5FD_truncate(file.get(), false), 0);
  EXPECT_GE(H5FD_lock(file.get(), false), 0);
  EXPECT_GE(H5FD_unlock(file.get()), 0);
  EXPECT_GE(H5FD_ctl(file.get(), H5FD_CTL_TEST_OPCODE, 0, nullptr, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
