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
// Tests the C stdio virtual file driver (third_party/hdf5/src/H5FDstdio.c).
//
// Build caveat: Windows-specific code paths (H5_HAVE_WIN32_API) are compiled
// out in the default Linux google3 build; flock() locking is enabled
// (H5_HAVE_FLOCK=1).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDstdio.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <system_error>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDstdio_private.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

ScopedPlist MakeStdioFapl() {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid()) {
    if (H5Pset_fapl_stdio(fapl.get()) < 0) {
      fapl.reset();
    }
  }
  return fapl;
}

template <typename Fn>
bool RunForkedChild(Fn&& fn) {
  const pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    const int rc = fn();
    H5close();
    std::exit(rc);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Exercises H5FD__stdio_init() singleton branches in isolated child processes
// so the parent process retains its default unset environment behavior.
class StdioInitEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    best_effort_ok_ = RunChildWithEnv("BEST_EFFORT", "stdio_env_best.h5");
    true_ok_ = RunChildWithEnv("TRUE", "stdio_env_true.h5");
    one_ok_ = RunChildWithEnv("1", "stdio_env_one.h5");
    delete_first_ok_ = RunDeleteFirstChild("stdio_env_delfirst.h5");
  }

  static bool best_effort_ok() { return best_effort_ok_; }
  static bool true_ok() { return true_ok_; }
  static bool one_ok() { return one_ok_; }
  static bool delete_first_ok() { return delete_first_ok_; }

 private:
  static bool RunChildWithEnv(const char* env_val, const char* filename) {
    return RunForkedChild([env_val, filename]() {
      if (setenv(HDF5_USE_FILE_LOCKING, env_val, 1) != 0) {
        return 1;
      }
      TempFile tf(filename);
      ScopedPlist fapl = MakeStdioFapl();
      if (!fapl.valid()) {
        return 2;
      }
      H5FD_t* f = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                           HADDR_UNDEF);
      if (f == nullptr) {
        return 3;
      }
      if (H5FDclose(f) < 0) {
        return 4;
      }
      if (H5FDdelete(tf.c_str(), fapl.get()) < 0) {
        return 5;
      }
      return 0;
    });
  }

  static bool RunDeleteFirstChild(const char* filename) {
    return RunForkedChild([filename]() {
      unsetenv(HDF5_USE_FILE_LOCKING);
      TempFile tf(filename);
      ScopedPlist fapl = MakeStdioFapl();
      if (!fapl.valid()) {
        return 1;
      }
      // Calling H5FDdelete before H5FDopen exercises H5FD__stdio_init() from
      // H5FD_stdio_delete().
      H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
      if (H5FDdelete(tf.c_str(), fapl.get()) >= 0) {
        return 2;
      }
      return 0;
    });
  }

  static inline bool best_effort_ok_ = false;
  static inline bool true_ok_ = false;
  static inline bool one_ok_ = false;
  static inline bool delete_first_ok_ = false;
};

[[maybe_unused]] const ::testing::Environment* const kStdioInitEnv =
    ::testing::AddGlobalTestEnvironment(new StdioInitEnvironment);

TEST(H5FdStdioTest, PreInitEnvironmentLockingAndDeleteFirstSucceeded) {
  EXPECT_TRUE(StdioInitEnvironment::best_effort_ok());
  EXPECT_TRUE(StdioInitEnvironment::true_ok());
  EXPECT_TRUE(StdioInitEnvironment::one_ok());
  EXPECT_TRUE(StdioInitEnvironment::delete_first_ok());
}

TEST(H5FdStdioTest, SetFaplStdioAndDriverQuery) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  EXPECT_GE(H5Pset_fapl_stdio(fapl.get()), 0);
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_STDIO);

  unsigned long flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_STDIO, &flags), 0);
  EXPECT_EQ(
      flags,
      static_cast<unsigned long>(
          H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
          H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA |
          H5FD_FEAT_DEFAULT_VFD_COMPATIBLE));

  // Passing a non-FAPL property list (or H5P_DEFAULT) must fail.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_fapl_stdio(dcpl.get()), 0);
  EXPECT_LT(H5Pset_fapl_stdio(H5P_DEFAULT), 0);
}

TEST(H5FdStdioTest, OpenErrorChecksAndFlagCombinations) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempDir dir("stdio_open_flags");
  const std::filesystem::path file_path = dir.child("test.h5");

  {
    ScopedErrorSilence silence;
    // Invalid name, zero/undef/overflow maxaddr.
    EXPECT_EQ(H5FDopen(nullptr, H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
              nullptr);
    EXPECT_EQ(H5FDopen("", H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF), nullptr);
    EXPECT_EQ(H5FD_stdio_g.open(nullptr, H5F_ACC_RDONLY, fapl.get(), 1024),
              nullptr);
    EXPECT_EQ(H5FD_stdio_g.open("", H5F_ACC_RDONLY, fapl.get(), 1024), nullptr);
    EXPECT_EQ(
        H5FD_stdio_g.open(file_path.c_str(), H5F_ACC_RDONLY, fapl.get(), 0),
        nullptr);
    EXPECT_EQ(H5FD_stdio_g.open(file_path.c_str(), H5F_ACC_RDONLY, fapl.get(),
                                HADDR_UNDEF),
              nullptr);
    const haddr_t overflow_maxaddr = haddr_t{1} << 63;
    EXPECT_EQ(H5FDopen(file_path.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR,
                       fapl.get(), overflow_maxaddr),
              nullptr);

    // Non-existent file without CREAT (both RDONLY and RDWR).
    EXPECT_EQ(
        H5FDopen(file_path.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF),
        nullptr);
    EXPECT_EQ(
        H5FDopen(file_path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF),
        nullptr);

    // Uncreatable path inside missing parent directory hits fopen() failure.
    const std::filesystem::path missing_dir_file =
        dir.child("no_such_subdir") / "bad.h5";
    EXPECT_EQ(H5FDopen(missing_dir_file.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR,
                       fapl.get(), HADDR_UNDEF),
              nullptr);
  }

  // 1. Create a new file and write initial bytes.
  {
    H5FD_t* f = H5FDopen(file_path.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR,
                         fapl.get(), HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    absl::Cleanup close_f = [&f] { H5FDclose(f); };
    ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 4), 0);
    constexpr std::array<uint8_t, 4> kInitial = {10, 20, 30, 40};
    ASSERT_GE(
        H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kInitial.size(),
                  kInitial.data()),
        0);
  }

  // 2. CREAT | EXCL on an existing file must fail with H5E_FILEEXISTS.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5FDopen(file_path.c_str(), H5F_ACC_CREAT | H5F_ACC_EXCL | H5F_ACC_RDWR,
                 fapl.get(), HADDR_UNDEF),
        nullptr);

    // Invalid fapl_id fails H5Pget_file_locking() inside H5FD_stdio_open().
    H5FD_t* bad_fapl_file = H5FD_stdio_g.open(
        file_path.c_str(), H5F_ACC_RDONLY, H5I_INVALID_HID, 4096);
    if (bad_fapl_file != nullptr) {
      H5FD_stdio_g.close(bad_fapl_file);
    } else {
      EXPECT_EQ(bad_fapl_file, nullptr);
    }
  }

  // 3. Re-open RDWR without TRUNC preserves existing file size and data.
  {
    H5FD_t* f =
        H5FDopen(file_path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    absl::Cleanup close_f = [&f] { H5FDclose(f); };
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 4);
    ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 4), 0);
    std::array<uint8_t, 4> buf{};
    ASSERT_GE(H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, buf.size(), buf.data()),
              0);
    EXPECT_THAT(buf, ElementsAre(10, 20, 30, 40));
  }

  // 4. Re-open RDWR | TRUNC truncates existing file back to EOF 0 via freopen.
  {
    H5FD_t* f = H5FDopen(file_path.c_str(), H5F_ACC_RDWR | H5F_ACC_TRUNC,
                         fapl.get(), HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    absl::Cleanup close_f = [&f] { H5FDclose(f); };
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 0);
  }
}

TEST(H5FdStdioTest, ReadWriteSeekOptimizationAndBoundaryErrors) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_rw.h5");

  {
    H5FD_t* f = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                         HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    absl::Cleanup close_f = [&f] { H5FDclose(f); };

    ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 64), 0);

    // 1. First write triggers seek; second contiguous write skips seek.
    constexpr std::array<uint8_t, 4> kPart1 = {1, 2, 3, 4};
    constexpr std::array<uint8_t, 4> kPart2 = {5, 6, 7, 8};
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPart1.size(),
                        kPart1.data()),
              0);
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 4, kPart2.size(),
                        kPart2.data()),
              0);
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 8);

    // 2. Overwriting inside existing EOF does not shrink EOF.
    constexpr std::array<uint8_t, 2> kPatch = {99, 100};
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 2, kPatch.size(),
                        kPatch.data()),
              0);
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 8);

    // 3. First read after write triggers seek; sequential read skips seek.
    std::array<uint8_t, 4> read1{};
    std::array<uint8_t, 4> read2{};
    read1.fill(0xff);
    read2.fill(0xff);
    ASSERT_GE(
        H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, read1.size(), read1.data()),
        0);
    ASSERT_GE(
        H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 4, read2.size(), read2.data()),
        0);
    EXPECT_THAT(read1, ElementsAre(1, 2, 99, 100));
    EXPECT_THAT(read2, ElementsAre(5, 6, 7, 8));

    // 4. Write immediately after read seeks back to write position.
    constexpr std::array<uint8_t, 2> kPatch2 = {42, 43};
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPatch2.size(),
                        kPatch2.data()),
              0);

    // 5. Reading straddling EOF returns file bytes followed by zeros.
    std::array<uint8_t, 8> straddle{};
    straddle.fill(0xaa);
    ASSERT_GE(H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 4, straddle.size(),
                       straddle.data()),
              0);
    EXPECT_THAT(straddle, ElementsAre(5, 6, 7, 8, 0, 0, 0, 0));

    // 6. Reading completely past EOF zero-fills without touching file position.
    std::array<uint8_t, 4> past_eof{};
    past_eof.fill(0x55);
    ASSERT_GE(H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 16, past_eof.size(),
                       past_eof.data()),
              0);
    EXPECT_THAT(past_eof, ElementsAre(0, 0, 0, 0));

    // 7. Zero-byte read directly through driver callback is a no-op.
    uint8_t dummy = 0x77;
    EXPECT_GE(
        H5FD_stdio_g.read(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, 0, &dummy), 0);
    EXPECT_EQ(dummy, 0x77);

    // 8. Address and region overflow errors in both read and write.
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5FD_stdio_g.read(f, H5FD_MEM_DRAW, H5P_DEFAULT, HADDR_UNDEF, 4,
                          read1.data()),
        0);
    EXPECT_LT(H5FD_stdio_g.read(f, H5FD_MEM_DRAW, H5P_DEFAULT,
                                (haddr_t{1} << 63) - 2, 8, read1.data()),
              0);
    EXPECT_LT(
        H5FD_stdio_g.write(f, H5FD_MEM_DRAW, H5P_DEFAULT, HADDR_UNDEF, 4,
                           kPart1.data()),
        0);
    EXPECT_LT(H5FD_stdio_g.write(f, H5FD_MEM_DRAW, H5P_DEFAULT,
                                 (haddr_t{1} << 63) - 2, 8, kPart1.data()),
              0);
  }

  // 9. Writing to a file opened RDONLY hits fwrite() error path.
  {
    H5FD_t* ro =
        H5FDopen(tf.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF);
    ASSERT_NE(ro, nullptr);
    absl::Cleanup close_ro = [&ro] { H5FDclose(ro); };
    ASSERT_GE(H5FDset_eoa(ro, H5FD_MEM_DRAW, 16), 0);

    constexpr std::array<uint8_t, 4> kWriteData = {1, 2, 3, 4};
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDwrite(ro, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kWriteData.size(),
                        kWriteData.data()),
              0);
  }
}

TEST(H5FdStdioTest, ReadReturnsZerosWhenUnderlyingFileWasExternallyShrunk) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_shrunk.h5");

  H5FD_t* f = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                       HADDR_UNDEF);
  ASSERT_NE(f, nullptr);
  absl::Cleanup close_f = [&f] { H5FDclose(f); };

  ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 64), 0);
  std::array<uint8_t, 64> ones{};
  ones.fill(0x11);
  ASSERT_GE(
      H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, ones.size(), ones.data()), 0);
  ASSERT_GE(H5FDflush(f, H5P_DEFAULT, false), 0);

  // Shrink the OS file beneath the open FILE* so addr < cached file->eof yet
  // fread() reaches feof().
  std::error_code ec;
  std::filesystem::resize_file(tf.path(), 8, ec);
  ASSERT_FALSE(ec);

  std::array<uint8_t, 16> read_buf{};
  read_buf.fill(0xcc);
  ASSERT_GE(
      H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 32, read_buf.size(),
               read_buf.data()),
      0);
  for (uint8_t byte : read_buf) {
    EXPECT_EQ(byte, 0);
  }
}

TEST(H5FdStdioTest, AllocEoaEofTruncateFlushAndGetHandle) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_alloc_trunc.h5");

  {
    H5FD_t* f = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                         HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    absl::Cleanup close_f = [&f] { H5FDclose(f); };

    // 1. Allocate blocks via H5FDalloc and verify EOA progression.
    EXPECT_EQ(H5FDget_eoa(f, H5FD_MEM_DRAW), 0);
    const haddr_t a1 = H5FDalloc(f, H5FD_MEM_DRAW, H5P_DEFAULT, 32);
    const haddr_t a2 = H5FDalloc(f, H5FD_MEM_SUPER, H5P_DEFAULT, 16);
    EXPECT_EQ(a1, 0);
    EXPECT_EQ(a2, 32);
    EXPECT_EQ(H5FDget_eoa(f, H5FD_MEM_DRAW), 48);

    // 2. Truncate extends physical file size from 0 to 48.
    ASSERT_GE(H5FDtruncate(f, H5P_DEFAULT, false), 0);
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 48);
    EXPECT_EQ(std::filesystem::file_size(tf.path()), 48);

    // 3. Truncate when EOA == EOF is a safe no-op.
    ASSERT_GE(H5FDtruncate(f, H5P_DEFAULT, false), 0);

    // 4. Lower EOA and truncate shrinks physical file size.
    ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 16), 0);
    ASSERT_GE(H5FDtruncate(f, H5P_DEFAULT, true), 0);
    EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 16);
    EXPECT_EQ(std::filesystem::file_size(tf.path()), 16);

    // 5. Flush with closing=false and closing=true.
    ASSERT_GE(H5FDflush(f, H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDflush(f, H5P_DEFAULT, true), 0);

    // 6. Query feature flags (both non-null and null flags pointer).
    unsigned long flags = 0;
    ASSERT_GE(H5FDquery(f, &flags), 0);
    EXPECT_NE(flags & H5FD_FEAT_DEFAULT_VFD_COMPATIBLE, 0UL);
    EXPECT_GE(H5FD_stdio_g.query(f, nullptr), 0);

    // 7. Retrieve underlying FILE** handle.
    void* raw_handle = nullptr;
    ASSERT_GE(H5FDget_vfd_handle(f, fapl.get(), &raw_handle), 0);
    ASSERT_NE(raw_handle, nullptr);
    FILE* fp = *static_cast<FILE**>(raw_handle);
    EXPECT_NE(fp, nullptr);
  }

  // 8. On RDONLY handle, flush is a no-op, truncate succeeds when eoa <= eof,
  // and truncate fails when eoa > eof.
  {
    H5FD_t* ro =
        H5FDopen(tf.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF);
    ASSERT_NE(ro, nullptr);
    absl::Cleanup close_ro = [&ro] { H5FDclose(ro); };

    EXPECT_GE(H5FDflush(ro, H5P_DEFAULT, false), 0);
    ASSERT_GE(H5FDset_eoa(ro, H5FD_MEM_DRAW, 16), 0);
    EXPECT_GE(H5FDtruncate(ro, H5P_DEFAULT, false), 0);

    ASSERT_GE(H5FDset_eoa(ro, H5FD_MEM_DRAW, 32), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDtruncate(ro, H5P_DEFAULT, false), 0);
  }
}

TEST(H5FdStdioTest, CmpOrdersSameFileDistinctInodesAndDistinctDevices) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf1("stdio_cmp1.h5");
  TempFile tf2("stdio_cmp2.h5");

  H5FD_t* f1 = H5FDopen(tf1.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                        HADDR_UNDEF);
  ASSERT_NE(f1, nullptr);
  absl::Cleanup close_f1 = [&f1] { H5FDclose(f1); };

  H5FD_t* f1_dup =
      H5FDopen(tf1.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF);
  ASSERT_NE(f1_dup, nullptr);
  absl::Cleanup close_f1_dup = [&f1_dup] { H5FDclose(f1_dup); };

  H5FD_t* f2 = H5FDopen(tf2.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                        HADDR_UNDEF);
  ASSERT_NE(f2, nullptr);
  absl::Cleanup close_f2 = [&f2] { H5FDclose(f2); };

  EXPECT_EQ(H5FDcmp(f1, f1_dup), 0);

  const int cmp_1_2 = H5FDcmp(f1, f2);
  const int cmp_2_1 = H5FDcmp(f2, f1);
  EXPECT_NE(cmp_1_2, 0);
  EXPECT_EQ(cmp_1_2, -cmp_2_1);

  // Compare against /dev/null if available on a different st_dev to exercise
  // device comparison branches in both directions.
  struct stat st_tf{};
  struct stat st_null{};
  if (stat(tf1.c_str(), &st_tf) == 0 && stat("/dev/null", &st_null) == 0 &&
      st_tf.st_dev != st_null.st_dev) {
    H5FD_t* f_null =
        H5FDopen("/dev/null", H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF);
    if (f_null != nullptr) {
      absl::Cleanup close_null = [&f_null] { H5FDclose(f_null); };
      const int c_a = H5FDcmp(f1, f_null);
      const int c_b = H5FDcmp(f_null, f1);
      EXPECT_NE(c_a, 0);
      EXPECT_EQ(c_a, -c_b);
    }
  }
}

TEST(H5FdStdioTest, LockUnlockAndLockContention) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_lock.h5");

  H5FD_t* f1 = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                        HADDR_UNDEF);
  ASSERT_NE(f1, nullptr);
  absl::Cleanup close_f1 = [&f1] { H5FDclose(f1); };

  H5FD_t* f2 = H5FDopen(tf.c_str(), H5F_ACC_RDONLY, fapl.get(), HADDR_UNDEF);
  ASSERT_NE(f2, nullptr);
  absl::Cleanup close_f2 = [&f2] { H5FDclose(f2); };

  // 1. Shared lock on f1 allows shared lock on f2, then unlock both.
  ASSERT_GE(H5FDlock(f1, false), 0);
  ASSERT_GE(H5FDlock(f2, false), 0);
  ASSERT_GE(H5FDunlock(f2), 0);
  ASSERT_GE(H5FDunlock(f1), 0);

  // 2. Exclusive lock on f1 causes non-blocking lock on f2 to fail.
  ASSERT_GE(H5FDlock(f1, true), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDlock(f2, false), 0);
    EXPECT_LT(H5FDlock(f2, true), 0);
  }
  EXPECT_GE(H5FDunlock(f1), 0);
}

TEST(H5FdStdioTest, DeleteExistingAndNonexistentFiles) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_delete.h5");

  {
    H5FD_t* f = H5FDopen(tf.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR, fapl.get(),
                         HADDR_UNDEF);
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5FDclose(f), 0);
  }
  ASSERT_TRUE(std::filesystem::exists(tf.path()));

  EXPECT_GE(H5FDdelete(tf.c_str(), fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(tf.path()));

  ScopedErrorSilence silence;
  EXPECT_LT(H5FDdelete(tf.c_str(), fapl.get()), 0);
}

TEST(H5FdStdioTest, HighLevelHdf5DatasetRoundTripWithStdioVfd) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempFile tf("stdio_h5file.h5");

  constexpr std::array<int32_t, 6> kWriteValues = {11, 22, 33, 44, 55, 66};
  constexpr hsize_t kDims[1] = {kWriteValues.size()};

  // 1. Create an HDF5 file and dataset using H5Pset_fapl_stdio().
  {
    ScopedFile file(
        H5Fcreate(tf.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    void* vfd_handle = nullptr;
    ASSERT_GE(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &vfd_handle), 0);
    ASSERT_NE(vfd_handle, nullptr);
    EXPECT_NE(*static_cast<FILE**>(vfd_handle), nullptr);

    ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "values", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kWriteValues.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // 2. Re-open read-only and verify dataset contents, then delete via H5Fdelete.
  {
    ScopedFile file(H5Fopen(tf.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "values", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<int32_t, 6> read_values{};
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_values.data()),
              0);
    EXPECT_THAT(read_values, ElementsAre(11, 22, 33, 44, 55, 66));
  }

  EXPECT_GE(H5Fdelete(tf.c_str(), fapl.get()), 0);
  EXPECT_FALSE(std::filesystem::exists(tf.path()));
}

TEST(H5FdStdioTest, FifoHandleTriggersSeekAndTruncateErrors) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempDir dir("stdio_fifo");
  const std::filesystem::path fifo_path = dir.child("test.fifo");
  ASSERT_EQ(mkfifo(fifo_path.c_str(), 0600), 0);

  // Hold both ends of the FIFO open non-blocking so fopen(..., "rb+") succeeds
  // without blocking while lseek/ftruncate fail with ESPIPE/EINVAL.
  const int keepalive_fd = open(fifo_path.c_str(), O_RDWR | O_NONBLOCK);
  ASSERT_GE(keepalive_fd, 0);
  absl::Cleanup close_keepalive = [keepalive_fd] { close(keepalive_fd); };

  // 1. Open FIFO RDWR; initial fseek(..., SEEK_END) fails and clears op.
  H5FD_t* f = H5FDopen(fifo_path.c_str(), H5F_ACC_RDWR, fapl.get(), HADDR_UNDEF);
  ASSERT_NE(f, nullptr);
  absl::Cleanup close_f = [&f] { H5FDclose(f); };
  EXPECT_EQ(H5FDget_eof(f, H5FD_MEM_DRAW), 0);

  // 2. Writing attempts fseek(..., SEEK_SET) on the pipe and fails.
  ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 16), 0);
  constexpr std::array<uint8_t, 4> kPayload = {1, 2, 3, 4};
  ScopedErrorSilence silence;
  EXPECT_LT(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPayload.size(),
                      kPayload.data()),
            0);

  // 3. Truncating with eoa != eof fails in file_ftruncate() on the FIFO fd.
  EXPECT_LT(H5FDtruncate(f, H5P_DEFAULT, false), 0);
}

TEST(H5FdStdioTest, UnderlyingFdIoErrorsOnReadFlushLockAndUnlock) {
  ScopedPlist fapl = MakeStdioFapl();
  ASSERT_TRUE(fapl.valid());
  TempDir dir("stdio_io_errors");
  const std::filesystem::path file_path = dir.child("target.h5");

  H5FD_t* f = H5FDopen(file_path.c_str(), H5F_ACC_CREAT | H5F_ACC_RDWR,
                       fapl.get(), HADDR_UNDEF);
  ASSERT_NE(f, nullptr);
  absl::Cleanup close_f = [&f] { H5FDclose(f); };

  ASSERT_GE(H5FDset_eoa(f, H5FD_MEM_DRAW, 16), 0);
  constexpr std::array<uint8_t, 8> kInitData = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kInitData.size(),
                      kInitData.data()),
            0);
  ASSERT_GE(H5FDflush(f, H5P_DEFAULT, false), 0);

  void* raw_handle = nullptr;
  ASSERT_GE(H5FDget_vfd_handle(f, fapl.get(), &raw_handle), 0);
  FILE* fp = *static_cast<FILE**>(raw_handle);
  ASSERT_NE(fp, nullptr);
  const int fd = fileno(fp);
  ASSERT_GE(fd, 0);

  // Save the writable fd BEFORE swapping so restore_fd runs prior to close_f.
  const int saved_fd = dup(fd);
  ASSERT_GE(saved_fd, 0);
  absl::Cleanup restore_fd = [fd, saved_fd, fp] {
    dup2(saved_fd, fd);
    close(saved_fd);
    std::clearerr(fp);
  };

  ScopedErrorSilence silence;

  // 1. Replace fd with a pipe read end so fseeko(..., SEEK_SET) in read fails.
  {
    int pipe_fds[2] = {-1, -1};
    ASSERT_EQ(pipe(pipe_fds), 0);
    absl::Cleanup close_pipe = [&pipe_fds] {
      close(pipe_fds[0]);
      close(pipe_fds[1]);
    };
    ASSERT_GE(dup2(pipe_fds[0], fd), 0);
    std::array<uint8_t, 4> read_buf{};
    EXPECT_LT(H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, read_buf.size(),
                       read_buf.data()),
              0);
  }

  // 2. Replace fd with a directory fd so fseeko(0) succeeds yet fread fails
  //    with EISDIR while leaving fclose() able to close cleanly once restored.
  {
    const int dir_fd = open(dir.path().c_str(), O_RDONLY);
    ASSERT_GE(dir_fd, 0);
    absl::Cleanup close_dir_fd = [dir_fd] { close(dir_fd); };
    ASSERT_GE(dup2(dir_fd, fd), 0);
    std::array<uint8_t, 4> read_buf{};
    EXPECT_LT(H5FDread(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, read_buf.size(),
                       read_buf.data()),
              0);
  }

  // 3. Buffer a small write on the real fd, then swap in an O_RDONLY fd so
  //    fflush() fails with EBADF in flush, lock, and unlock.
  {
    const int ro_fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_GE(ro_fd, 0);
    absl::Cleanup close_ro_fd = [ro_fd] { close(ro_fd); };
    constexpr std::array<uint8_t, 4> kPatch = {9, 8, 7, 6};

    ASSERT_GE(dup2(saved_fd, fd), 0);
    std::clearerr(fp);
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPatch.size(),
                        kPatch.data()),
              0);
    ASSERT_GE(dup2(ro_fd, fd), 0);
    EXPECT_LT(H5FDflush(f, H5P_DEFAULT, false), 0);

    ASSERT_GE(dup2(saved_fd, fd), 0);
    std::clearerr(fp);
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPatch.size(),
                        kPatch.data()),
              0);
    ASSERT_GE(dup2(ro_fd, fd), 0);
    EXPECT_LT(H5FDlock(f, false), 0);

    ASSERT_GE(dup2(saved_fd, fd), 0);
    std::clearerr(fp);
    ASSERT_GE(H5FDwrite(f, H5FD_MEM_DRAW, H5P_DEFAULT, 0, kPatch.size(),
                        kPatch.data()),
              0);
    ASSERT_GE(dup2(ro_fd, fd), 0);
    EXPECT_LT(H5FDunlock(f), 0);

    ASSERT_GE(dup2(saved_fd, fd), 0);
    std::clearerr(fp);
    EXPECT_GE(H5FDunlock(f), 0);
  }
}

}  // namespace
}  // namespace hdf5
