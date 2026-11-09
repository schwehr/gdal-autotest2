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
// Tests the Onion Virtual File Driver (VFD): FAPL configuration, config-string
// parsing, page-aligned and unaligned revision histories, attaching history to
// canonical sec2 files, write locks, ctl opcodes, and error handling
// (third_party/hdf5/src/H5FDonion.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDonion.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5FDonion_priv.h"
#include "third_party/hdf5/src/H5Iprivate.h"
}  // extern "C"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
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

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr uint64_t kCtlGetNumRevisions = 20001;

struct H5FDDeleter {
  void operator()(H5FD_t* fd) const {
    if (fd != nullptr) {
      H5FDclose(fd);
    }
  }
};
using ScopedVfd = std::unique_ptr<H5FD_t, H5FDDeleter>;

// Mock backing VFD used via "{backing_fapl_id: ...;}" config strings to
// deterministically exercise onion I/O, superblock, and close failure paths.
struct OnionMockControl {
  int fail_open_on_call = 0;
  int open_calls = 0;
  int fail_set_eoa_on_call = 0;
  int set_eoa_calls = 0;
  int fail_read_on_call = 0;
  int read_calls = 0;
  int fail_write_on_call = 0;
  int write_calls = 0;
  int fail_close_on_call = 0;
  int close_calls = 0;
  bool enable_sb = false;
  bool fail_sb_encode = false;
  bool fail_sb_decode = false;

  void Reset() {
    fail_open_on_call = 0;
    open_calls = 0;
    fail_set_eoa_on_call = 0;
    set_eoa_calls = 0;
    fail_read_on_call = 0;
    read_calls = 0;
    fail_write_on_call = 0;
    write_calls = 0;
    fail_close_on_call = 0;
    close_calls = 0;
    enable_sb = false;
    fail_sb_encode = false;
    fail_sb_decode = false;
  }
};

struct OnionMockFile {
  H5FD_t pub;
  haddr_t eoa = 0;
  haddr_t eof = 0;
  std::vector<uint8_t> data;
};

OnionMockControl g_onion_mock{};

H5FD_t* OnionMockOpen(const char* name, unsigned /*flags*/, hid_t /*fapl_id*/,
                      haddr_t maxaddr) {
  ++g_onion_mock.open_calls;
  if (name == nullptr || *name == '\0' ||
      g_onion_mock.open_calls == g_onion_mock.fail_open_on_call) {
    return nullptr;
  }
  auto* f = static_cast<OnionMockFile*>(std::calloc(1, sizeof(OnionMockFile)));
  if (f == nullptr) {
    return nullptr;
  }
  new (&f->data) std::vector<uint8_t>();
  f->pub.maxaddr = maxaddr;
  return reinterpret_cast<H5FD_t*>(f);
}

herr_t OnionMockClose(H5FD_t* _file) {
  ++g_onion_mock.close_calls;
  const bool should_fail =
      (g_onion_mock.close_calls == g_onion_mock.fail_close_on_call);
  auto* f = reinterpret_cast<OnionMockFile*>(_file);
  if (f != nullptr) {
    f->data.~vector();
    std::free(f);
  }
  return should_fail ? -1 : 0;
}

int OnionMockCmp(const H5FD_t* f1, const H5FD_t* f2) {
  return (f1 < f2) ? -1 : ((f1 > f2) ? 1 : 0);
}

hsize_t OnionMockSbSize(H5FD_t* /*file*/) {
  return g_onion_mock.enable_sb ? 8 : 0;
}

herr_t OnionMockSbEncode(H5FD_t* /*file*/, char* name, unsigned char* buf) {
  if (g_onion_mock.fail_sb_encode) {
    return -1;
  }
  std::memcpy(name, "MOCKSB00", 9);
  std::memset(buf, 0x5A, 8);
  return 0;
}

herr_t OnionMockSbDecode(H5FD_t* /*file*/, const char* /*name*/,
                         const unsigned char* /*buf*/) {
  return g_onion_mock.fail_sb_decode ? -1 : 0;
}

haddr_t OnionMockGetEoa(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* f = reinterpret_cast<const OnionMockFile*>(_file);
  return f->eoa;
}

herr_t OnionMockSetEoa(H5FD_t* _file, H5FD_mem_t /*type*/, haddr_t addr) {
  ++g_onion_mock.set_eoa_calls;
  if (g_onion_mock.set_eoa_calls == g_onion_mock.fail_set_eoa_on_call) {
    return -1;
  }
  auto* f = reinterpret_cast<OnionMockFile*>(_file);
  f->eoa = addr;
  return 0;
}

haddr_t OnionMockGetEof(const H5FD_t* _file, H5FD_mem_t /*type*/) {
  const auto* f = reinterpret_cast<const OnionMockFile*>(_file);
  return std::max(f->eof, f->eoa);
}

herr_t OnionMockRead(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, void* buf) {
  ++g_onion_mock.read_calls;
  if (g_onion_mock.read_calls == g_onion_mock.fail_read_on_call) {
    return -1;
  }
  auto* f = reinterpret_cast<OnionMockFile*>(_file);
  auto* out = static_cast<uint8_t*>(buf);
  std::memset(out, 0, size);
  if (addr < f->data.size()) {
    const size_t avail = std::min<size_t>(size, f->data.size() - addr);
    std::memcpy(out, f->data.data() + addr, avail);
  }
  return 0;
}

herr_t OnionMockWrite(H5FD_t* _file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                      haddr_t addr, size_t size, const void* buf) {
  ++g_onion_mock.write_calls;
  if (g_onion_mock.write_calls == g_onion_mock.fail_write_on_call) {
    return -1;
  }
  auto* f = reinterpret_cast<OnionMockFile*>(_file);
  if (addr + size > f->data.size()) {
    f->data.resize(addr + size, 0);
  }
  std::memcpy(f->data.data() + addr, buf, size);
  f->eof = std::max(f->eof, addr + size);
  return 0;
}

H5FD_class_t MakeOnionMockVfdClass() {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = 510;
  cls.name = "mock_onion_backing";
  cls.maxaddr = H5FD_MAXADDR;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.sb_size = OnionMockSbSize;
  cls.sb_encode = OnionMockSbEncode;
  cls.sb_decode = OnionMockSbDecode;
  cls.open = OnionMockOpen;
  cls.close = OnionMockClose;
  cls.cmp = OnionMockCmp;
  cls.get_eoa = OnionMockGetEoa;
  cls.set_eoa = OnionMockSetEoa;
  cls.get_eof = OnionMockGetEof;
  cls.read = OnionMockRead;
  cls.write = OnionMockWrite;
  for (size_t i = 0; i < static_cast<size_t>(H5FD_MEM_NTYPES); ++i) {
    cls.fl_map[i] = H5FD_MEM_DEFAULT;
  }
  return cls;
}

H5FD_onion_fapl_info_t MakeOnionInfo(
    uint32_t page_size = 4096,
    uint64_t revision_num = H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST,
    uint8_t creation_flags = 0, const char* comment = "test revision",
    hid_t backing_fapl_id = H5P_DEFAULT) {
  H5FD_onion_fapl_info_t info{};
  info.version = H5FD_ONION_FAPL_INFO_VERSION_CURR;
  info.backing_fapl_id = backing_fapl_id;
  info.page_size = page_size;
  info.store_target = H5FD_ONION_STORE_TARGET_ONION;
  info.revision_num = revision_num;
  info.force_write_open = 0;
  info.creation_flags = creation_flags;
  if (comment != nullptr) {
    std::snprintf(info.comment, sizeof(info.comment), "%s", comment);
  }
  return info;
}

TEST(H5FDonionTest, RegisterAndUnregisterLifecycle) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_library = [] {
    H5Eclear2(H5E_DEFAULT);
    EXPECT_GE(H5close(), 0);
    EXPECT_GE(H5open(), 0);
  };

  const hid_t initial_id = H5FD_ONION;
  ASSERT_NE(initial_id, H5I_INVALID_HID);
  EXPECT_EQ(H5I_get_type(H5FD_ONION_id_g), H5I_VFL);

  // Idempotent when already registered.
  EXPECT_GE(H5FD__onion_register(), 0);
  EXPECT_EQ(H5FD_ONION_id_g, initial_id);

  // Unregister + re-register cleanly.
  ASSERT_GE(H5FD__onion_unregister(), 0);
  EXPECT_EQ(H5FD_ONION_id_g, H5I_INVALID_HID);
  ASSERT_GE(H5I_dec_ref(initial_id), 0);
  ASSERT_GE(H5FD__onion_register(), 0);
  EXPECT_NE(H5FD_ONION_id_g, H5I_INVALID_HID);

  // Full library close/reopen exercises package teardown and init.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5FD_ONION_id_g, H5I_INVALID_HID);
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_ONION, H5I_INVALID_HID);
}

TEST(H5FDonionTest, SetAndGetFaplOnionValidAndInvalidArgs) {
  // 1. Round-trip with H5P_DEFAULT backing FAPL and explicit sec2 backing FAPL.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  const auto info =
      MakeOnionInfo(64, 3, H5FD_ONION_FAPL_INFO_CREATE_FLAG_ENABLE_PAGE_ALIGNMENT,
                    "custom comment", H5P_DEFAULT);
  ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);

  H5FD_onion_fapl_info_t out{};
  ASSERT_GE(H5Pget_fapl_onion(fapl.get(), &out), 0);
  EXPECT_EQ(out.version, H5FD_ONION_FAPL_INFO_VERSION_CURR);
  EXPECT_EQ(out.page_size, 64u);
  EXPECT_EQ(out.revision_num, 3u);
  EXPECT_EQ(out.creation_flags,
            H5FD_ONION_FAPL_INFO_CREATE_FLAG_ENABLE_PAGE_ALIGNMENT);
  EXPECT_STREQ(out.comment, "custom comment");

  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
  const auto info_sec2 = MakeOnionInfo(
      1, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, 0, "sec2", sec2_fapl.get());
  ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info_sec2), 0);

  // 2. Reject invalid FAPLs, versions, non-power-of-two page sizes, and
  // unsupported backing drivers.
  ScopedErrorSilence silence;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist core_fapl = MakeCoreFapl();

  EXPECT_LT(H5Pget_fapl_onion(fapl.get(), nullptr), 0);
  EXPECT_LT(H5Pget_fapl_onion(H5P_DEFAULT, &out), 0);
  EXPECT_LT(H5Pget_fapl_onion(dcpl.get(), &out), 0);
  EXPECT_LT(H5Pget_fapl_onion(sec2_fapl.get(), &out), 0);

  ScopedPlist bare_onion(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(bare_onion.get(), H5FD_ONION, nullptr), 0);
  EXPECT_LT(H5Pget_fapl_onion(bare_onion.get(), &out), 0);

  EXPECT_LT(H5Pset_fapl_onion(H5P_DEFAULT, &info), 0);
  EXPECT_LT(H5Pset_fapl_onion(dcpl.get(), &info), 0);
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), nullptr), 0);

  auto bad_info = info;
  bad_info.version = 99;
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), &bad_info), 0);

  bad_info = info;
  bad_info.page_size = 3;
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), &bad_info), 0);

  bad_info.page_size = 0;
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), &bad_info), 0);

  bad_info = info;
  bad_info.backing_fapl_id = dcpl.get();
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), &bad_info), 0);

  bad_info.backing_fapl_id = core_fapl.get();
  EXPECT_LT(H5Pset_fapl_onion(fapl.get(), &bad_info), 0);
}

TEST(H5FDonionTest, Hdf5FileMultiRevisionHistoryAndPageAlignment) {
  TempDir dir("onion_h5");
  ScopedPlist sec2_backing(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_backing.get()), 0);

  for (uint8_t create_flags :
       {uint8_t{0},
        uint8_t{H5FD_ONION_FAPL_INFO_CREATE_FLAG_ENABLE_PAGE_ALIGNMENT}}) {
    const std::string path =
        dir.child(create_flags ? "aligned.h5" : "unaligned.h5").string();

    // Use page_size=64 so an HDF5 file spans many pages (>= 10 entries in
    // archival_index), avoiding single-entry archival lookup edge cases.
    constexpr uint32_t kFilePageSize = 64;

    // 1. Create revision 0 and write initial dataset.
    auto info = MakeOnionInfo(kFilePageSize,
                              H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST,
                              create_flags, "rev0",
                              create_flags ? sec2_backing.get() : H5P_DEFAULT);
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);

    std::array<int32_t, 64> rev0_data{};
    std::array<int32_t, 64> rev1_data{};
    std::array<int32_t, 64> rev2_data{};
    for (size_t i = 0; i < 64; ++i) {
      rev0_data[i] = static_cast<int32_t>(i + 1);
      rev1_data[i] = static_cast<int32_t>(1000 + i);
      rev2_data[i] = static_cast<int32_t>(2000 + i);
    }
    const hsize_t dims[1] = {rev0_data.size()};
    {
      ScopedFile file(
          H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedSpace space(H5Screate_simple(1, dims, nullptr));
      ScopedDataset dset(H5Dcreate2(file.get(), "/data", H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, rev0_data.data()),
                0);
    }

    uint64_t rev_count = 0;
    ASSERT_GE(H5FDonion_get_revision_count(path.c_str(), fapl.get(), &rev_count),
              0);
    EXPECT_EQ(rev_count, 1u);

    // 2. Append revision 1 and revision 2 (exercises >1 revision history array
    // growth inside H5FD__onion_commit_new_revision_record). Pass an explicit
    // sec2 backing FAPL because H5FD__onion_open_rw passes backing_fapl_id
    // directly to H5FD_open().
    for (size_t rev = 1; rev <= 2; ++rev) {
      info = MakeOnionInfo(kFilePageSize,
                           H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, 0,
                           rev == 1 ? "rev1" : "rev2", sec2_backing.get());
      ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);
      ScopedFile file(H5Fopen(path.c_str(), H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedDataset dset(H5Dopen2(file.get(), "/data", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      const auto& src = (rev == 1) ? rev1_data : rev2_data;
      ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, src.data()),
                0);
    }
    ASSERT_GE(H5FDonion_get_revision_count(path.c_str(), fapl.get(), &rev_count),
              0);
    EXPECT_EQ(rev_count, 3u);

    // 3. Read back committed revisions (1-based in H5FD__onion_open: 1 ->
    // record[0], 2 -> record[1]) and LATEST -> record[2].
    auto verify_revision = [&](uint64_t rev_num,
                               const std::array<int32_t, 64>& expected) {
      auto ro_info = MakeOnionInfo(kFilePageSize, rev_num, 0, nullptr,
                                   H5P_DEFAULT);
      ScopedPlist ro_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_fapl_onion(ro_fapl.get(), &ro_info), 0);
      ScopedFile ro_file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, ro_fapl.get()));
      ASSERT_TRUE(ro_file.valid());
      ScopedDataset ro_dset(H5Dopen2(ro_file.get(), "/data", H5P_DEFAULT));
      std::array<int32_t, 64> actual{};
      ASSERT_GE(H5Dread(ro_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                        H5P_DEFAULT, actual.data()),
                0);
      EXPECT_THAT(actual, ElementsAreArray(expected));
    };
    verify_revision(1, rev0_data);
    verify_revision(2, rev1_data);
    verify_revision(H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, rev2_data);
  }
}

TEST(H5FDonionTest, DirectVfdPartialCrossPageArchivalReadWriteAndSbOps) {
  TempDir dir("onion_rw_direct");
  const std::string path = dir.child("direct.bin").string();
  ScopedPlist sec2_backing(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_backing.get()), 0);

  constexpr uint32_t kPageSize = 16;
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  auto info =
      MakeOnionInfo(kPageSize, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST,
                    H5FD_ONION_FAPL_INFO_CREATE_FLAG_ENABLE_PAGE_ALIGNMENT,
                    "rev0 direct", sec2_backing.get());
  ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);

  // 1. Create revision 0 via H5FDopen directly with small pages (16 bytes).
  {
    ScopedVfd fd(H5FDopen(path.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
    EXPECT_EQ(H5FDget_eoa(fd.get(), H5FD_MEM_DEFAULT), 0u);
    EXPECT_EQ(H5FDget_eof(fd.get(), H5FD_MEM_DEFAULT), 0u);

    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 64), 0);
    EXPECT_EQ(H5FDget_eoa(fd.get(), H5FD_MEM_DRAW), 64u);

    // Zero-length write and read return cleanly without touching backing pages.
    uint8_t dummy = 0;
    EXPECT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 0, &dummy), 0);
    EXPECT_GE(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 0, &dummy), 0);

    // Partial write inside page 0 (offset=4, len=6 -> head=4, tail=6).
    const std::array<uint8_t, 6> p0_mid = {1, 2, 3, 4, 5, 6};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 4, p0_mid.size(),
                        p0_mid.data()),
              0);

    // Overwrite live rev_index page 0 partially (offset=6, len=2).
    const std::array<uint8_t, 2> p0_patch = {0x77, 0x88};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 6,
                        p0_patch.size(), p0_patch.data()),
              0);

    // Unaligned cross-page write with exact page-multiple length (offset=10,
    // len=16 -> spans page 0 and page 1).
    std::array<uint8_t, 16> cross_even{};
    for (size_t i = 0; i < cross_even.size(); ++i) {
      cross_even[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 10,
                        cross_even.size(), cross_even.data()),
              0);

    // Unaligned cross-page write where head + (len % page_size) > page_size
    // (offset=44 -> head=12, len=10 -> spans page 2 and page 3).
    const std::array<uint8_t, 10> cross_spill = {0x10, 0x11, 0x12, 0x13, 0x14,
                                                 0x15, 0x16, 0x17, 0x18, 0x19};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 44,
                        cross_spill.size(), cross_spill.data()),
              0);

    // Read spanning across both live rev_index pages and unwritten hole pages.
    std::array<uint8_t, 32> live_read{};
    ASSERT_GE(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 6,
                       live_read.size(), live_read.data()),
              0);
    EXPECT_EQ(live_read[0], 0x77);
    EXPECT_EQ(live_read[1], 0x88);
    EXPECT_EQ(live_read[4], 0xA0);
    EXPECT_EQ(live_read[19], 0xAF);
    EXPECT_EQ(live_read[26], 0u);  // Unwritten page reads as zero.

    // Also exercise sb_size / sb_encode / sb_load pass-throughs (sec2 returns
    // 0 size and no-ops encode/decode).
    EXPECT_EQ(H5FD_sb_size(fd.get()), 0u);
    char sb_name[16]{};
    uint8_t sb_buf[8]{};
    EXPECT_GE(H5FD_sb_encode(fd.get(), sb_name, sb_buf), 0);
    EXPECT_GE(H5FD_sb_load(fd.get(), "none", sb_buf), 0);
  }

  // 2. Reopen RDWR as rev 1 to exercise partial-page modification of an entry
  // present in the archival_index and unaligned cross-page reads from archive.
  {
    ScopedVfd fd(H5FDopen(path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 64), 0);

    // Read unaligned cross-page from archival_index.
    std::array<uint8_t, 16> archive_read{};
    ASSERT_GE(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 10,
                       archive_read.size(), archive_read.data()),
              0);
    EXPECT_EQ(archive_read[0], 0xA0);
    EXPECT_EQ(archive_read[15], 0xAF);

    // Modify page 0 (which is in archival_index, not yet in rev_index) with
    // head/tail gaps.
    const std::array<uint8_t, 2> arch_patch = {0xE1, 0xE2};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 4,
                        arch_patch.size(), arch_patch.data()),
              0);

    // Test H5FDctl on open file: valid revision count & unknown opcodes.
    uint64_t nrev = 99;
    void* out_ptr = &nrev;
    ASSERT_GE(H5FDctl(fd.get(), kCtlGetNumRevisions, 0, nullptr, &out_ptr), 0);
    EXPECT_EQ(nrev, 1u);
    EXPECT_GE(H5FDctl(fd.get(), 99999, 0, nullptr, nullptr), 0);

    ScopedErrorSilence silence;
    void* null_out = nullptr;
    EXPECT_LT(H5FDctl(fd.get(), kCtlGetNumRevisions, 0, nullptr, &null_out), 0);
    EXPECT_LT(H5FDctl(fd.get(), kCtlGetNumRevisions, 0, nullptr, nullptr), 0);
    EXPECT_LT(
        H5FDctl(fd.get(), 99999, H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr,
                nullptr),
        0);
  }

  // 3. Reopen RDONLY at revision_num=0 (pre-history origin -> zeros) and
  // revision_num=1 (initial committed record), plus write rejection & OOB read.
  auto ro0_info = MakeOnionInfo(kPageSize, 0, 0, nullptr, H5P_DEFAULT);
  ScopedPlist ro0_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(ro0_fapl.get(), &ro0_info), 0);
  {
    ScopedVfd ro0_fd(
        H5FDopen(path.c_str(), H5F_ACC_RDONLY, ro0_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(ro0_fd, nullptr);
    ASSERT_GE(H5FDset_eoa(ro0_fd.get(), H5FD_MEM_DRAW, 16), 0);
    std::array<uint8_t, 8> ro0_buf{};
    ro0_buf.fill(0x55);
    ASSERT_GE(H5FDread(ro0_fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                       ro0_buf.size(), ro0_buf.data()),
              0);
    EXPECT_THAT(ro0_buf, ElementsAre(0, 0, 0, 0, 0, 0, 0, 0));
  }

  auto ro1_info = MakeOnionInfo(kPageSize, 1, 0, nullptr, H5P_DEFAULT);
  ScopedPlist ro1_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(ro1_fapl.get(), &ro1_info), 0);
  ScopedVfd ro_fd(
      H5FDopen(path.c_str(), H5F_ACC_RDONLY, ro1_fapl.get(), HADDR_UNDEF));
  ASSERT_NE(ro_fd, nullptr);
  ASSERT_GE(H5FDset_eoa(ro_fd.get(), H5FD_MEM_DRAW, 64), 0);
  std::array<uint8_t, 4> ro_buf{};
  ASSERT_GE(H5FDread(ro_fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 6, ro_buf.size(),
                     ro_buf.data()),
            0);
  EXPECT_THAT(ro_buf, ElementsAre(0x77, 0x88, 5, 6));

  {
    ScopedErrorSilence silence;
    const uint8_t byte = 0x5A;
    EXPECT_LT(H5FDwrite(ro_fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 1, &byte),
              0);
    std::array<uint8_t, 16> too_far{};
    EXPECT_LT(H5FDread(ro_fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 60,
                       too_far.size(), too_far.data()),
              0);
  }
}

TEST(H5FDonionTest, AttachOnionHistoryToExistingCanonicalSec2File) {
  TempDir dir("onion_canon");
  for (uint8_t flags :
       {uint8_t{0},
        uint8_t{H5FD_ONION_FAPL_INFO_CREATE_FLAG_ENABLE_PAGE_ALIGNMENT}}) {
    const std::string path =
        dir.child(flags ? "canon_aligned.bin" : "canon_plain.bin").string();

    // 1. Write a plain canonical sec2 file of 20 bytes (not a page multiple of
    // page_size=8) with no .onion companion file yet.
    constexpr std::string_view kCanonBytes = "CANONICAL_BASE_BYTES";
    static_assert(kCanonBytes.size() == 20);
    {
      ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
      ScopedVfd canon(H5FDopen(path.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               sec2_fapl.get(), HADDR_UNDEF));
      ASSERT_NE(canon, nullptr);
      ASSERT_GE(H5FDset_eoa(canon.get(), H5FD_MEM_DRAW, kCanonBytes.size()), 0);
      ASSERT_GE(H5FDwrite(canon.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                          kCanonBytes.size(), kCanonBytes.data()),
                0);
    }

    ScopedPlist sec2_backing(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_sec2(sec2_backing.get()), 0);
    auto info = MakeOnionInfo(8, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, flags,
                              "attached onion", sec2_backing.get());
    ScopedPlist onion_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_onion(onion_fapl.get(), &info), 0);

    // 2. RDONLY fail when .onion does not exist yet.
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(
          H5FDopen(path.c_str(), H5F_ACC_RDONLY, onion_fapl.get(), HADDR_UNDEF),
          nullptr);
    }

    // 3. RDWR open creates initial .onion file and attaches history. Write to
    // page 0 and page 2 (2 entries in archival_index), with page 2 spanning
    // across origin_eof=20 with head gap and tail beyond origin_eof.
    {
      ScopedVfd attached(
          H5FDopen(path.c_str(), H5F_ACC_RDWR, onion_fapl.get(), HADDR_UNDEF));
      ASSERT_NE(attached, nullptr);
      EXPECT_TRUE(std::filesystem::exists(path + ".onion"));
      ASSERT_GE(H5FDset_eoa(attached.get(), H5FD_MEM_DRAW, 32), 0);

      const uint8_t head_patch = 'W';
      ASSERT_GE(H5FDwrite(attached.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, 1,
                          &head_patch),
                0);
      // Partial write inside page 2 (16..23) where origin_eof ends at byte 20.
      const std::array<uint8_t, 3> patch = {'X', 'Y', 'Z'};
      ASSERT_GE(H5FDwrite(attached.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 18,
                          patch.size(), patch.data()),
                0);
    }

    uint64_t nrev = 0;
    ASSERT_GE(
        H5FDonion_get_revision_count(path.c_str(), onion_fapl.get(), &nrev), 0);
    EXPECT_EQ(nrev, 1u);

    // Read revision 0 (canonical original bytes) and latest revision (patched).
    {
      auto ro0_info = MakeOnionInfo(8, 0, 0, nullptr, H5P_DEFAULT);
      ScopedPlist ro0_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_fapl_onion(ro0_fapl.get(), &ro0_info), 0);
      ScopedVfd ro0(H5FDopen(path.c_str(), H5F_ACC_RDONLY, ro0_fapl.get(),
                             HADDR_UNDEF));
      ASSERT_NE(ro0, nullptr);
      ASSERT_GE(H5FDset_eoa(ro0.get(), H5FD_MEM_DRAW, 20), 0);
      std::array<char, 20> base{};
      ASSERT_GE(H5FDread(ro0.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, base.size(),
                         base.data()),
                0);
      EXPECT_EQ(std::string_view(base.data(), base.size()), kCanonBytes);
    }

    ScopedVfd ro(
        H5FDopen(path.c_str(), H5F_ACC_RDONLY, onion_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(ro, nullptr);
    ASSERT_GE(H5FDset_eoa(ro.get(), H5FD_MEM_DRAW, 28), 0);
    std::array<uint8_t, 28> got{};
    ASSERT_GE(H5FDread(ro.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, got.size(),
                       got.data()),
              0);
    EXPECT_EQ(got[2], 'W');
    EXPECT_EQ(std::memcmp(got.data() + 3, kCanonBytes.data() + 3, 15), 0);
    EXPECT_THAT(std::vector<uint8_t>(got.begin() + 18, got.begin() + 24),
                ElementsAre('X', 'Y', 'Z', 0, 0, 0));
  }
}

TEST(H5FDonionTest, DriverByNameConfigStringsAndErrorCases) {
  TempDir dir("onion_cfg");
  const std::string path = dir.child("by_name.h5").string();

  // 1. Create via curly-brace config string specifying every recognized token.
  const std::string create_cfg =
      "{version: H5FD_ONION_FAPL_INFO_VERSION_CURR; "
      "backing_fapl_id: H5P_DEFAULT; page_size: 8; "
      "revision_num: H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST; "
      "force_write_open: 0; creation_flags: 1; comment: cfg_create;}";
  ScopedPlist fapl_cfg(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver_by_name(fapl_cfg.get(), "onion", create_cfg.c_str()),
            0);

  {
    ScopedVfd fd(H5FDopen(path.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl_cfg.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 16), 0);
    const std::array<uint8_t, 8> payload = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, payload.size(),
                        payload.data()),
              0);
  }

  // 2. Append a second revision passing explicit numeric backing_fapl_id and
  // H5I_INVALID_HID token paths. Note: H5FD__onion_open() consumes one app ref
  // of a parsed numeric backing_fapl_id on close of the open call.
  const hid_t sec2_raw = H5Pcreate(H5P_FILE_ACCESS);
  ASSERT_GE(sec2_raw, 0);
  ASSERT_GE(H5Pset_fapl_sec2(sec2_raw), 0);
  absl::Cleanup close_sec2_raw = [&] {
    if (H5Iis_valid(sec2_raw) > 0) {
      H5Pclose(sec2_raw);
    }
  };
  const std::string rev_cfg =
      "{backing_fapl_id: " + std::to_string(sec2_raw) +
      "; page_size: 8; revision_num: 1; comment: cfg_rev2;}";
  ScopedPlist fapl_rev2(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver_by_name(fapl_rev2.get(), "onion", rev_cfg.c_str()),
            0);
  {
    ScopedVfd fd(H5FDopen(path.c_str(), H5F_ACC_RDWR, fapl_rev2.get(),
                          HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
  }

  // 3. Reopen read-only via integer shortcut ("0", "1") and H5I_INVALID_HID.
  for (const char* shortcut : {"0", "1", "{backing_fapl_id: H5I_INVALID_HID;}"}) {
    ScopedPlist fapl_short(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_driver_by_name(fapl_short.get(), "onion", shortcut), 0);
    ScopedVfd fd(
        H5FDopen(path.c_str(), H5F_ACC_RDONLY, fapl_short.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
  }

  // 4. Reject empty, whitespace-only, unknown-token, and missing config strs.
  ScopedErrorSilence silence;
  for (const char* bad_cfg : {"", "{ }", "{unknown_key: 42;}"}) {
    ScopedPlist bad_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_driver_by_name(bad_fapl.get(), "onion", bad_cfg), 0);
    EXPECT_EQ(
        H5FDopen(path.c_str(), H5F_ACC_RDONLY, bad_fapl.get(), HADDR_UNDEF),
        nullptr);
  }

  ScopedPlist bare_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(bare_fapl.get(), H5FD_ONION, nullptr), 0);
  EXPECT_EQ(
      H5FDopen(path.c_str(), H5F_ACC_RDONLY, bare_fapl.get(), HADDR_UNDEF),
      nullptr);
}

TEST(H5FDonionTest, WriteLockOutOfRangeRevisionCorruptOnionAndRevCountErrors) {
  TempDir dir("onion_errors");
  const std::string path = dir.child("lock_and_err.h5").string();

  ScopedPlist sec2_backing(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_backing.get()), 0);
  auto info = MakeOnionInfo(16, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST, 0,
                            "base", sec2_backing.get());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);

  // Create initial file.
  {
    ScopedVfd fd(H5FDopen(path.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
  }

  ScopedErrorSilence silence;

  // 1. Concurrent write-open lock rejection and recovery file cleanup on close.
  {
    ScopedVfd holder(H5FDopen(path.c_str(), H5F_ACC_RDWR, fapl.get(),
                              HADDR_UNDEF));
    ASSERT_NE(holder, nullptr);
    EXPECT_TRUE(std::filesystem::exists(path + ".onion.recovery"));
    EXPECT_EQ(H5FDopen(path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
              nullptr);
  }
  EXPECT_FALSE(std::filesystem::exists(path + ".onion.recovery"));

  // 2. Out-of-range revision ID and bad store target / empty name.
  auto bad_rev_info = MakeOnionInfo(16, 42, 0, "oob", H5P_DEFAULT);
  ScopedPlist bad_rev_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(bad_rev_fapl.get(), &bad_rev_info), 0);
  EXPECT_EQ(
      H5FDopen(path.c_str(), H5F_ACC_RDONLY, bad_rev_fapl.get(), HADDR_UNDEF),
      nullptr);

  auto bad_target_info = info;
  bad_target_info.store_target =
      static_cast<H5FD_onion_target_file_constant_t>(99);
  ScopedPlist bad_target_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(bad_target_fapl.get(), &bad_target_info), 0);
  EXPECT_EQ(H5FDopen(path.c_str(), H5F_ACC_RDONLY, bad_target_fapl.get(),
                     HADDR_UNDEF),
            nullptr);
  EXPECT_EQ(H5FDopen("", H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF), nullptr);

  // 3. H5FDonion_get_revision_count argument validation & nonexistent file.
  uint64_t rev_count = 0;
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
  EXPECT_LT(H5FDonion_get_revision_count(nullptr, fapl.get(), &rev_count), 0);
  EXPECT_LT(H5FDonion_get_revision_count("", fapl.get(), &rev_count), 0);
  EXPECT_LT(H5FDonion_get_revision_count(path.c_str(), fapl.get(), nullptr), 0);
  EXPECT_LT(
      H5FDonion_get_revision_count(path.c_str(), dcpl.get(), &rev_count), 0);
  EXPECT_LT(
      H5FDonion_get_revision_count(path.c_str(), sec2_fapl.get(), &rev_count),
      0);
  EXPECT_LT(H5FDonion_get_revision_count(dir.child("missing.h5").c_str(),
                                         fapl.get(), &rev_count),
            0);

  // 4. Corrupt .onion header signature causes open failure without leaking.
  const std::string corrupt_hdr = dir.child("corrupt_hdr.h5").string();
  {
    ScopedVfd fd(H5FDopen(corrupt_hdr.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
  }
  {
    std::fstream ofs(corrupt_hdr + ".onion",
                     std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    ofs.seekp(0);
    ofs.write("BAD!", 4);
  }
  EXPECT_EQ(
      H5FDopen(corrupt_hdr.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
      nullptr);

  // 5. Corrupt .onion history record triggers ingest_history failure.
  const std::string corrupt_hist = dir.child("corrupt_hist.h5").string();
  {
    ScopedVfd fd(H5FDopen(corrupt_hist.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
  }
  {
    const auto sz = std::filesystem::file_size(corrupt_hist + ".onion");
    ASSERT_GT(sz, 4u);
    std::fstream ofs(corrupt_hist + ".onion",
                     std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    ofs.seekp(static_cast<std::streamoff>(sz - 12));
    ofs.write("XXXX", 4);
  }
  EXPECT_EQ(
      H5FDopen(corrupt_hist.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
      nullptr);
}

TEST(H5FDonionTest, RegisterFailsWhenVflIdGroupIsUnavailable) {
  ASSERT_GE(H5open(), 0);
  ASSERT_NE(H5FD_ONION, H5I_INVALID_HID);

  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    if (H5FD__onion_unregister() < 0) std::exit(2);
    if (H5I_clear_type(H5I_VFL, true, false) < 0) std::exit(3);
    if (H5I_dec_type_ref(H5I_VFL) < 0) std::exit(4);

    int exit_code = 0;
    {
      ScopedErrorSilence silence;
      const herr_t reg_rc = H5FD__onion_register();
      const hid_t post_id = H5FD_ONION_id_g;
      exit_code = (reg_rc < 0 && post_id == H5I_INVALID_HID) ? 0 : 5;
    }
    std::exit(exit_code);
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(H5FDonionTest, DirectCallbacksInvalidOpenArgsCreateTrapsAndCorruptRecord) {
  TempDir dir("onion_traps");
  const std::string valid_path = dir.child("valid.h5").string();

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  const auto info = MakeOnionInfo(16, H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST,
                                  0, "valid", H5P_DEFAULT);
  ASSERT_GE(H5Pset_fapl_onion(fapl.get(), &info), 0);

  // 1. Direct H5FD_class_t::open / read / write edge cases bypassed by H5FD_*.
  const auto* onion_cls =
      static_cast<const H5FD_class_t*>(H5I_object(H5FD_ONION));
  ASSERT_NE(onion_cls, nullptr);

  {
    ScopedVfd fd(H5FDopen(valid_path.c_str(),
                          H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                          fapl.get(), HADDR_UNDEF));
    ASSERT_NE(fd, nullptr);
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 16), 0);

    uint8_t byte = 0x42;
    EXPECT_GE(fd->cls->write(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 0, &byte),
              0);
    EXPECT_GE(fd->cls->read(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 0, &byte),
              0);

    ScopedErrorSilence silence;
    std::array<uint8_t, 16> oob_buf{};
    EXPECT_LT(fd->cls->read(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 8,
                            oob_buf.size(), oob_buf.data()),
              0);
    EXPECT_EQ(
        onion_cls->open(valid_path.c_str(), H5F_ACC_RDONLY, fapl.get(), 0),
        nullptr);
    EXPECT_EQ(onion_cls->open(valid_path.c_str(), H5F_ACC_RDONLY,
                              H5I_INVALID_HID, HADDR_MAX),
              nullptr);
  }

  ScopedErrorSilence silence;

  // 2. Non-power-of-two page_size via config_str triggers H5FD__onion_open
  // validation after H5Pset_driver_by_name.
  ScopedPlist bad_ps_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(
      H5Pset_driver_by_name(bad_ps_fapl.get(), "onion", "{page_size: 3;}"), 0);
  EXPECT_EQ(H5FDopen(valid_path.c_str(), H5F_ACC_RDONLY, bad_ps_fapl.get(),
                     HADDR_UNDEF),
            nullptr);

  // 3. Create/truncate failures across canonical, .onion, and .onion.recovery
  // backing open calls.
  EXPECT_EQ(H5FDopen(dir.child("missing_subdir/file.h5").c_str(),
                     H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC, fapl.get(),
                     HADDR_UNDEF),
            nullptr);

  const std::string trap_onion = dir.child("trap_onion.h5").string();
  ASSERT_TRUE(std::filesystem::create_directory(trap_onion + ".onion"));
  EXPECT_EQ(H5FDopen(trap_onion.c_str(),
                     H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC, fapl.get(),
                     HADDR_UNDEF),
            nullptr);

  const std::string trap_recov = dir.child("trap_recov.h5").string();
  ASSERT_TRUE(
      std::filesystem::create_directory(trap_recov + ".onion.recovery"));
  EXPECT_EQ(H5FDopen(trap_recov.c_str(),
                     H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC, fapl.get(),
                     HADDR_UNDEF),
            nullptr);

  // 4. RW reopen failure inside H5FD__onion_open_rw with revision_num = 0 and
  // H5P_DEFAULT backing FAPL (skips ingest_revision_record, so no leak).
  auto rw0_info = MakeOnionInfo(16, 0, 0, "fail_rw", H5P_DEFAULT);
  ScopedPlist rw0_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_fapl_onion(rw0_fapl.get(), &rw0_info), 0);
  EXPECT_EQ(
      H5FDopen(valid_path.c_str(), H5F_ACC_RDWR, rw0_fapl.get(), HADDR_UNDEF),
      nullptr);

  // 5. Corrupt the first revision record signature ("ORRS" at offset 40 in
  // unaligned valid.h5.onion) so H5FD__onion_ingest_revision_record fails at
  // initial signature check cleanly before allocating record buffers.
  {
    std::fstream ofs(valid_path + ".onion",
                     std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(ofs.is_open());
    ofs.seekp(H5FD_ONION_ENCODED_SIZE_HEADER);
    ofs.write("BADR", 4);
  }
  EXPECT_EQ(
      H5FDopen(valid_path.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
      nullptr);
}

TEST(H5FDonionTest, MockBackingVfdCreateAttachIoCloseAndSuperblockFailures) {
  TempDir dir("onion_mock");
  const H5FD_class_t mock_cls = MakeOnionMockVfdClass();
  const hid_t mock_vfd_id = H5FDregister(&mock_cls);
  ASSERT_GE(mock_vfd_id, 0);
  absl::Cleanup cleanup_mock = [&] {
    g_onion_mock.Reset();
    H5FDunregister(mock_vfd_id);
  };

  ScopedPlist mock_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_driver(mock_fapl.get(), mock_vfd_id, nullptr), 0);

  // Helper that clones `backing_src_fapl` because H5FD__onion_open() consumes
  // one app ref when `config_str` contains a numeric backing_fapl_id.
  auto open_onion_with_fapl = [](const char* name, unsigned flags,
                                 hid_t backing_src_fapl) -> ScopedVfd {
    const hid_t dup = H5Pcopy(backing_src_fapl);
    EXPECT_GE(dup, 0);
    const std::string cfg =
        "{backing_fapl_id: " + std::to_string(dup) + "; page_size: 16;}";
    ScopedPlist onion_fapl(H5Pcreate(H5P_FILE_ACCESS));
    EXPECT_GE(H5Pset_driver_by_name(onion_fapl.get(), "onion", cfg.c_str()), 0);
    H5FD_t* raw = H5FDopen(name, flags, onion_fapl.get(), HADDR_UNDEF);
    if (H5Iis_valid(dup) > 0) {
      H5Pclose(dup);
    }
    return ScopedVfd(raw);
  };

  // Canonical sec2 file (no .onion on disk) used to enter the attach path.
  const std::string canon_path = dir.child("canon_mock.bin").string();
  {
    ScopedPlist sec2_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_sec2(sec2_fapl.get()), 0);
    ScopedVfd canon(H5FDopen(canon_path.c_str(),
                             H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                             sec2_fapl.get(), HADDR_UNDEF));
    ASSERT_NE(canon, nullptr);
    ASSERT_GE(H5FDset_eoa(canon.get(), H5FD_MEM_DRAW, 16), 0);
    const std::array<uint8_t, 16> base_bytes{};
    ASSERT_GE(H5FDwrite(canon.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                        base_bytes.size(), base_bytes.data()),
              0);
  }

  ScopedErrorSilence silence;

  // 1. Invalid backing FAPL class (H5P_DATASET_CREATE) triggers
  // H5FD__onion_get_legit_fapl_id() failure in create_truncate and attach.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    EXPECT_EQ(
        open_onion_with_fapl("invalid_backing_create",
                             H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                             dcpl.get()),
        nullptr);
    EXPECT_EQ(
        open_onion_with_fapl(canon_path.c_str(), H5F_ACC_RDWR, dcpl.get()),
        nullptr);
  }

  // 2. H5FD__onion_create_truncate_onion() set_eoa / write / cleanup-close
  // failure paths (calls 1..3 = original, recovery/onion order).
  for (int call = 1; call <= 3; ++call) {
    g_onion_mock.Reset();
    g_onion_mock.fail_set_eoa_on_call = call;
    EXPECT_EQ(open_onion_with_fapl("mock_eoa_fail",
                                   H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                   mock_fapl.get()),
              nullptr);
  }
  for (int call = 1; call <= 3; ++call) {
    g_onion_mock.Reset();
    g_onion_mock.fail_write_on_call = call;
    EXPECT_EQ(open_onion_with_fapl("mock_wr_fail",
                                   H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                   mock_fapl.get()),
              nullptr);
  }
  for (int close_call = 1; close_call <= 3; ++close_call) {
    g_onion_mock.Reset();
    g_onion_mock.fail_write_on_call = 3;
    g_onion_mock.fail_close_on_call = close_call;
    EXPECT_EQ(open_onion_with_fapl("mock_open_cleanup_close_fail",
                                   H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                   mock_fapl.get()),
              nullptr);
  }

  // 3. Attach-to-canonical mock failures (open #1, write #3/#4, close recovery
  // cleanup inside H5FD__onion_open_rw). Failures between head_buf/hist_buf
  // allocation and free (set_eoa #1, write #1/#2 in H5FDonion.c:1062-1101)
  // leak head_buf/hist_buf upstream and are omitted for leak-free runs.
  g_onion_mock.Reset();
  g_onion_mock.fail_open_on_call = 1;
  EXPECT_EQ(
      open_onion_with_fapl(canon_path.c_str(), H5F_ACC_RDWR, mock_fapl.get()),
      nullptr);

  g_onion_mock.Reset();
  g_onion_mock.fail_write_on_call = 4;
  EXPECT_EQ(
      open_onion_with_fapl(canon_path.c_str(), H5F_ACC_RDWR, mock_fapl.get()),
      nullptr);

  g_onion_mock.Reset();
  g_onion_mock.fail_write_on_call = 3;
  g_onion_mock.fail_close_on_call = 1;
  EXPECT_EQ(
      open_onion_with_fapl(canon_path.c_str(), H5F_ACC_RDWR, mock_fapl.get()),
      nullptr);

  // 4. Superblock encode/decode failures and live read/write errors on an open
  // mock-backed onion handle.
  {
    g_onion_mock.Reset();
    g_onion_mock.enable_sb = true;
    ScopedVfd fd = open_onion_with_fapl(
        "mock_live", H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
        mock_fapl.get());
    ASSERT_NE(fd, nullptr);
    ASSERT_EQ(H5FD_sb_size(fd.get()), 8u);

    char sb_name[16]{};
    std::array<uint8_t, 8> sb_buf{};
    g_onion_mock.fail_sb_encode = true;
    EXPECT_LT(H5FD_sb_encode(fd.get(), sb_name, sb_buf.data()), 0);
    g_onion_mock.fail_sb_encode = false;
    ASSERT_GE(H5FD_sb_encode(fd.get(), sb_name, sb_buf.data()), 0);

    g_onion_mock.fail_sb_decode = true;
    EXPECT_LT(H5FD_sb_load(fd.get(), sb_name, sb_buf.data()), 0);
    g_onion_mock.enable_sb = false;
    g_onion_mock.fail_sb_decode = false;

    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 64), 0);
    std::array<uint8_t, 16> page0{};
    page0.fill(0x55);
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, page0.size(),
                        page0.data()),
              0);

    std::array<uint8_t, 4> rd_buf{};
    g_onion_mock.fail_read_on_call = g_onion_mock.read_calls + 1;
    EXPECT_LT(H5FDread(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, rd_buf.size(),
                       rd_buf.data()),
              0);

    g_onion_mock.fail_set_eoa_on_call = g_onion_mock.set_eoa_calls + 1;
    EXPECT_LT(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, page0.size(),
                        page0.data()),
              0);

    g_onion_mock.fail_write_on_call = g_onion_mock.write_calls + 1;
    EXPECT_LT(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, page0.size(),
                        page0.data()),
              0);

    const std::array<uint8_t, 4> patch = {1, 2, 3, 4};
    g_onion_mock.fail_read_on_call = g_onion_mock.read_calls + 1;
    EXPECT_LT(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, patch.size(),
                        patch.data()),
              0);

    g_onion_mock.fail_write_on_call = g_onion_mock.write_calls + 1;
    EXPECT_LT(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 2, patch.size(),
                        patch.data()),
              0);
  }

  // 5. Commit/history/header/backing-close failures inside H5FD__onion_close().
  const std::array<uint8_t, 16> kOnePage{};
  {
    g_onion_mock.Reset();
    ScopedVfd fd = open_onion_with_fapl(
        "close_commit_eoa", H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
        mock_fapl.get());
    ASSERT_NE(fd, nullptr);
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 16), 0);
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                        kOnePage.size(), kOnePage.data()),
              0);
    g_onion_mock.fail_set_eoa_on_call = g_onion_mock.set_eoa_calls + 1;
    EXPECT_LT(H5FDclose(fd.release()), 0);
  }

  for (int close_wr_offset = 1; close_wr_offset <= 3; ++close_wr_offset) {
    g_onion_mock.Reset();
    ScopedVfd fd = open_onion_with_fapl(
        "close_wr_fail", H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
        mock_fapl.get());
    ASSERT_NE(fd, nullptr);
    ASSERT_GE(H5FDset_eoa(fd.get(), H5FD_MEM_DRAW, 16), 0);
    ASSERT_GE(H5FDwrite(fd.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0,
                        kOnePage.size(), kOnePage.data()),
              0);
    g_onion_mock.fail_write_on_call =
        g_onion_mock.write_calls + close_wr_offset;
    EXPECT_LT(H5FDclose(fd.release()), 0);
  }

  for (int close_idx = 1; close_idx <= 3; ++close_idx) {
    g_onion_mock.Reset();
    ScopedVfd fd = open_onion_with_fapl(
        "close_handle_fail", H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
        mock_fapl.get());
    ASSERT_NE(fd, nullptr);
    g_onion_mock.fail_close_on_call = close_idx;
    EXPECT_LT(H5FDclose(fd.release()), 0);
  }
}

}  // namespace
}  // namespace hdf5
