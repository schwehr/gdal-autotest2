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
// Tests the multi/split virtual file driver (VFD), covering FAPL property list
// configuration, file creation/I/O, member remapping via superblock decoding,
// direct VFL operations, and member-driver error handling
// (third_party/hdf5/src/H5FDmulti.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDmulti.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#include "third_party/hdf5/src/H5FDprivate.h"
}  // extern "C"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

constexpr size_t kNumMemTypes = static_cast<size_t>(H5FD_MEM_NTYPES);

struct H5FDDeleter {
  void operator()(H5FD_t* fd) const {
    if (fd != nullptr) {
      H5FDclose(fd);
    }
  }
};
using ScopedVfd = std::unique_ptr<H5FD_t, H5FDDeleter>;

// RAII wrapper for output arrays populated by H5Pget_fapl_multi().
struct MultiConfigBuffers {
  H5FD_mem_t memb_map[H5FD_MEM_NTYPES]{};
  hid_t memb_fapl[H5FD_MEM_NTYPES]{};
  char* memb_name[H5FD_MEM_NTYPES]{};
  haddr_t memb_addr[H5FD_MEM_NTYPES]{};
  bool relax = false;

  MultiConfigBuffers() {
    for (size_t i = 0; i < kNumMemTypes; ++i) {
      memb_fapl[i] = H5I_INVALID_HID;
      memb_name[i] = nullptr;
      memb_addr[i] = HADDR_UNDEF;
    }
  }

  ~MultiConfigBuffers() { Reset(); }

  void Reset() {
    for (size_t i = 0; i < kNumMemTypes; ++i) {
      if (memb_fapl[i] >= 0 && H5Iis_valid(memb_fapl[i]) > 0) {
        H5Pclose(memb_fapl[i]);
      }
      memb_fapl[i] = H5I_INVALID_HID;
      std::free(memb_name[i]);
      memb_name[i] = nullptr;
    }
  }
};

// State shared across instances of the mock VFD used to exercise member-driver
// error paths inside H5FDmulti.c without requiring real disk failures.
struct MockControl {
  bool fail_get_eoa = false;
  bool fail_set_eoa = false;
  bool fail_get_eof = false;
  bool fail_alloc = false;
  bool fail_flush = false;
  bool fail_truncate = false;
  bool fail_lock_on_second = false;
  bool fail_unlock = false;
  int lock_calls = 0;
  int unlock_calls = 0;
};

struct MockVfdFile {
  H5FD_t pub;
  haddr_t eoa = 0;
  haddr_t eof = 0;
  haddr_t alloc_pos = 0;
  std::vector<uint8_t> data;
};

MockControl g_mock_ctl{};

H5FD_t* MockOpen(const char* name, unsigned /*flags*/, hid_t /*fapl_id*/,
                 haddr_t maxaddr) {
  if (name == nullptr || *name == '\0') {
    return nullptr;
  }
  auto* f = static_cast<MockVfdFile*>(std::calloc(1, sizeof(MockVfdFile)));
  if (f == nullptr) {
    return nullptr;
  }
  new (&f->data) std::vector<uint8_t>();
  f->pub.maxaddr = maxaddr;
  return reinterpret_cast<H5FD_t*>(f);
}

herr_t MockClose(H5FD_t* _file) {
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  if (f != nullptr) {
    f->data.~vector();
    std::free(f);
  }
  return 0;
}

int MockCmp(const H5FD_t* f1, const H5FD_t* f2) {
  return (f1 < f2) ? -1 : ((f1 > f2) ? 1 : 0);
}

haddr_t MockGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  if (g_mock_ctl.fail_get_eoa) {
    return HADDR_UNDEF;
  }
  const auto* f = reinterpret_cast<const MockVfdFile*>(_file);
  return f->eoa;
}

herr_t MockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  if (g_mock_ctl.fail_set_eoa) {
    return -1;
  }
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  f->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  if (g_mock_ctl.fail_get_eof) {
    return HADDR_UNDEF;
  }
  const auto* f = reinterpret_cast<const MockVfdFile*>(_file);
  return std::max(f->eof, f->eoa);
}

haddr_t MockAlloc(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                  hsize_t size) {
  if (g_mock_ctl.fail_alloc) {
    return HADDR_UNDEF;
  }
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  const haddr_t addr = f->alloc_pos;
  f->alloc_pos += size;
  if (f->alloc_pos > f->eoa) {
    f->eoa = f->alloc_pos;
  }
  return addr;
}

herr_t MockFree(H5FD_t* /*file*/, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t /*addr*/, hsize_t /*size*/) {
  return 0;
}

herr_t MockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  auto* out = static_cast<uint8_t*>(buf);
  std::memset(out, 0, size);
  if (addr < f->data.size()) {
    const size_t avail = std::min<size_t>(size, f->data.size() - addr);
    std::memcpy(out, f->data.data() + addr, avail);
  }
  return 0;
}

herr_t MockWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                 haddr_t addr, size_t size, const void* buf) {
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  if (addr + size > f->data.size()) {
    f->data.resize(addr + size, 0);
  }
  std::memcpy(f->data.data() + addr, buf, size);
  f->eof = std::max(f->eof, addr + size);
  return 0;
}

herr_t MockFlush(H5FD_t* /*file*/, hid_t /*dxpl_id*/, bool /*closing*/) {
  return g_mock_ctl.fail_flush ? -1 : 0;
}

herr_t MockTruncate(H5FD_t* _file, hid_t /*dxpl_id*/, bool /*closing*/) {
  if (g_mock_ctl.fail_truncate) {
    return -1;
  }
  auto* f = reinterpret_cast<MockVfdFile*>(_file);
  if (f->eoa < f->data.size()) {
    f->data.resize(f->eoa);
  }
  f->eof = f->eoa;
  return 0;
}

herr_t MockLock(H5FD_t* /*file*/, bool /*rw*/) {
  ++g_mock_ctl.lock_calls;
  if (g_mock_ctl.fail_lock_on_second && g_mock_ctl.lock_calls >= 2) {
    return -1;
  }
  return 0;
}

herr_t MockUnlock(H5FD_t* /*file*/) {
  ++g_mock_ctl.unlock_calls;
  return g_mock_ctl.fail_unlock ? -1 : 0;
}

herr_t MockDelete(const char* /*name*/, hid_t /*fapl*/) { return 0; }

H5FD_class_t MakeMockVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 500;
  cls.name = "mock_multi_member";
  cls.maxaddr = static_cast<haddr_t>(1) << 30;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = MockOpen;
  cls.close = MockClose;
  cls.cmp = MockCmp;
  cls.alloc = MockAlloc;
  cls.free = MockFree;
  cls.get_eoa = MockGetEoa;
  cls.set_eoa = MockSetEoa;
  cls.get_eof = MockGetEof;
  cls.read = MockRead;
  cls.write = MockWrite;
  cls.flush = MockFlush;
  cls.truncate = MockTruncate;
  cls.lock = MockLock;
  cls.unlock = MockUnlock;
  cls.del = MockDelete;
  for (size_t i = 0; i < kNumMemTypes; ++i) {
    cls.fl_map[i] = H5FD_MEM_DEFAULT;
  }
  return cls;
}

TEST(H5FDmultiTest, SplitConfigDefaultsFormatStringsAndValidation) {
  // 1. Default extensions (nullptr) produce "%s.meta" / "%s.raw".
  ScopedPlist fapl_default(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(fapl_default.get(), 0);
  ASSERT_GE(H5Pset_fapl_split(fapl_default.get(), nullptr, H5P_DEFAULT, nullptr,
                              H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Pget_driver(fapl_default.get()), H5FD_MULTI);

  MultiConfigBuffers cfg;
  ASSERT_GE(H5Pget_fapl_multi(fapl_default.get(), cfg.memb_map, cfg.memb_fapl,
                              cfg.memb_name, cfg.memb_addr, &cfg.relax),
            0);
  EXPECT_TRUE(cfg.relax);
  EXPECT_EQ(cfg.memb_map[H5FD_MEM_SUPER], H5FD_MEM_SUPER);
  EXPECT_EQ(cfg.memb_map[H5FD_MEM_OHDR], H5FD_MEM_SUPER);
  EXPECT_EQ(cfg.memb_map[H5FD_MEM_DRAW], H5FD_MEM_DRAW);
  EXPECT_EQ(cfg.memb_map[H5FD_MEM_GHEAP], H5FD_MEM_DRAW);
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_SUPER], "%s.meta");
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_DRAW], "%s.raw");
  EXPECT_EQ(cfg.memb_addr[H5FD_MEM_SUPER], 0u);
  EXPECT_EQ(cfg.memb_addr[H5FD_MEM_DRAW], HADDR_MAX / 2);

  // 2. Suffix extensions without "%s" get "%s" prepended, while strings with
  // "%s" are preserved verbatim. Custom member FAPLs are preserved too.
  ScopedPlist meta_fapl = MakeCoreFapl();
  ScopedPlist raw_fapl = MakeCoreFapl();
  ASSERT_TRUE(meta_fapl.valid());
  ASSERT_TRUE(raw_fapl.valid());

  ScopedPlist fapl_suffix(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_split(fapl_suffix.get(), "-m.h5", meta_fapl.get(),
                              "-r.h5", raw_fapl.get()),
            0);
  cfg.Reset();
  ASSERT_GE(H5Pget_fapl_multi(fapl_suffix.get(), cfg.memb_map, cfg.memb_fapl,
                              cfg.memb_name, cfg.memb_addr, &cfg.relax),
            0);
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_SUPER], "%s-m.h5");
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_DRAW], "%s-r.h5");
  EXPECT_EQ(H5Pget_driver(cfg.memb_fapl[H5FD_MEM_SUPER]), H5FD_CORE);
  EXPECT_EQ(H5Pget_driver(cfg.memb_fapl[H5FD_MEM_DRAW]), H5FD_CORE);

  ScopedPlist fapl_fmt(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_split(fapl_fmt.get(), "dir/%s-meta.h5", H5P_DEFAULT,
                              "dir/%s-raw.h5", H5P_DEFAULT),
            0);
  cfg.Reset();
  ASSERT_GE(H5Pget_fapl_multi(fapl_fmt.get(), cfg.memb_map, cfg.memb_fapl,
                              cfg.memb_name, cfg.memb_addr, &cfg.relax),
            0);
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_SUPER], "dir/%s-meta.h5");
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_DRAW], "dir/%s-raw.h5");

  // Querying with all NULL output pointers is allowed.
  EXPECT_GE(H5Pget_fapl_multi(fapl_fmt.get(), nullptr, nullptr, nullptr,
                              nullptr, nullptr),
            0);

  // 3. Invalid member plist fails without leaking property lists.
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  EXPECT_LT(H5Pset_fapl_split(fapl_fmt.get(), "-m.h5", dcpl.get(), "-r.h5",
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Pset_fapl_split(fapl_fmt.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                              dcpl.get()),
            0);
}

TEST(H5FDmultiTest, MultiConfigCustomMapsNullSubfieldsAndValidation) {
  // 1. All-NULL configuration sets defaults cleanly and survives H5Pcopy.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(fapl.get(), 0);
  ASSERT_GE(H5Pset_fapl_multi(fapl.get(), nullptr, nullptr, nullptr, nullptr,
                              false),
            0);
  ScopedPlist copied(H5Pcopy(fapl.get()));
  ASSERT_TRUE(copied.valid());

  MultiConfigBuffers cfg;
  ASSERT_GE(H5Pget_fapl_multi(copied.get(), cfg.memb_map, cfg.memb_fapl,
                              cfg.memb_name, cfg.memb_addr, &cfg.relax),
            0);
  EXPECT_FALSE(cfg.relax);
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_SUPER], "%s-s.h5");
  EXPECT_STREQ(cfg.memb_name[H5FD_MEM_DRAW], "%s-r.h5");

  // 2. Map BTREE -> SUPER with invalid FAPL ID and NULL name on the aliased
  // slot (BTREE), exercising < 0 / NULL branches in get, copy, and free.
  H5FD_mem_t memb_map[H5FD_MEM_NTYPES]{};
  hid_t memb_fapl[H5FD_MEM_NTYPES]{};
  const char* memb_name[H5FD_MEM_NTYPES]{};
  haddr_t memb_addr[H5FD_MEM_NTYPES]{};
  for (size_t i = 0; i < kNumMemTypes; ++i) {
    memb_map[i] = H5FD_MEM_SUPER;
    memb_fapl[i] = H5P_DEFAULT;
    memb_name[i] = "%s.meta";
    memb_addr[i] = 0;
  }
  memb_map[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
  memb_name[H5FD_MEM_DRAW] = "%s.raw";
  memb_addr[H5FD_MEM_DRAW] = HADDR_MAX / 4;
  memb_fapl[H5FD_MEM_BTREE] = H5I_INVALID_HID;
  memb_name[H5FD_MEM_BTREE] = nullptr;

  ScopedPlist custom_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_multi(custom_fapl.get(), memb_map, memb_fapl, memb_name,
                              memb_addr, true),
            0);
  ScopedPlist custom_copy(H5Pcopy(custom_fapl.get()));
  ASSERT_TRUE(custom_copy.valid());
  cfg.Reset();
  ASSERT_GE(H5Pget_fapl_multi(custom_copy.get(), cfg.memb_map, cfg.memb_fapl,
                              cfg.memb_name, cfg.memb_addr, &cfg.relax),
            0);
  EXPECT_EQ(cfg.memb_fapl[H5FD_MEM_BTREE], H5I_INVALID_HID);
  EXPECT_EQ(cfg.memb_name[H5FD_MEM_BTREE], nullptr);
  EXPECT_EQ(cfg.memb_addr[H5FD_MEM_DRAW], HADDR_MAX / 4);

  // 3. Invalid FAPL, non-multi driver, and out-of-range/invalid member inputs.
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);

  EXPECT_LT(H5Pset_fapl_multi(H5P_DEFAULT, nullptr, nullptr, nullptr, nullptr,
                              true),
            0);
  EXPECT_LT(H5Pset_fapl_multi(dcpl.get(), nullptr, nullptr, nullptr, nullptr,
                              true),
            0);
  EXPECT_LT(H5Pget_fapl_multi(H5P_DEFAULT, nullptr, nullptr, nullptr, nullptr,
                              nullptr),
            0);
  EXPECT_LT(
      H5Pget_fapl_multi(dcpl.get(), nullptr, nullptr, nullptr, nullptr, nullptr),
      0);
  EXPECT_LT(H5Pget_fapl_multi(sec2_fapl.get(), nullptr, nullptr, nullptr,
                              nullptr, nullptr),
            0);

  // Pass non-NULL memb_fapl/memb_name so negative populate_config calls exit
  // cleanly before allocating per-member sec2 FAPLs.
  memb_map[H5FD_MEM_SUPER] = H5FD_MEM_NTYPES;
  EXPECT_LT(H5Pset_fapl_multi(custom_fapl.get(), memb_map, memb_fapl, memb_name,
                              memb_addr, true),
            0);
  memb_map[H5FD_MEM_SUPER] = H5FD_MEM_SUPER;
  memb_fapl[H5FD_MEM_SUPER] = dcpl.get();
  EXPECT_LT(H5Pset_fapl_multi(custom_fapl.get(), memb_map, memb_fapl, memb_name,
                              memb_addr, true),
            0);
  memb_fapl[H5FD_MEM_SUPER] = H5P_DEFAULT;
  memb_name[H5FD_MEM_SUPER] = nullptr;
  EXPECT_LT(H5Pset_fapl_multi(custom_fapl.get(), memb_map, memb_fapl, memb_name,
                              memb_addr, true),
            0);
  memb_name[H5FD_MEM_SUPER] = "";
  EXPECT_LT(H5Pset_fapl_multi(custom_fapl.get(), memb_map, memb_fapl, memb_name,
                              memb_addr, true),
            0);
}

TEST(H5FDmultiTest, SplitAndMultiHdf5DatasetRoundTripAndDelete) {
  TempDir dir("multi_e2e");
  const std::string split_prefix = dir.child("split_file").string();

  // 1. Create a split HDF5 file, populate contiguous and chunked datasets,
  // and verify FAPL and VFD-handle queries.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_split(fapl.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                              H5P_DEFAULT),
            0);

  ScopedFile file(H5Fcreate(split_prefix.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file.get(), "/group", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr auto kInput = std::to_array<int32_t>({11, 22, 33, 44, 55, 66});
  const hsize_t dims[1] = {kInput.size()};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());

  // Contiguous dataset + chunked dataset exercise raw, B-tree, and ohdr I/O.
  ScopedDataset dset_contig(H5Dcreate2(file.get(), "/contig", H5T_NATIVE_INT32,
                                       space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(dset_contig.valid());
  ASSERT_GE(H5Dwrite(dset_contig.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kInput.data()),
            0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  const hsize_t chunk[1] = {2};
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, chunk), 0);
  ScopedDataset dset_chunked(
      H5Dcreate2(grp.get(), "chunked", H5T_NATIVE_INT32, space.get(),
                 H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(dset_chunked.valid());
  ASSERT_GE(H5Dwrite(dset_chunked.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kInput.data()),
            0);

  // Check FAPL retrieval from an open file (triggers H5FD_multi_fapl_get).
  ScopedPlist file_fapl(H5Fget_access_plist(file.get()));
  ASSERT_TRUE(file_fapl.valid());
  MultiConfigBuffers got_cfg;
  ASSERT_GE(H5Pget_fapl_multi(file_fapl.get(), got_cfg.memb_map,
                              got_cfg.memb_fapl, got_cfg.memb_name,
                              got_cfg.memb_addr, &got_cfg.relax),
            0);
  EXPECT_STREQ(got_cfg.memb_name[H5FD_MEM_SUPER], "%s-m.h5");
  got_cfg.Reset();
  file_fapl.reset();

  // Exercise H5Fget_vfd_handle via H5FD_multi_get_handle for both valid and
  // out-of-range H5Pset_multi_type settings.
  ScopedPlist handle_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_multi_type(handle_fapl.get(), H5FD_MEM_DRAW), 0);
  void* raw_handle = nullptr;
  ASSERT_GE(H5Fget_vfd_handle(file.get(), handle_fapl.get(), &raw_handle), 0);
  EXPECT_NE(raw_handle, nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &raw_handle), 0);
    ASSERT_GE(
        H5Pset_multi_type(handle_fapl.get(), static_cast<H5FD_mem_t>(999)), 0);
    EXPECT_LT(H5Fget_vfd_handle(file.get(), handle_fapl.get(), &raw_handle), 0);
  }

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  dset_chunked.reset();
  dset_contig.reset();
  space.reset();
  grp.reset();
  file.reset();

  EXPECT_TRUE(std::filesystem::exists(split_prefix + "-m.h5"));
  EXPECT_TRUE(std::filesystem::exists(split_prefix + "-r.h5"));

  // 2. Reopen read-only with a different member map (everything mapped to
  // SUPER) while preserving the superblock filename "%s-m.h5" so
  // H5FD_multi_sb_decode switches the map back to split and opens "%s-r.h5".
  H5FD_mem_t initial_map[H5FD_MEM_NTYPES]{};
  hid_t initial_fapl[H5FD_MEM_NTYPES]{};
  const char* initial_name[H5FD_MEM_NTYPES]{};
  haddr_t initial_addr[H5FD_MEM_NTYPES]{};
  for (size_t i = 0; i < kNumMemTypes; ++i) {
    initial_map[i] = H5FD_MEM_SUPER;
    initial_fapl[i] = H5P_DEFAULT;
    initial_name[i] = "%s-m.h5";
    initial_addr[i] = 0;
  }
  ScopedPlist multi_ro_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_multi(multi_ro_fapl.get(), initial_map, initial_fapl,
                              initial_name, initial_addr, true),
            0);
  ScopedFile ro_file(
      H5Fopen(split_prefix.c_str(), H5F_ACC_RDONLY, multi_ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());
  ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "/group/chunked", H5P_DEFAULT));
  ASSERT_TRUE(ro_dset.valid());
  std::array<int32_t, kInput.size()> output{};
  ASSERT_GE(H5Dread(ro_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, output.data()),
            0);
  EXPECT_THAT(output, ElementsAreArray(kInput));
  ro_dset.reset();
  ro_file.reset();

  // 3. Reopen RDWR with identical split FAPL to verify writable persistence.
  ScopedFile rw_file(
      H5Fopen(split_prefix.c_str(), H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(rw_file.valid());
  rw_file.reset();

  // 4. Delete using explicit split FAPL, then test env-var-selected
  // H5FD_multi_delete (both multi default and HDF5_DRIVER="split").
  ASSERT_GE(H5Fdelete(split_prefix.c_str(), fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(split_prefix + "-m.h5"));
  EXPECT_FALSE(std::filesystem::exists(split_prefix + "-r.h5"));
}

TEST(H5FDmultiTest, DirectVfdOpenEnvDriverRelaxAndSuperblockDecodePaths) {
  TempDir dir("multi_vfd");
  const std::string multi_prefix = dir.child("multi_env").string();
  const std::string split_prefix = dir.child("split_env").string();

  const char* prev_env = std::getenv("HDF5_DRIVER");
  std::string saved_env = prev_env ? prev_env : "";
  const bool had_env = (prev_env != nullptr);
  absl::Cleanup restore_env = [&] {
    if (had_env) {
      setenv("HDF5_DRIVER", saved_env.c_str(), 1);
    } else {
      unsetenv("HDF5_DRIVER");
    }
  };

  // 1. Open with a driver FAPL carrying NULL driver_info (`!fa` branch in
  // H5FD_multi_open and H5FD_multi_delete) first with HDF5_DRIVER unset, then
  // with HDF5_DRIVER="split".
  unsetenv("HDF5_DRIVER");
  ScopedPlist noinfo_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(noinfo_fapl.get(), H5FD_MULTI, nullptr), 0);

  ScopedVfd mfile(H5FDopen(multi_prefix.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           noinfo_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(mfile, nullptr);
  ASSERT_GE(H5FDset_eoa(mfile.get(), H5FD_MEM_SUPER, 64), 0);
  const std::array<uint8_t, 8> meta_bytes = {'M', 'E', 'T', 'A', 1, 2, 3, 4};
  ASSERT_GE(
      H5FDwrite(mfile.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0, meta_bytes.size(),
                meta_bytes.data()),
      0);
  mfile.reset();
  EXPECT_TRUE(std::filesystem::exists(multi_prefix + "-s.h5"));
  ASSERT_GE(H5FDdelete(multi_prefix.c_str(), noinfo_fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(multi_prefix + "-s.h5"));

  ASSERT_EQ(setenv("HDF5_DRIVER", "split", 1), 0);
  ScopedVfd sfile(H5FDopen(split_prefix.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           noinfo_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(sfile, nullptr);
  sfile.reset();
  EXPECT_TRUE(std::filesystem::exists(split_prefix + ".meta"));
  EXPECT_TRUE(std::filesystem::exists(split_prefix + ".raw"));
  ASSERT_GE(H5FDdelete(split_prefix.c_str(), noinfo_fapl.get()), 0);
  unsetenv("HDF5_DRIVER");

  // 2. Relaxed mode missing-member handling on get_eoa / get_eof vs
  // non-relaxed open failure and missing-superblock failure.
  ScopedPlist relax_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_split(relax_fapl.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                              H5P_DEFAULT),
            0);
  const std::string partial = dir.child("partial").string();
  {
    ScopedVfd wf(H5FDopen(partial.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          relax_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(wf, nullptr);
    ASSERT_GE(H5FDset_eoa(wf.get(), H5FD_MEM_SUPER, 128), 0);
    ASSERT_GE(H5FDwrite(wf.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0,
                        meta_bytes.size(), meta_bytes.data()),
              0);
  }
  ASSERT_TRUE(std::filesystem::remove(partial + "-r.h5"));

  // Read-only open with relax=true succeeds with DRAW member NULL.
  {
    ScopedVfd rf(H5FDopen(partial.c_str(), H5F_ACC_RDONLY, relax_fapl.get(),
                          HADDR_UNDEF));
    ASSERT_NE(rf, nullptr);
    EXPECT_NE(H5FDget_eoa(rf.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
    EXPECT_NE(H5FDget_eoa(rf.get(), H5FD_MEM_DRAW), HADDR_UNDEF);
    EXPECT_NE(H5FDget_eof(rf.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
    EXPECT_NE(H5FDget_eof(rf.get(), H5FD_MEM_DRAW), HADDR_UNDEF);
    EXPECT_EQ(H5FDget_eoa(rf.get(), H5FD_MEM_SUPER), 0u);
    EXPECT_GE(H5FDget_eof(rf.get(), H5FD_MEM_SUPER), meta_bytes.size());
  }

  // Delete remaining superblock file to test opening missing superblock.
  ASSERT_TRUE(std::filesystem::remove(partial + "-m.h5"));
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen(partial.c_str(), H5F_ACC_RDONLY, relax_fapl.get(), HADDR_UNDEF),
        nullptr);

    // Non-relaxed missing raw member fails inside open_members().
    ScopedVfd wf2(H5FDopen(partial.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           relax_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(wf2, nullptr);
    wf2.reset();
    ASSERT_TRUE(std::filesystem::remove(partial + "-r.h5"));

    ScopedPlist strict_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_multi(strict_fapl.get(), nullptr, nullptr, nullptr,
                                nullptr, false),
              0);
    const std::string strict_path = dir.child("strict").string();
    ScopedVfd sw(H5FDopen(strict_path.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          strict_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(sw, nullptr);
    sw.reset();
    ASSERT_TRUE(std::filesystem::remove(strict_path + "-r.h5"));
    EXPECT_EQ(H5FDopen(strict_path.c_str(), H5F_ACC_RDONLY, strict_fapl.get(),
                       HADDR_UNDEF),
              nullptr);
  }

  // 3. Direct H5FD_sb_size / H5FD_sb_encode / H5FD_sb_load with map change and
  // invalid superblock driver name.
  const std::string sb_prefix = dir.child("sb_test").string();
  ScopedPlist sb_fapl1(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_split(sb_fapl1.get(), "-m.h5", H5P_DEFAULT, "-r.h5",
                              H5P_DEFAULT),
            0);
  ScopedVfd fd1(H5FDopen(sb_prefix.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         sb_fapl1.get(), HADDR_UNDEF));
  ASSERT_NE(fd1, nullptr);
  ASSERT_GE(H5FDset_eoa(fd1.get(), H5FD_MEM_SUPER, 64), 0);

  const hsize_t sb_bytes = H5FD_sb_size(fd1.get());
  ASSERT_GT(sb_bytes, 0u);
  char sb_name[16]{};
  std::vector<uint8_t> sb_buf(sb_bytes, 0);
  ASSERT_GE(H5FD_sb_encode(fd1.get(), sb_name, sb_buf.data()), 0);
  EXPECT_STREQ(sb_name, "NCSAmult");
  fd1.reset();

  // Open another handle where BTREE initially maps to its own file, then load
  // the 2-member superblock so map_changed closes the unused BTREE member and
  // commits memb_eoa[H5FD_MEM_SUPER] > 0.
  H5FD_mem_t map3[H5FD_MEM_NTYPES]{};
  hid_t fapl3[H5FD_MEM_NTYPES]{};
  const char* name3[H5FD_MEM_NTYPES]{};
  haddr_t addr3[H5FD_MEM_NTYPES]{};
  for (size_t i = 0; i < kNumMemTypes; ++i) {
    map3[i] = H5FD_MEM_SUPER;
    fapl3[i] = H5P_DEFAULT;
    name3[i] = "%s-m.h5";
    addr3[i] = 0;
  }
  map3[H5FD_MEM_BTREE] = H5FD_MEM_BTREE;
  name3[H5FD_MEM_BTREE] = "%s-b-extra.h5";
  addr3[H5FD_MEM_BTREE] = HADDR_MAX / 4;
  map3[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
  name3[H5FD_MEM_DRAW] = "%s-r.h5";
  addr3[H5FD_MEM_DRAW] = HADDR_MAX / 2;

  ScopedPlist sb_fapl2(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_multi(sb_fapl2.get(), map3, fapl3, name3, addr3, false),
            0);
  ScopedVfd fd2(H5FDopen(sb_prefix.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         sb_fapl2.get(), HADDR_UNDEF));
  ASSERT_NE(fd2, nullptr);
  ASSERT_GE(H5FD_sb_load(fd2.get(), sb_name, sb_buf.data()), 0);
  // Exercises the v1.6 backward-compatibility guard in H5FD_multi_set_eoa
  // when memb_eoa[H5FD_MEM_SUPER] > 0 and eoa > memb_next[SUPER] / 2.
  EXPECT_GE(H5FDset_eoa(fd2.get(), H5FD_MEM_SUPER, (HADDR_MAX / 4) + 128), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FD_sb_load(fd2.get(), "BADMAGIC", sb_buf.data()), 0);
  }
  fd2.reset();

  // 4. Too-long filename checks, cmp, query, ctl, lock/unlock, alloc/free.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FDopen("", H5F_ACC_RDONLY, sb_fapl1.get(), HADDR_UNDEF),
              nullptr);
    const std::string too_long(1100, 'a');
    EXPECT_EQ(H5FDopen(too_long.c_str(),
                       H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                       sb_fapl1.get(), HADDR_UNDEF),
              nullptr);
    EXPECT_LT(H5FDdelete(too_long.c_str(), sb_fapl1.get()), 0);
    EXPECT_LT(H5FDdelete(dir.child("nonexistent_prefix").c_str(),
                         sb_fapl1.get()),
              0);
  }
}

TEST(H5FDmultiTest, DirectVfdOpsCompareDisjointMembersAndMockFailurePaths) {
  TempDir dir("multi_ops");

  // 1. Register mock member VFD to exercise alloc/free, read/write, cmp with
  // disjoint open members, and member-error propagation paths cleanly.
  const H5FD_class_t mock_cls = MakeMockVfdClass();
  const hid_t mock_vfd_id = H5FDregister(&mock_cls);
  ASSERT_GE(mock_vfd_id, 0);
  absl::Cleanup unregister_mock = [&] {
    g_mock_ctl = MockControl{};
    H5FDunregister(mock_vfd_id);
  };

  ScopedPlist memb_mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(memb_mock_fapl.get(), mock_vfd_id, nullptr), 0);

  auto make_all_to_fapl = [&](H5FD_mem_t target, const char* pat) {
    H5FD_mem_t map[H5FD_MEM_NTYPES]{};
    hid_t fapls[H5FD_MEM_NTYPES]{};
    const char* names[H5FD_MEM_NTYPES]{};
    haddr_t addrs[H5FD_MEM_NTYPES]{};
    for (size_t i = 0; i < kNumMemTypes; ++i) {
      map[i] = target;
      fapls[i] = memb_mock_fapl.get();
      names[i] = pat;
      addrs[i] = 0;
    }
    ScopedPlist f(H5Pcreate(H5P_FILE_ACCESS));
    EXPECT_GE(H5Pset_fapl_multi(f.get(), map, fapls, names, addrs, false), 0);
    return f;
  };

  // Disjoint open members in H5FD_multi_cmp: file A only has BTREE open while
  // file B only has OHDR open, covering both cmp = -1 and cmp = 1 branches.
  ScopedPlist fapl_btree = make_all_to_fapl(H5FD_MEM_BTREE, "%s-b");
  ScopedPlist fapl_ohdr = make_all_to_fapl(H5FD_MEM_OHDR, "%s-o");
  const std::string p_a = dir.child("a").string();
  const std::string p_b = dir.child("b").string();
  ScopedVfd fa(H5FDopen(p_a.c_str(),
                        H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                        fapl_btree.get(), HADDR_UNDEF));
  ScopedVfd fb(H5FDopen(p_b.c_str(),
                        H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                        fapl_ohdr.get(), HADDR_UNDEF));
  ASSERT_NE(fa, nullptr);
  ASSERT_NE(fb, nullptr);
  EXPECT_EQ(H5FDcmp(fa.get(), fb.get()), -1);
  EXPECT_EQ(H5FDcmp(fb.get(), fa.get()), 1);

  // Check feature flags, type map, lock/unlock, truncate, ctl, alloc/free.
  unsigned long qflags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_MULTI, &qflags), 0);
  EXPECT_NE(qflags & H5FD_FEAT_DATA_SIEVE, 0u);
  EXPECT_NE(qflags & H5FD_FEAT_PAGED_AGGR, 0u);

  unsigned long fflags = 0;
  ASSERT_GE(H5FDquery(fa.get(), &fflags), 0);
  EXPECT_EQ(fflags, qflags);

  H5FD_mem_t tmap[H5FD_MEM_NTYPES]{};
  ASSERT_GE(H5FD_get_fs_type_map(fa.get(), tmap), 0);
  EXPECT_EQ(tmap[H5FD_MEM_SUPER], H5FD_MEM_BTREE);

  ASSERT_GE(H5FD_set_paged_aggr(fa.get(), true), 0);
  const haddr_t allocated =
      H5FDalloc(fa.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 32);
  ASSERT_NE(allocated, HADDR_UNDEF);
  constexpr std::array<uint8_t, 4> kPayload = {0xAA, 0xBB, 0xCC, 0xDD};
  ASSERT_GE(H5FDwrite(fa.get(), H5FD_MEM_SUPER, H5P_DEFAULT, allocated,
                      kPayload.size(), kPayload.data()),
            0);
  std::array<uint8_t, 4> read_back{};
  ASSERT_GE(H5FDread(fa.get(), H5FD_MEM_SUPER, H5P_DEFAULT, allocated,
                     read_back.size(), read_back.data()),
            0);
  EXPECT_THAT(read_back, ElementsAreArray(kPayload));
  EXPECT_GE(H5FDfree(fa.get(), H5FD_MEM_SUPER, H5P_DEFAULT, allocated, 32), 0);

  ASSERT_GE(H5FDflush(fa.get(), H5P_DEFAULT, false), 0);
  ASSERT_GE(H5FDtruncate(fa.get(), H5P_DEFAULT, false), 0);
  ASSERT_GE(H5FDlock(fa.get(), true), 0);
  ASSERT_GE(H5FDunlock(fa.get()), 0);

  // Unknown ctl opcode succeeds when flag is 0 and fails when fail flag set.
  EXPECT_GE(H5FDctl(fa.get(), 12345, 0, nullptr, nullptr), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5FDctl(fa.get(), 12345, H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr, nullptr),
        0);
  }
  fb.reset();
  fa.reset();

  // 2. Two-member mock multi file to test member failure paths + lock rollback.
  H5FD_mem_t two_map[H5FD_MEM_NTYPES]{};
  hid_t two_fapl[H5FD_MEM_NTYPES]{};
  const char* two_name[H5FD_MEM_NTYPES]{};
  haddr_t two_addr[H5FD_MEM_NTYPES]{};
  for (size_t i = 0; i < kNumMemTypes; ++i) {
    two_map[i] = H5FD_MEM_SUPER;
    two_fapl[i] = memb_mock_fapl.get();
    two_name[i] = "%s-meta";
    two_addr[i] = 0;
  }
  two_map[H5FD_MEM_DRAW] = H5FD_MEM_DRAW;
  two_name[H5FD_MEM_DRAW] = "%s-raw";
  two_addr[H5FD_MEM_DRAW] = 1024;

  ScopedPlist fapl_two(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(
      H5Pset_fapl_multi(fapl_two.get(), two_map, two_fapl, two_name, two_addr,
                        false),
      0);
  const std::string p_two = dir.child("two").string();
  ScopedVfd ftwo(H5FDopen(p_two.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl_two.get(), HADDR_UNDEF));
  ASSERT_NE(ftwo, nullptr);
  EXPECT_EQ(H5FDcmp(ftwo.get(), ftwo.get()), 0);

  {
    ScopedErrorSilence silence;
    g_mock_ctl.fail_get_eoa = true;
    EXPECT_EQ(H5FDget_eoa(ftwo.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
    EXPECT_EQ(H5FDget_eoa(ftwo.get(), H5FD_MEM_DRAW), HADDR_UNDEF);
    g_mock_ctl.fail_get_eoa = false;

    g_mock_ctl.fail_set_eoa = true;
    EXPECT_LT(H5FDset_eoa(ftwo.get(), H5FD_MEM_DRAW, 1088), 0);
    g_mock_ctl.fail_set_eoa = false;

    g_mock_ctl.fail_get_eof = true;
    EXPECT_EQ(H5FDget_eof(ftwo.get(), H5FD_MEM_DEFAULT), HADDR_UNDEF);
    EXPECT_EQ(H5FDget_eof(ftwo.get(), H5FD_MEM_DRAW), HADDR_UNDEF);
    g_mock_ctl.fail_get_eof = false;

    g_mock_ctl.fail_alloc = true;
    EXPECT_EQ(H5FDalloc(ftwo.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16),
              HADDR_UNDEF);
    g_mock_ctl.fail_alloc = false;

    g_mock_ctl.fail_flush = true;
    EXPECT_LT(H5FDflush(ftwo.get(), H5P_DEFAULT, false), 0);
    g_mock_ctl.fail_flush = false;

    g_mock_ctl.fail_truncate = true;
    EXPECT_LT(H5FDtruncate(ftwo.get(), H5P_DEFAULT, false), 0);
    g_mock_ctl.fail_truncate = false;

    // Lock succeeds on SUPER and fails on DRAW, triggering rollback unlock loop
    // cleanly; repeat once with unlock failure during rollback.
    g_mock_ctl.lock_calls = 0;
    g_mock_ctl.unlock_calls = 0;
    g_mock_ctl.fail_lock_on_second = true;
    EXPECT_LT(H5FDlock(ftwo.get(), true), 0);
    EXPECT_GE(g_mock_ctl.unlock_calls, 1);

    g_mock_ctl.lock_calls = 0;
    g_mock_ctl.fail_unlock = true;
    EXPECT_LT(H5FDlock(ftwo.get(), true), 0);
    g_mock_ctl.fail_lock_on_second = false;

    EXPECT_LT(H5FDunlock(ftwo.get()), 0);
    g_mock_ctl.fail_unlock = false;
  }

  // Also test read-only strict failure when mock memb returns NULL because of
  // empty member name pattern result? No, cover remaining non-relaxed error
  // on get_eoa/get_eof when a member slot is explicitly closed/NULLed isn't
  // needed, already tested above.
  ftwo.reset();
}

}  // namespace
}  // namespace hdf5
