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
// Tests HDF5 page buffering lifecycle, read/write caching, LRU eviction,
// partial/multi-page I/O, new-page hints, entry updates/removals, flushes,
// statistics, and error paths (third_party/hdf5/src/H5PB.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL, so parallel-only bypass paths are compiled out and NDEBUG
// is temporarily aligned around internal headers to match `:hdf5` struct
// layouts.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5PB.c

// `:hdf5` is built with -DNDEBUG; align NDEBUG across internal headers so
// struct layouts pulled via H5Fpkg.h match the compiled library.
#ifndef NDEBUG
#define H5PB_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_c_try
extern "C" {
#define H5F_FRIEND
#define H5PB_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5PBpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5PB_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PB_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr size_t kPageSize = 512;

// RAII API context so H5FD_read / H5FD_write can read DXPL state via H5CX.
class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5open() >= 0 && H5CX_push(&node_) >= 0) {
      pushed_ = true;
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

// Redirects POSIX stdout into a pipe so H5PB_print_stats output can be verified
// without polluting test logs.
class StdoutCapture {
 public:
  StdoutCapture() {
    std::fflush(stdout);
    int fds[2] = {-1, -1};
    if (pipe(fds) == 0) {
      read_fd_ = fds[0];
      saved_stdout_ = dup(STDOUT_FILENO);
      if (saved_stdout_ >= 0 && dup2(fds[1], STDOUT_FILENO) >= 0) {
        active_ = true;
      }
      close(fds[1]);
    }
  }

  ~StdoutCapture() { Finish(); }

  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  std::string Finish() {
    if (finished_) {
      return captured_;
    }
    finished_ = true;
    std::fflush(stdout);
    if (saved_stdout_ >= 0) {
      if (active_) {
        dup2(saved_stdout_, STDOUT_FILENO);
      }
      close(saved_stdout_);
      saved_stdout_ = -1;
    }
    if (read_fd_ >= 0) {
      std::array<char, 512> buf{};
      ssize_t n = 0;
      while ((n = read(read_fd_, buf.data(), buf.size())) > 0) {
        captured_.append(buf.data(), static_cast<size_t>(n));
      }
      close(read_fd_);
      read_fd_ = -1;
    }
    return captured_;
  }

 private:
  int read_fd_ = -1;
  int saved_stdout_ = -1;
  bool active_ = false;
  bool finished_ = false;
  std::string captured_;
};

// Minimal in-memory mock H5FD_t + H5F_shared_t harness targeting H5PB.c
// without disk I/O or superblock setup overhead.
class MockPbFile {
 public:
  explicit MockPbFile(hsize_t backing_size = 16 * kPageSize,
                      hsize_t page_size = kPageSize)
      : storage(backing_size, 0), eoa(backing_size), eof(backing_size) {
    fd.pub.driver_id = H5I_INVALID_HID;
    fd.pub.cls = &kDriverClass;
    fd.pub.access_flags = H5F_ACC_RDWR;
    fd.pub.maxaddr = backing_size;
    fd.owner = this;

    f_sh.lf = &fd.pub;
    f_sh.flags = H5F_ACC_RDWR;
    f_sh.fs_strategy = H5F_FSPACE_STRATEGY_PAGE;
    f_sh.fs_page_size = page_size;
    f_sh.accum.loc = HADDR_UNDEF;
  }

  ~MockPbFile() {
    ScopedErrorSilence silence;
    // Restore healthy flags so H5PB_dest can flush and free all PB allocations
    // cleanly under LeakSanitizer even if a test injected I/O errors.
    fail_read = false;
    fail_write = false;
    fail_get_eoa = false;
    fail_get_eof = false;
    eoa = storage.size();
    eof = storage.size();
    f_sh.flags = H5F_ACC_RDWR;
    H5PB_dest(&f_sh);
    H5F__accum_reset(&f_sh, /*flush=*/false, /*force=*/true);
  }

  MockPbFile(const MockPbFile&) = delete;
  MockPbFile& operator=(const MockPbFile&) = delete;

  void FillBacking(haddr_t addr, size_t len, uint8_t seed = 1) {
    for (size_t i = 0; i < len; ++i) {
      storage[addr + i] = static_cast<uint8_t>((seed + i) & 0xFF);
    }
  }

  struct MockFd {
    H5FD_t pub{};
    MockPbFile* owner = nullptr;
  };

  static haddr_t GetEoa(const H5FD_t* file, H5FD_mem_t /*type*/) {
    const auto* mock = reinterpret_cast<const MockFd*>(file);
    if (mock->owner->fail_get_eoa) {
      return HADDR_UNDEF;
    }
    return mock->owner->eoa;
  }

  static haddr_t GetEof(const H5FD_t* file, H5FD_mem_t /*type*/) {
    const auto* mock = reinterpret_cast<const MockFd*>(file);
    if (mock->owner->fail_get_eof) {
      return HADDR_UNDEF;
    }
    return mock->owner->eof;
  }

  static herr_t Read(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, void* buf) {
    auto* mock = reinterpret_cast<MockFd*>(file);
    auto* self = mock->owner;
    ++self->read_calls;
    self->last_read_addr = addr;
    self->last_read_size = size;
    if (self->fail_read) {
      return -1;
    }
    if (addr + size > self->storage.size()) {
      return -1;
    }
    std::memcpy(buf, self->storage.data() + addr, size);
    return 0;
  }

  static herr_t Write(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                      haddr_t addr, size_t size, const void* buf) {
    auto* mock = reinterpret_cast<MockFd*>(file);
    auto* self = mock->owner;
    ++self->write_calls;
    self->last_write_addr = addr;
    self->last_write_size = size;
    if (self->fail_write) {
      return -1;
    }
    if (addr + size > self->storage.size()) {
      return -1;
    }
    std::memcpy(self->storage.data() + addr, buf, size);
    return 0;
  }

  static constexpr H5FD_class_t kDriverClass = []() {
    H5FD_class_t cls{};
    cls.version = H5FD_CLASS_VERSION;
    cls.value = static_cast<H5FD_class_value_t>(262);
    cls.name = "mock_pb_vfd";
    cls.maxaddr = HADDR_MAX - 1;
    cls.fc_degree = H5F_CLOSE_WEAK;
    cls.get_eoa = &MockPbFile::GetEoa;
    cls.get_eof = &MockPbFile::GetEof;
    cls.read = &MockPbFile::Read;
    cls.write = &MockPbFile::Write;
    return cls;
  }();

  std::vector<uint8_t> storage;
  haddr_t eoa = 0;
  haddr_t eof = 0;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_get_eoa = false;
  bool fail_get_eof = false;
  int read_calls = 0;
  int write_calls = 0;
  haddr_t last_read_addr = HADDR_UNDEF;
  size_t last_read_size = 0;
  haddr_t last_write_addr = HADDR_UNDEF;
  size_t last_write_size = 0;
  MockFd fd{};
  H5F_shared_t f_sh{};
};

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> out(size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = static_cast<uint8_t>((seed + i) & 0xFF);
  }
  return out;
}

H5PB_t* GetPageBufPtr(hid_t file_id) {
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
  if (f == nullptr || f->shared == nullptr) {
    return nullptr;
  }
  return f->shared->page_buf;
}

TEST(H5PBTest, PublicCoreFilePagedRoundTripAndStats) {
  // 1. Configure Core VFD with PAGE strategy and a 2-page buffer (8192 bytes).
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       /*persist=*/false, /*threshold=*/1),
            0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 8192, 50, 25), 0);

  ScopedFile file(H5Fcreate("pb_public.h5", H5F_ACC_TRUNC, fcpl.get(),
                            fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_NE(GetPageBufPtr(file.get()), nullptr);

  // 2. Write a small contiguous dataset (< page size) and read it back.
  constexpr hsize_t kCount = 64;
  ScopedSpace space(H5Screate_simple(1, &kCount, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "small_ds", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::vector<int32_t> write_data(kCount);
  for (hsize_t i = 0; i < kCount; ++i) {
    write_data[i] = static_cast<int32_t>(i * 3 + 7);
  }
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, write_data.data()),
            0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  std::vector<int32_t> read_data(kCount, -1);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_data.data()),
            0);
  EXPECT_EQ(read_data, write_data);

  // 3. Query and reset page buffering statistics via public APIs.
  std::array<unsigned, 2> accesses{};
  std::array<unsigned, 2> hits{};
  std::array<unsigned, 2> misses{};
  std::array<unsigned, 2> evictions{};
  std::array<unsigned, 2> bypasses{};
  ASSERT_GE(H5Fget_page_buffering_stats(file.get(), accesses.data(),
                                        hits.data(), misses.data(),
                                        evictions.data(), bypasses.data()),
            0);
  EXPECT_GT(accesses[0] + accesses[1], 0u);

  ASSERT_GE(H5Freset_page_buffering_stats(file.get()), 0);
  ASSERT_GE(H5Fget_page_buffering_stats(file.get(), accesses.data(),
                                        hits.data(), misses.data(),
                                        evictions.data(), bypasses.data()),
            0);
  EXPECT_THAT(accesses, Each(0u));
  EXPECT_THAT(hits, Each(0u));
  EXPECT_THAT(misses, Each(0u));
  EXPECT_THAT(evictions, Each(0u));
  EXPECT_THAT(bypasses, Each(0u));

  // 4. Write a large dataset (> page size) to exercise large raw-data bypass.
  constexpr hsize_t kLargeCount = 4096;
  ScopedSpace large_space(H5Screate_simple(1, &kLargeCount, nullptr));
  ASSERT_TRUE(large_space.valid());
  ScopedDataset large_dset(H5Dcreate2(file.get(), "large_ds", H5T_NATIVE_INT32,
                                      large_space.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(large_dset.valid());
  std::vector<int32_t> large_write(kLargeCount, 42);
  ASSERT_GE(H5Dwrite(large_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, large_write.data()),
            0);
  // Closing the dataset flushes its 16 KiB contiguous sieve buffer (>= 4 KiB
  // page size) through H5PB_write, exercising the public raw-data bypass path.
  large_dset.reset();
  large_space.reset();

  ScopedDataset reopen_large(H5Dopen2(file.get(), "large_ds", H5P_DEFAULT));
  ASSERT_TRUE(reopen_large.valid());
  std::vector<int32_t> large_read(kLargeCount, 0);
  ASSERT_GE(H5Dread(reopen_large.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, large_read.data()),
            0);
  EXPECT_EQ(large_read, large_write);

  ASSERT_GE(H5Fget_page_buffering_stats(file.get(), accesses.data(),
                                        hits.data(), misses.data(),
                                        evictions.data(), bypasses.data()),
            0);
  EXPECT_GT(bypasses[1], 0u);
}

TEST(H5PBTest, PublicFileCreateRejectsPageBufferWithoutPageStrategy) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 4096, 0, 0), 0);

  ScopedErrorSilence silence;
  ScopedFile bad_file(H5Fcreate("pb_nonpage.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                fapl.get()));
  EXPECT_FALSE(bad_file.valid());
}

TEST(H5PBTest, CreateValidatesStrategyAndSizes) {
  MockPbFile mock(8 * kPageSize, kPageSize);

  // 1. Non-PAGE file space strategy fails.
  mock.f_sh.fs_strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5PB_create(&mock.f_sh, 2 * kPageSize, 50, 50), 0);
  }
  EXPECT_EQ(mock.f_sh.page_buf, nullptr);

  // 2. Buffer size smaller than page_size and not a multiple fails.
  mock.f_sh.fs_strategy = H5F_FSPACE_STRATEGY_PAGE;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5PB_create(&mock.f_sh, kPageSize / 2, 0, 0), 0);
  }
  EXPECT_EQ(mock.f_sh.page_buf, nullptr);

  // 3. Buffer size > page_size that is not an exact multiple rounds down.
  ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize + 123, 50, 25), 0);
  ASSERT_NE(mock.f_sh.page_buf, nullptr);
  EXPECT_EQ(mock.f_sh.page_buf->max_size, 2 * kPageSize);
  EXPECT_EQ(mock.f_sh.page_buf->page_size, kPageSize);
  EXPECT_EQ(mock.f_sh.page_buf->min_meta_perc, 50u);
  EXPECT_EQ(mock.f_sh.page_buf->min_raw_perc, 25u);
  EXPECT_EQ(mock.f_sh.page_buf->min_meta_count, 1u);
  EXPECT_EQ(mock.f_sh.page_buf->min_raw_count, 0u);

  EXPECT_GE(H5PB_dest(&mock.f_sh), 0);
  EXPECT_EQ(mock.f_sh.page_buf, nullptr);

  // 4. Calling H5PB_flush and H5PB_dest when page_buf is null are no-ops.
  EXPECT_GE(H5PB_flush(&mock.f_sh), 0);
  EXPECT_GE(H5PB_dest(&mock.f_sh), 0);
}

TEST(H5PBTest, EnabledReflectsPresenceOfPageBuffer) {
  MockPbFile mock(4 * kPageSize, kPageSize);
  bool enabled = true;

  ASSERT_GE(H5PB_enabled(&mock.f_sh, H5FD_MEM_DRAW, &enabled), 0);
  EXPECT_FALSE(enabled);
  ASSERT_GE(H5PB_enabled(&mock.f_sh, H5FD_MEM_SUPER, &enabled), 0);
  EXPECT_FALSE(enabled);

  ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 0, 0), 0);
  ASSERT_GE(H5PB_enabled(&mock.f_sh, H5FD_MEM_DRAW, &enabled), 0);
  EXPECT_TRUE(enabled);
  ASSERT_GE(H5PB_enabled(&mock.f_sh, H5FD_MEM_OHDR, &enabled), 0);
  EXPECT_TRUE(enabled);
}

TEST(H5PBTest, StatsGetResetAndPrintStats) {
  MockPbFile mock(4 * kPageSize, kPageSize);
  ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 50, 50), 0);
  H5PB_t* pb = mock.f_sh.page_buf;
  ASSERT_NE(pb, nullptr);

  // Use non-zero denominators for both metadata and raw data hit rates:
  // H5PB_print_stats divides by (accesses[0] - bypasses[0]) and
  // (accesses[1] - bypasses[0]).
  pb->accesses[0] = 5;
  pb->hits[0] = 3;
  pb->misses[0] = 2;
  pb->evictions[0] = 1;
  pb->bypasses[0] = 1;

  pb->accesses[1] = 6;
  pb->hits[1] = 4;
  pb->misses[1] = 2;
  pb->evictions[1] = 1;
  pb->bypasses[1] = 2;

  std::array<unsigned, 2> accesses{};
  std::array<unsigned, 2> hits{};
  std::array<unsigned, 2> misses{};
  std::array<unsigned, 2> evictions{};
  std::array<unsigned, 2> bypasses{};
  ASSERT_GE(H5PB_get_stats(pb, accesses.data(), hits.data(), misses.data(),
                           evictions.data(), bypasses.data()),
            0);
  EXPECT_THAT(accesses, ElementsAre(5u, 6u));
  EXPECT_THAT(hits, ElementsAre(3u, 4u));
  EXPECT_THAT(misses, ElementsAre(2u, 2u));
  EXPECT_THAT(evictions, ElementsAre(1u, 1u));
  EXPECT_THAT(bypasses, ElementsAre(1u, 2u));

  {
    StdoutCapture cap;
    ASSERT_GE(H5PB_print_stats(pb), 0);
    const std::string printed = cap.Finish();
    EXPECT_THAT(printed, HasSubstr("PAGE BUFFER STATISTICS:"));
    EXPECT_THAT(printed, HasSubstr("METADATA"));
    EXPECT_THAT(printed, HasSubstr("RAWDATA"));
    EXPECT_THAT(printed, HasSubstr("Hit Rate ="));
  }

  ASSERT_GE(H5PB_reset_stats(pb), 0);
  ASSERT_GE(H5PB_get_stats(pb, accesses.data(), hits.data(), misses.data(),
                           evictions.data(), bypasses.data()),
            0);
  EXPECT_THAT(accesses, Each(0u));
  EXPECT_THAT(hits, Each(0u));
  EXPECT_THAT(misses, Each(0u));
  EXPECT_THAT(evictions, Each(0u));
  EXPECT_THAT(bypasses, Each(0u));
}

TEST(H5PBTest, BypassReadAndWriteWhenPageBufferDisabledOrLargeMetadata) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(8 * kPageSize, kPageSize);
  mock.FillBacking(0, 4 * kPageSize, 0x10);

  // 1. page_buf == nullptr bypasses directly to H5F__accum_read / write.
  const auto pat = MakePattern(64, 0x41);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 16, pat.size(), pat.data()),
            0);
  EXPECT_EQ(mock.write_calls, 1);

  std::vector<uint8_t> out(64, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 16, out.size(), out.data()),
            0);
  EXPECT_EQ(mock.read_calls, 1);
  EXPECT_EQ(out, pat);

  // Error propagation from H5F__accum_read / write when page_buf == nullptr.
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(
        H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 16, pat.size(), pat.data()), 0);
    mock.fail_write = false;
    mock.fail_read = true;
    EXPECT_LT(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 16, out.size(), out.data()),
              0);
    mock.fail_read = false;
  }

  // 2. Large metadata (>= page_size) bypasses PB and increments bypasses[0].
  ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 0, 0), 0);
  const auto big_meta = MakePattern(kPageSize, 0x70);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_SUPER, kPageSize, big_meta.size(),
                       big_meta.data()),
            0);
  EXPECT_EQ(mock.f_sh.page_buf->bypasses[0], 1u);
  EXPECT_EQ(mock.f_sh.page_buf->accesses[0], 0u);
  EXPECT_EQ(mock.f_sh.page_buf->LRU_list_len, 0u);

  std::vector<uint8_t> big_out(kPageSize, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, kPageSize, big_out.size(),
                      big_out.data()),
            0);
  EXPECT_EQ(mock.f_sh.page_buf->bypasses[0], 2u);
  EXPECT_EQ(big_out, big_meta);
}

TEST(H5PBTest, SmallReadWriteHitsMissesTwoPageSpanAndBoundaryClamp) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(8 * kPageSize, kPageSize);
  mock.FillBacking(0, 6 * kPageSize, 0x05);
  ASSERT_GE(H5PB_create(&mock.f_sh, 4 * kPageSize, 0, 0), 0);
  H5PB_t* pb = mock.f_sh.page_buf;

  // 1. Metadata read miss then hit.
  std::vector<uint8_t> meta_buf(32, 0);
  ASSERT_GE(
      H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 16, meta_buf.size(), meta_buf.data()),
      0);
  EXPECT_EQ(pb->accesses[0], 1u);
  EXPECT_EQ(pb->misses[0], 1u);
  EXPECT_EQ(pb->hits[0], 0u);
  EXPECT_EQ(mock.read_calls, 1);

  std::vector<uint8_t> meta_buf2(32, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 16, meta_buf2.size(),
                      meta_buf2.data()),
            0);
  EXPECT_EQ(meta_buf2, meta_buf);
  EXPECT_EQ(pb->hits[0], 1u);
  EXPECT_EQ(mock.read_calls, 1);

  // 2. Metadata read where offset + size > page_size clamps access_size on both
  // miss (page 1) and hit (page 1).
  std::vector<uint8_t> clamp_miss(16, 0xEE);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_OHDR, kPageSize + kPageSize - 4,
                      clamp_miss.size(), clamp_miss.data()),
            0);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(clamp_miss[i], mock.storage[kPageSize + kPageSize - 4 + i]);
  }
  for (size_t i = 4; i < 16; ++i) {
    EXPECT_EQ(clamp_miss[i], 0xEE);
  }
  std::vector<uint8_t> clamp_hit(16, 0xAA);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_OHDR, kPageSize + kPageSize - 4,
                      clamp_hit.size(), clamp_hit.data()),
            0);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(clamp_hit[i], mock.storage[kPageSize + kPageSize - 4 + i]);
  }
  for (size_t i = 4; i < 16; ++i) {
    EXPECT_EQ(clamp_hit[i], 0xAA);
  }

  // 3. Raw data write spanning two pages (pages 2 and 3), including GHEAP hit
  // and read-back spanning two pages.
  const auto span_pat = MakePattern(64, 0x90);
  const haddr_t span_addr = 2 * kPageSize + kPageSize - 24;
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, span_addr, span_pat.size(),
                       span_pat.data()),
            0);
  EXPECT_EQ(pb->misses[1], 2u);

  // Update part of the second page via H5FD_MEM_GHEAP to hit GHEAP stats path.
  const auto gheap_pat = MakePattern(12, 0x33);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_GHEAP, 3 * kPageSize + 4,
                       gheap_pat.size(), gheap_pat.data()),
            0);
  EXPECT_EQ(pb->hits[1], 1u);

  std::vector<uint8_t> span_out(64, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, span_addr, span_out.size(),
                      span_out.data()),
            0);
  std::vector<uint8_t> expected = span_pat;
  std::copy(gheap_pat.begin(), gheap_pat.end(), expected.begin() + 28);
  EXPECT_EQ(span_out, expected);

  // 4. Metadata write hitting existing page in cache (hits: step 1 + step 2 +
  // step 4 = 3).
  const auto meta_patch = MakePattern(8, 0xC1);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_SUPER, 8, meta_patch.size(),
                       meta_patch.data()),
            0);
  EXPECT_EQ(pb->hits[0], 3u);
}

TEST(H5PBTest, LargeRawReadOverlaysDirtyPagesAndHandlesGaps) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(10 * kPageSize, kPageSize);
  mock.FillBacking(0, 10 * kPageSize, 0x11);
  ASSERT_GE(H5PB_create(&mock.f_sh, 6 * kPageSize, 0, 0), 0);

  // Dirty page 1 (tail of first read page), page 2 (full middle page), and
  // page 7 (beyond read end); keep page 0 clean in PB and leave page 4 out.
  std::array<uint8_t, 8> p0_probe{};
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 0, p0_probe.size(),
                      p0_probe.data()),
            0);

  const size_t r1_first_len = kPageSize - 128;
  const auto p1_tail_dirty = MakePattern(r1_first_len, 0xA1);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, kPageSize + 128,
                       r1_first_len, p1_tail_dirty.data()),
            0);
  const auto p2_dirty = MakePattern(kPageSize - 8, 0xB0);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, 32,
                       p2_dirty.data()),
            0);
  const auto p7_dirty = MakePattern(16, 0xD0);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 7 * kPageSize, 16,
                       p7_dirty.data()),
            0);

  // 1. Unaligned large raw read spanning 2 pages (pages 1 and 2): exercises
  // dirty first partial page and dirty last partial page overlays.
  const haddr_t r1_addr = kPageSize + 128;
  const size_t r1_size = kPageSize;
  std::vector<uint8_t> r1_buf(r1_size, 0);
  ASSERT_GE(
      H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, r1_addr, r1_size, r1_buf.data()), 0);
  EXPECT_EQ(std::memcmp(r1_buf.data(), p1_tail_dirty.data(), r1_first_len), 0);
  EXPECT_EQ(std::memcmp(r1_buf.data() + r1_first_len, p2_dirty.data(), 32), 0);

  // 2. Aligned large raw read covering clean page 0 + dirty page 1 + dirty
  // page 2: tests clean page skip (`!is_dirty`) and full-page dirty copy (`else`
  // branch).
  std::vector<uint8_t> r2_buf(3 * kPageSize, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 0, r2_buf.size(),
                      r2_buf.data()),
            0);
  EXPECT_EQ(std::memcmp(r2_buf.data() + kPageSize + 128, p1_tail_dirty.data(),
                        r1_first_len),
            0);
  EXPECT_EQ(
      std::memcmp(r2_buf.data() + 2 * kPageSize, p2_dirty.data(), 32), 0);

  // 3. Read starting on uncached page 4 (not in PB) and ending on page 5 (not
  // in PB), while page 7 is in PB: tests `!node && i != 0` search.
  // First make page 5 dirty so `node = H5SL_find(..., &search_addr)` succeeds
  // at `i == 1`, while page 4 is absent.
  const auto p5_dirty = MakePattern(20, 0xE1);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 5 * kPageSize,
                       p5_dirty.size(), p5_dirty.data()),
            0);
  // Page buffer capacity is 6 pages (currently pages 0, 1, 2, 7, 5).
  std::vector<uint8_t> r3_buf(2 * kPageSize, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 4 * kPageSize, r3_buf.size(),
                      r3_buf.data()),
            0);
  EXPECT_EQ(std::memcmp(r3_buf.data() + kPageSize, p5_dirty.data(),
                        p5_dirty.size()),
            0);

  // 4. Read starting at cached page 5 and covering uncached page 6, while page
  // 7 (>= addr + size) follows in skip list: hits `page_entry->addr >= addr +
  // size -> break`.
  std::vector<uint8_t> r4_buf(kPageSize + 64, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 5 * kPageSize + 16,
                      r4_buf.size(), r4_buf.data()),
            0);
  EXPECT_EQ(std::memcmp(r4_buf.data(), p5_dirty.data() + 16, 4), 0);
}

TEST(H5PBTest, LargeRawWriteUpdatesPartialAndDiscardsFullPages) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(8 * kPageSize, kPageSize);
  mock.FillBacking(0, 8 * kPageSize, 0x20);
  ASSERT_GE(H5PB_create(&mock.f_sh, 5 * kPageSize, 0, 0), 0);
  H5PB_t* pb = mock.f_sh.page_buf;

  // Load page 0 (raw), page 1 (raw), page 2 (meta), page 3 (raw) into PB.
  std::array<uint8_t, 8> tmp{};
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 0, tmp.size(), tmp.data()), 0);
  ASSERT_GE(
      H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, kPageSize, tmp.size(), tmp.data()),
      0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 2 * kPageSize, tmp.size(),
                      tmp.data()),
            0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 3 * kPageSize, tmp.size(),
                      tmp.data()),
            0);
  EXPECT_EQ(pb->raw_count, 3u);
  EXPECT_EQ(pb->meta_count, 1u);

  // Write spanning from middle of page 0 through middle of page 3:
  // - page 0: partial tail updated & marked dirty
  // - page 1 (raw): fully overwritten -> discarded (raw_count--)
  // - page 2 (meta): fully overwritten -> discarded (meta_count--)
  // - page 3: partial head updated & marked dirty
  const haddr_t w_addr = 128;
  const size_t w_size = (kPageSize - 128) + 2 * kPageSize + 64;
  const auto w_pat = MakePattern(w_size, 0x55);
  ASSERT_GE(
      H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, w_addr, w_size, w_pat.data()), 0);

  EXPECT_EQ(pb->raw_count, 2u);
  EXPECT_EQ(pb->meta_count, 0u);
  EXPECT_EQ(pb->LRU_list_len, 2u);

  // Verify partial updates inside PB pages 0 and 3.
  std::vector<uint8_t> p0_tail(kPageSize - 128, 0);
  ASSERT_GE(
      H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 128, p0_tail.size(), p0_tail.data()),
      0);
  EXPECT_EQ(std::memcmp(p0_tail.data(), w_pat.data(), p0_tail.size()), 0);

  std::vector<uint8_t> p3_head(64, 0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 3 * kPageSize, p3_head.size(),
                      p3_head.data()),
            0);
  EXPECT_EQ(
      std::memcmp(p3_head.data(), w_pat.data() + (w_size - 64), 64), 0);

  // Also test large raw write where uncached first/last/middle pages are absent
  // from PB (exercises false branches in lines 1076, 1096, 1112).
  const auto uncached_w = MakePattern(3 * kPageSize, 0x66);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 4 * kPageSize + 32,
                       2 * kPageSize + 64, uncached_w.data()),
            0);
}

TEST(H5PBTest, AddNewPageUpdateEntryAndRemoveEntry) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(6 * kPageSize, kPageSize);
  mock.FillBacking(0, 6 * kPageSize, 0x07);
  ASSERT_GE(H5PB_create(&mock.f_sh, 4 * kPageSize, 0, 0), 0);
  H5PB_t* pb = mock.f_sh.page_buf;

  // 1. Add page 0 to mf_slist_ptr twice (second call is duplicate no-op).
  ASSERT_GE(H5PB_add_new_page(&mock.f_sh, H5FD_MEM_SUPER, 0), 0);
  ASSERT_GE(H5PB_add_new_page(&mock.f_sh, H5FD_MEM_SUPER, 0), 0);
  EXPECT_EQ(H5SL_count(pb->mf_slist_ptr), 1u);

  // Add page 1 to mf_slist_ptr and leave it untouched so H5PB_dest exercises
  // cleanup of non-empty mf_slist_ptr entries.
  ASSERT_GE(H5PB_add_new_page(&mock.f_sh, H5FD_MEM_DRAW, kPageSize), 0);
  EXPECT_EQ(H5SL_count(pb->mf_slist_ptr), 2u);

  // Write to page 0 (metadata) -> consumes mf_slist_ptr entry, zeroing
  // surrounding bytes instead of reading from VFD, and increments hits[0].
  const int prev_reads = mock.read_calls;
  const auto meta_slice = MakePattern(16, 0x88);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_SUPER, 32, meta_slice.size(),
                       meta_slice.data()),
            0);
  EXPECT_EQ(mock.read_calls, prev_reads);
  EXPECT_EQ(pb->hits[0], 1u);
  EXPECT_EQ(H5SL_count(pb->mf_slist_ptr), 1u);

  // Verify prefix [0..32) and suffix [48..256) were zeroed.
  std::array<uint8_t, 256> check_slice{};
  check_slice.fill(0xFF);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 0, check_slice.size(),
                      check_slice.data()),
            0);
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(check_slice[i], 0u);
  }
  EXPECT_EQ(std::memcmp(check_slice.data() + 32, meta_slice.data(), 16), 0);
  for (size_t i = 48; i < check_slice.size(); ++i) {
    EXPECT_EQ(check_slice[i], 0u);
  }

  // Load pages 2 and 3 (metadata) so LRU has 3 entries: [3, 2, 0].
  std::array<uint8_t, 8> scratch{};
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 2 * kPageSize, scratch.size(),
                      scratch.data()),
            0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 3 * kPageSize, scratch.size(),
                      scratch.data()),
            0);
  EXPECT_EQ(pb->LRU_list_len, 3u);

  // 2. H5PB_update_entry on absent page is a no-op; on present page updates
  // bytes and moves to LRU head.
  const auto upd = MakePattern(8, 0xEE);
  EXPECT_GE(H5PB_update_entry(pb, 4 * kPageSize + 4, upd.size(), upd.data()),
            0);
  EXPECT_GE(H5PB_update_entry(pb, 2 * kPageSize + 10, upd.size(), upd.data()),
            0);
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 2 * kPageSize + 10,
                      scratch.size(), scratch.data()),
            0);
  EXPECT_EQ(std::vector<uint8_t>(scratch.begin(), scratch.end()), upd);

  // 3. H5PB_remove_entry: absent address is a no-op; remove middle (3 after
  // update moved 2 to head -> LRU [2, 3, 0]), head (2), then last entry (0) to
  // exercise all H5PB__REMOVE head/tail pointer branches.
  EXPECT_GE(H5PB_remove_entry(&mock.f_sh, 5 * kPageSize), 0);
  EXPECT_GE(H5PB_remove_entry(&mock.f_sh, 3 * kPageSize), 0);
  EXPECT_GE(H5PB_remove_entry(&mock.f_sh, 2 * kPageSize), 0);
  EXPECT_GE(H5PB_remove_entry(&mock.f_sh, 0), 0);
  EXPECT_EQ(pb->LRU_list_len, 0u);
  EXPECT_EQ(pb->meta_count, 0u);
}

TEST(H5PBTest, WriteWhenAddrBeyondEofSkipsVfdReadAndNewRawMfHit) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(6 * kPageSize, kPageSize);
  mock.eof = 2 * kPageSize;
  mock.eoa = 5 * kPageSize;
  ASSERT_GE(H5PB_create(&mock.f_sh, 3 * kPageSize, 0, 0), 0);
  H5PB_t* pb = mock.f_sh.page_buf;

  // 1. Write to page 3 (>= eof, < eoa): skips VFD read, zeros surrounding
  // bytes.
  const auto p3_pat = MakePattern(16, 0x3A);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 3 * kPageSize + 8,
                       p3_pat.size(), p3_pat.data()),
            0);
  EXPECT_EQ(mock.read_calls, 0);
  EXPECT_EQ(pb->misses[1], 0u);

  // 2. Add page 4 as new raw page in mf_slist_ptr and write to it -> increments
  // hits[1] directly without VFD read.
  ASSERT_GE(H5PB_add_new_page(&mock.f_sh, H5FD_MEM_DRAW, 4 * kPageSize), 0);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 4 * kPageSize + 4,
                       p3_pat.size(), p3_pat.data()),
            0);
  EXPECT_EQ(mock.read_calls, 0);
  EXPECT_EQ(pb->hits[1], 1u);
}

TEST(H5PBTest, MakeSpaceEvictionThresholdsAndBypass) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. 100% metadata reservation: full of metadata -> raw read/write bypass PB.
  {
    MockPbFile mock(4 * kPageSize, kPageSize);
    mock.FillBacking(0, 4 * kPageSize, 0x15);
    ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 100, 0), 0);

    std::array<uint8_t, 8> buf{};
    ASSERT_GE(
        H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 0, buf.size(), buf.data()), 0);
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, kPageSize, buf.size(),
                        buf.data()),
              0);
    EXPECT_EQ(mock.f_sh.page_buf->meta_count, 2u);

    // Raw read bypasses via H5FD_read when PB cannot evict metadata.
    const int reads_before = mock.read_calls;
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, buf.size(),
                        buf.data()),
              0);
    EXPECT_EQ(mock.read_calls, reads_before + 1);
    EXPECT_EQ(buf[0], mock.storage[2 * kPageSize]);

    // Raw write bypasses via H5FD_write when PB cannot evict metadata.
    const auto raw_w = MakePattern(8, 0x99);
    const int writes_before = mock.write_calls;
    ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, raw_w.size(),
                         raw_w.data()),
              0);
    EXPECT_EQ(mock.write_calls, writes_before + 1);

    // Error propagation when bypass H5FD_read / H5FD_write fails.
    ScopedErrorSilence silence;
    mock.fail_read = true;
    EXPECT_LT(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, buf.size(),
                        buf.data()),
              0);
    mock.fail_read = false;
    mock.fail_write = true;
    EXPECT_LT(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, raw_w.size(),
                         raw_w.data()),
              0);
    mock.fail_write = false;
  }

  // 2. 100% raw data reservation: full of raw data -> metadata read/write
  // bypass PB.
  {
    MockPbFile mock(4 * kPageSize, kPageSize);
    mock.FillBacking(0, 4 * kPageSize, 0x25);
    ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 0, 100), 0);

    std::array<uint8_t, 8> buf{};
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 0, buf.size(), buf.data()),
              0);
    ASSERT_GE(
        H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, kPageSize, buf.size(), buf.data()),
        0);
    EXPECT_EQ(mock.f_sh.page_buf->raw_count, 2u);

    const int reads_before = mock.read_calls;
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 2 * kPageSize, buf.size(),
                        buf.data()),
              0);
    EXPECT_EQ(mock.read_calls, reads_before + 1);

    const auto meta_w = MakePattern(8, 0xAB);
    const int writes_before = mock.write_calls;
    ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_SUPER, 2 * kPageSize,
                         meta_w.size(), meta_w.data()),
              0);
    EXPECT_EQ(mock.write_calls, writes_before + 1);
  }

  // 3. 50%/50% split on a 2-page PB: exercise LRU prev walk skipping protected
  // page types and flushing dirty evicted pages.
  {
    MockPbFile mock(6 * kPageSize, kPageSize);
    mock.FillBacking(0, 6 * kPageSize, 0x35);
    ASSERT_GE(H5PB_create(&mock.f_sh, 2 * kPageSize, 50, 50), 0);
    H5PB_t* pb = mock.f_sh.page_buf;

    // Load page 0 (H5FD_MEM_SUPER == H5F_MEM_PAGE_META, dirty) then page 1
    // (H5FD_MEM_DRAW, dirty). LRU is [1 (head), 0 (tail)].
    const auto pat0 = MakePattern(8, 0x71);
    const auto pat1 = MakePattern(8, 0x72);
    ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_SUPER, 0, pat0.size(),
                         pat0.data()),
              0);
    ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, kPageSize, pat1.size(),
                         pat1.data()),
              0);

    // Insert page 2 (DRAW): min_meta_count == meta_count (1 == 1), so
    // H5PB__make_space skips tail page 0 (H5F_MEM_PAGE_META) via
    // `page_entry = page_entry->prev` and evicts dirty page 1 (DRAW)!
    std::array<uint8_t, 8> tmp{};
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, tmp.size(),
                        tmp.data()),
              0);
    EXPECT_EQ(pb->evictions[1], 1u);
    EXPECT_EQ(std::memcmp(mock.storage.data() + kPageSize, pat1.data(), 8), 0);

    // Now LRU is [2 (DRAW, head), 0 (SUPER, tail)]. Insert page 3 (SUPER):
    // min_raw_count == raw_count (1 == 1), tail is SUPER so evicts dirty
    // page 0 (SUPER).
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 3 * kPageSize, tmp.size(),
                        tmp.data()),
              0);
    EXPECT_EQ(pb->evictions[0], 1u);
    EXPECT_EQ(std::memcmp(mock.storage.data(), pat0.data(), 8), 0);

    // Make LRU [3 (SUPER, head), 2 (DRAW, tail)]. Insert page 4 (SUPER):
    // min_raw_count == raw_count (1 == 1), tail is DRAW -> walks `prev` to
    // page 3 (SUPER) and evicts clean page 3!
    ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 4 * kPageSize, tmp.size(),
                        tmp.data()),
              0);
    EXPECT_EQ(pb->evictions[0], 2u);
  }
}

TEST(H5PBTest, FlushAndEoaTruncationAndErrorPropagation) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockPbFile mock(4 * kPageSize, kPageSize);
  mock.FillBacking(0, 4 * kPageSize, 0x45);
  // Set EOA inside page 2 (2 * kPageSize + 64) to exercise partial-page read
  // and write clamping at EOA.
  mock.eoa = 2 * kPageSize + 64;
  mock.eof = 2 * kPageSize + 64;
  ASSERT_GE(H5PB_create(&mock.f_sh, 4 * kPageSize, 0, 0), 0);

  // 1. Read clean page 0, read partial page 1 truncated at EOA (temp EOA at
  // kPageSize + 96, then restore EOA to 2 * kPageSize + 64), and write dirty
  // page 2 (partial page up to EOA).
  std::array<uint8_t, 8> clean_probe{};
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_SUPER, 0, clean_probe.size(),
                      clean_probe.data()),
            0);

  mock.eoa = kPageSize + 96;
  mock.eof = kPageSize + 96;
  std::array<uint8_t, 16> eoa_read_probe{};
  ASSERT_GE(H5PB_read(&mock.f_sh, H5FD_MEM_DRAW, kPageSize + 16,
                      eoa_read_probe.size(), eoa_read_probe.data()),
            0);
  EXPECT_EQ(mock.last_read_addr, kPageSize);
  EXPECT_EQ(mock.last_read_size, 96u);
  EXPECT_EQ(std::memcmp(eoa_read_probe.data(),
                        mock.storage.data() + kPageSize + 16, 16),
            0);

  mock.eoa = 2 * kPageSize + 64;
  mock.eof = 2 * kPageSize + 64;
  const auto pat2 = MakePattern(16, 0xD4);
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, pat2.size(),
                       pat2.data()),
            0);

  // 2. Read-only intent skips flushing even when dirty entries exist.
  mock.f_sh.flags = H5F_ACC_RDONLY;
  const int writes_ro = mock.write_calls;
  ASSERT_GE(H5PB_flush(&mock.f_sh), 0);
  EXPECT_EQ(mock.write_calls, writes_ro);
  mock.f_sh.flags = H5F_ACC_RDWR;

  // 3. Flush error propagation when VFD write fails or get_eoa fails, both via
  // H5PB_flush and via H5PB_dest.
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(H5PB_flush(&mock.f_sh), 0);
    EXPECT_LT(H5PB_dest(&mock.f_sh), 0);
    mock.fail_write = false;

    mock.fail_get_eoa = true;
    EXPECT_LT(H5PB_flush(&mock.f_sh), 0);
    mock.fail_get_eoa = false;
  }

  // 4. Normal flush writes dirty page 2 clamped to 64 bytes (EOA - page_addr).
  ASSERT_GE(H5PB_flush(&mock.f_sh), 0);
  EXPECT_EQ(mock.last_write_addr, 2 * kPageSize);
  EXPECT_EQ(mock.last_write_size, 64u);
  EXPECT_EQ(std::memcmp(mock.storage.data() + 2 * kPageSize, pat2.data(), 16),
            0);

  // 5. Dirty page whose address exceeds shrunk EOA is discarded without VFD
  // write.
  ASSERT_GE(H5PB_write(&mock.f_sh, H5FD_MEM_DRAW, 2 * kPageSize, pat2.size(),
                       pat2.data()),
            0);
  mock.eoa = kPageSize;
  const int writes_before_shrink = mock.write_calls;
  ASSERT_GE(H5PB_flush(&mock.f_sh), 0);
  EXPECT_EQ(mock.write_calls, writes_before_shrink);
}

}  // namespace
}  // namespace hdf5
