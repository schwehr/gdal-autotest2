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
// Tests internal Virtual File Driver (VFD) dispatch, signature search, vector
// and selection I/O translation, request sorting, file deletion, and plugin/ID
// lookup helpers (third_party/hdf5/src/H5FDint.c).
//
// Google3 build caveats: compiled non-MPI on Linux with -DNDEBUG and dynamic
// plugins disabled (H5PL__open returns FAIL).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDint.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5PLprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5Sprivate.h"
}  // extern "C"

#ifdef H5FDINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5FD_class_value_t kMockVfdValueBase = 380;
constexpr haddr_t kDefaultMaxAddr = 1024 * 1024;

// Pushes and pops an HDF5 API context so internal H5FD_* helpers that touch
// H5CX (DXPL tracking, actual_selection_io_mode, no_selection_io_cause) run
// with a valid thread-local context and optionally propagate updates back to
// dxpl_id.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            bool update_dxpl_props = true)
      : update_dxpl_props_(update_dxpl_props) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
    }
  }

  ~ScopedApiContext() { pop(); }

  void pop() {
    if (pushed_) {
      H5CX_pop(update_dxpl_props_);
      pushed_ = false;
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool update_dxpl_props_ = true;
  bool pushed_ = false;
};

// Mutable state shared between a test and an in-memory mock VFD instance.
struct MockVfdState {
  std::vector<uint8_t> storage;
  haddr_t eoa = 0;

  bool fail_get_eoa = false;
  bool fail_set_eoa = false;
  int fail_set_eoa_on_call = 0;  // 1-indexed if > 0.
  bool fail_get_eof = false;
  bool fail_read = false;
  int fail_read_on_call = 0;  // 1-indexed if > 0.
  bool fail_write = false;
  int fail_write_on_call = 0;  // 1-indexed if > 0.
  bool fail_read_vector = false;
  bool fail_write_vector = false;
  bool fail_read_selection = false;
  bool fail_write_selection = false;
  bool fail_del = false;
  bool query_sets_swmr = true;

  int get_eoa_calls = 0;
  int set_eoa_calls = 0;
  int get_eof_calls = 0;
  int read_calls = 0;
  int write_calls = 0;
  int read_vector_calls = 0;
  int write_vector_calls = 0;
  int read_selection_calls = 0;
  int write_selection_calls = 0;
  int del_calls = 0;

  std::vector<haddr_t> observed_read_addrs;
  std::vector<haddr_t> observed_write_addrs;
  std::vector<haddr_t> observed_vector_addrs;
  std::vector<size_t> observed_vector_sizes;
  std::vector<haddr_t> observed_selection_offsets;
  uint32_t last_vector_count = 0;
  uint32_t last_selection_count = 0;
};

struct MockFaplInfo {
  MockVfdState* state = nullptr;
};

// Standard-layout file struct compatible with C H5FD_t pointer conversion.
struct MockVfdFile {
  H5FD_t pub{};
  MockVfdState* state = nullptr;
};

static_assert(std::is_standard_layout_v<MockVfdFile>);
static_assert(offsetof(MockVfdFile, pub) == 0);

MockVfdFile* AsMockFile(H5FD_t* f) { return reinterpret_cast<MockVfdFile*>(f); }
const MockVfdFile* AsMockFile(const H5FD_t* f) {
  return reinterpret_cast<const MockVfdFile*>(f);
}

void StorageRead(const MockVfdState& st, haddr_t addr, size_t size, void* buf) {
  auto* dst = static_cast<uint8_t*>(buf);
  std::fill_n(dst, size, uint8_t{0});
  if (addr < st.storage.size()) {
    const size_t avail = st.storage.size() - static_cast<size_t>(addr);
    const size_t n = std::min(size, avail);
    std::memcpy(dst, st.storage.data() + static_cast<size_t>(addr), n);
  }
}

void StorageWrite(MockVfdState* st, haddr_t addr, size_t size,
                  const void* buf) {
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > st->storage.size()) {
    st->storage.resize(end, uint8_t{0});
  }
  std::memcpy(st->storage.data() + static_cast<size_t>(addr), buf, size);
}

H5FD_t* MockOpen(const char* /*name*/, unsigned /*flags*/, hid_t fapl_id,
                 haddr_t maxaddr) {
  auto* mf = new MockVfdFile{};
  if (fapl_id != H5P_DEFAULT) {
    const auto* info =
        static_cast<const MockFaplInfo*>(H5Pget_driver_info(fapl_id));
    if (info != nullptr) {
      mf->state = info->state;
    }
  }
  mf->pub.maxaddr = maxaddr;
  return reinterpret_cast<H5FD_t*>(mf);
}

herr_t MockClose(H5FD_t* f) {
  delete AsMockFile(f);
  return 0;
}

herr_t MockQuery(const H5FD_t* f, unsigned long* flags) {
  if (f != nullptr) {
    const MockVfdFile* mf = AsMockFile(f);
    if (mf->state != nullptr && !mf->state->query_sets_swmr) {
      *flags = 0;
      return 0;
    }
  }
  *flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
  return 0;
}

haddr_t MockGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return HADDR_UNDEF;
  }
  ++mf->state->get_eoa_calls;
  if (mf->state->fail_get_eoa) {
    return HADDR_UNDEF;
  }
  return mf->state->eoa;
}

herr_t MockSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->set_eoa_calls;
  if (mf->state->fail_set_eoa ||
      (mf->state->fail_set_eoa_on_call > 0 &&
       mf->state->set_eoa_calls == mf->state->fail_set_eoa_on_call)) {
    return -1;
  }
  mf->state->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return HADDR_UNDEF;
  }
  ++mf->state->get_eof_calls;
  if (mf->state->fail_get_eof) {
    return HADDR_UNDEF;
  }
  return static_cast<haddr_t>(mf->state->storage.size());
}

herr_t MockRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/, haddr_t addr,
                size_t size, void* buf) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->read_calls;
  mf->state->observed_read_addrs.push_back(addr);
  if (mf->state->fail_read ||
      (mf->state->fail_read_on_call > 0 &&
       mf->state->read_calls == mf->state->fail_read_on_call)) {
    return -1;
  }
  StorageRead(*mf->state, addr, size, buf);
  return 0;
}

herr_t MockWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/, haddr_t addr,
                 size_t size, const void* buf) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->write_calls;
  mf->state->observed_write_addrs.push_back(addr);
  if (mf->state->fail_write ||
      (mf->state->fail_write_on_call > 0 &&
       mf->state->write_calls == mf->state->fail_write_on_call)) {
    return -1;
  }
  StorageWrite(mf->state, addr, size, buf);
  return 0;
}

herr_t MockReadVector(H5FD_t* f, hid_t /*dxpl*/, uint32_t count,
                      H5FD_mem_t /*types*/[], haddr_t addrs[], size_t sizes[],
                      void* bufs[]) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->read_vector_calls;
  mf->state->last_vector_count = count;
  mf->state->observed_vector_addrs.assign(addrs, addrs + count);
  mf->state->observed_vector_sizes.assign(sizes, sizes + count);
  if (mf->state->fail_read_vector) {
    return -1;
  }
  size_t cur_size = 0;
  bool extend_sizes = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!extend_sizes) {
      if (sizes[i] == 0) {
        extend_sizes = true;
        cur_size = sizes[i - 1];
      } else {
        cur_size = sizes[i];
      }
    }
    StorageRead(*mf->state, addrs[i], cur_size, bufs[i]);
  }
  return 0;
}

herr_t MockWriteVector(H5FD_t* f, hid_t /*dxpl*/, uint32_t count,
                       H5FD_mem_t /*types*/[], haddr_t addrs[], size_t sizes[],
                       const void* bufs[]) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->write_vector_calls;
  mf->state->last_vector_count = count;
  mf->state->observed_vector_addrs.assign(addrs, addrs + count);
  mf->state->observed_vector_sizes.assign(sizes, sizes + count);
  if (mf->state->fail_write_vector) {
    return -1;
  }
  size_t cur_size = 0;
  bool extend_sizes = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (!extend_sizes) {
      if (sizes[i] == 0) {
        extend_sizes = true;
        cur_size = sizes[i - 1];
      } else {
        cur_size = sizes[i];
      }
    }
    StorageWrite(mf->state, addrs[i], cur_size, bufs[i]);
  }
  return 0;
}

herr_t MockReadSelection(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                         size_t count, hid_t mem_spaces[], hid_t file_spaces[],
                         haddr_t offsets[], size_t element_sizes[],
                         void* bufs[]) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->read_selection_calls;
  mf->state->last_selection_count = static_cast<uint32_t>(count);
  mf->state->observed_selection_offsets.assign(offsets, offsets + count);
  if (mf->state->fail_read_selection) {
    return -1;
  }
  size_t elem_sz = 0;
  void* buf = nullptr;
  bool extend_sizes = false;
  bool extend_bufs = false;
  for (size_t i = 0; i < count; ++i) {
    if (!extend_sizes) {
      if (element_sizes[i] == 0) {
        extend_sizes = true;
        elem_sz = element_sizes[i - 1];
      } else {
        elem_sz = element_sizes[i];
      }
    }
    if (!extend_bufs) {
      if (bufs[i] == nullptr) {
        extend_bufs = true;
        buf = bufs[i - 1];
      } else {
        buf = bufs[i];
      }
    }
    const hssize_t mem_pts = H5Sget_select_npoints(mem_spaces[i]);
    const hssize_t file_pts = H5Sget_select_npoints(file_spaces[i]);
    if (mem_pts < 0 || file_pts < 0 || mem_pts != file_pts) {
      return -1;
    }
    StorageRead(*mf->state, offsets[i], static_cast<size_t>(file_pts) * elem_sz,
                buf);
  }
  return 0;
}

herr_t MockWriteSelection(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                          size_t count, hid_t mem_spaces[], hid_t file_spaces[],
                          haddr_t offsets[], size_t element_sizes[],
                          const void* bufs[]) {
  MockVfdFile* mf = AsMockFile(f);
  if (mf->state == nullptr) {
    return -1;
  }
  ++mf->state->write_selection_calls;
  mf->state->last_selection_count = static_cast<uint32_t>(count);
  mf->state->observed_selection_offsets.assign(offsets, offsets + count);
  if (mf->state->fail_write_selection) {
    return -1;
  }
  size_t elem_sz = 0;
  const void* buf = nullptr;
  bool extend_sizes = false;
  bool extend_bufs = false;
  for (size_t i = 0; i < count; ++i) {
    if (!extend_sizes) {
      if (element_sizes[i] == 0) {
        extend_sizes = true;
        elem_sz = element_sizes[i - 1];
      } else {
        elem_sz = element_sizes[i];
      }
    }
    if (!extend_bufs) {
      if (bufs[i] == nullptr) {
        extend_bufs = true;
        buf = bufs[i - 1];
      } else {
        buf = bufs[i];
      }
    }
    const hssize_t mem_pts = H5Sget_select_npoints(mem_spaces[i]);
    const hssize_t file_pts = H5Sget_select_npoints(file_spaces[i]);
    if (mem_pts < 0 || file_pts < 0 || mem_pts != file_pts) {
      return -1;
    }
    StorageWrite(mf->state, offsets[i], static_cast<size_t>(file_pts) * elem_sz,
                 buf);
  }
  return 0;
}

herr_t MockDelete(const char* /*name*/, hid_t fapl_id) {
  if (fapl_id != H5P_DEFAULT) {
    const auto* info =
        static_cast<const MockFaplInfo*>(H5Pget_driver_info(fapl_id));
    if (info != nullptr && info->state != nullptr) {
      ++info->state->del_calls;
      if (info->state->fail_del) {
        return -1;
      }
    }
  }
  return 0;
}

enum class MockVfdMode {
  kSelectionVectorScalar,
  kVectorAndScalar,
  kScalarOnly,
  kMinimalNoQueryNoDel,
};

H5FD_class_t MakeMockVfdClass(MockVfdMode mode, H5FD_class_value_t value,
                              const char* name) {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = value;
  cls.name = name;
  cls.maxaddr = kDefaultMaxAddr;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.fapl_size = sizeof(MockFaplInfo);
  cls.open = MockOpen;
  cls.close = MockClose;
  cls.get_eoa = MockGetEoa;
  cls.set_eoa = MockSetEoa;
  cls.get_eof = MockGetEof;
  cls.read = MockRead;
  cls.write = MockWrite;
  if (mode != MockVfdMode::kMinimalNoQueryNoDel) {
    cls.query = MockQuery;
    cls.del = MockDelete;
  }
  if (mode == MockVfdMode::kSelectionVectorScalar ||
      mode == MockVfdMode::kVectorAndScalar) {
    cls.read_vector = MockReadVector;
    cls.write_vector = MockWriteVector;
  }
  if (mode == MockVfdMode::kSelectionVectorScalar) {
    cls.read_selection = MockReadSelection;
    cls.write_selection = MockWriteSelection;
  }
  const H5FD_mem_t fl_map[H5FD_MEM_NTYPES] = H5FD_FLMAP_SINGLE;
  std::memcpy(cls.fl_map, fl_map, sizeof(fl_map));
  return cls;
}

// RAII handle registering a mock VFD class, configuring a FAPL, and opening
// an H5FD_t* in-memory mock file.
class ScopedMockVfdFile {
 public:
  ScopedMockVfdFile(MockVfdMode mode, MockVfdState* state,
                    H5FD_class_value_t value = kMockVfdValueBase,
                    const char* name = "mock_h5fdint_vfd") {
    cls_ = MakeMockVfdClass(mode, value, name);
    driver_id_ = H5FDregister(&cls_);
    if (driver_id_ < 0) {
      return;
    }
    fapl_.reset(H5Pcreate(H5P_FILE_ACCESS));
    if (!fapl_.valid()) {
      return;
    }
    MockFaplInfo info{state};
    if (H5Pset_driver(fapl_.get(), driver_id_, &info) < 0) {
      return;
    }
    file_ = H5FDopen("mock_mem_file", H5F_ACC_RDWR | H5F_ACC_CREAT, fapl_.get(),
                     kDefaultMaxAddr);
  }

  ~ScopedMockVfdFile() {
    if (file_ != nullptr) {
      H5FDclose(file_);
      file_ = nullptr;
    }
    fapl_.reset();
    if (driver_id_ >= 0) {
      H5FDunregister(driver_id_);
      driver_id_ = H5I_INVALID_HID;
    }
  }

  ScopedMockVfdFile(const ScopedMockVfdFile&) = delete;
  ScopedMockVfdFile& operator=(const ScopedMockVfdFile&) = delete;

  H5FD_t* file() const { return file_; }
  hid_t driver_id() const { return driver_id_; }
  hid_t fapl_id() const { return fapl_.get(); }
  const H5FD_class_t* cls() const { return &cls_; }

 private:
  H5FD_class_t cls_{};
  hid_t driver_id_ = H5I_INVALID_HID;
  ScopedPlist fapl_;
  H5FD_t* file_ = nullptr;
};

H5S_t* SpacePtr(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

TEST(H5FDintTest, LocateSignatureFindsAtZeroAndPowerOfTwo) {
  MockVfdState state;
  state.storage.assign(4096, uint8_t{0});
  state.eoa = 4096;
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Signature at offset 0.
  std::memcpy(state.storage.data(), H5F_SIGNATURE, H5F_SIGNATURE_LEN);
  haddr_t sig_addr = HADDR_UNDEF;
  ASSERT_GE(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  EXPECT_EQ(sig_addr, 0u);

  // 2. Signature at power-of-two offset 1024 (after zeros at 0 and 512).
  std::fill_n(state.storage.data(), H5F_SIGNATURE_LEN, uint8_t{0});
  std::memcpy(state.storage.data() + 1024, H5F_SIGNATURE, H5F_SIGNATURE_LEN);
  sig_addr = HADDR_UNDEF;
  ASSERT_GE(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  EXPECT_EQ(sig_addr, 1024u);

  // 3. No signature present: resets EOA to saved value and returns HADDR_UNDEF.
  std::fill(state.storage.begin(), state.storage.end(), uint8_t{0});
  state.storage.resize(1024);
  state.eoa = 64;
  sig_addr = 123;
  ASSERT_GE(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  EXPECT_EQ(sig_addr, HADDR_UNDEF);
  EXPECT_EQ(state.eoa, 64u);
}

TEST(H5FDintTest, LocateSignatureErrorPaths) {
  MockVfdState state;
  state.storage.assign(100, uint8_t{0});
  state.eoa = 50;
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);

  ScopedErrorSilence silence;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  haddr_t sig_addr = 0;

  // 1. Both EOF and EOA undefined -> cannot obtain EOF/EOA value.
  state.fail_get_eof = true;
  state.fail_get_eoa = true;
  EXPECT_LT(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  state.fail_get_eof = false;
  state.fail_get_eoa = false;

  // 2. set_eoa fails during search loop.
  state.fail_set_eoa = true;
  EXPECT_LT(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  state.fail_set_eoa = false;

  // 3. read fails during search loop.
  state.fail_read = true;
  EXPECT_LT(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  state.fail_read = false;

  // 4. With size 100 (maxpow=9, single iteration n=8), set_eoa succeeds on
  // call 1 in loop, then fails on call 2 when resetting EOA.
  state.set_eoa_calls = 0;
  state.fail_set_eoa_on_call = 2;
  EXPECT_LT(H5FD_locate_signature(vfd.file(), &sig_addr), 0);
  state.fail_set_eoa_on_call = 0;
}

TEST(H5FDintTest, ScalarReadWriteCoversBaseAddrSwmrAndErrors) {
  MockVfdState state;
  state.eoa = 1024;
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  const std::array<uint8_t, 4> kPattern = {0x12, 0x34, 0x56, 0x78};
  std::array<uint8_t, 4> out{};

  // 1. Zero-size no-op read/write.
  EXPECT_GE(
      H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 0, kPattern.data()),
      0);
  EXPECT_GE(H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 0, out.data()),
            0);
  EXPECT_EQ(state.write_calls, 0);
  EXPECT_EQ(state.read_calls, 0);

  // 2. Raw data write + read sets H5D_SCALAR_IO on DXPL.
  ASSERT_GE(H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 16,
                      kPattern.size(), kPattern.data()),
            0);
  uint32_t actual_mode = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode & H5D_SCALAR_IO, 0u);

  ASSERT_GE(H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 16, out.size(),
                     out.data()),
            0);
  EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x78));

  // 3. Metadata (non-DRAW) write/read clears raw selection-IO tracking.
  ASSERT_GE(H5FDwrite(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 32,
                      kPattern.size(), kPattern.data()),
            0);
  actual_mode = 0xff;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_EQ(actual_mode, 0u);

  ASSERT_GE(H5FDread(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 32, out.size(),
                     out.data()),
            0);
  EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x78));

  // 4. base_addr shifts physical driver address in H5FD_write/H5FD_read, while
  // public H5FDwrite/H5FDread subtract base_addr before calling the internal
  // helper.
  ASSERT_GE(H5FD_set_base_addr(vfd.file(), 64), 0);
  EXPECT_EQ(H5FD_get_base_addr(vfd.file()), 64u);
  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5FD_write(vfd.file(), H5FD_MEM_DRAW, 10, kPattern.size(),
                         kPattern.data()),
              0);
    EXPECT_EQ(state.observed_write_addrs.back(), 74u);

    out.fill(0);
    ASSERT_GE(H5FD_read(vfd.file(), H5FD_MEM_DRAW, 10, out.size(), out.data()),
              0);
    EXPECT_EQ(state.observed_read_addrs.back(), 74u);
    EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x78));
  }
  ASSERT_GE(H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 80,
                      kPattern.size(), kPattern.data()),
            0);
  EXPECT_EQ(state.observed_write_addrs.back(), 80u);
  out.fill(0);
  ASSERT_GE(H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 80, out.size(),
                     out.data()),
            0);
  EXPECT_THAT(out, ElementsAre(0x12, 0x34, 0x56, 0x78));

  // 5. SWMR read skips get_eoa / overflow check even past EOA.
  vfd.file()->access_flags |= H5F_ACC_SWMR_READ;
  const int eoa_before = state.get_eoa_calls;
  ASSERT_GE(
      H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 2000, 4, out.data()), 0);
  EXPECT_EQ(state.get_eoa_calls, eoa_before);
  vfd.file()->access_flags &= ~static_cast<unsigned>(H5F_ACC_SWMR_READ);

  // 6. Error branches: EOA failure, overflow, and driver read/write failures.
  ScopedErrorSilence silence;
  state.fail_get_eoa = true;
  EXPECT_LT(H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 4, out.data()),
            0);
  EXPECT_LT(
      H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 4, kPattern.data()),
      0);
  state.fail_get_eoa = false;

  EXPECT_LT(
      H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 2000, 4, out.data()), 0);
  EXPECT_LT(H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 2000, 4,
                      kPattern.data()),
            0);

  state.fail_read = true;
  EXPECT_LT(H5FDread(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 4, out.data()),
            0);
  state.fail_read = false;

  state.fail_write = true;
  EXPECT_LT(
      H5FDwrite(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 0, 4, kPattern.data()),
      0);
  state.fail_write = false;
}

TEST(H5FDintTest, VectorReadWriteDirectAndBaseAddrRestore) {
  MockVfdState state;
  state.eoa = 4096;
  ScopedMockVfdFile vfd(MockVfdMode::kVectorAndScalar, &state);
  ASSERT_NE(vfd.file(), nullptr);
  ASSERT_GE(H5FD_set_base_addr(vfd.file(), 128), 0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  // 1. Zero-count vector calls are no-ops.
  EXPECT_GE(H5FDwrite_vector(vfd.file(), dxpl.get(), 0, nullptr, nullptr,
                             nullptr, nullptr),
            0);
  EXPECT_GE(H5FDread_vector(vfd.file(), dxpl.get(), 0, nullptr, nullptr,
                            nullptr, nullptr),
            0);

  // 2. Extend sizes/types with raw data present.
  std::array<uint8_t, 4> w0 = {1, 2, 3, 4};
  std::array<uint8_t, 4> w1 = {5, 6, 7, 8};
  std::array<uint8_t, 4> w2 = {9, 10, 11, 12};
  std::array<H5FD_mem_t, 3> types = {H5FD_MEM_SUPER, H5FD_MEM_DRAW,
                                     H5FD_MEM_NOLIST};
  std::array<haddr_t, 3> addrs = {10, 40, 80};
  std::array<size_t, 3> sizes = {4, 0, 0};
  std::array<const void*, 3> wbufs = {w0.data(), w1.data(), w2.data()};

  ASSERT_GE(H5FDwrite_vector(vfd.file(), dxpl.get(), 3, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  EXPECT_THAT(state.observed_vector_addrs, ElementsAre(138u, 168u, 208u));

  uint32_t actual_mode = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode & H5D_VECTOR_IO, 0u);

  std::array<uint8_t, 4> r0{};
  std::array<uint8_t, 4> r1{};
  std::array<uint8_t, 4> r2{};
  std::array<void*, 3> rbufs = {r0.data(), r1.data(), r2.data()};
  ASSERT_GE(H5FDread_vector(vfd.file(), dxpl.get(), 3, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  EXPECT_THAT(r0, ElementsAre(1, 2, 3, 4));
  EXPECT_THAT(r1, ElementsAre(5, 6, 7, 8));
  EXPECT_THAT(r2, ElementsAre(9, 10, 11, 12));

  // 3. Metadata-only SWMR read_vector (no DRAW) and SWMR read_vector with DRAW.
  vfd.file()->access_flags |= H5F_ACC_SWMR_READ;
  std::array<H5FD_mem_t, 2> meta_types = {H5FD_MEM_SUPER, H5FD_MEM_NOLIST};
  std::array<haddr_t, 2> swmr_addrs = {10, 5000};
  std::array<size_t, 2> swmr_sizes = {4, 4};
  std::array<void*, 2> swmr_rbufs = {r0.data(), r1.data()};
  ASSERT_GE(
      H5FDread_vector(vfd.file(), dxpl.get(), 2, meta_types.data(),
                      swmr_addrs.data(), swmr_sizes.data(), swmr_rbufs.data()),
      0);
  EXPECT_THAT(swmr_addrs, ElementsAre(10u, 5000u));
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_EQ(actual_mode, 0u);

  std::array<H5FD_mem_t, 2> swmr_raw_types = {H5FD_MEM_SUPER, H5FD_MEM_DRAW};
  ASSERT_GE(
      H5FDread_vector(vfd.file(), dxpl.get(), 2, swmr_raw_types.data(),
                      swmr_addrs.data(), swmr_sizes.data(), swmr_rbufs.data()),
      0);
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode & H5D_VECTOR_IO, 0u);
  vfd.file()->access_flags &= ~static_cast<unsigned>(H5F_ACC_SWMR_READ);

  // 4. Error branches verify addrs are restored even on failure.
  ScopedErrorSilence silence;
  state.fail_get_eoa = true;
  EXPECT_LT(H5FDread_vector(vfd.file(), dxpl.get(), 3, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  EXPECT_LT(H5FDwrite_vector(vfd.file(), dxpl.get(), 3, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  state.fail_get_eoa = false;

  std::array<haddr_t, 2> oob_addrs = {10, 4090};
  EXPECT_LT(
      H5FDread_vector(vfd.file(), dxpl.get(), 2, swmr_raw_types.data(),
                      oob_addrs.data(), swmr_sizes.data(), swmr_rbufs.data()),
      0);
  EXPECT_THAT(oob_addrs, ElementsAre(10u, 4090u));
  EXPECT_LT(H5FDwrite_vector(vfd.file(), dxpl.get(), 2, swmr_raw_types.data(),
                             oob_addrs.data(), swmr_sizes.data(), wbufs.data()),
            0);
  EXPECT_THAT(oob_addrs, ElementsAre(10u, 4090u));

  state.fail_read_vector = true;
  EXPECT_LT(H5FDread_vector(vfd.file(), dxpl.get(), 3, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  state.fail_read_vector = false;

  state.fail_write_vector = true;
  EXPECT_LT(H5FDwrite_vector(vfd.file(), dxpl.get(), 3, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  EXPECT_THAT(addrs, ElementsAre(10u, 40u, 80u));
  state.fail_write_vector = false;
}

TEST(H5FDintTest, VectorReadWriteScalarFallback) {
  MockVfdState state;
  state.eoa = 1024;
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  std::array<uint8_t, 4> w0 = {21, 22, 23, 24};
  std::array<uint8_t, 4> w1 = {31, 32, 33, 34};
  std::array<H5FD_mem_t, 2> types = {H5FD_MEM_DRAW, H5FD_MEM_NOLIST};
  std::array<haddr_t, 2> addrs = {0, 32};
  std::array<size_t, 2> sizes = {4, 0};
  std::array<const void*, 2> wbufs = {w0.data(), w1.data()};

  ASSERT_GE(H5FDwrite_vector(vfd.file(), dxpl.get(), 2, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  EXPECT_EQ(state.write_calls, 2);

  uint32_t actual_mode = 0;
  uint32_t no_cause = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &no_cause), 0);
  EXPECT_NE(actual_mode & H5D_SCALAR_IO, 0u);
  EXPECT_NE(no_cause & H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);

  std::array<uint8_t, 4> r0{};
  std::array<uint8_t, 4> r1{};
  std::array<void*, 2> rbufs = {r0.data(), r1.data()};
  std::array<H5FD_mem_t, 3> explicit_types = {H5FD_MEM_SUPER, H5FD_MEM_DRAW,
                                              H5FD_MEM_NOLIST};
  std::array<haddr_t, 3> explicit_addrs = {0, 32, 0};
  std::array<size_t, 3> explicit_sizes = {4, 4, 0};
  std::array<void*, 3> explicit_rbufs = {r0.data(), r1.data(), r0.data()};
  ASSERT_GE(H5FDread_vector(vfd.file(), dxpl.get(), 3, explicit_types.data(),
                            explicit_addrs.data(), explicit_sizes.data(),
                            explicit_rbufs.data()),
            0);
  EXPECT_EQ(state.read_calls, 3);
  EXPECT_THAT(r0, ElementsAre(21, 22, 23, 24));
  EXPECT_THAT(r1, ElementsAre(31, 32, 33, 34));

  // Metadata-only scalar fallback omits H5D_SCALAR_IO flag.
  std::array<H5FD_mem_t, 2> meta_types = {H5FD_MEM_SUPER, H5FD_MEM_NOLIST};
  ASSERT_GE(H5FDwrite_vector(vfd.file(), dxpl.get(), 2, meta_types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_vector(vfd.file(), dxpl.get(), 2, meta_types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);

  // Driver scalar callback failure inside loop.
  ScopedErrorSilence silence;
  state.fail_read_on_call = state.read_calls + 2;
  EXPECT_LT(H5FDread_vector(vfd.file(), dxpl.get(), 2, types.data(),
                            addrs.data(), sizes.data(), rbufs.data()),
            0);
  state.fail_read_on_call = 0;

  state.fail_write_on_call = state.write_calls + 2;
  EXPECT_LT(H5FDwrite_vector(vfd.file(), dxpl.get(), 2, types.data(),
                             addrs.data(), sizes.data(), wbufs.data()),
            0);
  state.fail_write_on_call = 0;
}

TEST(H5FDintTest, SelectionReadWriteDirectLargeCountAndErrors) {
  MockVfdState state;
  state.eoa = 8192;
  ScopedMockVfdFile vfd(MockVfdMode::kSelectionVectorScalar, &state);
  ASSERT_NE(vfd.file(), nullptr);
  ASSERT_GE(H5FD_set_base_addr(vfd.file(), 32), 0);

  // Create 10 (> H5FD_LOCAL_SEL_ARR_LEN == 8) dataspace pairs to exercise
  // dynamic ID array allocation in H5FD_read_selection / H5FD_write_selection.
  constexpr uint32_t kCount = 10;
  constexpr hsize_t kDim = 2;
  std::vector<ScopedSpace> mem_spaces;
  std::vector<ScopedSpace> file_spaces;
  std::array<H5S_t*, kCount> mem_ptrs{};
  std::array<H5S_t*, kCount> file_ptrs{};
  std::array<hid_t, kCount> mem_ids{};
  std::array<hid_t, kCount> file_ids{};
  std::array<haddr_t, kCount> offsets{};
  std::array<size_t, kCount> elem_sizes{};
  std::array<std::array<uint32_t, 2>, kCount> wdata{};
  std::array<std::array<uint32_t, 2>, kCount> rdata{};
  std::array<const void*, kCount> wbufs{};
  std::array<void*, kCount> rbufs{};

  for (uint32_t i = 0; i < kCount; ++i) {
    mem_spaces.emplace_back(H5Screate_simple(1, &kDim, nullptr));
    file_spaces.emplace_back(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(mem_spaces.back().valid());
    ASSERT_TRUE(file_spaces.back().valid());
    mem_ids[i] = mem_spaces.back().get();
    file_ids[i] = file_spaces.back().get();
    mem_ptrs[i] = SpacePtr(mem_ids[i]);
    file_ptrs[i] = SpacePtr(file_ids[i]);
    offsets[i] = i * 64;
    elem_sizes[i] = sizeof(uint32_t);
    wdata[i] = {100 + i, 200 + i};
    wbufs[i] = wdata[i].data();
    rbufs[i] = rdata[i].data();
  }

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());

    // 1. Zero-count internal H5FD_read_selection / H5FD_write_selection.
    EXPECT_GE(H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 0, nullptr,
                                   nullptr, nullptr, nullptr, nullptr),
              0);
    EXPECT_GE(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 0, nullptr,
                                  nullptr, nullptr, nullptr, nullptr),
              0);

    // 2. count > 8 direct selection callback path with raw data + base_addr.
    ASSERT_GE(
        H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, kCount, mem_ptrs.data(),
                             file_ptrs.data(), offsets.data(),
                             elem_sizes.data(), wbufs.data()),
        0);
    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(state.observed_selection_offsets[0], 32u);

    ASSERT_GE(
        H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, kCount, mem_ptrs.data(),
                            file_ptrs.data(), offsets.data(), elem_sizes.data(),
                            rbufs.data()),
        0);
    EXPECT_EQ(offsets[0], 0u);
    for (uint32_t i = 0; i < kCount; ++i) {
      EXPECT_THAT(rdata[i], ElementsAre(100 + i, 200 + i));
    }
  }
  uint32_t actual_mode = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode & H5D_SELECTION_IO, 0u);

  // 3. Public H5FDwrite_selection / H5FDread_selection with count = 0 and 2.
  EXPECT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 0,
                                nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  EXPECT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 0,
                               nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 2,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), wbufs.data()),
            0);
  ASSERT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 2,
                               mem_ids.data(), file_ids.data(), offsets.data(),
                               elem_sizes.data(), rbufs.data()),
            0);

  // 4. Non-DRAW metadata selection does not set H5D_SELECTION_IO.
  ASSERT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), wbufs.data()),
            0);
  actual_mode = 0xff;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_EQ(actual_mode, 0u);

  // 5. SWMR flag bypasses get_eoa/overflow in H5FD_read_selection and _id.
  vfd.file()->access_flags |= H5F_ACC_SWMR_READ;
  std::array<haddr_t, 1> swmr_off = {16000};
  const int eoa_before = state.get_eoa_calls;
  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_GE(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                                  file_ptrs.data(), swmr_off.data(),
                                  elem_sizes.data(), rbufs.data()),
              0);
  }
  ASSERT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                               mem_ids.data(), file_ids.data(), swmr_off.data(),
                               elem_sizes.data(), rbufs.data()),
            0);
  EXPECT_EQ(swmr_off[0], 16000u);
  EXPECT_EQ(state.get_eoa_calls, eoa_before);
  vfd.file()->access_flags &= ~static_cast<unsigned>(H5F_ACC_SWMR_READ);

  // 6. Error branches for H5FD_read/write_selection and _id.
  ScopedErrorSilence silence;
  {
    ScopedApiContext ctx(dxpl.get());
    state.fail_get_eoa = true;
    EXPECT_LT(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                                  file_ptrs.data(), offsets.data(),
                                  elem_sizes.data(), rbufs.data()),
              0);
    EXPECT_LT(
        H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                             file_ptrs.data(), offsets.data(),
                             elem_sizes.data(), wbufs.data()),
        0);
    state.fail_get_eoa = false;

    std::array<haddr_t, 1> oob = {9000};
    EXPECT_LT(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                                  file_ptrs.data(), oob.data(),
                                  elem_sizes.data(), rbufs.data()),
              0);
    EXPECT_EQ(oob[0], 9000u);
    EXPECT_LT(H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 1,
                                   mem_ptrs.data(), file_ptrs.data(),
                                   oob.data(), elem_sizes.data(), wbufs.data()),
              0);
    EXPECT_EQ(oob[0], 9000u);

    state.fail_read_selection = true;
    EXPECT_LT(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                                  file_ptrs.data(), offsets.data(),
                                  elem_sizes.data(), rbufs.data()),
              0);
    state.fail_read_selection = false;

    state.fail_write_selection = true;
    EXPECT_LT(
        H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 1, mem_ptrs.data(),
                             file_ptrs.data(), offsets.data(),
                             elem_sizes.data(), wbufs.data()),
        0);
    state.fail_write_selection = false;
  }

  state.fail_get_eoa = true;
  EXPECT_LT(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                               mem_ids.data(), file_ids.data(), offsets.data(),
                               elem_sizes.data(), rbufs.data()),
            0);
  EXPECT_LT(H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), wbufs.data()),
            0);
  state.fail_get_eoa = false;

  std::array<haddr_t, 1> oob_id = {9000};
  EXPECT_LT(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                               mem_ids.data(), file_ids.data(), oob_id.data(),
                               elem_sizes.data(), rbufs.data()),
            0);
  EXPECT_LT(H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                                mem_ids.data(), file_ids.data(), oob_id.data(),
                                elem_sizes.data(), wbufs.data()),
            0);

  state.fail_read_selection = true;
  EXPECT_LT(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                               mem_ids.data(), file_ids.data(), offsets.data(),
                               elem_sizes.data(), rbufs.data()),
            0);
  state.fail_read_selection = false;

  state.fail_write_selection = true;
  EXPECT_LT(H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                                mem_ids.data(), file_ids.data(), offsets.data(),
                                elem_sizes.data(), wbufs.data()),
            0);
  state.fail_write_selection = false;
}

TEST(H5FDintTest, SelectionTranslationVectorGrowthRefillAsymmetryAndErrors) {
  MockVfdState state;
  state.eoa = 65536;
  ScopedMockVfdFile vfd(MockVfdMode::kVectorAndScalar, &state);
  ASSERT_NE(vfd.file(), nullptr);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  // 1. Over-128-sequence strided hyperslab triggers:
  //   - >8 vector array H5MM_malloc growth,
  //   - >16 vector array H5MM_realloc doubling,
  //   - >128 (H5FD_SEQ_LIST_LEN) file and memory sequence list refill.
  constexpr hsize_t kTotalLen = 280;
  constexpr hsize_t kStridedCount = 140;
  constexpr hsize_t kStart = 0;
  constexpr hsize_t kStride = 2;
  constexpr hsize_t kOne = 1;
  ScopedSpace strided_file(H5Screate_simple(1, &kTotalLen, nullptr));
  ScopedSpace strided_mem(H5Screate_simple(1, &kTotalLen, nullptr));
  ASSERT_GE(H5Sselect_hyperslab(strided_file.get(), H5S_SELECT_SET, &kStart,
                                &kStride, &kStridedCount, &kOne),
            0);
  ASSERT_GE(H5Sselect_hyperslab(strided_mem.get(), H5S_SELECT_SET, &kStart,
                                &kStride, &kStridedCount, &kOne),
            0);

  std::vector<uint32_t> strided_w(kTotalLen, 0);
  for (hsize_t i = 0; i < kTotalLen; i += 2) {
    strided_w[i] = static_cast<uint32_t>(i + 10);
  }
  std::vector<uint32_t> strided_r(kTotalLen, 0xdeadbeefu);

  haddr_t off = 0;
  size_t esize = sizeof(uint32_t);
  const void* wptr = strided_w.data();
  void* rptr = strided_r.data();

  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());
    H5S_t* m_ptr = SpacePtr(strided_mem.get());
    H5S_t* f_ptr = SpacePtr(strided_file.get());
    ASSERT_GE(H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 1, &m_ptr, &f_ptr,
                                   &off, &esize, &wptr),
              0);
    EXPECT_EQ(state.last_vector_count, kStridedCount);

    ASSERT_GE(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, &m_ptr, &f_ptr,
                                  &off, &esize, &rptr),
              0);
    EXPECT_EQ(state.last_vector_count, kStridedCount);
  }
  for (hsize_t i = 0; i < kTotalLen; ++i) {
    if (i % 2 == 0) {
      EXPECT_EQ(strided_r[i], static_cast<uint32_t>(i + 10));
    } else {
      EXPECT_EQ(strided_r[i], 0xdeadbeefu);
    }
  }
  uint32_t actual_mode = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  EXPECT_NE(actual_mode & H5D_VECTOR_IO, 0u);

  // 2. Asymmetric sequence lengths:
  //    a) Contiguous file space (1 long seq) vs scattered mem space (many
  //    short)
  //       -> exercises file_len[file_seq_i] > io_len trimming.
  //    b) Scattered file space vs contiguous mem space
  //       -> exercises mem_len[mem_seq_i] > io_len trimming.
  constexpr hsize_t kFour = 4;
  constexpr hsize_t kEight = 8;
  ScopedSpace contig_space(H5Screate_simple(1, &kFour, nullptr));
  ScopedSpace scatter_space(H5Screate_simple(1, &kEight, nullptr));
  ASSERT_GE(H5Sselect_hyperslab(scatter_space.get(), H5S_SELECT_SET, &kStart,
                                &kStride, &kFour, &kOne),
            0);

  std::array<uint32_t, kEight> scatter_buf = {1, 0, 2, 0, 3, 0, 4, 0};
  std::array<uint32_t, kEight> scatter_out{};
  std::array<uint32_t, kFour> contig_in = {10, 20, 30, 40};
  std::array<uint32_t, kFour> contig_out{};
  haddr_t asym_off = 4096;
  const void* asym_wbuf = scatter_buf.data();
  void* asym_rbuf = scatter_out.data();
  hid_t m_id = scatter_space.get();
  hid_t f_id = contig_space.get();
  ASSERT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1,
                                &m_id, &f_id, &asym_off, &esize, &asym_wbuf),
            0);
  ASSERT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1, &m_id,
                               &f_id, &asym_off, &esize, &asym_rbuf),
            0);
  EXPECT_THAT(scatter_out, ElementsAre(1u, 0u, 2u, 0u, 3u, 0u, 4u, 0u));

  // Reverse asymmetry: contiguous mem (f_id) into scattered file (m_id) ->
  // exercises mem_len > io_len trimming on both write and read.
  haddr_t asym_off2 = 4608;
  const void* contig_wptr = contig_in.data();
  void* contig_rptr = contig_out.data();
  ASSERT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1,
                                &f_id, &m_id, &asym_off2, &esize, &contig_wptr),
            0);
  ASSERT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1, &f_id,
                               &m_id, &asym_off2, &esize, &contig_rptr),
            0);
  EXPECT_THAT(contig_out, ElementsAre(10u, 20u, 30u, 40u));

  // 3. H5FD_read_selection_id / H5FD_write_selection_id with count = 10 (> 8)
  // and extend_sizes / extend_bufs optimization + metadata-only (type != DRAW).
  constexpr uint32_t kLarge = 10;
  std::vector<ScopedSpace> m_arr;
  std::vector<ScopedSpace> f_arr;
  std::array<hid_t, kLarge> m_ids{};
  std::array<hid_t, kLarge> f_ids{};
  std::array<haddr_t, kLarge> offs{};
  std::array<size_t, kLarge> esizes{};
  std::array<const void*, kLarge> wb{};
  std::array<void*, kLarge> rb{};
  std::array<uint32_t, kFour> shared_w = {41, 42, 43, 44};
  std::array<uint32_t, kFour> shared_r{};
  for (uint32_t i = 0; i < kLarge; ++i) {
    m_arr.emplace_back(H5Screate_simple(1, &kFour, nullptr));
    f_arr.emplace_back(H5Screate_simple(1, &kFour, nullptr));
    m_ids[i] = m_arr.back().get();
    f_ids[i] = f_arr.back().get();
    offs[i] = 8192 + i * 32;
    esizes[i] = (i < 2) ? sizeof(uint32_t) : 0;
    wb[i] = (i == 0) ? shared_w.data() : nullptr;
    rb[i] = (i == 0) ? shared_r.data() : nullptr;
  }
  ASSERT_GE(H5FDwrite_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), kLarge,
                                m_ids.data(), f_ids.data(), offs.data(),
                                esizes.data(), wb.data()),
            0);
  ASSERT_GE(H5FDread_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), kLarge,
                               m_ids.data(), f_ids.data(), offs.data(),
                               esizes.data(), rb.data()),
            0);
  EXPECT_THAT(shared_r, ElementsAre(41u, 42u, 43u, 44u));

  // 4. Error paths in selection_id and translate:
  ScopedErrorSilence silence;
  // Invalid mem/file space IDs -> H5I_object_verify fails.
  hid_t bad_id = H5I_INVALID_HID;
  hid_t good_id = contig_space.get();
  EXPECT_LT(
      H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &bad_id,
                          &good_id, &asym_off, &esize, &asym_wbuf),
      0);
  EXPECT_LT(
      H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &good_id,
                          &bad_id, &asym_off, &esize, &asym_wbuf),
      0);
  EXPECT_LT(
      H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &bad_id,
                         &good_id, &asym_off, &esize, &asym_rbuf),
      0);
  EXPECT_LT(
      H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &good_id,
                         &bad_id, &asym_off, &esize, &asym_rbuf),
      0);

  // Driver vector error inside translate (both _id and H5S_t* paths).
  state.fail_write_vector = true;
  EXPECT_LT(
      H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &good_id,
                          &good_id, &asym_off, &esize, &asym_wbuf),
      0);
  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());
    H5S_t* good_ptr = SpacePtr(good_id);
    EXPECT_LT(H5FD_write_selection(vfd.file(), H5FD_MEM_DRAW, 1, &good_ptr,
                                   &good_ptr, &asym_off, &esize, &asym_wbuf),
              0);
  }
  state.fail_write_vector = false;

  state.fail_read_vector = true;
  EXPECT_LT(
      H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &good_id,
                         &good_id, &asym_off, &esize, &asym_rbuf),
      0);
  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());
    H5S_t* good_ptr = SpacePtr(good_id);
    EXPECT_LT(H5FD_read_selection(vfd.file(), H5FD_MEM_DRAW, 1, &good_ptr,
                                  &good_ptr, &asym_off, &esize, &asym_rbuf),
              0);
  }
  state.fail_read_vector = false;

  // Mismatched selection counts: 1-element file space vs 2-point memory space
  // triggers "file selection terminated before memory selection" (since
  // :hdf5 compiles with -DNDEBUG, assert(nelmts == hss_nelmts) is elided).
  ScopedSpace one_pt_space(H5Screate_simple(1, &kOne, nullptr));
  ScopedSpace two_pt_space(H5Screate_simple(1, &kFour, nullptr));
  const std::array<hsize_t, 2> kCoords = {0, 2};
  ASSERT_GE(
      H5Sselect_elements(two_pt_space.get(), H5S_SELECT_SET, 2, kCoords.data()),
      0);
  hid_t one_id = one_pt_space.get();
  hid_t two_id = two_pt_space.get();
  EXPECT_LT(H5FDread_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1,
                               &two_id, &one_id, &asym_off, &esize, &asym_rbuf),
            0);
  EXPECT_LT(
      H5FDwrite_selection(vfd.file(), H5FD_MEM_DRAW, dxpl.get(), 1, &two_id,
                          &one_id, &asym_off, &esize, &asym_wbuf),
      0);
}

TEST(H5FDintTest, SelectionSkipCallbackWrappersVectorAndScalar) {
  MockVfdState state;
  state.eoa = 4096;
  ScopedMockVfdFile vfd(MockVfdMode::kSelectionVectorScalar, &state);
  ASSERT_NE(vfd.file(), nullptr);

  constexpr hsize_t kDim = 2;
  ScopedSpace mem_space(H5Screate_simple(1, &kDim, nullptr));
  ScopedSpace file_space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(mem_space.valid());
  ASSERT_TRUE(file_space.valid());
  hid_t m_id = mem_space.get();
  hid_t f_id = file_space.get();
  haddr_t off = 64;
  size_t esize = sizeof(uint32_t);
  std::array<uint32_t, 2> wdata = {77, 88};
  std::array<uint32_t, 2> rdata{};
  const void* wbuf = wdata.data();
  void* rbuf = rdata.data();

  // 1. vector_from_selection skips selection_cb and uses vector_cb.
  ASSERT_GE(
      H5FDwrite_vector_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                      &m_id, &f_id, &off, &esize, &wbuf),
      0);
  EXPECT_EQ(state.write_selection_calls, 0);
  EXPECT_EQ(state.write_vector_calls, 1);

  ASSERT_GE(
      H5FDread_vector_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                     &m_id, &f_id, &off, &esize, &rbuf),
      0);
  EXPECT_EQ(state.read_selection_calls, 0);
  EXPECT_EQ(state.read_vector_calls, 1);
  EXPECT_THAT(rdata, ElementsAre(77u, 88u));

  // 2. from_selection skips both selection_cb and vector_cb, using scalar I/O.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  {
    ScopedApiContext ctx(dxpl.get());
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5FD_write_from_selection(vfd.file(), H5FD_MEM_DRAW, 1, &m_id,
                                        &f_id, &off, &esize, &wbuf),
              0);
    EXPECT_EQ(state.write_selection_calls, 0);
    EXPECT_EQ(state.write_vector_calls, 1);
    EXPECT_EQ(state.write_calls, 1);

    rdata.fill(0);
    ASSERT_GE(H5FD_read_from_selection(vfd.file(), H5FD_MEM_DRAW, 1, &m_id,
                                       &f_id, &off, &esize, &rbuf),
              0);
    EXPECT_EQ(state.read_selection_calls, 0);
    EXPECT_EQ(state.read_vector_calls, 1);
    EXPECT_EQ(state.read_calls, 1);
    EXPECT_THAT(rdata, ElementsAre(77u, 88u));
  }
  uint32_t actual_mode = 0;
  uint32_t no_cause = 0;
  ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &actual_mode), 0);
  ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &no_cause), 0);
  EXPECT_NE(actual_mode & H5D_SCALAR_IO, 0u);
  EXPECT_NE(no_cause & H5D_SEL_IO_NO_VECTOR_OR_SELECTION_IO_CB, 0u);

  // Also test public wrappers and error propagation for _from_selection.
  ASSERT_GE(H5FDwrite_from_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1,
                                     &m_id, &f_id, &off, &esize, &wbuf),
            0);
  ASSERT_GE(H5FDread_from_selection(vfd.file(), H5FD_MEM_SUPER, dxpl.get(), 1,
                                    &m_id, &f_id, &off, &esize, &rbuf),
            0);

  ScopedErrorSilence silence;
  state.fail_read = true;
  EXPECT_LT(H5FDread_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                    &m_id, &f_id, &off, &esize, &rbuf),
            0);
  state.fail_read = false;

  state.fail_write = true;
  EXPECT_LT(H5FDwrite_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                     &m_id, &f_id, &off, &esize, &wbuf),
            0);
  state.fail_write = false;

  state.fail_read_vector = true;
  EXPECT_LT(
      H5FDread_vector_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                     &m_id, &f_id, &off, &esize, &rbuf),
      0);
  state.fail_read_vector = false;

  state.fail_write_vector = true;
  EXPECT_LT(
      H5FDwrite_vector_from_selection(vfd.file(), H5FD_MEM_DRAW, H5P_DEFAULT, 1,
                                      &m_id, &f_id, &off, &esize, &wbuf),
      0);
  state.fail_write_vector = false;
}

TEST(H5FDintTest, EoaEofAndDriverQueryAdjustBaseAddrAndHandleMissingCbs) {
  MockVfdState state;
  state.storage.resize(512, uint8_t{0});
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);
  ASSERT_GE(H5FD_set_base_addr(vfd.file(), 64), 0);

  // 1. EOA / EOF relative to base_addr.
  ASSERT_GE(H5FD_set_eoa(vfd.file(), H5FD_MEM_DEFAULT, 256), 0);
  EXPECT_EQ(state.eoa, 320u);
  EXPECT_EQ(H5FD_get_eoa(vfd.file(), H5FD_MEM_DEFAULT), 256u);
  EXPECT_EQ(H5FD_get_eof(vfd.file(), H5FD_MEM_DEFAULT), 448u);

  // 2. query callback present vs nullptr.
  unsigned long flags = 0;
  ASSERT_GE(H5FD_driver_query(vfd.cls(), &flags), 0);
  EXPECT_NE(flags & H5FD_FEAT_SUPPORTS_SWMR_IO, 0u);

  H5FD_class_t no_query_cls =
      MakeMockVfdClass(MockVfdMode::kMinimalNoQueryNoDel, kMockVfdValueBase + 1,
                       "mock_no_query");
  flags = 0x1234;
  ASSERT_GE(H5FD_driver_query(&no_query_cls, &flags), 0);
  EXPECT_EQ(flags, 0u);

  // 3. Direct H5FD_t struct with get_eof == nullptr returns maxaddr -
  // base_addr.
  H5FD_t fake_file{};
  fake_file.cls = &no_query_cls;
  fake_file.maxaddr = 4096;
  fake_file.base_addr = 128;
  no_query_cls.get_eof = nullptr;
  EXPECT_EQ(H5FD_get_eof(&fake_file, H5FD_MEM_DEFAULT), 3968u);

  // 4. Error returns from set_eoa, get_eoa, and get_eof callbacks.
  ScopedErrorSilence silence;
  state.fail_set_eoa = true;
  EXPECT_LT(H5FD_set_eoa(vfd.file(), H5FD_MEM_DEFAULT, 128), 0);
  state.fail_set_eoa = false;

  state.fail_get_eoa = true;
  EXPECT_EQ(H5FD_get_eoa(vfd.file(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
  state.fail_get_eoa = false;

  state.fail_get_eof = true;
  EXPECT_EQ(H5FD_get_eof(vfd.file(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
  state.fail_get_eof = false;
}

TEST(H5FDintTest, SortVectorIoReqCoversSortedUnsortedFixedAndDuplicates) {
  int a = 1;
  int b = 2;
  int c = 3;

  // 1. Zero-count and already-sorted requests do not allocate new arrays.
  bool was_sorted = false;
  H5FD_mem_t* s_types = nullptr;
  haddr_t* s_addrs = nullptr;
  size_t* s_sizes = nullptr;
  H5_flexible_const_ptr_t* s_bufs = nullptr;
  ASSERT_GE(
      H5FD_sort_vector_io_req(&was_sorted, 0, nullptr, nullptr, nullptr,
                              nullptr, &s_types, &s_addrs, &s_sizes, &s_bufs),
      0);
  EXPECT_TRUE(was_sorted);

  std::array<H5FD_mem_t, 3> sorted_types = {H5FD_MEM_DRAW, H5FD_MEM_SUPER,
                                            H5FD_MEM_NOLIST};
  std::array<haddr_t, 3> sorted_addrs = {10, 20, 30};
  std::array<size_t, 3> sorted_sizes = {4, 8, 0};
  std::array<H5_flexible_const_ptr_t, 3> sorted_bufs{};
  sorted_bufs[0].vp = &a;
  sorted_bufs[1].vp = &b;
  sorted_bufs[2].vp = &c;
  ASSERT_GE(H5FD_sort_vector_io_req(&was_sorted, 3, sorted_types.data(),
                                    sorted_addrs.data(), sorted_sizes.data(),
                                    sorted_bufs.data(), &s_types, &s_addrs,
                                    &s_sizes, &s_bufs),
            0);
  EXPECT_TRUE(was_sorted);
  EXPECT_EQ(s_addrs, sorted_addrs.data());

  // 2. Unsorted request with fixed trailing size/type optimization.
  std::array<H5FD_mem_t, 3> u_types = {H5FD_MEM_DRAW, H5FD_MEM_SUPER,
                                       H5FD_MEM_NOLIST};
  std::array<size_t, 3> u_sizes = {4, 8, 0};
  {
    s_types = nullptr;
    s_addrs = nullptr;
    s_sizes = nullptr;
    s_bufs = nullptr;
    absl::Cleanup free_sorted = [&] {
      std::free(s_types);
      std::free(s_addrs);
      std::free(s_sizes);
      std::free(s_bufs);
    };
    std::array<haddr_t, 3> u_addrs = {30, 10, 20};
    ASSERT_GE(
        H5FD_sort_vector_io_req(&was_sorted, 3, u_types.data(), u_addrs.data(),
                                u_sizes.data(), sorted_bufs.data(), &s_types,
                                &s_addrs, &s_sizes, &s_bufs),
        0);
    EXPECT_FALSE(was_sorted);
    ASSERT_NE(s_addrs, nullptr);
    EXPECT_EQ(s_addrs[0], 10u);
    EXPECT_EQ(s_addrs[1], 20u);
    EXPECT_EQ(s_addrs[2], 30u);
    EXPECT_EQ(s_sizes[0], 8u);
    EXPECT_EQ(s_sizes[1], 8u);
    EXPECT_EQ(s_sizes[2], 4u);
    EXPECT_EQ(s_types[0], H5FD_MEM_SUPER);
    EXPECT_EQ(s_types[1], H5FD_MEM_SUPER);
    EXPECT_EQ(s_types[2], H5FD_MEM_DRAW);
    EXPECT_EQ(s_bufs[0].vp, &b);
    EXPECT_EQ(s_bufs[1].vp, &c);
    EXPECT_EQ(s_bufs[2].vp, &a);
  }

  // 3. Duplicate adjacent addrs (sorted prefix and unsorted duplicate).
  ScopedErrorSilence silence;
  std::array<haddr_t, 2> dup_sorted = {10, 10};
  s_types = nullptr;
  s_addrs = nullptr;
  s_sizes = nullptr;
  s_bufs = nullptr;
  EXPECT_LT(H5FD_sort_vector_io_req(&was_sorted, 2, sorted_types.data(),
                                    dup_sorted.data(), sorted_sizes.data(),
                                    sorted_bufs.data(), &s_types, &s_addrs,
                                    &s_sizes, &s_bufs),
            0);

  std::array<haddr_t, 3> dup_unsorted = {30, 10, 10};
  EXPECT_LT(H5FD_sort_vector_io_req(&was_sorted, 3, u_types.data(),
                                    dup_unsorted.data(), u_sizes.data(),
                                    sorted_bufs.data(), &s_types, &s_addrs,
                                    &s_sizes, &s_bufs),
            0);
  EXPECT_EQ(s_types, nullptr);
  EXPECT_EQ(s_addrs, nullptr);
  EXPECT_EQ(s_sizes, nullptr);
  EXPECT_EQ(s_bufs, nullptr);
}

TEST(H5FDintTest, SortSelectionIoReqCoversSortedUnsortedFixedAndDuplicates) {
  int a = 1;
  int b = 2;

  bool was_sorted = false;
  hid_t* s_mids = nullptr;
  hid_t* s_fids = nullptr;
  haddr_t* s_offs = nullptr;
  size_t* s_esizes = nullptr;
  H5_flexible_const_ptr_t* s_bufs = nullptr;

  // 1. Zero-count selection sort.
  ASSERT_GE(H5FD_sort_selection_io_req(&was_sorted, 0, nullptr, nullptr,
                                       nullptr, nullptr, nullptr, &s_mids,
                                       &s_fids, &s_offs, &s_esizes, &s_bufs),
            0);
  EXPECT_TRUE(was_sorted);

  // 2. Already sorted selection request.
  std::array<hid_t, 3> mids = {1, 2, 3};
  std::array<hid_t, 3> fids = {4, 5, 6};
  std::array<haddr_t, 3> sorted_offs = {10, 20, 30};
  std::array<size_t, 3> esizes = {4, 8, 0};
  std::array<H5_flexible_const_ptr_t, 3> bufs{};
  bufs[0].cvp = &a;
  bufs[1].cvp = &b;
  bufs[2].cvp = nullptr;
  ASSERT_GE(
      H5FD_sort_selection_io_req(&was_sorted, 3, mids.data(), fids.data(),
                                 sorted_offs.data(), esizes.data(), bufs.data(),
                                 &s_mids, &s_fids, &s_offs, &s_esizes, &s_bufs),
      0);
  EXPECT_TRUE(was_sorted);
  EXPECT_EQ(s_offs, sorted_offs.data());

  // 3. Unsorted selection request with fixed element_sizes and fixed bufs.
  {
    s_mids = nullptr;
    s_fids = nullptr;
    s_offs = nullptr;
    s_esizes = nullptr;
    s_bufs = nullptr;
    absl::Cleanup free_sorted = [&] {
      std::free(s_mids);
      std::free(s_fids);
      std::free(s_offs);
      std::free(s_esizes);
      std::free(s_bufs);
    };
    std::array<haddr_t, 3> u_offs = {30, 10, 20};
    ASSERT_GE(H5FD_sort_selection_io_req(&was_sorted, 3, mids.data(),
                                         fids.data(), u_offs.data(),
                                         esizes.data(), bufs.data(), &s_mids,
                                         &s_fids, &s_offs, &s_esizes, &s_bufs),
              0);
    EXPECT_FALSE(was_sorted);
    ASSERT_NE(s_offs, nullptr);
    EXPECT_EQ(s_offs[0], 10u);
    EXPECT_EQ(s_offs[1], 20u);
    EXPECT_EQ(s_offs[2], 30u);
    EXPECT_EQ(s_mids[0], 2);
    EXPECT_EQ(s_mids[1], 3);
    EXPECT_EQ(s_mids[2], 1);
    EXPECT_EQ(s_fids[0], 5);
    EXPECT_EQ(s_fids[1], 6);
    EXPECT_EQ(s_fids[2], 4);
    EXPECT_EQ(s_esizes[0], 8u);
    EXPECT_EQ(s_esizes[1], 8u);
    EXPECT_EQ(s_esizes[2], 4u);
    EXPECT_EQ(s_bufs[0].cvp, &b);
    EXPECT_EQ(s_bufs[1].cvp, &b);
    EXPECT_EQ(s_bufs[2].cvp, &a);
  }

  // 4. Unsorted duplicate fails cleanly without leaking.
  ScopedErrorSilence silence;
  std::array<haddr_t, 3> dup_offs = {30, 10, 10};
  s_mids = nullptr;
  s_fids = nullptr;
  s_offs = nullptr;
  s_esizes = nullptr;
  s_bufs = nullptr;
  EXPECT_LT(
      H5FD_sort_selection_io_req(&was_sorted, 3, mids.data(), fids.data(),
                                 dup_offs.data(), esizes.data(), bufs.data(),
                                 &s_mids, &s_fids, &s_offs, &s_esizes, &s_bufs),
      0);
  EXPECT_EQ(s_mids, nullptr);
  EXPECT_EQ(s_fids, nullptr);
  EXPECT_EQ(s_offs, nullptr);
  EXPECT_EQ(s_esizes, nullptr);
  EXPECT_EQ(s_bufs, nullptr);
}

TEST(H5FDintTest, DeleteCoversSuccessUnsupportedCallbackAndErrors) {
  MockVfdState state;
  ScopedMockVfdFile vfd(MockVfdMode::kScalarOnly, &state);
  ASSERT_NE(vfd.file(), nullptr);

  EXPECT_GE(H5FD_delete("dummy_mem_file", vfd.fapl_id()), 0);
  EXPECT_EQ(state.del_calls, 1);

  ScopedErrorSilence silence;
  // Driver del callback failure.
  state.fail_del = true;
  EXPECT_LT(H5FD_delete("dummy_mem_file", vfd.fapl_id()), 0);
  state.fail_del = false;

  // Invalid FAPL ID and non-FAPL property list.
  EXPECT_LT(H5FD_delete("dummy_mem_file", H5I_INVALID_HID), 0);
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  EXPECT_LT(H5FD_delete("dummy_mem_file", dxpl.get()), 0);

  // Registered VFD without a del callback.
  H5FD_class_t no_del_cls =
      MakeMockVfdClass(MockVfdMode::kMinimalNoQueryNoDel, kMockVfdValueBase + 2,
                       "mock_no_del_vfd");
  const hid_t no_del_id = H5FDregister(&no_del_cls);
  ASSERT_GE(no_del_id, 0);
  absl::Cleanup unreg_no_del = [no_del_id] { H5FDunregister(no_del_id); };

  ScopedPlist no_del_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(no_del_fapl.valid());
  ASSERT_GE(H5Pset_driver(no_del_fapl.get(), no_del_id, nullptr), 0);
  EXPECT_LT(H5FD_delete("dummy_mem_file", no_del_fapl.get()), 0);

  // Stale/unregistered driver ID inside FAPL (driver_info is nullptr so FAPL
  // teardown skips driver info lookup cleanly).
  std::move(unreg_no_del).Cancel();
  ASSERT_GE(H5FDunregister(no_del_id), 0);
  EXPECT_LT(H5FD_delete("dummy_mem_file", no_del_fapl.get()), 0);
}

TEST(H5FDintTest, PluginLoadAndDriverLookupByNameAndValue) {
  ASSERT_GE(H5open(), 0);

  // 1. H5FD_check_plugin_load by name and value (match, mismatch, null name).
  H5FD_class_t cls = MakeMockVfdClass(MockVfdMode::kScalarOnly,
                                      kMockVfdValueBase + 3, "plugin_test_vfd");
  H5PL_key_t key{};
  bool success = false;

  key.vfd.kind = H5FD_GET_DRIVER_BY_NAME;
  key.vfd.u.name = "plugin_test_vfd";
  ASSERT_GE(H5FD_check_plugin_load(&cls, &key, &success), 0);
  EXPECT_TRUE(success);

  success = false;
  key.vfd.u.name = "other_vfd";
  ASSERT_GE(H5FD_check_plugin_load(&cls, &key, &success), 0);
  EXPECT_FALSE(success);

  cls.name = nullptr;
  key.vfd.u.name = "plugin_test_vfd";
  ASSERT_GE(H5FD_check_plugin_load(&cls, &key, &success), 0);
  EXPECT_FALSE(success);

  key.vfd.kind = H5FD_GET_DRIVER_BY_VALUE;
  key.vfd.u.value = kMockVfdValueBase + 3;
  ASSERT_GE(H5FD_check_plugin_load(&cls, &key, &success), 0);
  EXPECT_TRUE(success);

  success = false;
  key.vfd.u.value = kMockVfdValueBase + 99;
  ASSERT_GE(H5FD_check_plugin_load(&cls, &key, &success), 0);
  EXPECT_FALSE(success);

  // 2. Registered driver checks on built-in Core VFD.
  hid_t found_id = H5I_INVALID_HID;
  EXPECT_GT(H5FD_is_driver_registered_by_name("core", &found_id), 0);
  EXPECT_EQ(found_id, H5FD_CORE);
  EXPECT_GT(H5FD_is_driver_registered_by_name("core", nullptr), 0);
  EXPECT_EQ(H5FD_is_driver_registered_by_name("no_such_vfd_xyz", nullptr), 0);

  found_id = H5I_INVALID_HID;
  EXPECT_GT(H5FD_is_driver_registered_by_value(H5_VFD_CORE, &found_id), 0);
  EXPECT_EQ(found_id, H5FD_CORE);
  EXPECT_GT(H5FD_is_driver_registered_by_value(H5_VFD_CORE, nullptr), 0);
  EXPECT_EQ(H5FD_is_driver_registered_by_value(99999, nullptr), 0);

  // 3. H5FD_get_driver_id_by_name / _value with both is_api=true and false.
  const hid_t by_name_api = H5FD_get_driver_id_by_name("core", true);
  ASSERT_GE(by_name_api, 0);
  EXPECT_EQ(by_name_api, H5FD_CORE);
  EXPECT_GE(H5I_dec_app_ref(by_name_api), 0);

  const hid_t by_name_int = H5FD_get_driver_id_by_name("core", false);
  ASSERT_GE(by_name_int, 0);
  EXPECT_GE(H5I_dec_ref(by_name_int), 0);

  EXPECT_EQ(H5FD_get_driver_id_by_name("no_such_vfd_xyz", true),
            H5I_INVALID_HID);

  const hid_t by_val_api = H5FD_get_driver_id_by_value(H5_VFD_CORE, true);
  ASSERT_GE(by_val_api, 0);
  EXPECT_EQ(by_val_api, H5FD_CORE);
  EXPECT_GE(H5I_dec_app_ref(by_val_api), 0);

  const hid_t by_val_int = H5FD_get_driver_id_by_value(H5_VFD_CORE, false);
  ASSERT_GE(by_val_int, 0);
  EXPECT_GE(H5I_dec_ref(by_val_int), 0);

  EXPECT_EQ(H5FD_get_driver_id_by_value(99999, true), H5I_INVALID_HID);

  // 4. H5FD_register_driver_by_name / _value for already-registered driver,
  // plus error path when plugins are disabled/unavailable in google3.
  const hid_t reg_name_api = H5FD_register_driver_by_name("core", true);
  ASSERT_GE(reg_name_api, 0);
  EXPECT_GE(H5I_dec_app_ref(reg_name_api), 0);

  const hid_t reg_name_int = H5FD_register_driver_by_name("core", false);
  ASSERT_GE(reg_name_int, 0);
  EXPECT_GE(H5I_dec_ref(reg_name_int), 0);

  const hid_t reg_val_api = H5FD_register_driver_by_value(H5_VFD_CORE, true);
  ASSERT_GE(reg_val_api, 0);
  EXPECT_GE(H5I_dec_app_ref(reg_val_api), 0);

  const hid_t reg_val_int = H5FD_register_driver_by_value(H5_VFD_CORE, false);
  ASSERT_GE(reg_val_int, 0);
  EXPECT_GE(H5I_dec_ref(reg_val_int), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5FD_register_driver_by_name("no_such_vfd_xyz", true),
            H5I_INVALID_HID);
  EXPECT_EQ(H5FD_register_driver_by_value(99999, true), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
