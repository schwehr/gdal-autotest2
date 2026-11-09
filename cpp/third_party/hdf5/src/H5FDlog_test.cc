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
// Tests the POSIX logging virtual file driver (H5FD_LOG), FAPL configuration,
// VFD callbacks, I/O counters/flavors/timers, file locking, deletion, and
// error paths (third_party/hdf5/src/H5FDlog.c).
//
// Google3 Linux builds define H5_HAVE_PREADWRITE=1 and do not define
// H5_HAVE_WIN32_API, so non-preadwrite seek tracking and Win32 handle branches
// are compiled out.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDlog.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDLOG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
}  // extern "C"

#ifdef H5FDLOG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDLOG_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct H5FdDeleter {
  void operator()(H5FD_t* file) const {
    if (file != nullptr) {
      H5FDclose(file);
    }
  }
};

using ScopedFdFile = std::unique_ptr<H5FD_t, H5FdDeleter>;

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST(H5FDlogTest, ConstantsAndRegistrationMatchPublicDefinitions) {
  static_assert(H5FD_LOG_VALUE == H5_VFD_LOG);
  static_assert(H5FD_LOG_META_IO == H5FD_LOG_TRUNCATE);
  static_assert(H5FD_LOG_LOC_IO ==
                (H5FD_LOG_LOC_READ | H5FD_LOG_LOC_WRITE | H5FD_LOG_LOC_SEEK));
  static_assert(H5FD_LOG_FILE_IO == (H5FD_LOG_FILE_READ | H5FD_LOG_FILE_WRITE));
  static_assert(H5FD_LOG_NUM_IO == (H5FD_LOG_NUM_READ | H5FD_LOG_NUM_WRITE |
                                    H5FD_LOG_NUM_SEEK | H5FD_LOG_NUM_TRUNCATE));

  EXPECT_GT(H5FDis_driver_registered_by_name("log"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5FD_LOG_VALUE), 0);
  EXPECT_NE(H5FD_LOG, H5I_INVALID_HID);

  // Calling H5FD__log_register when already registered is a safe no-op.
  EXPECT_GE(H5FD__log_register(), 0);

  unsigned long driver_flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_LOG, &driver_flags), 0);
  EXPECT_NE(driver_flags & H5FD_FEAT_AGGREGATE_METADATA, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_ACCUMULATE_METADATA, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_DATA_SIEVE, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_AGGREGATE_SMALLDATA, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_POSIX_COMPAT_HANDLE, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_SUPPORTS_SWMR_IO, 0UL);
  EXPECT_NE(driver_flags & H5FD_FEAT_DEFAULT_VFD_COMPATIBLE, 0UL);
  EXPECT_EQ(driver_flags & H5FD_FEAT_IGNORE_DRVRINFO, 0UL);
}

TEST(H5FDlogTest, SetFaplLogValidatesArgsAndSupportsCopyAndNullLogfile) {
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_fapl_log(H5P_DEFAULT, nullptr, 0, 0), 0);
    EXPECT_LT(H5Pset_fapl_log(H5P_DATASET_XFER_DEFAULT, nullptr, 0, 0), 0);
  }

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  // Set without logfile, then copy and overwrite with logfile to cover both
  // NULL and non-NULL branches in fapl_copy and fapl_free.
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), nullptr, H5FD_LOG_LOC_IO, 0), 0);
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_LOG);

  ScopedPlist copied_null(H5Pcopy(fapl.get()));
  ASSERT_TRUE(copied_null.valid());
  EXPECT_EQ(H5Pget_driver(copied_null.get()), H5FD_LOG);

  TempDir dir("h5fd_log_fapl_copy");
  const std::string log_path = dir.child("dummy.log").string();
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), log_path.c_str(), H5FD_LOG_ALL, 4096),
            0);

  ScopedPlist copied_named(H5Pcopy(fapl.get()));
  ASSERT_TRUE(copied_named.valid());
  EXPECT_EQ(H5Pget_driver(copied_named.get()), H5FD_LOG);
}

TEST(H5FDlogTest, HighLevelRoundTripAndFaplGetAndDeleteSucceed) {
  TempDir dir("h5fd_log_round_trip");
  const std::string h5_path = dir.child("sample.h5").string();
  const std::string write_log = dir.child("write.log").string();
  const std::string read_log = dir.child("read.log").string();

  // 1. Create an HDF5 file with H5FD_LOG_ALL and a 1 MiB tracking buffer.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_log(fapl.get(), write_log.c_str(), H5FD_LOG_ALL, 1 << 20),
        0);

    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    // Retrieve FAPL via H5Fget_access_plist (exercises H5FD__log_fapl_get).
    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(got_fapl.valid());
    EXPECT_EQ(H5Pget_driver(got_fapl.get()), H5FD_LOG);

    // Retrieve POSIX fd handle via H5Fget_vfd_handle.
    void* raw_handle = nullptr;
    ASSERT_GE(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &raw_handle), 0);
    ASSERT_NE(raw_handle, nullptr);
    EXPECT_GE(*static_cast<const int*>(raw_handle), 0);

    constexpr std::array<hsize_t, 1> kDims = {4};
    ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset dset(H5Dcreate2(file.get(), "/vals", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    constexpr std::array<int32_t, 4> kInput = {11, 22, 33, 44};
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kInput.data()),
              0);
  }

  // 2. Re-open the file read-only and verify dataset contents.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_log(fapl.get(), read_log.c_str(), H5FD_LOG_ALL, 1 << 20),
        0);

    ScopedFile file(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedDataset dset(H5Dopen2(file.get(), "/vals", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::array<int32_t, 4> output = {};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, output.data()),
              0);
    EXPECT_THAT(output, ElementsAre(11, 22, 33, 44));
  }

  const std::string write_log_text = ReadTextFile(write_log);
  EXPECT_THAT(write_log_text, HasSubstr("Open took:"));
  EXPECT_THAT(write_log_text, HasSubstr("Stat took:"));
  EXPECT_THAT(write_log_text, HasSubstr("Close took:"));
  EXPECT_THAT(write_log_text, HasSubstr("Allocated"));
  EXPECT_THAT(write_log_text, HasSubstr("Written"));
  EXPECT_THAT(write_log_text, HasSubstr("Dumping write I/O information:"));
  EXPECT_THAT(write_log_text, HasSubstr("Dumping read I/O information:"));
  EXPECT_THAT(write_log_text, HasSubstr("Dumping I/O flavor information:"));

  // 3. Delete via H5Fdelete and verify non-existent/corrupt opens fail.
  ScopedPlist del_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(del_fapl.valid());
  ASSERT_GE(H5Pset_fapl_log(del_fapl.get(), nullptr, 0, 0), 0);
  ASSERT_GE(H5Fdelete(h5_path.c_str(), del_fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(h5_path));

  {
    ScopedErrorSilence silence;
    ScopedFile missing(
        H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, del_fapl.get()));
    EXPECT_EQ(missing.get(), H5I_INVALID_HID);

    // Deleting a non-existent file fails cleanly.
    EXPECT_LT(H5FDdelete(h5_path.c_str(), del_fapl.get()), 0);

    // Corrupt non-HDF5 file fails H5Fopen.
    const std::string corrupt_path = dir.child("corrupt.h5").string();
    {
      std::ofstream out(corrupt_path, std::ios::binary);
      out << "corrupt-not-hdf5-header";
    }
    ScopedFile corrupt(
        H5Fopen(corrupt_path.c_str(), H5F_ACC_RDONLY, del_fapl.get()));
    EXPECT_EQ(corrupt.get(), H5I_INVALID_HID);
  }
}

TEST(H5FDlogTest, VfdAllFlagsLoggingTracksAllocFreeEoaIoTruncateAndClose) {
  TempDir dir("h5fd_log_vfd_all");
  const std::string data_path = dir.child("vfd_all.dat").string();
  const std::string log_path = dir.child("vfd_all.log").string();

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), log_path.c_str(), H5FD_LOG_ALL, 4096),
            0);

  // 1. Open via H5FDopen with CREAT|TRUNC|RDWR, allocate blocks with distinct
  //    flavors, write varying counts, read past EOF, shrink EOA, and truncate.
  {
    ScopedFdFile file(H5FDopen(data_path.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(file, nullptr);

    unsigned long feat_flags = 0;
    ASSERT_GE(H5FDquery(file.get(), &feat_flags), 0);
    EXPECT_EQ(feat_flags & H5FD_FEAT_IGNORE_DRVRINFO, 0UL);

    // Allocate 32 bytes as SUPER, 32 as BTREE, 32 as DRAW, 32 as OHDR.
    const haddr_t a_super =
        H5FDalloc(file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 32);
    const haddr_t a_btree =
        H5FDalloc(file.get(), H5FD_MEM_BTREE, H5P_DEFAULT, 32);
    const haddr_t a_draw =
        H5FDalloc(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 32);
    const haddr_t a_ohdr =
        H5FDalloc(file.get(), H5FD_MEM_OHDR, H5P_DEFAULT, 32);
    EXPECT_EQ(a_super, haddr_t{0});
    EXPECT_EQ(a_btree, haddr_t{32});
    EXPECT_EQ(a_draw, haddr_t{64});
    EXPECT_EQ(a_ohdr, haddr_t{96});
    EXPECT_EQ(H5FDget_eoa(file.get(), H5FD_MEM_DEFAULT), haddr_t{128});

    // Free OHDR block (logs Freed and resets flavor to H5FD_MEM_DEFAULT),
    // then extend EOA via H5FDset_eoa (marks [128..160) as LHEAP) and
    // shrink back to 128 via H5FDset_eoa (logs Freed and resets flavor).
    ASSERT_GE(H5FDfree(file.get(), H5FD_MEM_OHDR, H5P_DEFAULT, a_ohdr, 32), 0);
    ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_LHEAP, 160), 0);
    ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_LHEAP, 128), 0);

    // Write [0..16) twice, [16..48) once, and [96..112) as DEFAULT ("(fresh)")
    // so nwrite and flavor run-length loops emit multiple transitions.
    std::array<uint8_t, 32> payload = {};
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>(0x30 + i);
    }
    ASSERT_GE(H5FDwrite(file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0, 16,
                        payload.data()),
              0);
    ASSERT_GE(H5FDwrite(file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0, 16,
                        payload.data()),
              0);
    ASSERT_GE(H5FDwrite(file.get(), H5FD_MEM_BTREE, H5P_DEFAULT, 32, 16,
                        payload.data()),
              0);
    // Overwrite within existing EOF with a non-DEFAULT flavor on cleared OHDR
    // region [96..112) to trigger " (fresh)" flavor update during write.
    ASSERT_GE(H5FDwrite(file.get(), H5FD_MEM_GHEAP, H5P_DEFAULT, 96, 16,
                        payload.data()),
              0);
    EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{112});

    // Read [0..16) twice, [32..48) once, and [112..128) past physical EOF
    // (returns zero-filled bytes from the pread == 0 branch).
    std::array<uint8_t, 16> read_buf = {};
    ASSERT_GE(H5FDread(file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0, 16,
                       read_buf.data()),
              0);
    ASSERT_GE(H5FDread(file.get(), H5FD_MEM_SUPER, H5P_DEFAULT, 0, 16,
                       read_buf.data()),
              0);
    EXPECT_EQ(read_buf[0], 0x30);
    ASSERT_GE(H5FDread(file.get(), H5FD_MEM_BTREE, H5P_DEFAULT, 32, 16,
                       read_buf.data()),
              0);

    read_buf.fill(0xAA);
    ASSERT_GE(H5FDread(file.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 112, 16,
                       read_buf.data()),
              0);
    for (uint8_t byte : read_buf) {
      EXPECT_EQ(byte, 0);
    }

    // Truncate extends EOF from 112 to EOA (128), then a second truncate when
    // EOA == EOF is a safe no-op.
    ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);
    EXPECT_EQ(H5FDget_eof(file.get(), H5FD_MEM_DEFAULT), haddr_t{128});
    ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);

    ASSERT_GE(H5FDclose(file.release()), 0);
  }

  const std::string log = ReadTextFile(log_path);
  EXPECT_THAT(log, HasSubstr("Open took:"));
  EXPECT_THAT(log, HasSubstr("Stat took:"));
  EXPECT_THAT(log, HasSubstr("(H5FD_MEM_SUPER) Allocated"));
  EXPECT_THAT(log, HasSubstr("(H5FD_MEM_OHDR) Freed"));
  EXPECT_THAT(log, HasSubstr("(H5FD_MEM_GHEAP) Written (fresh)"));
  EXPECT_THAT(log, HasSubstr("(H5FD_MEM_SUPER) Read ("));
  EXPECT_THAT(log, HasSubstr("Truncate: To        128 ("));
  EXPECT_THAT(log, HasSubstr("Close took:"));
  EXPECT_THAT(log, HasSubstr("Total number of read operations: 4"));
  EXPECT_THAT(log, HasSubstr("Total number of write operations: 4"));
  EXPECT_THAT(log, HasSubstr("Total number of seek operations: 0"));
  EXPECT_THAT(log, HasSubstr("Total number of truncate operations: 1"));
  EXPECT_THAT(log, HasSubstr("Total time in read operations:"));
  EXPECT_THAT(log, HasSubstr("Total time in write operations:"));
  EXPECT_THAT(log, HasSubstr("Total time in seek operations:"));
  EXPECT_THAT(log, HasSubstr("Total time in truncate operations:"));
  EXPECT_THAT(log, HasSubstr("written to   2 times"));
  EXPECT_THAT(log, HasSubstr("read from   2 times"));
  EXPECT_THAT(log, HasSubstr("flavor is H5FD_MEM_SUPER"));
  EXPECT_THAT(log, HasSubstr("flavor is H5FD_MEM_GHEAP"));
}

TEST(H5FDlogTest, VfdNonTimedAndZeroFlagModesAndDefaultDriverConfig) {
  TempDir dir("h5fd_log_modes");

  // 1. Flags without TIME_* exercise non-timed branches of LOC_READ, LOC_WRITE,
  //    TRUNCATE, and skip FREE/ALLOC logging while keeping flags != 0.
  const std::string data_notime = dir.child("notime.dat").string();
  const std::string log_notime = dir.child("notime.log").string();
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_fapl_log(
            fapl.get(), log_notime.c_str(),
            H5FD_LOG_LOC_READ | H5FD_LOG_LOC_WRITE | H5FD_LOG_TRUNCATE, 0),
        0);

    ScopedFdFile file(H5FDopen(data_notime.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(file, nullptr);

    ASSERT_GE(H5FDalloc(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 64), 0);
    ASSERT_GE(H5FDfree(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 64), 0);
    ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 96), 0);
    ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 64), 0);

    std::array<uint8_t, 16> data = {1, 2, 3, 4};
    ASSERT_GE(
        H5FDwrite(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16, data.data()),
        0);
    // Write again at address 0 so addr (16) <= file->eof (16) (false branch).
    ASSERT_GE(
        H5FDwrite(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, data.data()),
        0);
    std::array<uint8_t, 16> out = {};
    ASSERT_GE(
        H5FDread(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 16, out.data()), 0);
    EXPECT_EQ(out[0], 1);
    ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDclose(file.release()), 0);
  }
  const std::string notime_text = ReadTextFile(log_notime);
  EXPECT_THAT(notime_text, HasSubstr("(H5FD_MEM_DRAW) Written\n"));
  EXPECT_THAT(notime_text, HasSubstr("(H5FD_MEM_DRAW) Read\n"));
  EXPECT_THAT(notime_text, HasSubstr("Truncate: To         64\n"));

  // 2. flags == 0 skips all logging and buffer allocation.
  const std::string data_zero = dir.child("zero.dat").string();
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_log(fapl.get(), nullptr, 0, 0), 0);

    ScopedFdFile file(H5FDopen(data_zero.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(file, nullptr);
    EXPECT_NE(H5FDalloc(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 32),
              HADDR_UNDEF);
    ASSERT_GE(H5FDfree(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 32), 0);
    ASSERT_GE(H5FDset_eoa(file.get(), H5FD_MEM_DRAW, 64), 0);
    const std::array<uint8_t, 8> wbuf = {9, 8, 7, 6, 5, 4, 3, 2};
    ASSERT_GE(
        H5FDwrite(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, wbuf.data()),
        0);
    std::array<uint8_t, 8> rbuf = {};
    ASSERT_GE(
        H5FDread(file.get(), H5FD_MEM_DRAW, H5P_DEFAULT, 0, 8, rbuf.data()), 0);
    EXPECT_THAT(rbuf, ElementsAre(9, 8, 7, 6, 5, 4, 3, 2));
    ASSERT_GE(H5FDtruncate(file.get(), H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDclose(file.release()), 0);
  }

  // 3. H5Pset_driver(..., H5FD_LOG, nullptr) triggers H5FD_log_default_config_g
  //    with logfile == NULL (logfp = stderr) and default flags (no I/O logs).
  const std::string data_default = dir.child("default.dat").string();
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl.get(), H5FD_LOG, nullptr), 0);

    ScopedFdFile file(H5FDopen(data_default.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(file, nullptr);
    ASSERT_GE(H5FDclose(file.release()), 0);
  }
}

TEST(H5FDlogTest, CmpAndExclusiveOpenAndLockUnlockBehavior) {
  TempDir dir("h5fd_log_cmp_lock");
  const std::string path_a = dir.child("a.dat").string();
  const std::string path_b = dir.child("b.dat").string();

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), nullptr, 0, 0), 0);

  ScopedFdFile fa(H5FDopen(path_a.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                           fapl.get(), H5FD_MAXADDR));
  ASSERT_NE(fa, nullptr);
  ScopedFdFile fb(H5FDopen(path_b.c_str(),
                           H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_EXCL,
                           fapl.get(), H5FD_MAXADDR));
  ASSERT_NE(fb, nullptr);

  // Opening an existing file with H5F_ACC_EXCL fails.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen(path_b.c_str(), H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_EXCL,
                 fapl.get(), H5FD_MAXADDR),
        nullptr);
  }

  // Opening the same file again compares equal (0); distinct files compare != 0
  // with antisymmetric signs based on inode ordering.
  ScopedFdFile fa_dup(
      H5FDopen(path_a.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
  ASSERT_NE(fa_dup, nullptr);
  EXPECT_EQ(H5FDcmp(fa.get(), fa_dup.get()), 0);

  const int cmp_ab = H5FDcmp(fa.get(), fb.get());
  const int cmp_ba = H5FDcmp(fb.get(), fa.get());
  EXPECT_NE(cmp_ab, 0);
  EXPECT_EQ(cmp_ab, -cmp_ba);

  // Compare across different filesystem devices when /dev/null has a distinct
  // st_dev from TempDir.
  struct stat st_a{};
  struct stat st_null{};
  if (stat(path_a.c_str(), &st_a) == 0 && stat("/dev/null", &st_null) == 0 &&
      st_a.st_dev != st_null.st_dev) {
    ScopedFdFile fnull(
        H5FDopen("/dev/null", H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(fnull, nullptr);
    const int cmp_an = H5FDcmp(fa.get(), fnull.get());
    const int cmp_na = H5FDcmp(fnull.get(), fa.get());
    EXPECT_EQ(cmp_an, st_a.st_dev < st_null.st_dev ? -1 : 1);
    EXPECT_EQ(cmp_na, -cmp_an);
  }

  // Shared lock, unlock, exclusive lock, and lock contention error.
  ASSERT_GE(H5FDlock(fa.get(), /*rw=*/false), 0);
  ASSERT_GE(H5FDunlock(fa.get()), 0);
  ASSERT_GE(H5FDlock(fa.get(), /*rw=*/true), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDlock(fa_dup.get(), /*rw=*/true), 0);
  }
  ASSERT_GE(H5FDunlock(fa.get()), 0);
}

TEST(H5FDlogTest, FamilyToSingleAndFileLockEnvOverrideOpenProperties) {
  TempDir dir("h5fd_log_props");
  const std::string data_path = dir.child("props.dat").string();

  // 1. Set H5F_ACS_FAMILY_TO_SINGLE_NAME to true on the FAPL and verify
  //    H5FDquery reports H5FD_FEAT_IGNORE_DRVRINFO.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_log(fapl.get(), nullptr, 0, 0), 0);
    const bool fam_to_single = true;
    ASSERT_GE(H5Pset(fapl.get(), H5F_ACS_FAMILY_TO_SINGLE_NAME, &fam_to_single),
              0);

    ScopedFdFile file(H5FDopen(data_path.c_str(),
                               H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                               fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(file, nullptr);
    unsigned long flags = 0;
    ASSERT_GE(H5FDquery(file.get(), &flags), 0);
    EXPECT_NE(flags & H5FD_FEAT_IGNORE_DRVRINFO, 0UL);
  }

  // 2. Exercise HDF5_USE_FILE_LOCKING env override path during
  // H5FD__init_package
  //    (also exercising H5FD__log_unregister and H5FD__log_register via
  //    H5close/H5open) and restore global HDF5 state on scope exit.
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

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), nullptr, 0, 0), 0);
  ScopedFdFile file(
      H5FDopen(data_path.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
  ASSERT_NE(file, nullptr);
}

TEST(H5FDlogTest, OpenReadWriteTruncateAndHandleErrorPaths) {
  TempDir dir("h5fd_log_errors");
  const std::string data_path = dir.child("err.dat").string();
  const std::string write_err_log = dir.child("write_err.log").string();
  const std::string read_err_log = dir.child("read_err.log").string();

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_log(fapl.get(), write_err_log.c_str(),
                            H5FD_LOG_LOC_READ | H5FD_LOG_LOC_WRITE, 0),
            0);

  // Seed a regular file first using H5P_FILE_ACCESS_DEFAULT directly (exercises
  // the H5P_FILE_ACCESS_DEFAULT skip and NULL flags in H5FD__log_query).
  {
    const H5FD_class_t* log_cls = H5FD_get_class(H5FD_LOG);
    ASSERT_NE(log_cls, nullptr);
    EXPECT_GE(log_cls->query(nullptr, nullptr), 0);

    H5FD_t* seed = log_cls->open(data_path.c_str(),
                                 H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                                 H5P_FILE_ACCESS_DEFAULT, H5FD_MAXADDR);
    ASSERT_NE(seed, nullptr);
    EXPECT_GE(log_cls->close(seed), 0);
  }

  ScopedErrorSilence silence;

  // 1. Invalid names and missing path via H5FDopen.
  EXPECT_EQ(H5FDopen(nullptr, H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR),
            nullptr);
  EXPECT_EQ(H5FDopen("", H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR), nullptr);
  EXPECT_EQ(H5FDopen(dir.child("no_such_file.dat").c_str(), H5F_ACC_RDONLY,
                     fapl.get(), H5FD_MAXADDR),
            nullptr);

  // 2. Direct driver class open checks for maxaddr == 0, HADDR_UNDEF, overflow,
  //    and non-FAPL plist.
  const H5FD_class_t* log_cls = H5FD_get_class(H5FD_LOG);
  ASSERT_NE(log_cls, nullptr);
  EXPECT_EQ(log_cls->open(data_path.c_str(), H5F_ACC_RDONLY, fapl.get(), 0),
            nullptr);
  EXPECT_EQ(
      log_cls->open(data_path.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
      nullptr);
  EXPECT_EQ(log_cls->open(data_path.c_str(), H5F_ACC_RDONLY, fapl.get(),
                          H5FD_MAXADDR + 1),
            nullptr);
  EXPECT_EQ(log_cls->open(data_path.c_str(), H5F_ACC_RDONLY,
                          H5P_DATASET_XFER_DEFAULT, H5FD_MAXADDR),
            nullptr);

  // 3. Open read-only and exercise write error, truncate error, NULL
  //    file_handle error, and address/overflow checks on read & write.
  {
    ScopedFdFile ro(
        H5FDopen(data_path.c_str(), H5F_ACC_RDONLY, fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(ro, nullptr);
    ASSERT_GE(H5FDset_eoa(ro.get(), H5FD_MEM_DEFAULT, 64), 0);

    std::array<uint8_t, 16> buf = {};
    EXPECT_LT(
        H5FDwrite(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, 16, buf.data()),
        0);
    EXPECT_LT(H5FDtruncate(ro.get(), H5P_DEFAULT, false), 0);
    EXPECT_LT(ro->cls->get_handle(ro.get(), H5P_DEFAULT, nullptr), 0);

    EXPECT_LT(ro->cls->read(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                            HADDR_UNDEF, 4, buf.data()),
              0);
    EXPECT_LT(ro->cls->read(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                            H5FD_MAXADDR, 16, buf.data()),
              0);
    EXPECT_LT(ro->cls->write(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                             HADDR_UNDEF, 4, buf.data()),
              0);
    EXPECT_LT(ro->cls->write(ro.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT,
                             H5FD_MAXADDR, 16, buf.data()),
              0);
  }

  // 4. Replace the underlying fd of an open H5FD_LOG file with an O_WRONLY fd
  //    so pread() fails with EBADF while H5FDclose still closes cleanly. Use a
  //    separate log path so step 3's write error log is preserved.
  {
    ScopedPlist rw_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(rw_fapl.valid());
    ASSERT_GE(H5Pset_fapl_log(rw_fapl.get(), read_err_log.c_str(),
                              H5FD_LOG_LOC_READ, 0),
              0);
    ScopedFdFile rw(
        H5FDopen(data_path.c_str(), H5F_ACC_RDWR, rw_fapl.get(), H5FD_MAXADDR));
    ASSERT_NE(rw, nullptr);
    ASSERT_GE(H5FDset_eoa(rw.get(), H5FD_MEM_DEFAULT, 64), 0);

    void* handle_ptr = nullptr;
    ASSERT_GE(H5FDget_vfd_handle(rw.get(), H5P_DEFAULT, &handle_ptr), 0);
    int* fd_ptr = static_cast<int*>(handle_ptr);
    ASSERT_NE(fd_ptr, nullptr);

    const int wronly_fd = open(data_path.c_str(), O_WRONLY);
    ASSERT_GE(wronly_fd, 0);
    const int dup_rc = dup2(wronly_fd, *fd_ptr);
    close(wronly_fd);
    ASSERT_EQ(dup_rc, *fd_ptr);

    std::array<uint8_t, 16> rbuf = {};
    EXPECT_LT(
        H5FDread(rw.get(), H5FD_MEM_DEFAULT, H5P_DEFAULT, 0, 16, rbuf.data()),
        0);
  }

  EXPECT_THAT(ReadTextFile(write_err_log), HasSubstr("Error! Writing:"));
  EXPECT_THAT(ReadTextFile(read_err_log), HasSubstr("Error! Reading:"));
}

}  // namespace
}  // namespace hdf5
