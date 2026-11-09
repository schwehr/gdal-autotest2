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
// Tests the Splitter Virtual File Driver (H5FD_SPLITTER): registration,
// H5Pset_fapl_splitter/H5Pget_fapl_splitter configuration and validation,
// mirrored HDF5 dataset creation and raw sec2 byte verification, default "_wo"
// path synthesis, delete/log error handling, and passthrough + fault-injection
// behavior for all VFD callbacks (third_party/hdf5/src/H5FDsplitter.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDsplitter.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDSPLITTER_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
}  // extern "C"

#ifdef H5FDSPLITTER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDSPLITTER_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

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

  H5FD_t* get() const { return file_; }
  bool valid() const { return file_ != nullptr; }

 private:
  H5FD_t* file_ = nullptr;
};

class ScopedVfdDriver {
 public:
  explicit ScopedVfdDriver(const H5FD_class_t* cls) : id_(H5FDregister(cls)) {}
  ~ScopedVfdDriver() { reset(); }

  ScopedVfdDriver(const ScopedVfdDriver&) = delete;
  ScopedVfdDriver& operator=(const ScopedVfdDriver&) = delete;

  void reset() {
    if (id_ != H5I_INVALID_HID) {
      H5FDunregister(id_);
      id_ = H5I_INVALID_HID;
    }
  }

  hid_t get() const { return id_; }
  bool valid() const { return id_ >= 0; }

 private:
  hid_t id_ = H5I_INVALID_HID;
};

struct SplitterConfigGuard {
  H5FD_splitter_vfd_config_t cfg{};

  SplitterConfigGuard() {
    cfg.magic = H5FD_SPLITTER_MAGIC;
    cfg.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
    cfg.rw_fapl_id = H5P_DEFAULT;
    cfg.wo_fapl_id = H5P_DEFAULT;
  }

  ~SplitterConfigGuard() {
    if (cfg.rw_fapl_id >= 0 && cfg.rw_fapl_id != H5P_DEFAULT) {
      H5Pclose(cfg.rw_fapl_id);
    }
    if (cfg.wo_fapl_id >= 0 && cfg.wo_fapl_id != H5P_DEFAULT) {
      H5Pclose(cfg.wo_fapl_id);
    }
  }
};

void SetPathBuffer(char (&dst)[H5FD_SPLITTER_PATH_MAX + 1],
                   const std::string& src) {
  std::memset(dst, 0, sizeof(dst));
  std::strncpy(dst, src.c_str(), H5FD_SPLITTER_PATH_MAX);
}

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

struct MockChannelState {
  int cmp_id = 0;
  bool fail_open = false;
  bool fail_query = false;
  unsigned long query_flags = H5FD_FEAT_DEFAULT_VFD_COMPATIBLE;  // NOLINT
  bool fail_get_type_map = false;
  bool fail_alloc = false;
  haddr_t next_alloc = 0;
  bool fail_free = false;
  bool fail_get_eoa = false;
  haddr_t eoa = 0;
  bool fail_set_eoa = false;
  bool fail_get_eof = false;
  haddr_t eof = 0;
  bool fail_get_handle = false;
  int fake_handle_val = 42;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_flush = false;
  bool fail_truncate = false;
  bool fail_lock = false;
  bool fail_unlock = false;
  bool fail_ctl = false;
  bool fail_sb_encode = false;
  bool fail_sb_decode = false;
  hsize_t sb_size = 16;

  int open_calls = 0;
  int read_calls = 0;
  int write_calls = 0;
  int flush_calls = 0;
  int truncate_calls = 0;
  int alloc_calls = 0;
  int free_calls = 0;
  int lock_calls = 0;
  int unlock_calls = 0;
  int ctl_calls = 0;
  std::vector<uint8_t> last_written;
};

struct MockSplitterController {
  MockChannelState rw;
  MockChannelState wo;
  unsigned long driver_query_flags = H5FD_FEAT_DEFAULT_VFD_COMPATIBLE;  // NOLINT
  bool fail_driver_query = false;
  bool delete_rw_ok = true;
  bool delete_wo_ok = true;
  int delete_calls = 0;
};

MockSplitterController* g_ctrl = nullptr;

struct MockChannelFile {
  H5FD_t pub;
  MockChannelState* state;
  int id;
};

H5FD_t* MockChOpen(const char* name, unsigned /*flags*/, hid_t /*fapl*/,
                   haddr_t /*maxaddr*/) {
  if (g_ctrl == nullptr || name == nullptr) {
    return nullptr;
  }
  const bool is_wo = (std::strncmp(name, "wo", 2) == 0);
  MockChannelState* st = is_wo ? &g_ctrl->wo : &g_ctrl->rw;
  ++st->open_calls;
  if (st->fail_open) {
    return nullptr;
  }
  auto* f = new MockChannelFile{};
  f->state = st;
  f->id = st->cmp_id;
  return &f->pub;
}

herr_t MockChClose(H5FD_t* file) {
  auto* mf = reinterpret_cast<MockChannelFile*>(file);
  delete mf;
  return 0;
}

int MockChCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* m1 = reinterpret_cast<const MockChannelFile*>(f1);
  const auto* m2 = reinterpret_cast<const MockChannelFile*>(f2);
  if (m1->id < m2->id) return -1;
  if (m1->id > m2->id) return 1;
  return 0;
}

herr_t MockChQuery(const H5FD_t* f, unsigned long* flags) {  // NOLINT
  if (f == nullptr) {
    if (g_ctrl != nullptr && g_ctrl->fail_driver_query) {
      return -1;
    }
    if (flags != nullptr) {
      *flags = g_ctrl ? g_ctrl->driver_query_flags
                      : H5FD_FEAT_DEFAULT_VFD_COMPATIBLE;
    }
    return 0;
  }
  const auto* mf = reinterpret_cast<const MockChannelFile*>(f);
  if (mf->state->fail_query) {
    return -1;
  }
  if (flags != nullptr) {
    *flags = mf->state->query_flags;
  }
  return 0;
}

herr_t MockChGetTypeMap(const H5FD_t* f, H5FD_mem_t* type_map) {
  const auto* mf = reinterpret_cast<const MockChannelFile*>(f);
  if (mf->state->fail_get_type_map) {
    return -1;
  }
  for (int i = 0; i < H5FD_MEM_NTYPES; ++i) {
    type_map[i] = H5FD_MEM_SUPER;
  }
  return 0;
}

hsize_t MockChSbSize(H5FD_t* f) {
  const auto* mf = reinterpret_cast<const MockChannelFile*>(f);
  return mf->state->sb_size;
}

herr_t MockChSbEncode(H5FD_t* f, char* name, unsigned char* buf) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  if (mf->state->fail_sb_encode) {
    return -1;
  }
  std::memcpy(name, "MOCKVFD ", 9);
  for (hsize_t i = 0; i < mf->state->sb_size; ++i) {
    buf[i] = static_cast<unsigned char>(i + 1);
  }
  return 0;
}

herr_t MockChSbDecode(H5FD_t* f, const char* /*name*/,
                      const unsigned char* /*buf*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  return mf->state->fail_sb_decode ? -1 : 0;
}

haddr_t MockChAlloc(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                    hsize_t size) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->alloc_calls;
  if (mf->state->fail_alloc) {
    return HADDR_UNDEF;
  }
  const haddr_t out = mf->state->next_alloc;
  mf->state->next_alloc += size;
  return out;
}

herr_t MockChFree(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                  haddr_t /*addr*/, hsize_t /*size*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->free_calls;
  return mf->state->fail_free ? -1 : 0;
}

haddr_t MockChGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const auto* mf = reinterpret_cast<const MockChannelFile*>(f);
  return mf->state->fail_get_eoa ? HADDR_UNDEF : mf->state->eoa;
}

herr_t MockChSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  if (mf->state->fail_set_eoa) {
    return -1;
  }
  mf->state->eoa = addr;
  return 0;
}

haddr_t MockChGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const auto* mf = reinterpret_cast<const MockChannelFile*>(f);
  return mf->state->fail_get_eof ? HADDR_UNDEF : mf->state->eof;
}

herr_t MockChGetHandle(H5FD_t* f, hid_t /*fapl*/, void** handle) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  if (mf->state->fail_get_handle) {
    return -1;
  }
  *handle = &mf->state->fake_handle_val;
  return 0;
}

herr_t MockChRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                  haddr_t /*addr*/, size_t size, void* buf) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->read_calls;
  if (mf->state->fail_read) {
    return -1;
  }
  std::memset(buf, 0x5a, size);
  return 0;
}

herr_t MockChWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl*/,
                   haddr_t /*addr*/, size_t size, const void* buf) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->write_calls;
  if (mf->state->fail_write) {
    return -1;
  }
  const auto* bytes = static_cast<const uint8_t*>(buf);
  mf->state->last_written.assign(bytes, bytes + size);
  return 0;
}

herr_t MockChFlush(H5FD_t* f, hid_t /*dxpl*/, bool /*closing*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->flush_calls;
  return mf->state->fail_flush ? -1 : 0;
}

herr_t MockChTruncate(H5FD_t* f, hid_t /*dxpl*/, bool /*closing*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->truncate_calls;
  return mf->state->fail_truncate ? -1 : 0;
}

herr_t MockChLock(H5FD_t* f, bool /*rw*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->lock_calls;
  return mf->state->fail_lock ? -1 : 0;
}

herr_t MockChUnlock(H5FD_t* f) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->unlock_calls;
  return mf->state->fail_unlock ? -1 : 0;
}

herr_t MockChDel(const char* name, hid_t /*fapl*/) {
  if (g_ctrl == nullptr || name == nullptr) {
    return -1;
  }
  ++g_ctrl->delete_calls;
  const bool is_wo = (std::strncmp(name, "wo", 2) == 0);
  return (is_wo ? g_ctrl->delete_wo_ok : g_ctrl->delete_rw_ok) ? 0 : -1;
}

herr_t MockChCtl(H5FD_t* f, uint64_t /*op_code*/, uint64_t /*flags*/,
                 const void* /*input*/, void** /*output*/) {
  auto* mf = reinterpret_cast<MockChannelFile*>(f);
  ++mf->state->ctl_calls;
  return mf->state->fail_ctl ? -1 : 0;
}

H5FD_class_t MakeMockChannelClass(H5FD_class_value_t val, const char* name) {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = val;
  cls.name = name;
  cls.maxaddr = H5FD_MAXADDR;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.sb_size = &MockChSbSize;
  cls.sb_encode = &MockChSbEncode;
  cls.sb_decode = &MockChSbDecode;
  cls.open = &MockChOpen;
  cls.close = &MockChClose;
  cls.cmp = &MockChCmp;
  cls.query = &MockChQuery;
  cls.get_type_map = &MockChGetTypeMap;
  cls.alloc = &MockChAlloc;
  cls.free = &MockChFree;
  cls.get_eoa = &MockChGetEoa;
  cls.set_eoa = &MockChSetEoa;
  cls.get_eof = &MockChGetEof;
  cls.get_handle = &MockChGetHandle;
  cls.read = &MockChRead;
  cls.write = &MockChWrite;
  cls.flush = &MockChFlush;
  cls.truncate = &MockChTruncate;
  cls.lock = &MockChLock;
  cls.unlock = &MockChUnlock;
  cls.del = &MockChDel;
  cls.ctl = &MockChCtl;
  cls.fl_map[H5FD_MEM_DEFAULT] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_SUPER] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_BTREE] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
  cls.fl_map[H5FD_MEM_GHEAP] = H5FD_MEM_DRAW;
  cls.fl_map[H5FD_MEM_LHEAP] = H5FD_MEM_SUPER;
  cls.fl_map[H5FD_MEM_OHDR] = H5FD_MEM_SUPER;
  return cls;
}

TEST(H5FDsplitterTest, RegistrationConstantsAndQueryWithoutFile) {
  static_assert(H5FD_SPLITTER_VALUE == H5_VFD_SPLITTER);
  static_assert(H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION == 1);
  static_assert(H5FD_SPLITTER_PATH_MAX == 4096);
  static_assert(H5FD_SPLITTER_MAGIC == 0x2B916880);

  EXPECT_GT(H5FDis_driver_registered_by_name("splitter"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5FD_SPLITTER_VALUE), 0);
  EXPECT_NE(H5FD_SPLITTER, H5I_INVALID_HID);

  // Calling H5FD__splitter_register when already registered is a safe no-op.
  EXPECT_GE(H5FD__splitter_register(), 0);

  unsigned long flags = 0xffUL;  // NOLINT(runtime/int)
  ASSERT_GE(H5FDdriver_query(H5FD_SPLITTER, &flags), 0);
  EXPECT_EQ(flags, 0UL);

  const H5FD_class_t* cls = H5FD_get_class(H5FD_SPLITTER);
  ASSERT_NE(cls, nullptr);
  EXPECT_GE(cls->query(nullptr, nullptr), 0);

  // Exercise H5FD__splitter_unregister and re-registration via H5close/H5open.
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);
  EXPECT_GT(H5FDis_driver_registered_by_name("splitter"), 0);
}

TEST(H5FDsplitterTest, SetAndGetFaplConfigurationAndValidation) {
  TempDir dir("h5fd_splitter_fapl");

  // 1. Default rw/wo FAPLs fall back to Sec2; configured strings round-trip.
  {
    H5FD_splitter_vfd_config_t in_cfg{};
    in_cfg.magic = H5FD_SPLITTER_MAGIC;
    in_cfg.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
    in_cfg.rw_fapl_id = H5P_DEFAULT;
    in_cfg.wo_fapl_id = H5P_DEFAULT;
    in_cfg.ignore_wo_errs = true;
    SetPathBuffer(in_cfg.wo_path, dir.child("mirror.h5").string());
    SetPathBuffer(in_cfg.log_file_path, dir.child("err.log").string());

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_splitter(fapl.get(), &in_cfg), 0);
    EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_SPLITTER);

    // Exercise H5Pcopy -> H5FD__splitter_fapl_copy and H5Pclose -> fapl_free.
    ScopedPlist fapl_copy(H5Pcopy(fapl.get()));
    ASSERT_TRUE(fapl_copy.valid());

    SplitterConfigGuard out_cfg;
    ASSERT_GE(H5Pget_fapl_splitter(fapl_copy.get(), &out_cfg.cfg), 0);
    EXPECT_STREQ(out_cfg.cfg.wo_path, in_cfg.wo_path);
    EXPECT_STREQ(out_cfg.cfg.log_file_path, in_cfg.log_file_path);
    EXPECT_TRUE(out_cfg.cfg.ignore_wo_errs);
    EXPECT_EQ(H5Pget_driver(out_cfg.cfg.rw_fapl_id), H5FD_SEC2);
    EXPECT_EQ(H5Pget_driver(out_cfg.cfg.wo_fapl_id), H5FD_SEC2);
  }

  // 2. FAPL with H5Pset_driver(..., H5FD_SPLITTER, nullptr) hits default_fapl
  //    synthesis inside H5Pget_fapl_splitter.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_SPLITTER, nullptr), 0);

    SplitterConfigGuard out_cfg;
    ASSERT_GE(H5Pget_fapl_splitter(fapl.get(), &out_cfg.cfg), 0);
    EXPECT_STREQ(out_cfg.cfg.wo_path, "");
    EXPECT_STREQ(out_cfg.cfg.log_file_path, "");
    EXPECT_FALSE(out_cfg.cfg.ignore_wo_errs);
    EXPECT_EQ(H5Pget_driver(out_cfg.cfg.rw_fapl_id), H5FD_SEC2);
    EXPECT_EQ(H5Pget_driver(out_cfg.cfg.wo_fapl_id), H5FD_SEC2);
  }

  // 3. Invalid arguments for H5Pset_fapl_splitter and H5Pget_fapl_splitter.
  MockSplitterController ctrl;
  g_ctrl = &ctrl;
  absl::Cleanup clear_ctrl = [] { g_ctrl = nullptr; };

  const H5FD_class_t mock_cls =
      MakeMockChannelClass(H5_VFD_RESERVED + 11, "splitter_mock_compat");
  ScopedVfdDriver mock_drv(&mock_cls);
  ASSERT_TRUE(mock_drv.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist fam_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_TRUE(sec2_fapl.valid());
  ASSERT_TRUE(fam_fapl.valid());
  ASSERT_TRUE(mock_fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
  ASSERT_GE(H5Pset_fapl_family(fam_fapl.get(), 4096, H5P_DEFAULT), 0);
  ASSERT_GE(H5Pset_driver(mock_fapl.get(), mock_drv.get(), nullptr), 0);

  ScopedErrorSilence silence;

  H5FD_splitter_vfd_config_t bad{};
  bad.magic = 0x12345678;
  bad.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  bad.magic = H5FD_SPLITTER_MAGIC;
  bad.version = 999;
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  bad.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
  EXPECT_LT(H5Pset_fapl_splitter(H5I_INVALID_HID, &bad), 0);
  EXPECT_LT(H5Pset_fapl_splitter(H5P_DATASET_XFER_DEFAULT, &bad), 0);

  // Bad rw_fapl_id.
  bad.rw_fapl_id = H5P_DATASET_XFER_DEFAULT;
  bad.wo_fapl_id = H5P_DEFAULT;
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  // Bad wo_fapl_id: invalid hid, non-FAPL, query failure, and unsuitable VFD.
  bad.rw_fapl_id = H5P_DEFAULT;
  bad.wo_fapl_id = H5I_INVALID_HID;
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  bad.wo_fapl_id = H5P_DATASET_XFER_DEFAULT;
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  bad.wo_fapl_id = fam_fapl.get();  // Lacks H5FD_FEAT_DEFAULT_VFD_COMPATIBLE.
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);

  ctrl.fail_driver_query = true;
  bad.wo_fapl_id = mock_fapl.get();
  EXPECT_LT(H5Pset_fapl_splitter(fapl.get(), &bad), 0);
  ctrl.fail_driver_query = false;

  // H5Pget_fapl_splitter error branches.
  SplitterConfigGuard out_bad;
  EXPECT_LT(H5Pget_fapl_splitter(H5P_DATASET_XFER_DEFAULT, &out_bad.cfg), 0);
  EXPECT_LT(H5Pget_fapl_splitter(sec2_fapl.get(), nullptr), 0);
  EXPECT_LT(H5Pget_fapl_splitter(sec2_fapl.get(), &out_bad.cfg), 0);

  out_bad.cfg.magic = 0;
  EXPECT_LT(H5Pget_fapl_splitter(sec2_fapl.get(), &out_bad.cfg), 0);
  out_bad.cfg.magic = H5FD_SPLITTER_MAGIC;
  out_bad.cfg.version = 0;
  EXPECT_LT(H5Pget_fapl_splitter(sec2_fapl.get(), &out_bad.cfg), 0);
}

TEST(H5FDsplitterTest, HighLevelSec2MirroringGetAccessPlistAndDelete) {
  TempDir dir("h5fd_splitter_mirror");
  const std::string rw_path = dir.child("primary.h5").string();
  const std::string wo_path = dir.child("mirror.h5").string();
  const std::string log_path = dir.child("splitter.log").string();

  ScopedPlist rw_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist wo_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist sp_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(rw_fapl.valid());
  ASSERT_TRUE(wo_fapl.valid());
  ASSERT_TRUE(sp_fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(rw_fapl.get()), 0);
  ASSERT_GE(H5Pset_fapl_sec2(wo_fapl.get()), 0);

  H5FD_splitter_vfd_config_t cfg{};
  cfg.magic = H5FD_SPLITTER_MAGIC;
  cfg.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
  cfg.rw_fapl_id = rw_fapl.get();
  cfg.wo_fapl_id = wo_fapl.get();
  cfg.ignore_wo_errs = false;
  SetPathBuffer(cfg.wo_path, wo_path);
  SetPathBuffer(cfg.log_file_path, log_path);
  ASSERT_GE(H5Pset_fapl_splitter(sp_fapl.get(), &cfg), 0);

  {
    ScopedFile file(
        H5Fcreate(rw_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sp_fapl.get()));
    ASSERT_TRUE(file.valid());

    void* fd_handle = nullptr;
    ASSERT_GE(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &fd_handle), 0);
    ASSERT_NE(fd_handle, nullptr);
    EXPECT_GE(*static_cast<const int*>(fd_handle), 0);

    ScopedPlist active_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(active_fapl.valid());
    SplitterConfigGuard active_cfg;
    ASSERT_GE(H5Pget_fapl_splitter(active_fapl.get(), &active_cfg.cfg), 0);
    EXPECT_STREQ(active_cfg.cfg.wo_path, wo_path.c_str());
    EXPECT_STREQ(active_cfg.cfg.log_file_path, log_path.c_str());

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/vals", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    constexpr std::array<int32_t, 4> kValues = {11, 22, 33, 44};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kValues.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  EXPECT_EQ(ReadWholeFile(rw_path), ReadWholeFile(wo_path));

  for (const std::string& p : {rw_path, wo_path}) {
    ScopedFile ro(H5Fopen(p.c_str(), H5F_ACC_RDONLY, rw_fapl.get()));
    ASSERT_TRUE(ro.valid());
    ScopedDataset dset(H5Dopen2(ro.get(), "/vals", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, 4> got = {};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, got.data()),
              0);
    EXPECT_THAT(got, ElementsAre(11, 22, 33, 44));
  }

  ASSERT_GE(H5Fdelete(rw_path.c_str(), sp_fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(rw_path));
  EXPECT_FALSE(std::filesystem::exists(wo_path));
}

TEST(H5FDsplitterTest, DefaultWoPathSynthesisAndDeletePaths) {
  TempDir dir("h5fd_splitter_default_wo");

  ScopedPlist default_sp_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(default_sp_fapl.valid());
  ASSERT_GE(H5Pset_driver(default_sp_fapl.get(), H5FD_SPLITTER, nullptr), 0);

  // 1. ".h5" extension -> "<stem>_wo.h5".
  {
    const std::string h5_rw = dir.child("alpha.h5").string();
    const std::string h5_wo = dir.child("alpha_wo.h5").string();
    ScopedFdFile f(H5FDopen(h5_rw.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                            default_sp_fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(f.valid());
    f.reset();
    EXPECT_TRUE(std::filesystem::exists(h5_rw));
    EXPECT_TRUE(std::filesystem::exists(h5_wo));
    ASSERT_GE(H5FDdelete(h5_rw.c_str(), default_sp_fapl.get()), 0);
    EXPECT_FALSE(std::filesystem::exists(h5_rw));
    EXPECT_FALSE(std::filesystem::exists(h5_wo));
  }

  // 2. Other extension ".dat" -> "<stem>_wo.dat", deleted via H5P_DEFAULT path
  //    in splitter_cls->del.
  {
    const std::string dat_rw = dir.child("beta.dat").string();
    const std::string dat_wo = dir.child("beta_wo.dat").string();
    ScopedFdFile f(H5FDopen(dat_rw.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                            default_sp_fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(f.valid());
    f.reset();
    EXPECT_TRUE(std::filesystem::exists(dat_rw));
    EXPECT_TRUE(std::filesystem::exists(dat_wo));

    const H5FD_class_t* cls = H5FD_get_class(H5FD_SPLITTER);
    ASSERT_NE(cls, nullptr);
    ASSERT_GE(cls->del(dat_rw.c_str(), H5P_FILE_ACCESS_DEFAULT), 0);
    EXPECT_FALSE(std::filesystem::exists(dat_rw));
    EXPECT_FALSE(std::filesystem::exists(dat_wo));
  }

  // 3. No extension in relative path -> "<name>_wo".
  {
    std::error_code ec;
    const std::filesystem::path prev_cwd = std::filesystem::current_path(ec);
    ASSERT_FALSE(ec);
    absl::Cleanup restore_cwd = [&] {
      std::error_code restore_ec;
      std::filesystem::current_path(prev_cwd, restore_ec);
    };
    std::filesystem::current_path(dir.path(), ec);
    ASSERT_FALSE(ec);

    ScopedFdFile f(H5FDopen("plain_file",
                            H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                            default_sp_fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(f.valid());
    f.reset();
    EXPECT_TRUE(std::filesystem::exists(dir.child("plain_file")));
    EXPECT_TRUE(std::filesystem::exists(dir.child("plain_file_wo")));
    ASSERT_GE(H5FDdelete("plain_file", default_sp_fapl.get()), 0);
  }

  // 4. Overlength paths and missing R/W or W/O delete failures.
  {
    ScopedErrorSilence silence;
    const std::string too_long(H5FD_SPLITTER_PATH_MAX + 10, 'x');
    EXPECT_EQ(
        H5FDopen(too_long.c_str(), H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                 default_sp_fapl.get(), H5FD_MAXADDR),
        nullptr);
    EXPECT_LT(H5FDdelete(too_long.c_str(), default_sp_fapl.get()), 0);

    const H5FD_class_t* cls = H5FD_get_class(H5FD_SPLITTER);
    ASSERT_NE(cls, nullptr);
    EXPECT_LT(cls->del(too_long.c_str(), H5P_FILE_ACCESS_DEFAULT), 0);
    EXPECT_LT(cls->del("unused", H5P_DEFAULT), 0);

    // R/W exists while W/O is missing -> R/W deleted, W/O delete fails.
    const std::string only_rw = dir.child("only_rw.h5").string();
    {
      std::ofstream out(only_rw);
      out << "data";
    }
    EXPECT_LT(H5FDdelete(only_rw.c_str(), default_sp_fapl.get()), 0);
    EXPECT_FALSE(std::filesystem::exists(only_rw));

    // Missing R/W fails immediately.
    EXPECT_LT(H5FDdelete(only_rw.c_str(), default_sp_fapl.get()), 0);
  }
}

TEST(H5FDsplitterTest, OpenArgumentValidationAndLogFileFailure) {
  TempDir dir("h5fd_splitter_open_errs");
  const H5FD_class_t* cls = H5FD_get_class(H5FD_SPLITTER);
  ASSERT_NE(cls, nullptr);

  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ScopedPlist sp_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(sec2_fapl.valid());
  ASSERT_TRUE(sp_fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);

  H5FD_splitter_vfd_config_t cfg{};
  cfg.magic = H5FD_SPLITTER_MAGIC;
  cfg.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
  cfg.rw_fapl_id = H5P_DEFAULT;
  cfg.wo_fapl_id = H5P_DEFAULT;
  SetPathBuffer(cfg.wo_path, dir.child("wo.h5").string());
  // Point log_file_path at an existing directory so fopen(..., "w") fails.
  SetPathBuffer(cfg.log_file_path, dir.string());
  ASSERT_GE(H5Pset_fapl_splitter(sp_fapl.get(), &cfg), 0);

  ScopedErrorSilence silence;
  EXPECT_EQ(cls->open(nullptr, H5F_ACC_RDONLY, sp_fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(cls->open("", H5F_ACC_RDONLY, sp_fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(cls->open("a", H5F_ACC_RDONLY, sp_fapl.get(), 0), nullptr);
  EXPECT_EQ(cls->open("a", H5F_ACC_RDONLY, sp_fapl.get(), HADDR_UNDEF),
            nullptr);
  EXPECT_EQ(cls->open("a", H5F_ACC_RDONLY, sp_fapl.get(), H5FD_MAXADDR + 1),
            nullptr);
  EXPECT_EQ(cls->open("a", H5F_ACC_RDONLY, sec2_fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(cls->open(dir.child("rw.h5").c_str(),
                      H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                      sp_fapl.get(), H5FD_MAXADDR),
            nullptr);
}

TEST(H5FDsplitterTest, MockPassthroughAndChannelFaultInjection) {
  TempDir dir("h5fd_splitter_mock");
  const std::string log_path = dir.child("wo_errors.log").string();

  MockSplitterController ctrl;
  g_ctrl = &ctrl;
  absl::Cleanup clear_ctrl = [] { g_ctrl = nullptr; };

  const H5FD_class_t mock_cls =
      MakeMockChannelClass(H5_VFD_RESERVED + 12, "splitter_mock_io");
  ScopedVfdDriver mock_drv(&mock_cls);
  ASSERT_TRUE(mock_drv.valid());

  ScopedPlist ch_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(ch_fapl.valid());
  ASSERT_GE(H5Pset_driver(ch_fapl.get(), mock_drv.get(), nullptr), 0);

  auto make_splitter_fapl = [&](bool ignore_wo_errs,
                                const std::string& log_file) {
    ScopedPlist sp(H5Pcreate(H5P_FILE_ACCESS));
    if (!sp.valid()) return sp;
    H5FD_splitter_vfd_config_t cfg{};
    cfg.magic = H5FD_SPLITTER_MAGIC;
    cfg.version = H5FD_CURR_SPLITTER_VFD_CONFIG_VERSION;
    cfg.rw_fapl_id = ch_fapl.get();
    cfg.wo_fapl_id = ch_fapl.get();
    cfg.ignore_wo_errs = ignore_wo_errs;
    SetPathBuffer(cfg.wo_path, "wo_file");
    SetPathBuffer(cfg.log_file_path, log_file);
    if (H5Pset_fapl_splitter(sp.get(), &cfg) < 0) {
      sp.reset();
    }
    return sp;
  };

  // 1. Strict mode (ignore_wo_errs = false): passthrough and failures on both
  //    R/W and W/O channels.
  {
    ScopedPlist sp_fapl = make_splitter_fapl(/*ignore_wo_errs=*/false, "");
    ASSERT_TRUE(sp_fapl.valid());

    // Open failure on R/W and open failure on W/O (which cleans up R/W).
    {
      ScopedErrorSilence silence;
      ctrl.rw.fail_open = true;
      EXPECT_EQ(
          H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR),
          nullptr);
      ctrl.rw.fail_open = false;

      ctrl.wo.fail_open = true;
      EXPECT_EQ(
          H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR),
          nullptr);
      ctrl.wo.fail_open = false;
    }

    ctrl.rw.cmp_id = 1;
    ctrl.rw.eoa = 128;
    ctrl.rw.eof = 256;
    ctrl.rw.next_alloc = 16;
    ctrl.wo.cmp_id = 1;
    ctrl.wo.next_alloc = 16;

    ScopedFdFile f1(
        H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR));
    ctrl.rw.cmp_id = 2;
    ScopedFdFile f2(
        H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(f1.valid());
    ASSERT_TRUE(f2.valid());

    EXPECT_LT(H5FDcmp(f1.get(), f2.get()), 0);
    EXPECT_EQ(H5FDcmp(f1.get(), f1.get()), 0);

    // query & get_type_map
    unsigned long qflags = 0;  // NOLINT(runtime/int)
    ASSERT_GE(H5FDquery(f1.get(), &qflags), 0);
    EXPECT_EQ(qflags, H5FD_FEAT_DEFAULT_VFD_COMPATIBLE);

    std::array<H5FD_mem_t, H5FD_MEM_NTYPES> type_map = {};
    ASSERT_GE(H5FD_get_fs_type_map(f1.get(), type_map.data()), 0);
    EXPECT_EQ(type_map[0], H5FD_MEM_SUPER);

    // sb_size / sb_encode / sb_load
    EXPECT_EQ(H5FD_sb_size(f1.get()), hsize_t{16});
    char sb_name[9] = {};
    std::array<uint8_t, 16> sb_buf = {};
    ASSERT_GE(H5FD_sb_encode(f1.get(), sb_name, sb_buf.data()), 0);
    EXPECT_STREQ(sb_name, "MOCKVFD ");
    ASSERT_GE(H5FD_sb_load(f1.get(), sb_name, sb_buf.data()), 0);

    // get_handle
    void* raw_handle = nullptr;
    ASSERT_GE(H5FDget_vfd_handle(f1.get(), H5P_DEFAULT, &raw_handle), 0);
    EXPECT_EQ(*static_cast<const int*>(raw_handle), 42);

    // alloc, free, set_eoa, get_eoa, get_eof
    EXPECT_EQ(H5FDalloc(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 8), haddr_t{16});
    EXPECT_EQ(ctrl.wo.alloc_calls, 1);
    ASSERT_GE(H5FDfree(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, 8), 0);
    EXPECT_EQ(ctrl.wo.free_calls, 1);
    ASSERT_GE(H5FDset_eoa(f1.get(), H5FD_MEM_DEFAULT, 256), 0);
    EXPECT_EQ(ctrl.rw.eoa, haddr_t{256});
    EXPECT_EQ(ctrl.wo.eoa, haddr_t{256});
    EXPECT_EQ(H5FDget_eoa(f1.get(), H5FD_MEM_DEFAULT), haddr_t{256});
    EXPECT_EQ(H5FDget_eof(f1.get(), H5FD_MEM_DEFAULT), haddr_t{256});

    // read only calls R/W; write calls both
    constexpr std::array<uint8_t, 4> kWriteBytes = {1, 2, 3, 4};
    ASSERT_GE(H5FDwrite(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4,
                        kWriteBytes.data()),
              0);
    EXPECT_THAT(ctrl.rw.last_written, ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(ctrl.wo.last_written, ElementsAre(1, 2, 3, 4));

    std::array<uint8_t, 4> read_buf = {};
    const int wo_reads_before = ctrl.wo.read_calls;
    ASSERT_GE(
        H5FDread(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4, read_buf.data()),
        0);
    EXPECT_EQ(ctrl.wo.read_calls, wo_reads_before);

    // flush, truncate, lock, unlock
    ASSERT_GE(H5FDflush(f1.get(), H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDtruncate(f1.get(), H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDlock(f1.get(), true), 0);
    ASSERT_GE(H5FDunlock(f1.get()), 0);

    // ctl: unknown with 0, FAIL_IF_UNKNOWN, and ROUTE_TO_TERMINAL_VFD_FLAG.
    EXPECT_GE(H5FDctl(f1.get(), 999, 0, nullptr, nullptr), 0);
    EXPECT_GE(
        H5FDctl(f1.get(), 999, H5FD_CTL_ROUTE_TO_TERMINAL_VFD_FLAG, nullptr,
                nullptr),
        0);
    EXPECT_EQ(ctrl.rw.ctl_calls, 1);

    // Fault injection on every callback in strict mode.
    {
      ScopedErrorSilence silence;
      const H5FD_class_t* sp_cls = H5FD_get_class(H5FD_SPLITTER);
      ASSERT_NE(sp_cls, nullptr);

      EXPECT_LT(
          H5FDctl(f1.get(), 999, H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr,
                  nullptr),
          0);
      ctrl.rw.fail_ctl = true;
      EXPECT_LT(
          H5FDctl(f1.get(), 999, H5FD_CTL_ROUTE_TO_TERMINAL_VFD_FLAG, nullptr,
                  nullptr),
          0);
      ctrl.rw.fail_ctl = false;

      ctrl.rw.fail_query = true;
      EXPECT_LT(H5FDquery(f1.get(), &qflags), 0);
      ctrl.rw.fail_query = false;

      ctrl.rw.fail_get_type_map = true;
      EXPECT_LT(H5FD_get_fs_type_map(f1.get(), type_map.data()), 0);
      ctrl.rw.fail_get_type_map = false;

      ctrl.rw.fail_sb_encode = true;
      EXPECT_LT(H5FD_sb_encode(f1.get(), sb_name, sb_buf.data()), 0);
      ctrl.rw.fail_sb_encode = false;

      ctrl.rw.fail_sb_decode = true;
      EXPECT_LT(H5FD_sb_load(f1.get(), sb_name, sb_buf.data()), 0);
      ctrl.rw.fail_sb_decode = false;

      ctrl.rw.fail_get_handle = true;
      EXPECT_LT(H5FDget_vfd_handle(f1.get(), H5P_DEFAULT, &raw_handle), 0);
      ctrl.rw.fail_get_handle = false;

      ctrl.rw.fail_get_eoa = true;
      EXPECT_EQ(H5FDget_eoa(f1.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
      ctrl.rw.fail_get_eoa = false;

      ctrl.rw.fail_get_eof = true;
      EXPECT_EQ(H5FDget_eof(f1.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
      ctrl.rw.fail_get_eof = false;

      // Direct read/write checks for address/dxpl errors + channel errors.
      EXPECT_LT(sp_cls->read(f1.get(), H5FD_MEM_DRAW, H5P_DATASET_XFER_DEFAULT,
                             HADDR_UNDEF, 4, read_buf.data()),
                0);
      EXPECT_LT(sp_cls->read(f1.get(), H5FD_MEM_DRAW, H5P_DATASET_XFER_DEFAULT,
                             H5FD_MAXADDR, 16, read_buf.data()),
                0);
      ctrl.rw.fail_read = true;
      EXPECT_LT(H5FDread(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4,
                         read_buf.data()),
                0);
      ctrl.rw.fail_read = false;

      EXPECT_LT(sp_cls->write(f1.get(), H5FD_MEM_DRAW, H5I_INVALID_HID, 0, 4,
                              kWriteBytes.data()),
                0);
      ctrl.rw.fail_write = true;
      EXPECT_LT(H5FDwrite(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4,
                          kWriteBytes.data()),
                0);
      ctrl.rw.fail_write = false;
      ctrl.wo.fail_write = true;
      EXPECT_LT(H5FDwrite(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4,
                          kWriteBytes.data()),
                0);
      ctrl.wo.fail_write = false;

      ctrl.rw.fail_flush = true;
      EXPECT_LT(H5FDflush(f1.get(), H5P_DEFAULT, false), 0);
      ctrl.rw.fail_flush = false;
      ctrl.wo.fail_flush = true;
      EXPECT_LT(H5FDflush(f1.get(), H5P_DEFAULT, false), 0);
      ctrl.wo.fail_flush = false;

      ctrl.rw.fail_truncate = true;
      EXPECT_LT(H5FDtruncate(f1.get(), H5P_DEFAULT, false), 0);
      ctrl.rw.fail_truncate = false;
      ctrl.wo.fail_truncate = true;
      EXPECT_LT(H5FDtruncate(f1.get(), H5P_DEFAULT, false), 0);
      ctrl.wo.fail_truncate = false;

      ctrl.rw.fail_set_eoa = true;
      EXPECT_LT(H5FDset_eoa(f1.get(), H5FD_MEM_DEFAULT, 512), 0);
      ctrl.rw.fail_set_eoa = false;
      ctrl.wo.fail_set_eoa = true;
      EXPECT_LT(H5FDset_eoa(f1.get(), H5FD_MEM_DEFAULT, 512), 0);
      ctrl.wo.fail_set_eoa = false;

      ctrl.rw.fail_alloc = true;
      EXPECT_EQ(H5FDalloc(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 8),
                HADDR_UNDEF);
      ctrl.rw.fail_alloc = false;
      ctrl.wo.fail_alloc = true;
      EXPECT_EQ(H5FDalloc(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 8),
                HADDR_UNDEF);
      ctrl.wo.fail_alloc = false;

      ctrl.rw.fail_free = true;
      EXPECT_LT(H5FDfree(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, 8), 0);
      ctrl.rw.fail_free = false;
      ctrl.wo.fail_free = true;
      EXPECT_LT(H5FDfree(f1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, 8), 0);
      ctrl.wo.fail_free = false;

      ctrl.rw.fail_lock = true;
      EXPECT_LT(H5FDlock(f1.get(), true), 0);
      ctrl.rw.fail_lock = false;
      ctrl.wo.fail_lock = true;
      EXPECT_LT(H5FDlock(f1.get(), true), 0);
      ctrl.wo.fail_lock = false;

      ctrl.rw.fail_unlock = true;
      EXPECT_LT(H5FDunlock(f1.get()), 0);
      ctrl.rw.fail_unlock = false;
      ctrl.wo.fail_unlock = true;
      EXPECT_LT(H5FDunlock(f1.get()), 0);
      ctrl.wo.fail_unlock = false;
    }
  }

  // 2. Permissive mode (ignore_wo_errs = true) with log_file_path:
  //    a) W/O fails individual operations while W/O file is open -> succeeds
  //       (except unlock) and logs entries.
  {
    ScopedPlist sp_fapl = make_splitter_fapl(/*ignore_wo_errs=*/true, log_path);
    ASSERT_TRUE(sp_fapl.valid());

    ctrl.rw.eoa = 128;
    ctrl.wo.eoa = 128;
    ScopedFdFile f(
        H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(f.valid());

    ScopedErrorSilence silence;
    ctrl.wo.fail_set_eoa = true;
    EXPECT_GE(H5FDset_eoa(f.get(), H5FD_MEM_DEFAULT, 256), 0);
    ctrl.wo.fail_set_eoa = false;

    ctrl.wo.fail_write = true;
    constexpr std::array<uint8_t, 4> kData = {9, 8, 7, 6};
    EXPECT_GE(
        H5FDwrite(f.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 4, kData.data()), 0);
    ctrl.wo.fail_write = false;

    ctrl.wo.fail_flush = true;
    EXPECT_GE(H5FDflush(f.get(), H5P_DEFAULT, false), 0);
    ctrl.wo.fail_flush = false;

    ctrl.wo.fail_truncate = true;
    EXPECT_GE(H5FDtruncate(f.get(), H5P_DEFAULT, false), 0);
    ctrl.wo.fail_truncate = false;

    ctrl.wo.fail_alloc = true;
    EXPECT_NE(H5FDalloc(f.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 8), HADDR_UNDEF);
    ctrl.wo.fail_alloc = false;

    ctrl.wo.fail_free = true;
    EXPECT_GE(H5FDfree(f.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8), 0);
    ctrl.wo.fail_free = false;

    ctrl.wo.fail_lock = true;
    EXPECT_GE(H5FDlock(f.get(), false), 0);
    ctrl.wo.fail_lock = false;

    // Close explicitly so logfp flushes and verify logged messages.
    f.reset();
    const std::string log_contents = ReadWholeFile(log_path);
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_set_eoa"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_write"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_flush"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_truncate"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_alloc"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_free"));
    EXPECT_THAT(log_contents, HasSubstr("H5FD__splitter_lock"));
  }

  //    b) W/O open fails with ignore_wo_errs = true -> file opens with
  //       wo_file == NULL; lock/unlock/close skip NULL wo_file cleanly.
  //       (Avoid calling H5FDset_eoa here because H5FD_set_eoa assumes non-NULL.)
  {
    ScopedPlist sp_fapl = make_splitter_fapl(/*ignore_wo_errs=*/true, log_path);
    ASSERT_TRUE(sp_fapl.valid());

    ScopedErrorSilence silence;
    ctrl.wo.fail_open = true;
    ScopedFdFile f(
        H5FDopen("rw_file", H5F_ACC_RDWR, sp_fapl.get(), H5FD_MAXADDR));
    ctrl.wo.fail_open = false;
    ASSERT_TRUE(f.valid());

    EXPECT_GE(H5FDlock(f.get(), false), 0);
    EXPECT_GE(H5FDunlock(f.get()), 0);
    f.reset();

    EXPECT_THAT(ReadWholeFile(log_path),
                HasSubstr("H5FD__splitter_open: unable to open W/O file"));
  }
}

}  // namespace
}  // namespace hdf5
