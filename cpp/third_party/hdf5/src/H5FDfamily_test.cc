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
// Tests the Family Virtual File Driver (VFD) public FAPL configuration,
// multi-member HDF5 file creation/reopen/superblock driver-info encode/decode,
// repartitioning properties, raw H5FD I/O/EOA/EOF/locking/handle/delete paths,
// default printf-style filename synthesis, and member VFD error handling
// (third_party/hdf5/src/H5FDfamily.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDfamily.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match its internal struct layout when
// including internal headers via hdf5_internal_prelude.h.
#ifndef NDEBUG
#define H5FDFAMILY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
}  // extern "C"
#ifdef H5FDFAMILY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDFAMILY_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

constexpr hsize_t kDefaultTestMembSize = 4096;

// RAII wrapper for raw H5FD_t* handles opened via H5FDopen.
class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(H5FD_t* fd) : fd_(fd) {}
  ~ScopedFd() { reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = nullptr; }
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      reset(other.fd_);
      other.fd_ = nullptr;
    }
    return *this;
  }

  herr_t reset(H5FD_t* next = nullptr) {
    herr_t rc = 0;
    if (fd_ != nullptr) {
      rc = H5FDclose(fd_);
    }
    fd_ = next;
    return rc;
  }

  H5FD_t* get() const { return fd_; }
  bool valid() const { return fd_ != nullptr; }

 private:
  H5FD_t* fd_ = nullptr;
};

ScopedPlist MakeFamilyFapl(hsize_t memb_size,
                           hid_t memb_fapl_id = H5P_DEFAULT) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid()) {
    if (H5Pset_fapl_family(fapl.get(), memb_size, memb_fapl_id) < 0) {
      fapl.reset();
    }
  }
  return fapl;
}

// In-memory fault-injecting VFD used as a member VFD for H5FDfamily to
// deterministically test member open/close/eoa/read/write/flush/truncate/
// lock/unlock/delete failure paths without touching disk.
struct MockMemberBacking {
  std::vector<uint8_t> data;
  haddr_t eoa = 0;
  bool locked = false;
  bool lock_rw = false;
};

struct MockVfdState {
  std::unordered_map<std::string, MockMemberBacking> files;
  int open_calls = 0;
  int query_calls = 0;
  int set_eoa_calls = 0;
  int lock_calls = 0;

  int fail_open_on_call = 0;
  int fail_query_null_file_on_call = 0;
  bool fail_close = false;
  int fail_set_eoa_on_call = 0;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_flush = false;
  bool fail_truncate = false;
  int fail_lock_on_call = 0;
  int fail_unlock_on_call = 0;
  int unlock_calls = 0;
  bool fail_delete = false;
  bool fail_get_handle = false;
};

MockVfdState* g_mock_vfd = nullptr;

struct MockMemberFile {
  H5FD_t pub{};
  std::string* name = nullptr;
  MockMemberBacking* backing = nullptr;
  int dummy_handle = 42;
};

static_assert(std::is_standard_layout_v<MockMemberFile>);
static_assert(offsetof(MockMemberFile, pub) == 0);

H5FD_t* MockOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                 haddr_t /*maxaddr*/) {
  if (g_mock_vfd == nullptr || name == nullptr) return nullptr;
  ++g_mock_vfd->open_calls;
  if (g_mock_vfd->fail_open_on_call == g_mock_vfd->open_calls) {
    return nullptr;
  }

  auto it = g_mock_vfd->files.find(name);
  if (it == g_mock_vfd->files.end()) {
    if ((flags & H5F_ACC_CREAT) == 0) {
      return nullptr;
    }
    it = g_mock_vfd->files.emplace(name, MockMemberBacking{}).first;
  } else if ((flags & H5F_ACC_EXCL) != 0 && (flags & H5F_ACC_CREAT) != 0) {
    return nullptr;
  } else if ((flags & H5F_ACC_TRUNC) != 0) {
    it->second.data.clear();
    it->second.eoa = 0;
  }

  auto* f = new MockMemberFile();
  f->name = new std::string(name);
  f->backing = &it->second;
  return &f->pub;
}

herr_t MockClose(H5FD_t* _file) {
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  const bool fail = (g_mock_vfd != nullptr && g_mock_vfd->fail_close);
  delete f->name;
  delete f;
  return fail ? -1 : 0;
}

int MockCmp(const H5FD_t* _f1, const H5FD_t* _f2) {
  const auto* f1 = reinterpret_cast<const MockMemberFile*>(_f1);
  const auto* f2 = reinterpret_cast<const MockMemberFile*>(_f2);
  if (*f1->name < *f2->name) return -1;
  if (*f1->name > *f2->name) return 1;
  return 0;
}

herr_t MockQuery(const H5FD_t* _file,
                 unsigned long* flags) {  // NOLINT(runtime/int)
  if (g_mock_vfd != nullptr && _file == nullptr) {
    ++g_mock_vfd->query_calls;
    if (g_mock_vfd->fail_query_null_file_on_call == g_mock_vfd->query_calls) {
      return -1;
    }
  }
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

haddr_t MockGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* f = reinterpret_cast<const MockMemberFile*>(_file);
  return f->backing->eoa;
}

herr_t MockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  if (g_mock_vfd != nullptr) {
    ++g_mock_vfd->set_eoa_calls;
    if (g_mock_vfd->fail_set_eoa_on_call == g_mock_vfd->set_eoa_calls) {
      return -1;
    }
  }
  f->backing->eoa = addr;
  return 0;
}

haddr_t MockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* f = reinterpret_cast<const MockMemberFile*>(_file);
  return static_cast<haddr_t>(f->backing->data.size());
}

herr_t MockGetHandle(H5FD_t* _file, hid_t /*fapl*/, void** file_handle) {
  if (g_mock_vfd != nullptr && g_mock_vfd->fail_get_handle) {
    return -1;
  }
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  *file_handle = &f->dummy_handle;
  return 0;
}

herr_t MockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                haddr_t addr, size_t size, void* buf) {
  if (g_mock_vfd != nullptr && g_mock_vfd->fail_read) {
    return -1;
  }
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  std::memset(buf, 0, size);
  if (addr < f->backing->data.size()) {
    const size_t avail = f->backing->data.size() - static_cast<size_t>(addr);
    const size_t to_copy = std::min(size, avail);
    std::memcpy(buf, f->backing->data.data() + addr, to_copy);
  }
  return 0;
}

herr_t MockWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                 haddr_t addr, size_t size, const void* buf) {
  if (g_mock_vfd != nullptr && g_mock_vfd->fail_write) {
    return -1;
  }
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > f->backing->data.size()) {
    f->backing->data.resize(end, 0);
  }
  if (size > 0) {
    std::memcpy(f->backing->data.data() + addr, buf, size);
  }
  return 0;
}

herr_t MockFlush(H5FD_t* /*_file*/, hid_t /*dxpl_id*/, bool /*closing*/) {
  return (g_mock_vfd != nullptr && g_mock_vfd->fail_flush) ? -1 : 0;
}

herr_t MockTruncate(H5FD_t* _file, hid_t /*dxpl_id*/, bool /*closing*/) {
  if (g_mock_vfd != nullptr && g_mock_vfd->fail_truncate) {
    return -1;
  }
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  f->backing->data.resize(static_cast<size_t>(f->backing->eoa), 0);
  return 0;
}

herr_t MockLock(H5FD_t* _file, bool rw) {
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  if (g_mock_vfd != nullptr) {
    ++g_mock_vfd->lock_calls;
    if (g_mock_vfd->fail_lock_on_call == g_mock_vfd->lock_calls) {
      return -1;
    }
  }
  f->backing->locked = true;
  f->backing->lock_rw = rw;
  return 0;
}

herr_t MockUnlock(H5FD_t* _file) {
  auto* f = reinterpret_cast<MockMemberFile*>(_file);
  if (g_mock_vfd != nullptr) {
    ++g_mock_vfd->unlock_calls;
    if (g_mock_vfd->fail_unlock_on_call == g_mock_vfd->unlock_calls) {
      return -1;
    }
  }
  f->backing->locked = false;
  return 0;
}

herr_t MockDelete(const char* name, hid_t /*fapl_id*/) {
  if (g_mock_vfd == nullptr || name == nullptr || g_mock_vfd->fail_delete) {
    return -1;
  }
  return g_mock_vfd->files.erase(name) > 0 ? 0 : -1;
}

H5FD_class_t MakeMockVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 260;
  cls.name = "mock_family_member";
  cls.maxaddr = HADDR_MAX;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.open = MockOpen;
  cls.close = MockClose;
  cls.cmp = MockCmp;
  cls.query = MockQuery;
  cls.get_eoa = MockGetEoa;
  cls.set_eoa = MockSetEoa;
  cls.get_eof = MockGetEof;
  cls.get_handle = MockGetHandle;
  cls.read = MockRead;
  cls.write = MockWrite;
  cls.flush = MockFlush;
  cls.truncate = MockTruncate;
  cls.lock = MockLock;
  cls.unlock = MockUnlock;
  cls.del = MockDelete;
  for (int i = 0; i < H5FD_MEM_NTYPES; ++i) {
    cls.fl_map[i] = H5FD_MEM_SUPER;
  }
  return cls;
}

TEST(H5FDfamilyTest, DriverRegistrationAndFeatureFlags) {
  EXPECT_EQ(H5FD_FAMILY_VALUE, H5_VFD_FAMILY);
  EXPECT_GT(H5FDis_driver_registered_by_name("family"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5_VFD_FAMILY), 0);
  EXPECT_GE(H5FD__family_register(), 0);

  unsigned long flags = 0;  // NOLINT(runtime/int)
  ASSERT_GE(H5FDdriver_query(H5FD_FAMILY, &flags), 0);
  EXPECT_EQ(
      flags & (H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
               H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA),
      static_cast<unsigned long>(
          H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
          H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA));
  EXPECT_EQ(flags & H5FD_FEAT_DIRTY_DRVRINFO_LOAD, 0UL);
}

TEST(H5FDfamilyTest, FaplSetGetCopyAndArgumentValidation) {
  // 1. Default member FAPL uses sec2 and preserves member size.
  ScopedPlist fapl = MakeFamilyFapl(kDefaultTestMembSize);
  ASSERT_TRUE(fapl.valid());
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_FAMILY);

  hsize_t memb_size = 0;
  hid_t raw_memb_fapl = H5I_INVALID_HID;
  ASSERT_GE(H5Pget_fapl_family(fapl.get(), &memb_size, &raw_memb_fapl), 0);
  ScopedPlist memb_fapl(raw_memb_fapl);
  EXPECT_EQ(memb_size, kDefaultTestMembSize);
  EXPECT_TRUE(memb_fapl.valid());
  EXPECT_EQ(H5Pget_driver(memb_fapl.get()), H5FD_SEC2);

  // Passing nullptr output pointers is explicitly permitted.
  EXPECT_GE(H5Pget_fapl_family(fapl.get(), nullptr, nullptr), 0);

  // 2. Custom member FAPL (stdio) and H5Pcopy exercise fapl_copy/fapl_free.
  ScopedPlist stdio_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(stdio_fapl.valid());
  ASSERT_GE(H5Pset_fapl_stdio(stdio_fapl.get()), 0);
  ScopedPlist custom_fapl = MakeFamilyFapl(8192, stdio_fapl.get());
  ASSERT_TRUE(custom_fapl.valid());

  ScopedPlist copied_fapl(H5Pcopy(custom_fapl.get()));
  ASSERT_TRUE(copied_fapl.valid());
  hsize_t copied_size = 0;
  hid_t copied_raw_memb = H5I_INVALID_HID;
  ASSERT_GE(
      H5Pget_fapl_family(copied_fapl.get(), &copied_size, &copied_raw_memb), 0);
  ScopedPlist copied_memb(copied_raw_memb);
  EXPECT_EQ(copied_size, 8192);
  EXPECT_EQ(H5Pget_driver(copied_memb.get()), H5FD_STDIO);

  // 3. Explicit H5P_FILE_ACCESS_DEFAULT as member FAPL hits the ref-inc path in
  // H5FD__family_fapl_copy.
  ScopedPlist def_memb_fapl = MakeFamilyFapl(16384, H5P_FILE_ACCESS_DEFAULT);
  ASSERT_TRUE(def_memb_fapl.valid());
  ScopedPlist copied_def(H5Pcopy(def_memb_fapl.get()));
  ASSERT_TRUE(copied_def.valid());
  hsize_t def_size = 0;
  hid_t def_raw_memb = H5I_INVALID_HID;
  ASSERT_GE(H5Pget_fapl_family(copied_def.get(), &def_size, &def_raw_memb), 0);
  ScopedPlist got_def_memb(def_raw_memb);
  EXPECT_EQ(def_size, 16384);
  EXPECT_TRUE(got_def_memb.valid());

  // 4. Family offset getter/setter round-trip.
  ASSERT_GE(H5Pset_family_offset(fapl.get(), 12345), 0);
  hsize_t offset = 0;
  ASSERT_GE(H5Pget_family_offset(fapl.get(), &offset), 0);
  EXPECT_EQ(offset, 12345);

  // 5. Negative argument validation.
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(H5Pset_fapl_family(H5P_DEFAULT, 4096, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pset_fapl_family(dcpl.get(), 4096, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pset_fapl_family(H5I_INVALID_HID, 4096, H5P_DEFAULT), 0);
  EXPECT_LT(H5Pset_fapl_family(fapl.get(), 4096, dcpl.get()), 0);
  EXPECT_LT(H5Pset_fapl_family(fapl.get(), 4096, H5I_INVALID_HID), 0);

  EXPECT_LT(H5Pget_fapl_family(dcpl.get(), &memb_size, nullptr), 0);
  EXPECT_LT(H5Pget_fapl_family(stdio_fapl.get(), &memb_size, nullptr), 0);

  // Setting the family driver with null driver info yields missing-info error
  // on H5Pget_fapl_family.
  ScopedPlist null_info_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(null_info_fapl.valid());
  ASSERT_GE(H5Pset_driver(null_info_fapl.get(), H5FD_FAMILY, nullptr), 0);
  EXPECT_LT(H5Pget_fapl_family(null_info_fapl.get(), &memb_size, nullptr), 0);
}

TEST(H5FDfamilyTest, Hdf5FileRoundTripSpansMultipleMembersAndReopens) {
  TempDir dir("h5fd_family_hdf5");
  const std::string pattern = dir.child("dataset_%06d.h5").string();

  constexpr int kNumInts = 4096;  // 16 KiB > 4 * 4096 member size
  std::vector<int32_t> write_data(kNumInts);
  std::iota(write_data.begin(), write_data.end(), 100);

  // 1. Create HDF5 file spanning multiple family member files.
  {
    ScopedPlist fapl = MakeFamilyFapl(kDefaultTestMembSize);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate(pattern.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> dims = {kNumInts};
    ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "vec", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_data.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    // Query FAPL from open file (exercises H5FD__family_fapl_get).
    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(got_fapl.valid());
    EXPECT_EQ(H5Pget_driver(got_fapl.get()), H5FD_FAMILY);
    hsize_t got_memb_size = 0;
    hid_t raw_memb = H5I_INVALID_HID;
    ASSERT_GE(H5Pget_fapl_family(got_fapl.get(), &got_memb_size, &raw_memb), 0);
    ScopedPlist got_memb(raw_memb);
    EXPECT_EQ(got_memb_size, kDefaultTestMembSize);

    // Retrieve underlying member POSIX fds at offsets in member 0 and member 2.
    ScopedPlist handle_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(handle_fapl.valid());
    void* handle0 = nullptr;
    ASSERT_GE(H5Pset_family_offset(handle_fapl.get(), 0), 0);
    ASSERT_GE(H5Fget_vfd_handle(file.get(), handle_fapl.get(), &handle0), 0);
    ASSERT_NE(handle0, nullptr);
    const int fd0 = *static_cast<int*>(handle0);
    EXPECT_GE(fd0, 0);

    void* handle2 = nullptr;
    ASSERT_GE(H5Pset_family_offset(handle_fapl.get(), kDefaultTestMembSize * 2),
              0);
    ASSERT_GE(H5Fget_vfd_handle(file.get(), handle_fapl.get(), &handle2), 0);
    ASSERT_NE(handle2, nullptr);
    const int fd2 = *static_cast<int*>(handle2);
    EXPECT_GE(fd2, 0);
    EXPECT_NE(fd0, fd2);

    // Negative handle queries: offset past end of family & invalid FAPL.
    ScopedErrorSilence silence;
    void* bad_handle = nullptr;
    ASSERT_GE(
        H5Pset_family_offset(handle_fapl.get(), kDefaultTestMembSize * 100), 0);
    EXPECT_LT(H5Fget_vfd_handle(file.get(), handle_fapl.get(), &bad_handle), 0);
    EXPECT_LT(H5Fget_vfd_handle(file.get(), H5I_INVALID_HID, &bad_handle), 0);
  }

  // Verify multiple physical member files exist on disk.
  EXPECT_TRUE(std::filesystem::exists(dir.child("dataset_000000.h5")));
  EXPECT_TRUE(std::filesystem::exists(dir.child("dataset_000001.h5")));
  EXPECT_TRUE(std::filesystem::exists(dir.child("dataset_000002.h5")));
  EXPECT_TRUE(std::filesystem::exists(dir.child("dataset_000003.h5")));

  // 2. Reopen standalone with H5F_FAMILY_DEFAULT so H5FD__family_sb_decode
  // auto-detects member size from the superblock, then open two handles
  // concurrently with exact member size to exercise H5FD__family_cmp.
  {
    ScopedPlist fapl_auto = MakeFamilyFapl(H5F_FAMILY_DEFAULT);
    ASSERT_TRUE(fapl_auto.valid());
    ScopedFile f_auto(
        H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, fapl_auto.get()));
    ASSERT_TRUE(f_auto.valid());

    ScopedDataset dset(H5Dopen2(f_auto.get(), "vec", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> read_data(kNumInts, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_data.data()),
              0);
    EXPECT_THAT(read_data, ElementsAreArray(write_data));
  }
  {
    ScopedPlist fapl_exact = MakeFamilyFapl(kDefaultTestMembSize);
    ASSERT_TRUE(fapl_exact.valid());

    ScopedFile f1(H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, fapl_exact.get()));
    ASSERT_TRUE(f1.valid());
    ScopedFile f2(H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, fapl_exact.get()));
    ASSERT_TRUE(f2.valid());

    ScopedDataset dset(H5Dopen2(f2.get(), "vec", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> read_data(kNumInts, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_data.data()),
              0);
    EXPECT_THAT(read_data, ElementsAreArray(write_data));
  }

  // 3. Reopening with mismatched member size fails in H5FD__family_sb_decode.
  {
    ScopedErrorSilence silence;
    ScopedPlist wrong_fapl = MakeFamilyFapl(kDefaultTestMembSize * 2);
    ASSERT_TRUE(wrong_fapl.valid());
    ScopedFile bad_open(
        H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, wrong_fapl.get()));
    EXPECT_FALSE(bad_open.valid());
  }

  // 4. Delete all member files using H5Fdelete.
  {
    ScopedPlist fapl = MakeFamilyFapl(kDefaultTestMembSize);
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Fdelete(pattern.c_str(), fapl.get()), 0);
  }
  EXPECT_FALSE(std::filesystem::exists(dir.child("dataset_000000.h5")));
  EXPECT_FALSE(std::filesystem::exists(dir.child("dataset_000001.h5")));
  EXPECT_FALSE(std::filesystem::exists(dir.child("dataset_000003.h5")));
}

TEST(H5FDfamilyTest, RepartNewsizeAndFileAccessDefaultMemberFapl) {
  TempDir dir("h5fd_family_repart");
  const std::string pattern = dir.child("repart_%06d.h5").string();

  // Create a single-member HDF5 file using H5P_FILE_ACCESS_DEFAULT as the
  // member FAPL (exercises H5FD__family_open's H5P_FILE_ACCESS_DEFAULT branch).
  {
    ScopedPlist fapl = MakeFamilyFapl(8192, H5P_FILE_ACCESS_DEFAULT);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate(pattern.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
  }

  // Reopen with H5F_ACS_FAMILY_NEWSIZE_NAME set (exercises h5repart paths in
  // H5FD__family_open and H5FD__family_sb_decode).
  {
    ScopedPlist fapl = MakeFamilyFapl(8192);
    ASSERT_TRUE(fapl.valid());
    const hsize_t new_size = 16384;
    ASSERT_GE(H5Pset(fapl.get(), H5F_ACS_FAMILY_NEWSIZE_NAME, &new_size), 0);
    ScopedFile file(H5Fopen(pattern.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(file.valid());
  }

  // Verify the updated member size persisted into the superblock.
  {
    ScopedPlist verify_fapl = MakeFamilyFapl(16384);
    ASSERT_TRUE(verify_fapl.valid());
    ScopedFile file(
        H5Fopen(pattern.c_str(), H5F_ACC_RDONLY, verify_fapl.get()));
    ASSERT_TRUE(file.valid());
  }
}

TEST(H5FDfamilyTest, RawVfdIoEoaEofCmpLockHandleTruncateAndGrowth) {
  TempDir dir("h5fd_family_raw");
  constexpr hsize_t kMembSize = 1024;
  const std::string pat_a = dir.child("raw_a_%06d.bin").string();
  const std::string pat_b = dir.child("raw_b_%06d.bin").string();

  ScopedPlist fapl = MakeFamilyFapl(kMembSize);
  ASSERT_TRUE(fapl.valid());

  // 1. Open new raw family file, expand EOA across 3 members, check EOF before
  // and after truncate.
  {
    ScopedFd fd(H5FDopen(pat_a.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());

    unsigned long qflags = 0;  // NOLINT(runtime/int)
    ASSERT_GE(H5FDquery(fd.get(), &qflags), 0);
    EXPECT_NE(qflags & H5FD_FEAT_DIRTY_DRVRINFO_LOAD, 0UL);

    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 2500), 0);
    EXPECT_EQ(H5FDget_eoa(fd.get(), H5FD_MEM_DEFAULT), 2500);
    // All 3 member files exist with physical EOF 0 -> loops down to i == 0.
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 0);

    // Write 1900 bytes starting at offset 512 (spans members 0, 1, and 2).
    std::vector<uint8_t> payload(1900);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xff);
    }
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 512,
                        payload.size(), payload.data()),
              0);
    ASSERT_GE(H5FDflush(fd.get(), H5P_DEFAULT, false), 0);
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 512 + payload.size());

    // Truncate pads member 2 to EOA (2500 total), then read back across all 3
    // members.
    ASSERT_GE(H5FDtruncate(fd.get(), H5P_DEFAULT, false), 0);
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 2500);

    std::vector<uint8_t> readback(payload.size(), 0xaa);
    ASSERT_GE(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 512,
                       readback.size(), readback.data()),
              0);
    EXPECT_THAT(readback, ElementsAreArray(payload));

    // Extend EOA to create member 3 with physical EOF 0 while member 2 has
    // non-zero EOF; H5FDget_eof skips member 3 and lands on member 2.
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 3500), 0);
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 2500);

    // Shrink EOA below a single member size to exercise setting remaining
    // members' EOA to 0.
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 300), 0);
    EXPECT_EQ(H5FDget_eoa(fd.get(), H5FD_MEM_DEFAULT), 300);

    // Exercise advisory lock (shared & exclusive) and unlock across members.
    EXPECT_GE(H5FDlock(fd.get(), false), 0);
    EXPECT_GE(H5FDunlock(fd.get()), 0);
    EXPECT_GE(H5FDlock(fd.get(), true), 0);
    EXPECT_GE(H5FDunlock(fd.get()), 0);
  }

  // 2. Open two handles to compare with H5FDcmp and verify RDONLY write error.
  {
    ScopedFd fd_a1(
        H5FDopen(pat_a.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF));
    ScopedFd fd_a2(
        H5FDopen(pat_a.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF));
    ScopedFd fd_b(H5FDopen(pat_b.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd_a1.valid());
    ASSERT_TRUE(fd_a2.valid());
    ASSERT_TRUE(fd_b.valid());

    EXPECT_EQ(H5FDcmp(fd_a1.get(), fd_a2.get()), 0);
    EXPECT_NE(H5FDcmp(fd_a1.get(), fd_b.get()), 0);

    ScopedErrorSilence silence;
    const std::array<uint8_t, 4> bytes = {1, 2, 3, 4};
    EXPECT_LT(H5FDwrite(fd_a1.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                        bytes.size(), bytes.data()),
              0);
  }

  // 3. Reopening a single-member file smaller than memb_size updates memb_size
  // to member 0's EOF.
  const std::string pat_small = dir.child("small_%06d.bin").string();
  {
    ScopedFd fd(H5FDopen(pat_small.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 256), 0);
    const std::array<uint8_t, 128> buf{};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, buf.size(),
                        buf.data()),
              0);
  }
  {
    ScopedFd fd(
        H5FDopen(pat_small.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 128);
  }

  // 4. Growth past initial 64-member array capacity in both set_eoa and open,
  // plus multi-member H5FDdelete.
  const std::string pat_many = dir.child("many_%06d.bin").string();
  ScopedPlist tiny_fapl = MakeFamilyFapl(/*memb_size=*/64);
  ASSERT_TRUE(tiny_fapl.valid());
  {
    ScopedFd fd(H5FDopen(pat_many.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         tiny_fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
    // 66 members * 64 bytes = 4224 bytes (> initial amembs of 64).
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 66 * 64), 0);
  }
  {
    ScopedFd fd(H5FDopen(pat_many.c_str(), H5F_ACC_RDONLY, tiny_fapl.get(),
                         HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
  }
  EXPECT_GE(H5FDdelete(pat_many.c_str(), tiny_fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(dir.child("many_000000.bin")));
  EXPECT_FALSE(std::filesystem::exists(dir.child("many_000065.bin")));
}

TEST(H5FDfamilyTest, DefaultConfigPrintfFilenamesAndOpenDeleteErrors) {
  TempDir dir("h5fd_family_names");

  // 1. Null driver info triggers default_config=true and automatic printf
  // format insertion for .h5, other extension (.dat), and extensionless
  // filenames.
  ScopedPlist null_info_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(null_info_fapl.valid());
  ASSERT_GE(H5Pset_driver(null_info_fapl.get(), H5FD_FAMILY, nullptr), 0);

  const std::array<std::pair<std::string, std::string>, 3> name_cases = {{
      {dir.child("plain.h5").string(), dir.child("plain-000000.h5").string()},
      {dir.child("plain.dat").string(), dir.child("plain-000000.dat").string()},
      {dir.child("noext").string(), dir.child("noext-000000").string()},
  }};

  for (const auto& [base_name, expected_member0] : name_cases) {
    {
      ScopedFd fd(H5FDopen(base_name.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           null_info_fapl.get(), HADDR_UNDEF));
      ASSERT_TRUE(fd.valid());
    }
    EXPECT_TRUE(std::filesystem::exists(expected_member0));
    EXPECT_GE(H5FDdelete(base_name.c_str(), null_info_fapl.get()), 0);
    EXPECT_FALSE(std::filesystem::exists(expected_member0));
  }

  // 2. Non-default config rejects non-unique filenames in open and delete.
  ScopedPlist explicit_fapl = MakeFamilyFapl(1024);
  ASSERT_TRUE(explicit_fapl.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5FDopen(dir.child("nonunique.h5").string().c_str(),
                       H5F_ACC_RDWR | H5F_ACC_CREAT, explicit_fapl.get(),
                       HADDR_UNDEF),
              nullptr);
    EXPECT_LT(H5FDdelete(dir.child("nonunique.h5").string().c_str(),
                         explicit_fapl.get()),
              0);
    EXPECT_LT(H5FDdelete(dir.child("missing_%06d.h5").string().c_str(),
                         explicit_fapl.get()),
              0);
    EXPECT_EQ(H5FDopen("", H5F_ACC_RDONLY, explicit_fapl.get(), HADDR_UNDEF),
              nullptr);
    // Internal H5FD_delete accepts empty string and exercises empty filename
    // guard in H5FD__family_get_default_printf_filename.
    EXPECT_LT(H5FD_delete("", null_info_fapl.get()), 0);
  }
}

TEST(H5FDfamilyTest, Hdf5DriverEnvVarDefaultFaplAndLibraryReinit) {
  TempDir dir("h5fd_family_env");
  const std::string pat = dir.child("env_%06d.bin").string();

  const char* prev_driver = std::getenv("HDF5_DRIVER");
  const bool had_driver = (prev_driver != nullptr);
  const std::string saved_driver = had_driver ? prev_driver : "";

  absl::Cleanup restore_env = [&] {
    if (had_driver) {
      setenv("HDF5_DRIVER", saved_driver.c_str(), 1);
    } else {
      unsetenv("HDF5_DRIVER");
    }
    H5close();
    H5open();
  };

  ASSERT_GE(H5close(), 0);
  ASSERT_EQ(setenv("HDF5_DRIVER", "family", 1), 0);
  ASSERT_GE(H5open(), 0);

  {
    ScopedFd fd(H5FDopen(pat.c_str(),
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         H5P_DEFAULT, HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
    unsigned long qflags = 0;  // NOLINT(runtime/int)
    ASSERT_GE(H5FDquery(fd.get(), &qflags), 0);
    // H5P_FILE_ACCESS_DEFAULT branch leaves repart_members false.
    EXPECT_EQ(qflags & H5FD_FEAT_DIRTY_DRVRINFO_LOAD, 0UL);
  }
  EXPECT_GE(H5FDdelete(pat.c_str(), H5P_DEFAULT), 0);
}

TEST(H5FDfamilyTest, FaultInjectedMemberVfdCoversErrorAndRollbackPaths) {
  MockVfdState state;
  g_mock_vfd = &state;
  absl::Cleanup clear_mock = [] { g_mock_vfd = nullptr; };

  const H5FD_class_t mock_cls = MakeMockVfdClass();
  const hid_t mock_driver_id = H5FDregister(&mock_cls);
  ASSERT_NE(mock_driver_id, H5I_INVALID_HID);
  absl::Cleanup unregister_mock = [&] { H5FDunregister(mock_driver_id); };

  ScopedPlist memb_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(memb_fapl.valid());
  ASSERT_GE(H5Pset_driver(memb_fapl.get(), mock_driver_id, nullptr), 0);

  ScopedPlist fam_fapl = MakeFamilyFapl(/*memb_size=*/128, memb_fapl.get());
  ASSERT_TRUE(fam_fapl.valid());

  // 1. Create a 2-member mock family file and exercise failure paths on
  // set_eoa, read, write, flush, truncate, lock rollback, and unlock.
  {
    ScopedFd fd(H5FDopen("mock_%06d.dat",
                         H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                         fam_fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());

    // Expand to 2 members (200 bytes -> member 0 size 128, member 1 size 72).
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 200), 0);

    ScopedErrorSilence silence;

    // Fail read and write on member.
    state.fail_read = true;
    std::array<uint8_t, 8> rbuf{};
    EXPECT_LT(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, rbuf.size(),
                       rbuf.data()),
              0);
    state.fail_read = false;

    state.fail_write = true;
    EXPECT_LT(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, rbuf.size(),
                        rbuf.data()),
              0);
    state.fail_write = false;

    // Fail flush and truncate on members.
    state.fail_flush = true;
    EXPECT_LT(H5FDflush(fd.get(), H5P_DEFAULT, false), 0);
    state.fail_flush = false;

    state.fail_truncate = true;
    EXPECT_LT(H5FDtruncate(fd.get(), H5P_DEFAULT, false), 0);
    state.fail_truncate = false;

    // Lock succeeds on member 0, fails on member 1 -> triggers unlock rollback
    // on member 0 (and test unlock error during rollback too).
    state.lock_calls = 0;
    state.fail_lock_on_call = 2;
    EXPECT_LT(H5FDlock(fd.get(), true), 0);
    EXPECT_FALSE(state.files.at("mock_000000.dat").locked);

    state.lock_calls = 0;
    state.unlock_calls = 0;
    state.fail_lock_on_call = 2;
    state.fail_unlock_on_call = 1;
    EXPECT_LT(H5FDlock(fd.get(), false), 0);
    state.fail_lock_on_call = 0;
    state.fail_unlock_on_call = 0;

    // Direct H5FDunlock error path.
    state.unlock_calls = 0;
    state.fail_unlock_on_call = 1;
    EXPECT_LT(H5FDunlock(fd.get()), 0);
    state.fail_unlock_on_call = 0;

    // Fail set_eoa on member 0 (addr > memb_size branch) and on member 1
    // (addr <= memb_size branch).
    state.set_eoa_calls = 0;
    state.fail_set_eoa_on_call = 1;
    EXPECT_LT(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 200), 0);

    state.set_eoa_calls = 0;
    state.fail_set_eoa_on_call = 2;
    EXPECT_LT(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 200), 0);
    state.fail_set_eoa_on_call = 0;

    // Exercise valid and failing member get_handle via H5FDget_vfd_handle.
    void* mock_handle = nullptr;
    EXPECT_GE(H5FDget_vfd_handle(fd.get(), fam_fapl.get(), &mock_handle), 0);
    EXPECT_NE(mock_handle, nullptr);
    state.fail_get_handle = true;
    EXPECT_LT(H5FDget_vfd_handle(fd.get(), fam_fapl.get(), &mock_handle), 0);
    state.fail_get_handle = false;

    // Fail member creation inside set_eoa.
    state.open_calls = 0;
    state.fail_open_on_call = 1;
    EXPECT_LT(H5FDset_eoa(fd.get(), H5FD_MEM_DEFAULT, 400), 0);
    state.fail_open_on_call = 0;
  }

  // 2. Fail H5FD_open on member 1 (via MockQuery failing on the 2nd pre-open
  // query) so H5FD__family_open cleans up already-opened member 0, including
  // when MockClose fails during error cleanup.
  {
    ScopedErrorSilence silence;
    state.query_calls = 0;
    state.fail_query_null_file_on_call = 2;
    state.fail_close = true;
    EXPECT_EQ(
        H5FDopen("mock_%06d.dat", H5F_ACC_RDONLY, fam_fapl.get(), HADDR_UNDEF),
        nullptr);
    state.fail_query_null_file_on_call = 0;
    state.fail_close = false;
  }

  // 3. Fail H5FD__family_close when closing a member returns < 0.
  {
    ScopedFd fd(
        H5FDopen("mock_%06d.dat", H5F_ACC_RDONLY, fam_fapl.get(), HADDR_UNDEF));
    ASSERT_TRUE(fd.valid());
    ScopedErrorSilence silence;
    state.fail_close = true;
    EXPECT_LT(fd.reset(), 0);
    state.fail_close = false;
  }

  // 4. Exercise member deletion failure and final cleanup deletion.
  {
    ScopedErrorSilence silence;
    state.fail_delete = true;
    EXPECT_LT(H5FDdelete("mock_%06d.dat", fam_fapl.get()), 0);
    state.fail_delete = false;
  }
  EXPECT_GE(H5FDdelete("mock_%06d.dat", fam_fapl.get()), 0);
}

}  // namespace
}  // namespace hdf5
