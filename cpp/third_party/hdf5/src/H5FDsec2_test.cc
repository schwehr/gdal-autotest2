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
// Tests the POSIX unbuffered Virtual File Driver (H5FD_SEC2): registration,
// FAPL setup, HDF5 file round-trips, raw VFD open/close/cmp/query/EOA/EOF/
// handle/read/write/truncate/lock/unlock/delete/ctl callbacks, file-lock env
// overrides, and error paths (third_party/hdf5/src/H5FDsec2.c).
//
// Google3 Linux builds define H5_HAVE_PREADWRITE=1 and do not define
// H5_HAVE_WIN32_API, so non-preadwrite seek tracking and Win32 handle branches
// are compiled out.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDsec2.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDSEC2_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
}  // extern "C"

#ifdef H5FDSEC2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDSEC2_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

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

ScopedPlist MakeSec2Fapl() {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid() && H5Pset_fapl_sec2(fapl.get()) < 0) {
    fapl.reset();
  }
  return fapl;
}

TEST(H5FDsec2Test, RegistrationConstantsAndFeatureFlags) {
  static_assert(H5FD_SEC2_VALUE == H5_VFD_SEC2);

  EXPECT_GT(H5FDis_driver_registered_by_name("sec2"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5FD_SEC2_VALUE), 0);
  EXPECT_NE(H5FD_SEC2, H5I_INVALID_HID);

  // Calling H5FD__sec2_register when already registered is a safe no-op.
  EXPECT_GE(H5FD__sec2_register(), 0);

  unsigned long flags = 0;  // NOLINT(runtime/int)
  ASSERT_GE(H5FDdriver_query(H5FD_SEC2, &flags), 0);
  EXPECT_NE(flags & H5FD_FEAT_AGGREGATE_METADATA, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_ACCUMULATE_METADATA, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_DATA_SIEVE, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_AGGREGATE_SMALLDATA, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_POSIX_COMPAT_HANDLE, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_SUPPORTS_SWMR_IO, 0UL);
  EXPECT_NE(flags & H5FD_FEAT_DEFAULT_VFD_COMPATIBLE, 0UL);
  EXPECT_EQ(flags & H5FD_FEAT_IGNORE_DRVRINFO, 0UL);

  // Query with NULL flags output pointer succeeds cleanly.
  const H5FD_class_t* cls = H5FD_get_class(H5FD_SEC2);
  ASSERT_NE(cls, nullptr);
  EXPECT_GE(cls->query(nullptr, nullptr), 0);
}

TEST(H5FDsec2Test, SetFaplSec2ValidatesArgs) {
  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_SEC2);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_fapl_sec2(H5P_DEFAULT), 0);
  EXPECT_LT(H5Pset_fapl_sec2(H5P_DATASET_XFER_DEFAULT), 0);
}

TEST(H5FDsec2Test, HighLevelRoundTripAndMissingAndCorruptOpenFailures) {
  TempDir dir("h5fd_sec2_roundtrip");
  const std::string h5_path = dir.child("sample.h5").string();

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  // 1. Create an HDF5 file using sec2, write an int32 dataset, and close.
  {
    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    void* handle_ptr = nullptr;
    ASSERT_GE(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &handle_ptr), 0);
    ASSERT_NE(handle_ptr, nullptr);
    EXPECT_GE(*static_cast<const int*>(handle_ptr), 0);

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset dset(H5Dcreate2(file.get(), "/data", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    constexpr std::array<int32_t, 4> kValues = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kValues.data()),
              0);
  }

  // 2. Reopen read-only and verify dataset contents.
  {
    ScopedFile file(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedDataset dset(H5Dopen2(file.get(), "/data", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 4> read_back = {-1, -1, -1, -1};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_back.data()),
              0);
    EXPECT_THAT(read_back, ElementsAre(10, 20, 30, 40));
  }

  // 3. Delete the file via H5Fdelete and verify missing/corrupt open checks.
  ASSERT_GE(H5Fdelete(h5_path.c_str(), fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(h5_path));

  ScopedErrorSilence silence;
  ScopedFile missing(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  EXPECT_EQ(missing.get(), H5I_INVALID_HID);

  EXPECT_LT(H5FDdelete(h5_path.c_str(), fapl.get()), 0);

  const std::string corrupt_path = dir.child("corrupt.h5").string();
  {
    std::ofstream out(corrupt_path, std::ios::binary);
    out << "not-an-hdf5-file-header";
  }
  ScopedFile corrupt(H5Fopen(corrupt_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  EXPECT_EQ(corrupt.get(), H5I_INVALID_HID);
}

TEST(H5FDsec2Test, RawVfdWriteReadEoaEofTruncateAndCtlBehavior) {
  TempDir dir("h5fd_sec2_io");
  const std::string path = dir.child("raw.dat").string();

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFdFile file(H5FDopen(path.c_str(),
                             H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                             fapl.get(), H5FD_MAXADDR));
  ASSERT_TRUE(file.valid());

  // 1. Initial EOA and EOF are both 0; advance EOA to cover writes.
  EXPECT_EQ(H5FDget_eoa(file.get(), H5FD_MEM_DEFAULT), haddr_t{0});
  EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{0});
  ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DEFAULT, 64), 0);
  EXPECT_EQ(H5FDget_eoa(file.get(), H5FD_MEM_DEFAULT), haddr_t{64});

  // 2. Write at [0..16) extending EOF to 16, then overwrite [4..8) without
  //    increasing EOF.
  constexpr std::array<uint8_t, 16> kPayload = {
      0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
      0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
  ASSERT_GE(
      H5FDwrite(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16, kPayload.data()),
      0);
  EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{16});

  constexpr std::array<uint8_t, 4> kPatch = {0xaa, 0xbb, 0xcc, 0xdd};
  ASSERT_GE(
      H5FDwrite(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 4, 4, kPatch.data()),
      0);
  EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{16});

  // 3. Read back [0..16), then read [16..32) past physical EOF (zero-filled).
  std::array<uint8_t, 16> rbuf = {};
  rbuf.fill(0xff);
  ASSERT_GE(
      H5FDread(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16, rbuf.data()), 0);
  EXPECT_THAT(rbuf,
              ElementsAre(0x10, 0x11, 0x12, 0x13, 0xaa, 0xbb, 0xcc, 0xdd, 0x18,
                          0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f));

  rbuf.fill(0x55);
  ASSERT_GE(
      H5FDread(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 16, 16, rbuf.data()), 0);
  for (uint8_t byte : rbuf) {
    EXPECT_EQ(byte, 0);
  }

  // 4. Truncate extends physical EOF to EOA (64); second truncate is a no-op;
  //    shrinking EOA to 32 and truncating shrinks the file to 32 bytes.
  ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);
  EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{64});
  ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);

  ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DEFAULT, 32), 0);
  ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, true), 0);
  EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{32});

  // 5. ctl succeeds silently when FAIL_IF_UNKNOWN_FLAG is clear and fails
  //    when FAIL_IF_UNKNOWN_FLAG is set.
  EXPECT_GE(H5FDctl(file.get(), H5FD_CTL_TEST_OPCODE, 0, nullptr, nullptr), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDctl(file.get(), H5FD_CTL_TEST_OPCODE,
                      H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr, nullptr),
              0);
  }
}

TEST(H5FDsec2Test, CmpExclusiveOpenAndLockUnlockBehavior) {
  TempDir dir("h5fd_sec2_cmp_lock");
  const std::string path_a = dir.child("a.dat").string();
  const std::string path_b = dir.child("b.dat").string();

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFdFile fa(H5FDopen(path_a.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), H5FD_MAXADDR));
  ASSERT_TRUE(fa.valid());
  ScopedFdFile fb(H5FDopen(path_b.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_EXCL,
                           fapl.get(), H5FD_MAXADDR));
  ASSERT_TRUE(fb.valid());

  // Re-opening existing file with H5F_ACC_EXCL fails.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen(path_b.c_str(), H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_EXCL,
                 fapl.get(), H5FD_MAXADDR),
        nullptr);
  }

  ScopedFdFile fa_dup(
      H5FDopen(path_a.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
  ASSERT_TRUE(fa_dup.valid());
  EXPECT_EQ(H5FDcmp(fa.get(), fa_dup.get()), 0);

  const int cmp_ab = H5FDcmp(fa.get(), fb.get());
  const int cmp_ba = H5FDcmp(fb.get(), fa.get());
  EXPECT_NE(cmp_ab, 0);
  EXPECT_EQ(cmp_ab, -cmp_ba);

  // Exercise st_dev comparison branches when /dev/null is on a distinct device.
  struct stat st_a{};
  struct stat st_null{};
  if (stat(path_a.c_str(), &st_a) == 0 && stat("/dev/null", &st_null) == 0 &&
      st_a.st_dev != st_null.st_dev) {
    ScopedFdFile fnull(
        H5FDopen("/dev/null", H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(fnull.valid());
    const int cmp_an = H5FDcmp(fa.get(), fnull.get());
    const int cmp_na = H5FDcmp(fnull.get(), fa.get());
    EXPECT_EQ(cmp_an, st_a.st_dev < st_null.st_dev ? -1 : 1);
    EXPECT_EQ(cmp_na, -cmp_an);
  }

  // Shared lock, unlock, exclusive lock, lock contention failure, unlock.
  ASSERT_GE(H5FDlock(fa.get(), /*rw=*/false), 0);
  ASSERT_GE(H5FDunlock(fa.get()), 0);
  ASSERT_GE(H5FDlock(fa.get(), /*rw=*/true), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDlock(fa_dup.get(), /*rw=*/true), 0);
  }
  ASSERT_GE(H5FDunlock(fa.get()), 0);
}

TEST(H5FDsec2Test, FamilyToSinglePropertyAndEnvLockingAndUnregister) {
  TempDir dir("h5fd_sec2_props");
  const std::string path = dir.child("props.dat").string();

  // 1. Non-default FAPL with H5F_ACS_FAMILY_TO_SINGLE_NAME sets
  //    H5FD_FEAT_IGNORE_DRVRINFO on H5FDquery.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    const bool fam_to_single = true;
    ASSERT_GE(H5Pset(fapl.get(), H5F_ACS_FAMILY_TO_SINGLE_NAME, &fam_to_single),
              0);

    ScopedFdFile file(H5FDopen(path.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(file.valid());
    unsigned long flags = 0;  // NOLINT(runtime/int)
    ASSERT_GE(H5FDquery(file.get(), &flags), 0);
    EXPECT_NE(flags & H5FD_FEAT_IGNORE_DRVRINFO, 0UL);
  }

  // 2. HDF5_USE_FILE_LOCKING env override + H5close/H5open (also exercises
  //    H5FD__sec2_unregister and H5FD__sec2_register).
  const char* prev_env = std::getenv("HDF5_USE_FILE_LOCKING");
  const bool had_env = (prev_env != nullptr);
  const std::string saved_env = prev_env ? prev_env : "";
  absl::Cleanup restore_env = [&] {
    if (had_env) {
      setenv("HDF5_USE_FILE_LOCKING", saved_env.c_str(), 1);
    } else {
      unsetenv("HDF5_USE_FILE_LOCKING");
    }
    H5close();
    H5open();
  };

  ASSERT_GE(H5close(), 0);
  ASSERT_EQ(setenv("HDF5_USE_FILE_LOCKING", "BEST_EFFORT", 1), 0);
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_ignore_disabled_file_locks_p, FAIL);

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFdFile file(
      H5FDopen(path.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
  ASSERT_TRUE(file.valid());
}

TEST(H5FDsec2Test, OpenReadWriteTruncateAndHandleErrorPaths) {
  TempDir dir("h5fd_sec2_errors");
  const std::string path = dir.child("err.dat").string();

  const H5FD_class_t* sec2_cls = H5FD_get_class(H5FD_SEC2);
  ASSERT_NE(sec2_cls, nullptr);

  // Seed file using H5P_FILE_ACCESS_DEFAULT directly to cover the
  // H5P_FILE_ACCESS_DEFAULT == fapl_id false branch in H5FD__sec2_open.
  {
    H5FD_t* seed = sec2_cls->open(path.c_str(),
                                  H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                  H5P_FILE_ACCESS_DEFAULT, H5FD_MAXADDR);
    ASSERT_NE(seed, nullptr);
    EXPECT_GE(sec2_cls->close(seed), 0);
  }

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());

  ScopedErrorSilence silence;

  // 1. Direct cls->open argument validation and FAPL property lookup errors.
  EXPECT_EQ(sec2_cls->open(nullptr, H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(sec2_cls->open("", H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(sec2_cls->open(path.c_str(), H5F_ACC_RDONLY, fapl.get(), 0),
            nullptr);
  EXPECT_EQ(
      sec2_cls->open(path.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
      nullptr);
  EXPECT_EQ(sec2_cls->open(path.c_str(), H5F_ACC_RDONLY, fapl.get(),
                           H5FD_MAXADDR + 1),
            nullptr);
  // H5P_DEFAULT makes H5I_object(fapl_id) return NULL after HDopen().
  EXPECT_EQ(
      sec2_cls->open(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT, H5FD_MAXADDR),
      nullptr);
  // H5P_DATASET_XFER_DEFAULT has no ignore-disabled-file-locks property.
  EXPECT_EQ(sec2_cls->open(path.c_str(), H5F_ACC_RDONLY,
                           H5P_DATASET_XFER_DEFAULT, H5FD_MAXADDR),
            nullptr);
  // Non-existent file open fails HDopen.
  EXPECT_EQ(H5FDopen(dir.child("no_such_file.dat").c_str(), H5F_ACC_RDONLY,
                     fapl.get(), H5FD_MAXADDR),
            nullptr);

  // 2. Read-only handle triggers pwrite/ftruncate errors, NULL file_handle
  //    error, and read/write address/overflow checks.
  {
    ScopedFdFile ro(
        H5FDopen(path.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(ro.valid());
    ASSERT_GE(H5FDset_eoa(ro.get(), H5FD_MEM_DEFAULT, 64), 0);

    std::array<uint8_t, 16> buf = {};
    EXPECT_LT(
        H5FDwrite(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, 16, buf.data()),
        0);
    EXPECT_LT(H5FDtruncate(ro.get(), H5P_DEFAULT, false), 0);
    EXPECT_LT(ro.get()->cls->get_handle(ro.get(), H5P_DEFAULT, nullptr), 0);

    EXPECT_LT(ro.get()->cls->read(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                                  HADDR_UNDEF, 4, buf.data()),
              0);
    EXPECT_LT(ro.get()->cls->read(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                                  H5FD_MAXADDR, 16, buf.data()),
              0);
    EXPECT_LT(ro.get()->cls->write(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                                   HADDR_UNDEF, 4, buf.data()),
              0);
    EXPECT_LT(ro.get()->cls->write(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                                   H5FD_MAXADDR, 16, buf.data()),
              0);
  }

  // 3. Replace open fd with O_WRONLY so pread() returns EBADF while close()
  //    still closes the descriptor cleanly.
  {
    ScopedFdFile rw(
        H5FDopen(path.c_str(), H5F_ACC_RDWR, fapl.get(), H5FD_MAXADDR));
    ASSERT_TRUE(rw.valid());
    ASSERT_GE(H5FDset_eoa(rw.get(), H5FD_MEM_DEFAULT, 64), 0);

    void* handle_ptr = nullptr;
    ASSERT_GE(H5FDget_vfd_handle(rw.get(), H5P_DEFAULT, &handle_ptr), 0);
    int* fd_ptr = static_cast<int*>(handle_ptr);
    ASSERT_NE(fd_ptr, nullptr);

    const int wronly_fd = open(path.c_str(), O_WRONLY);
    ASSERT_GE(wronly_fd, 0);
    const int dup_rc = dup2(wronly_fd, *fd_ptr);
    close(wronly_fd);
    ASSERT_EQ(dup_rc, *fd_ptr);

    std::array<uint8_t, 16> rbuf = {};
    rbuf.fill(0x77);
    EXPECT_LT(
        H5FDread(rw.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, 16, rbuf.data()),
        0);
  }
}

}  // namespace
}  // namespace hdf5
