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
// Tests internal metadata accumulator reads, writes, buffer resizing/sliding,
// partial/full free invalidation, dirty flushes, resets, and Core VFD integration
// (third_party/hdf5/src/H5Faccum.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Faccum.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug/stats fields from internal
// cache structs pulled indirectly by private headers. Define NDEBUG around the
// internal prelude and private headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5FACCUM_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_c_try
extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#ifdef H5FACCUM_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FACCUM_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Mirror internal H5Faccum.c thresholds for test sizing calculations.
constexpr size_t kAccumThrottle = 8;
constexpr size_t kAccumThreshold = 2048;
constexpr size_t kAccumMaxSize = 1024 * 1024;
constexpr size_t kAccumHalfMaxSize = kAccumMaxSize / 2;

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

// Minimal in-memory mock H5FD_t + H5F_shared_t harness targeting H5Faccum.c
// without disk I/O or touching real superblocks.
class MockAccumFile {
 public:
  explicit MockAccumFile(hsize_t backing_size = 4 * 1024 * 1024)
      : storage(backing_size, 0), eoa(backing_size) {
    fd.pub.driver_id = H5I_INVALID_HID;
    fd.pub.cls = &kDriverClass;
    fd.pub.access_flags = H5F_ACC_RDWR;
    fd.pub.maxaddr = backing_size;
    fd.owner = this;

    f_sh.lf = &fd.pub;
    f_sh.flags = H5F_ACC_RDWR;
    f_sh.feature_flags = H5FD_FEAT_ACCUMULATE_METADATA;
    f_sh.accum.loc = HADDR_UNDEF;
  }

  ~MockAccumFile() {
    ScopedErrorSilence silence;
    H5F__accum_reset(&f_sh, /*flush=*/false, /*force=*/true);
  }

  MockAccumFile(const MockAccumFile&) = delete;
  MockAccumFile& operator=(const MockAccumFile&) = delete;

  void FillBacking(haddr_t addr, size_t len, uint8_t start_val = 1) {
    for (size_t i = 0; i < len; ++i) {
      storage[addr + i] = static_cast<uint8_t>((start_val + i) & 0xFF);
    }
  }

  struct MockFd {
    H5FD_t pub{};
    MockAccumFile* owner = nullptr;
  };

  static haddr_t GetEoa(const H5FD_t* file, H5FD_mem_t /*type*/) {
    const auto* mock = reinterpret_cast<const MockFd*>(file);
    return mock->owner->eoa;
  }

  static herr_t Read(H5FD_t* file, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                     haddr_t addr, size_t size, void* buf) {
    auto* mock = reinterpret_cast<MockFd*>(file);
    auto* self = mock->owner;
    ++self->read_calls;
    if (self->fail_read ||
        (self->fail_read_after >= 0 && self->read_calls > self->fail_read_after)) {
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
    if (self->fail_write ||
        (self->fail_write_after >= 0 &&
         self->write_calls > self->fail_write_after)) {
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
    cls.value = static_cast<H5FD_class_value_t>(261);
    cls.name = "mock_accum_vfd";
    cls.maxaddr = HADDR_MAX - 1;
    cls.fc_degree = H5F_CLOSE_WEAK;
    cls.get_eoa = &MockAccumFile::GetEoa;
    cls.read = &MockAccumFile::Read;
    cls.write = &MockAccumFile::Write;
    return cls;
  }();

  std::vector<uint8_t> storage;
  haddr_t eoa = 0;
  bool fail_read = false;
  bool fail_write = false;
  int fail_read_after = -1;
  int fail_write_after = -1;
  int read_calls = 0;
  int write_calls = 0;
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

TEST(H5FaccumTest, BypassAccumulatorWhenFeatureDisabledOrDraw) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(65536);

  const auto raw = MakePattern(16, 0x30);
  // 1. H5FD_MEM_DRAW bypasses the accumulator even when feature_flags enables it.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_DRAW, 100, raw.size(),
                             raw.data()),
            0);
  EXPECT_EQ(mock.write_calls, 1);
  EXPECT_EQ(mock.f_sh.accum.size, 0u);
  EXPECT_EQ(mock.f_sh.accum.loc, HADDR_UNDEF);

  std::vector<uint8_t> read_back(16, 0);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_DRAW, 100, read_back.size(),
                            read_back.data()),
            0);
  EXPECT_EQ(mock.read_calls, 1);
  EXPECT_EQ(read_back, raw);

  // 2. Disabling H5FD_FEAT_ACCUMULATE_METADATA sends metadata I/O directly to VFD.
  mock.f_sh.feature_flags = 0;
  const auto meta = MakePattern(16, 0x50);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 200, meta.size(),
                             meta.data()),
            0);
  EXPECT_EQ(mock.write_calls, 2);
  EXPECT_EQ(mock.f_sh.accum.size, 0u);

  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_SUPER, 200, read_back.size(),
                            read_back.data()),
            0);
  EXPECT_EQ(mock.read_calls, 2);
  EXPECT_EQ(read_back, meta);

  // 3. Flush and reset with feature disabled are harmless no-ops.
  EXPECT_GE(H5F__accum_flush(&mock.f_sh), 0);
  EXPECT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/true, /*force=*/false), 0);

  // 4. VFD read/write error propagation when bypassed.
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 200, meta.size(),
                               meta.data()),
              0);
    mock.fail_write = false;
    mock.fail_read = true;
    EXPECT_LT(H5F__accum_read(&mock.f_sh, H5FD_MEM_SUPER, 200, read_back.size(),
                              read_back.data()),
              0);
  }
}

TEST(H5FaccumTest, ReadOverlapAdjoiningAndCacheHits) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(65536);
  mock.FillBacking(0, 2048, 0x10);

  // 1. Read while accumulator is empty reads directly from VFD without caching.
  std::vector<uint8_t> buf(32, 0);
  ASSERT_GE(
      H5F__accum_read(&mock.f_sh, H5FD_MEM_SUPER, 100, buf.size(), buf.data()),
      0);
  EXPECT_EQ(mock.read_calls, 1);
  EXPECT_EQ(buf, MakePattern(32, static_cast<uint8_t>(0x10 + 100)));

  // Failure when empty.
  {
    ScopedErrorSilence silence;
    mock.fail_read = true;
    EXPECT_LT(H5F__accum_read(&mock.f_sh, H5FD_MEM_SUPER, 100, 16, buf.data()),
              0);
    mock.fail_read = false;
  }

  // 2. Seed accumulator with dirty data at [1024, 1088).
  const auto seeded = MakePattern(64, 0xA0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1024, seeded.size(),
                             seeded.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 1024u);
  EXPECT_EQ(mock.f_sh.accum.size, 64u);
  EXPECT_TRUE(mock.f_sh.accum.dirty);

  // 3. Disjoint small read goes straight to VFD.
  const int reads_before_disjoint = mock.read_calls;
  std::vector<uint8_t> disjoint(16, 0);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 2000, disjoint.size(),
                            disjoint.data()),
            0);
  EXPECT_EQ(mock.read_calls, reads_before_disjoint + 1);
  EXPECT_EQ(disjoint, MakePattern(16, static_cast<uint8_t>(0x10 + 2000)));

  // 4. Strictly contained read hits accumulator with zero VFD calls.
  const int reads_before_hit = mock.read_calls;
  std::vector<uint8_t> hit(16, 0);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 1040, 16, hit.data()),
            0);
  EXPECT_EQ(mock.read_calls, reads_before_hit);
  EXPECT_EQ(hit, MakePattern(16, static_cast<uint8_t>(0xA0 + 16)));

  // 5. Read adjoining before [960, 1024): triggers alloc_size realloc if needed,
  //    amount_before read, and shifts dirty_off while remaining dirty.
  std::vector<uint8_t> before(64, 0);
  ASSERT_GE(
      H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 960, before.size(),
                      before.data()),
      0);
  EXPECT_EQ(mock.f_sh.accum.loc, 960u);
  EXPECT_EQ(mock.f_sh.accum.size, 128u);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 64u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 64u);
  EXPECT_EQ(before, MakePattern(64, static_cast<uint8_t>(0x10 + 960)));

  // 6. Read overlapping both ends [900, 1300) grows alloc_size and reads both
  //    prefix [900, 960) and suffix [1088, 1300) while preserving dirty slice.
  std::vector<uint8_t> both(400, 0);
  ASSERT_GE(
      H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 900, both.size(), both.data()),
      0);
  EXPECT_EQ(mock.f_sh.accum.loc, 900u);
  EXPECT_EQ(mock.f_sh.accum.size, 400u);
  EXPECT_EQ(mock.f_sh.accum.alloc_size, 512u);
  // Dirty slice [1024, 1088) inside `both` starts at offset 124.
  for (size_t i = 0; i < 64; ++i) {
    EXPECT_EQ(both[124 + i], seeded[i]);
  }
  EXPECT_EQ(both[0], static_cast<uint8_t>((0x10 + 900) & 0xFF));
  EXPECT_EQ(both[399], static_cast<uint8_t>((0x10 + 1299) & 0xFF));

  // 7. Read adjoining after [1300, 1332) skips amount_before and extends size.
  std::vector<uint8_t> after(32, 0);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 1300, after.size(),
                            after.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 900u);
  EXPECT_EQ(mock.f_sh.accum.size, 432u);
  EXPECT_EQ(after, MakePattern(32, static_cast<uint8_t>(0x10 + 1300)));

  // 8. Error paths during before-read and after-read.
  {
    ScopedErrorSilence silence;
    mock.fail_read = true;
    EXPECT_LT(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 880, 20, hit.data()),
              0);
    mock.fail_read = false;

    // Force first read (prefix) to succeed and second read (suffix) to fail.
    const haddr_t cur_loc = mock.f_sh.accum.loc;
    const size_t cur_size = mock.f_sh.accum.size;
    mock.fail_read_after = mock.read_calls + 1;
    std::vector<uint8_t> fail_both(cur_size + 40, 0);
    EXPECT_LT(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, cur_loc - 16,
                              fail_both.size(), fail_both.data()),
              0);
    mock.fail_read_after = -1;
  }
}

TEST(H5FaccumTest, LargeReadsOverlayDirtyAccumulatorRegions) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(2 * 1024 * 1024);
  mock.FillBacking(0, mock.storage.size(), 0x05);

  std::vector<uint8_t> big(kAccumMaxSize, 0);

  // 1. Error path when VFD read fails on large read.
  {
    ScopedErrorSilence silence;
    mock.fail_read = true;
    EXPECT_LT(H5F__accum_read(&mock.f_sh, H5FD_MEM_BTREE, 0, big.size(),
                              big.data()),
              0);
    mock.fail_read = false;
  }

  // 2. Clean large read skips dirty overlay check.
  ASSERT_GE(
      H5F__accum_read(&mock.f_sh, H5FD_MEM_BTREE, 0, big.size(), big.data()),
      0);
  EXPECT_EQ(big[0], 0x05);

  // 3. Read starting at addr == 0 and ending inside dirty region [1 MiB - 128,
  //    1 MiB + 128) hits H5Faccum.c line 227-228. Keep addr == 0 so line 228's
  //    (addr + size) - buf_off calculation equals the exact tail overlap.
  const std::vector<uint8_t> dirty_edge(256, 0xEE);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kAccumMaxSize - 128,
                             dirty_edge.size(), dirty_edge.data()),
            0);
  ASSERT_GE(
      H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 0, big.size(), big.data()), 0);
  EXPECT_NE(big[kAccumMaxSize - 129], 0xEE);
  for (size_t i = kAccumMaxSize - 128; i < kAccumMaxSize; ++i) {
    EXPECT_EQ(big[i], 0xEE);
  }

  // 4. Large read covering entire dirty region (addr <= dirty_loc and
  //    addr + size >= dirty_loc + dirty_len).
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  const std::vector<uint8_t> dirty_mid(256, 0xCC);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0x2000,
                             dirty_mid.size(), dirty_mid.data()),
            0);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 0x1000, big.size(),
                            big.data()),
            0);
  EXPECT_NE(big[0x0FFF], 0xCC);
  for (size_t i = 0; i < 256; ++i) {
    EXPECT_EQ(big[0x1000 + i], 0xCC);
  }
  EXPECT_NE(big[0x1100], 0xCC);

  // 5. Large read starting in the middle of the dirty region (addr > dirty_loc).
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 0x2080, big.size(),
                            big.data()),
            0);
  for (size_t i = 0; i < 128; ++i) {
    EXPECT_EQ(big[i], 0xCC);
  }
  EXPECT_NE(big[128], 0xCC);

  // 6. Clean accumulator overlapping large read does not overlay.
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  mock.FillBacking(0x2000, 256, 0x77);
  ASSERT_GE(H5F__accum_read(&mock.f_sh, H5FD_MEM_OHDR, 0x2000, big.size(),
                            big.data()),
            0);
  EXPECT_EQ(big[0], 0x77);
}

TEST(H5FaccumTest, SmallWriteAdjoiningOverlappingAndDisjoint) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(2 * 1024 * 1024);

  // 1. Initial write seeds accumulator cleanly with size 64 at 1000.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1000, 64,
                             MakePattern(64, 0x10).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 1000u);
  EXPECT_EQ(mock.f_sh.accum.size, 64u);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 64u);

  // Flush so we can exercise clean->dirty transitions on prepend/append/overlap.
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);

  // 2. Clean prepend adjoining [984, 1000).
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 984, 16,
                             MakePattern(16, 0x20).data()),
            0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 16u);

  // 3. Clean append after flushing [1064, 1080).
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1064, 16,
                             MakePattern(16, 0x30).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 80u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 16u);

  // Extend [984, 1080) to [984, 1100) cleanly so all contained write cases stay
  // strictly inside [984, 1100).
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1080, 20,
                             MakePattern(20, 0x38).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1050, 16,
                             MakePattern(16, 0x3C).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 66u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 16u);

  // 4. Write strictly contained within accumulator covering all dirty subcases:
  //    - dirty, starts > dirty_off, ends > dirty_end (line 503)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1060, 16,
                             MakePattern(16, 0x40).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 66u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 26u);

  //    - dirty, starts > dirty_off, ends <= dirty_end (noop on dirty bounds)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1062, 8,
                             MakePattern(8, 0x44).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 66u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 26u);

  //    - dirty, starts <= dirty_off, ends <= dirty_end
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1040, 20,
                             MakePattern(20, 0x48).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 56u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 36u);

  //    - dirty, starts <= dirty_off, ends > dirty_end (line 496)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1030, 60,
                             MakePattern(60, 0x4C).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 46u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 60u);

  //    - clean contained write
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1000, 10,
                             MakePattern(10, 0x50).data()),
            0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 16u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 10u);

  // 5. Overlap beginning:
  //    - dirty with size <= curr_dirty_end
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 980, 12,
                             MakePattern(12, 0x60).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 30u);

  //    - dirty with size > curr_dirty_end (overwrites beyond dirty end)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 960, 80,
                             MakePattern(80, 0x64).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 80u);

  //    - clean overlap beginning
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 950, 20,
                             MakePattern(20, 0x68).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 20u);

  // 6. Overlap ending:
  //    - dirty with new dirty_off <= old dirty_off
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 950, 160,
                             MakePattern(160, 0x70).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 160u);

  //    - dirty with new dirty_off > old dirty_off
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1090, 30,
                             MakePattern(30, 0x74).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 170u);

  //    - clean overlap ending
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1100, 40,
                             MakePattern(40, 0x78).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 150u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 40u);

  // 7. Overlap both ends: within alloc_size and requiring realloc.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 940, 220,
                             MakePattern(220, 0x80).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 940u);
  EXPECT_EQ(mock.f_sh.accum.size, 220u);

  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 900, 600,
                             MakePattern(600, 0x84).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 900u);
  EXPECT_EQ(mock.f_sh.accum.size, 600u);
  EXPECT_GE(mock.f_sh.accum.alloc_size, 1024u);

  // 8. Disjoint write flushes dirty accumulator, grows buffer, then shrinks via
  //    throttle (size < alloc_size / 8 && alloc_size > 2048).
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 4000, 8192,
                             MakePattern(8192, 0x90).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.alloc_size, 8192u);
  static_assert(kAccumThreshold == 2048 && kAccumThrottle == 8);
  // Write 64 bytes disjoint (< 8192 / 8 = 1024, alloc_size > 2048) -> shrink!
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 20000, 64,
                             MakePattern(64, 0x94).data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.alloc_size, 8192u / kAccumThrottle);

  // Clean disjoint write skips flush.
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  const int writes_before = mock.write_calls;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 30000, 32,
                             MakePattern(32, 0x98).data()),
            0);
  EXPECT_EQ(mock.write_calls, writes_before);

  // Error on disjoint flush when VFD fails write.
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 40000, 32,
                               MakePattern(32, 0x9C).data()),
              0);
    mock.fail_write = false;
  }
}

TEST(H5FaccumTest, AdjustExceedsMaxSizePrependAppendSlideAndErrors) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(4 * 1024 * 1024);

  constexpr size_t kKiB = 1024;

  // 1. Prepend with size > H5F_ACCUM_MAX_SIZE / 2 (600 KiB > 512 KiB) drops
  //    remnant completely (remnant_size = 0) and flushes dirty data first.
  const std::vector<uint8_t> chunk_600k(600 * kKiB, 0x11);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1024 * kKiB,
                             chunk_600k.size(), chunk_600k.data()),
            0);
  const std::vector<uint8_t> chunk_550k(550 * kKiB, 0x22);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1024 * kKiB - 550 * kKiB,
                             chunk_550k.size(), chunk_550k.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.size, 550 * kKiB);
  EXPECT_EQ(mock.write_calls, 1);

  // 2. Prepend <= 512 KiB where dirty data lies in truncated tail -> flushes.
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  const std::vector<uint8_t> chunk_768k(768 * kKiB, 0x33);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * kKiB,
                             chunk_768k.size(), chunk_768k.data()),
            0);
  const std::vector<uint8_t> chunk_300k(300 * kKiB, 0x44);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 212 * kKiB,
                             chunk_300k.size(), chunk_300k.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.size, 256 * kKiB + 300 * kKiB);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 300 * kKiB);

  // 3. Prepend <= 512 KiB where dirty region lies ONLY in kept prefix -> no flush!
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * kKiB,
                             chunk_768k.size(), chunk_768k.data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  // Dirty only first 1 KiB of [512 KiB, 1280 KiB). Prepend 300 KiB keeps 256 KiB.
  const std::vector<uint8_t> chunk_1k(1 * kKiB, 0x55);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * kKiB,
                             chunk_1k.size(), chunk_1k.data()),
            0);
  const int writes_before_keep = mock.write_calls;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 212 * kKiB,
                             chunk_300k.size(), chunk_300k.data()),
            0);
  EXPECT_EQ(mock.write_calls, writes_before_keep);

  // 4. Append slide branch A: shrink_size = dirty_off / 2 (when slack >= 2*size).
  //    size + accum->size must exceed 1 MiB (alloc_size) to enter adjust!
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  const std::vector<uint8_t> chunk_960k(960 * kKiB, 0x66);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, chunk_960k.size(),
                             chunk_960k.data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  // Dirty [512 KiB, 576 KiB) and append 100 KiB -> 960+100 = 1060 KiB > 1 MiB,
  // adjust_size = 164 <= 1024, slack = 1024 - (512 + 164) = 348 >= 200.
  const std::vector<uint8_t> chunk_64k(64 * kKiB, 0x67);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * kKiB,
                             chunk_64k.size(), chunk_64k.data()),
            0);
  const std::vector<uint8_t> chunk_100k(100 * kKiB, 0x68);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 960 * kKiB,
                             chunk_100k.size(), chunk_100k.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 256 * kKiB);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 256 * kKiB);

  // 5. Append slide branch B: shrink_size = dirty_off (slack < 2*size) AND
  //    new_size > alloc_size triggering H5FL_BLK_REALLOC inside H5F__accum_adjust.
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  const std::vector<uint8_t> chunk_900k(900 * kKiB, 0x71);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, chunk_900k.size(),
                             chunk_900k.data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 100 * kKiB,
                             chunk_100k.size(), chunk_100k.data()),
            0);
  // Append 300 KiB: remnant 800 + 300 = 1100 KiB > alloc_size (1 MiB) -> realloc!
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 900 * kKiB,
                             chunk_300k.size(), chunk_300k.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, 100 * kKiB);
  EXPECT_GE(mock.f_sh.accum.alloc_size, 1100 * kKiB);

  // 6. Append overflow when adjust_size > H5F_ACCUM_MAX_SIZE -> shrinks 512 KiB
  //    and flushes overlapping dirty head [0, 768 KiB).
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, chunk_768k.size(),
                             chunk_768k.data()),
            0);
  const int writes_before_flush = mock.write_calls;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 768 * kKiB,
                             chunk_300k.size(), chunk_300k.data()),
            0);
  EXPECT_EQ(mock.write_calls, writes_before_flush + 1);
  EXPECT_EQ(mock.f_sh.accum.loc, 512 * kKiB);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 256 * kKiB);

  // 7. VFD write error paths in adjust (via prepend, overlap-prepend, append,
  //    and overlap-append).
  {
    ScopedErrorSilence silence;
    // Inject write error when shrinking on prepend.
    ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
    ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * kKiB,
                               chunk_768k.size(), chunk_768k.data()),
              0);
    mock.fail_write = true;
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 212 * kKiB,
                               chunk_300k.size(), chunk_300k.data()),
              0);
    // For overlap-beginning, add_size must push accum->size + add_size > 1 MiB.
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 200 * kKiB,
                               chunk_550k.size(), chunk_550k.data()),
              0);
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1280 * kKiB,
                               chunk_300k.size(), chunk_300k.data()),
              0);
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1200 * kKiB,
                               chunk_550k.size(), chunk_550k.data()),
              0);
    mock.fail_write = false;
  }
}

TEST(H5FaccumTest, LargeWriteTrimsAccumulatorAndHandlesSwmrReset) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(3 * 1024 * 1024);
  const std::vector<uint8_t> big(kAccumMaxSize, 0xAB);

  // 1. Large write error path (VFD failure).
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, big.size(),
                               big.data()),
              0);
    mock.fail_write = false;
  }

  // 2. Large write starting <= loc and ending within accumulator:
  //    a) dirty completely overwritten (dirty_end <= addr + size)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 1024 * 1024, 1024,
                             MakePattern(1024, 0x10).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 512 * 1024, 256,
                             MakePattern(256, 0x20).data()),
            0);  // wait: reset to make a contiguous 2048-byte accum
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);

  constexpr haddr_t kBase = 1024 * 1024;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 2048,
                             MakePattern(2048, 0x10).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  // Dirty [kBase, kBase + 256); large write covers [0, kBase + 512) -> trims head.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 256,
                             MakePattern(256, 0x20).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 512 - kAccumMaxSize,
                             big.size(), big.data()),
            0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.loc, kBase + 512);
  EXPECT_EQ(mock.f_sh.accum.size, 1536u);

  //    b) dirty strictly after large write -> H5Faccum.c:764 sets dirty_off =
  //       overlap_size.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 1536, 256,
                             MakePattern(256, 0x30).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 768 - kAccumMaxSize,
                             big.size(), big.data()),
            0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.loc, kBase + 768);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 256u);

  //    c) dirty partially overlapped from the front (reset cleanly first).
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 2048,
                             MakePattern(2048, 0x32).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 256, 256,
                             MakePattern(256, 0x34).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 384 - kAccumMaxSize,
                             big.size(), big.data()),
            0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 128u);

  // 3. Large write covering entire accumulator resets without flushing.
  const int writes_before_full = mock.write_calls;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, big.size(),
                             big.data()),
            0);
  EXPECT_EQ(mock.write_calls, writes_before_full + 1);
  EXPECT_EQ(mock.f_sh.accum.loc, HADDR_UNDEF);
  EXPECT_EQ(mock.f_sh.accum.size, 0u);

  // 4. Large write starting after loc and ending past end:
  //    a) dirty completely overwritten (dirty_start >= addr)
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 2048,
                             MakePattern(2048, 0x40).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 1024, 512,
                             MakePattern(512, 0x41).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 1024, big.size(),
                             big.data()),
            0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.size, 1024u);

  //    b) dirty strictly before write -> untouched; then partially overlapped tail
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 256,
                             MakePattern(256, 0x42).data()),
            0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 512, big.size(),
                             big.data()),
            0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 256u);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase + 128, big.size(),
                             big.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 128u);

  // 5. SWMR write intent flushes & resets before large write; propagates flush err.
  mock.f_sh.flags |= H5F_ACC_SWMR_WRITE;
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, big.size(),
                             big.data()),
            0);
  EXPECT_EQ(mock.f_sh.accum.loc, HADDR_UNDEF);
  EXPECT_EQ(mock.f_sh.accum.size, 0u);

  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, kBase, 64,
                             MakePattern(64, 0x50).data()),
            0);
  {
    ScopedErrorSilence silence;
    mock.fail_write = true;
    EXPECT_LT(H5F__accum_write(&mock.f_sh, H5FD_MEM_OHDR, 0, big.size(),
                               big.data()),
              0);
    mock.fail_write = false;
  }
}

TEST(H5FaccumTest, FreeInvalidationAndResetForcePaths) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  MockAccumFile mock(65536);

  // 1. Free when empty is a no-op.
  EXPECT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 0, 100), 0);

  // 2. Complete overlap resets accumulator.
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 1000, 500,
                             MakePattern(500, 0x10).data()),
            0);
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 900, 700), 0);
  EXPECT_EQ(mock.f_sh.accum.loc, HADDR_UNDEF);
  EXPECT_EQ(mock.f_sh.accum.size, 0u);
  EXPECT_FALSE(mock.f_sh.accum.dirty);

  // 3. Free overlapping beginning:
  //    - entirely before dirty region
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 1000, 400,
                             MakePattern(400, 0x20).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 1200, 100,
                             MakePattern(100, 0x21).data()),
            0);
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 950, 150), 0);
  EXPECT_EQ(mock.f_sh.accum.loc, 1100u);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 100u);

  //    - ends within dirty region
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 1100, 140), 0);
  EXPECT_EQ(mock.f_sh.accum.loc, 1240u);
  EXPECT_EQ(mock.f_sh.accum.dirty_off, 0u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 60u);

  //    - encompasses dirty region while leaving clean tail
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 1200, 120), 0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.loc, 1320u);

  // 4. Free starting after accum->loc:
  //    a) addr < dirty_start, tail <= dirty_start -> flushes whole dirty region
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2000, 500,
                             MakePattern(500, 0x30).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2300, 100,
                             MakePattern(100, 0x31).data()),
            0);
  const int writes_before = mock.write_calls;
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 2100, 50), 0);
  EXPECT_EQ(mock.write_calls, writes_before + 1);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.size, 100u);

  //    b) addr < dirty_start, tail inside dirty -> flushes unfreed tail
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2000, 500,
                             MakePattern(500, 0x32).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2200, 200,
                             MakePattern(200, 0x33).data()),
            0);
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 2100, 150), 0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.size, 100u);

  //    c) addr < dirty_start, tail >= dirty_end -> skips flush
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2000, 500,
                             MakePattern(500, 0x34).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2200, 100,
                             MakePattern(100, 0x35).data()),
            0);
  const int writes_no_tail = mock.write_calls;
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 2100, 300), 0);
  EXPECT_EQ(mock.write_calls, writes_no_tail);
  EXPECT_FALSE(mock.f_sh.accum.dirty);

  //    d) addr >= dirty_start, tail < dirty_end (flushes tail) & addr == dirty_start
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 2000, 500,
                             MakePattern(500, 0x36).data()),
            0);
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 2000 + 200, 100), 0);
  EXPECT_TRUE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 200u);

  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 2000, 100), 0);
  EXPECT_EQ(mock.f_sh.accum.loc, 2100u);
  EXPECT_EQ(mock.f_sh.accum.dirty_len, 100u);

  // Reset to test addr == dirty_start inside [addr > accum->loc] case.
  ASSERT_GE(H5F__accum_reset(&mock.f_sh, /*flush=*/false, /*force=*/true), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 3000, 400,
                             MakePattern(400, 0x37).data()),
            0);
  ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
  ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 3100, 200,
                             MakePattern(200, 0x38).data()),
            0);
  ASSERT_GE(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 3100, 250), 0);
  EXPECT_FALSE(mock.f_sh.accum.dirty);
  EXPECT_EQ(mock.f_sh.accum.size, 100u);

  // 5. Write error paths inside H5F__accum_free, H5F__accum_flush, and
  //    H5F__accum_reset(..., flush=true, force=false/true).
  {
    ScopedErrorSilence silence;
    ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 4000, 400,
                               MakePattern(400, 0x40).data()),
              0);
    ASSERT_GE(H5F__accum_flush(&mock.f_sh), 0);
    ASSERT_GE(H5F__accum_write(&mock.f_sh, H5FD_MEM_SUPER, 4200, 150,
                               MakePattern(150, 0x41).data()),
              0);
    mock.fail_write = true;
    // Fail in H5F__accum_free branch 1 (tail <= dirty_start)
    EXPECT_LT(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 4100, 50), 0);
    // Fail in H5F__accum_free branch 2 (addr < dirty_start < tail < dirty_end)
    EXPECT_LT(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 4100, 150), 0);
    // Fail in H5F__accum_free branch 3 (addr >= dirty_start, tail < dirty_end)
    EXPECT_LT(H5F__accum_free(&mock.f_sh, H5FD_MEM_SUPER, 4220, 50), 0);
    // Fail in H5F__accum_flush & H5F__accum_reset(force=false) leaves buf alive.
    EXPECT_LT(H5F__accum_flush(&mock.f_sh), 0);
    EXPECT_LT(H5F__accum_reset(&mock.f_sh, /*flush=*/true, /*force=*/false), 0);
    EXPECT_NE(mock.f_sh.accum.buf, nullptr);
    // Fail with force=true still returns error but frees the buffer.
    EXPECT_LT(H5F__accum_reset(&mock.f_sh, /*flush=*/true, /*force=*/true), 0);
    EXPECT_EQ(mock.f_sh.accum.buf, nullptr);
    EXPECT_EQ(mock.f_sh.accum.size, 0u);
    mock.fail_write = false;
  }
}

TEST(H5FaccumTest, CoreVfdEndToEndMetadataRoundTrip) {
  // Verify Core VFD file image retains metadata flushed through the accumulator.
  std::vector<uint8_t> image;
  {
    ScopedFile file = CreateInMemoryFile("accum_core_roundtrip.h5");
    ASSERT_TRUE(file.valid());

    for (int i = 0; i < 8; ++i) {
      const std::string name = "g" + std::to_string(i);
      ScopedGroup grp(H5Gcreate2(file.get(), name.c_str(), H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }
    constexpr auto kData = std::to_array<int32_t>({10, 20, 30, 40});
    constexpr hsize_t kDim = kData.size();
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/g0/vals", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kData.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopened_image.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset dset(H5Dopen2(reopened.get(), "/g0/vals", H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 4> actual{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    actual.data()),
            0);
  EXPECT_THAT(actual, ElementsAre(10, 20, 30, 40));
}

}  // namespace
}  // namespace hdf5
