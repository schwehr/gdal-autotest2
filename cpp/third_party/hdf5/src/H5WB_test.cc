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
// Tests internal wrapped buffer primitives (H5WB_wrap, H5WB_actual,
// H5WB_actual_clear, H5WB_unwrap) and public callers that exercise H5WB_t
// (third_party/hdf5/src/H5WB.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5WB.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5WBprivate.h

// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5WBprivate.h"
}  // extern "C"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5WB_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5WB_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5WB_TEST_HAS_ASAN_OR_LSAN) &&                         \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5WB_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5WB_TEST_HAS_OTHER_SAN) &&                       \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5WB_TEST_HAS_OTHER_SAN 1
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

struct WrappedBufferDeleter {
  void operator()(H5WB_t* wb) const {
    if (wb != nullptr) {
      H5WB_unwrap(wb);
    }
  }
};

using ScopedWrappedBuffer = std::unique_ptr<H5WB_t, WrappedBufferDeleter>;

TEST(H5WbTest, WrapAndImmediateUnwrapWithoutActualBuffer) {
  ASSERT_GE(H5open(), 0);

  std::array<uint8_t, 32> backing{};
  backing.fill(0x5a);

  ScopedWrappedBuffer wb(H5WB_wrap(backing.data(), backing.size()));
  ASSERT_NE(wb, nullptr);
  EXPECT_GE(H5WB_unwrap(wb.release()), 0);
  EXPECT_THAT(backing, Each(0x5a));
}

TEST(H5WbTest, ActualUsesWrappedBufferWhenNeedFitsCapacity) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kWrappedSize = 64;
  std::array<uint8_t, kWrappedSize> backing{};
  backing.fill(0xa5);

  ScopedWrappedBuffer wb(H5WB_wrap(backing.data(), backing.size()));
  ASSERT_NE(wb, nullptr);

  // 1. Zero-byte and sub-capacity requests return the wrapped buffer without
  //    clobbering its existing contents.
  EXPECT_EQ(H5WB_actual(wb.get(), 0), backing.data());
  EXPECT_THAT(backing, Each(0xa5));

  void* small_ptr = H5WB_actual(wb.get(), 16);
  ASSERT_EQ(small_ptr, backing.data());
  EXPECT_THAT(backing, Each(0xa5));

  // 2. Exact-capacity request still reuses the wrapped buffer.
  auto* exact_ptr = static_cast<uint8_t*>(H5WB_actual(wb.get(), kWrappedSize));
  ASSERT_EQ(exact_ptr, backing.data());
  for (size_t i = 0; i < kWrappedSize; ++i) {
    exact_ptr[i] = static_cast<uint8_t>(i);
  }

  // 3. Shrinking back to a smaller size continues using the wrapped buffer.
  EXPECT_EQ(H5WB_actual(wb.get(), 1), backing.data());
  EXPECT_EQ(backing[0], 0);
  EXPECT_EQ(backing[63], 63);

  EXPECT_GE(H5WB_unwrap(wb.release()), 0);
}

TEST(H5WbTest, ActualAllocatesReusesAndGrowsExtraHeapBuffer) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kWrappedSize = 32;
  std::array<uint8_t, kWrappedSize> backing{};
  backing.fill(0x3c);

  ScopedWrappedBuffer wb(H5WB_wrap(backing.data(), backing.size()));
  ASSERT_NE(wb, nullptr);

  // 1. Initial fit uses the wrapped buffer.
  ASSERT_EQ(H5WB_actual(wb.get(), kWrappedSize), backing.data());

  // 2. Requesting one byte past wrapped_size allocates an external block.
  constexpr size_t kFirstExtraSize = kWrappedSize + 1;
  auto* first_extra =
      static_cast<uint8_t*>(H5WB_actual(wb.get(), kFirstExtraSize));
  ASSERT_NE(first_extra, nullptr);
  EXPECT_NE(first_extra, backing.data());
  EXPECT_THAT(backing, Each(0x3c));
  std::fill_n(first_extra, kFirstExtraSize, uint8_t{0x11});

  // 3. Subsequent requests <= alloc_size reuse the existing extra block.
  void* reused_same = H5WB_actual(wb.get(), kFirstExtraSize);
  EXPECT_EQ(reused_same, first_extra);
  EXPECT_EQ(first_extra[0], 0x11);
  EXPECT_EQ(first_extra[kFirstExtraSize - 1], 0x11);

  // 4. Requesting > alloc_size frees the prior extra block and allocates
  // larger.
  constexpr size_t kLargerExtraSize = 128;
  auto* larger_extra =
      static_cast<uint8_t*>(H5WB_actual(wb.get(), kLargerExtraSize));
  ASSERT_NE(larger_extra, nullptr);
  EXPECT_NE(larger_extra, backing.data());
  for (size_t i = 0; i < kLargerExtraSize; ++i) {
    larger_extra[i] = static_cast<uint8_t>(i ^ 0x7f);
  }

  // 5. Smaller request still > wrapped_size reuses the larger extra block.
  constexpr size_t kShrunkExtraSize = 64;
  EXPECT_EQ(H5WB_actual(wb.get(), kShrunkExtraSize), larger_extra);
  EXPECT_EQ(larger_extra[63], static_cast<uint8_t>(63 ^ 0x7f));
  EXPECT_THAT(backing, Each(0x3c));

  EXPECT_GE(H5WB_unwrap(wb.release()), 0);
}

TEST(H5WbTest, ActualClearZeroesWrappedAndExtraBuffers) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kWrappedSize = 32;
  std::array<uint8_t, kWrappedSize> backing{};
  backing.fill(0x99);

  ScopedWrappedBuffer wb(H5WB_wrap(backing.data(), backing.size()));
  ASSERT_NE(wb, nullptr);

  // 1. Partial clear on the wrapped buffer zeroes only the requested prefix.
  constexpr size_t kPrefix = 16;
  auto* cleared_prefix =
      static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), kPrefix));
  ASSERT_EQ(cleared_prefix, backing.data());
  for (size_t i = 0; i < kPrefix; ++i) {
    EXPECT_EQ(backing[i], 0);
  }
  for (size_t i = kPrefix; i < kWrappedSize; ++i) {
    EXPECT_EQ(backing[i], 0x99);
  }

  // 2. Full clear zeroes the entire wrapped buffer.
  ASSERT_EQ(H5WB_actual_clear(wb.get(), kWrappedSize), backing.data());
  EXPECT_THAT(backing, Each(0));

  // 3. Clearing past wrapped_size allocates and zeroes an extra buffer.
  backing.fill(0x77);
  constexpr size_t kExtraSize = 80;
  auto* extra = static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), kExtraSize));
  ASSERT_NE(extra, nullptr);
  EXPECT_NE(extra, backing.data());
  for (size_t i = 0; i < kExtraSize; ++i) {
    EXPECT_EQ(extra[i], 0);
  }
  EXPECT_THAT(backing, Each(0x77));

  // 4. Reusing the extra buffer via H5WB_actual_clear zeroes the new slice
  //    while leaving bytes past `need` untouched.
  std::fill_n(extra, kExtraSize, uint8_t{0xfe});
  constexpr size_t kReuseNeed = 48;
  auto* reused = static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), kReuseNeed));
  ASSERT_EQ(reused, extra);
  for (size_t i = 0; i < kReuseNeed; ++i) {
    EXPECT_EQ(reused[i], 0);
  }
  for (size_t i = kReuseNeed; i < kExtraSize; ++i) {
    EXPECT_EQ(reused[i], 0xfe);
  }

  // 5. Growing past alloc_size allocates and zeroes a new extra buffer.
  constexpr size_t kGrownSize = 160;
  auto* grown = static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), kGrownSize));
  ASSERT_NE(grown, nullptr);
  EXPECT_NE(grown, backing.data());
  for (size_t i = 0; i < kGrownSize; ++i) {
    EXPECT_EQ(grown[i], 0);
  }

  EXPECT_GE(H5WB_unwrap(wb.release()), 0);
}

TEST(H5WbTest, FreeListReuseAndGarbageCollectionWithMultipleWrappers) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kBatchSize = 16;
  constexpr size_t kStackCap = 16;
  std::vector<std::array<uint8_t, kStackCap>> backings(kBatchSize);
  std::vector<ScopedWrappedBuffer> wrappers;
  wrappers.reserve(kBatchSize);

  // 1. Allocate concurrent wrappers and extra blocks across two sizes.
  for (size_t i = 0; i < kBatchSize; ++i) {
    backings[i].fill(0x22);
    ScopedWrappedBuffer wb(H5WB_wrap(backings[i].data(), backings[i].size()));
    ASSERT_NE(wb, nullptr);
    const size_t need = (i % 2 == 0) ? 48 : 96;
    auto* buf = static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), need));
    ASSERT_NE(buf, nullptr);
    EXPECT_NE(buf, backings[i].data());
    EXPECT_EQ(buf[0], 0);
    buf[need - 1] = static_cast<uint8_t>(i);
    wrappers.push_back(std::move(wb));
  }

  // 2. Unwrap all wrappers to return H5WB_t and extra blocks to free lists,
  //    then re-allocate matching sizes and run garbage collection.
  wrappers.clear();
  for (size_t i = 0; i < kBatchSize; ++i) {
    ScopedWrappedBuffer wb(H5WB_wrap(backings[i].data(), backings[i].size()));
    ASSERT_NE(wb, nullptr);
    void* buf = H5WB_actual(wb.get(), 48);
    ASSERT_NE(buf, nullptr);
    EXPECT_GE(H5WB_unwrap(wb.release()), 0);
  }
  EXPECT_GE(H5garbage_collect(), 0);
}

TEST(H5WbTest, PublicDatasetFillAndLargeAttributeExerciseWrappedBuffers) {
  // H5Dfill.c and H5Adense.c use H5WB_wrap / H5WB_actual[_clear] / H5WB_unwrap
  // internally when filling datasets and encoding large dense attributes.
  ScopedFile file = CreateInMemoryFile("h5wb_public.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 4;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Create a dataset with an explicit integer fill value and read it back.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  constexpr int32_t kFillVal = 123456;
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_INT32, &kFillVal), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/filled", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, kDim> read_vals{};
  read_vals.fill(-1);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(kFillVal, kFillVal, kFillVal, kFillVal));

  // 2. Write and read a large (> 4 KiB) attribute to force H5WB extra-buffer
  //    allocation inside H5Adense.c / H5Oshared.c.
  constexpr hsize_t kAttrLen = 2048;
  ScopedSpace attr_space(H5Screate_simple(1, &kAttrLen, nullptr));
  ASSERT_TRUE(attr_space.valid());

  ScopedAttr attr(H5Acreate2(dset.get(), "large_attr", H5T_NATIVE_INT32,
                             attr_space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  std::vector<int32_t> attr_in(kAttrLen);
  for (size_t i = 0; i < kAttrLen; ++i) {
    attr_in[i] = static_cast<int32_t>(i * 3 + 7);
  }
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, attr_in.data()), 0);

  std::vector<int32_t> attr_out(kAttrLen, -1);
  ASSERT_GE(H5Aread(attr.get(), H5T_NATIVE_INT32, attr_out.data()), 0);
  EXPECT_THAT(attr_out, ElementsAreArray(attr_in));
}

TEST(H5WbTest, SingleByteWrappedBufferAndZeroLengthClear) {
  ASSERT_GE(H5open(), 0);

  std::array<uint8_t, 1> tiny = {0x42};
  ScopedWrappedBuffer wb(H5WB_wrap(tiny.data(), tiny.size()));
  ASSERT_NE(wb, nullptr);

  // Zero-byte clear keeps the single byte untouched.
  EXPECT_EQ(H5WB_actual_clear(wb.get(), 0), tiny.data());
  EXPECT_EQ(tiny[0], 0x42);

  // Exact 1-byte clear zeroes the byte in place; 2-byte request spills to heap.
  EXPECT_EQ(H5WB_actual_clear(wb.get(), 1), tiny.data());
  EXPECT_EQ(tiny[0], 0);

  tiny[0] = 0x55;
  auto* spilled = static_cast<uint8_t*>(H5WB_actual_clear(wb.get(), 2));
  ASSERT_NE(spilled, nullptr);
  EXPECT_NE(spilled, tiny.data());
  EXPECT_EQ(spilled[0], 0);
  EXPECT_EQ(spilled[1], 0);
  EXPECT_EQ(tiny[0], 0x55);

  EXPECT_GE(H5WB_unwrap(wb.release()), 0);
}

#if !defined(H5WB_TEST_HAS_ASAN_OR_LSAN) && !defined(H5WB_TEST_HAS_OTHER_SAN)
TEST(H5WbTest, AllocationFailureReturnsNullAndPushesError) {
  ASSERT_GE(H5open(), 0);

  // Run in an isolated threadsafe death-test child process with RLIMIT_AS
  // tightened so large H5FL_BLK_MALLOC calls fail cleanly while sanitizers
  // (whose allocators abort on OOM) are inactive.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    bool all_failed = false;
    {
      ScopedErrorSilence silence;

      // 1. Pre-allocate two wrappers (one with an initial small extra block)
      //    before tightening RLIMIT_AS.
      std::array<uint8_t, 16> buf_a{};
      std::array<uint8_t, 16> buf_b{};
      H5WB_t* wb_actual = H5WB_wrap(buf_a.data(), buf_a.size());
      H5WB_t* wb_clear = H5WB_wrap(buf_b.data(), buf_b.size());
      if (wb_actual == nullptr || wb_clear == nullptr) {
        std::exit(1);
      }
      if (H5WB_actual(wb_actual, 64) == nullptr) {
        std::exit(1);
      }

      // 2. Cap virtual address space to vm_bytes + 4 MiB.
      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(2);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(3);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(4);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(5);
      }

      // 3. Drain pre-mapped 16 MiB spans in the allocator page heap.
      constexpr size_t kLargeNeed = 16ULL << 20;
      std::vector<void*> drain;
      drain.reserve(256);
      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeNeed);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      // 4. Exercise OOM paths in H5WB_actual and H5WB_actual_clear.
      void* actual_oom = H5WB_actual(wb_actual, kLargeNeed);
      const ssize_t actual_errs = H5Eget_num(H5E_DEFAULT);
      H5Eclear2(H5E_DEFAULT);

      void* clear_oom = H5WB_actual_clear(wb_clear, kLargeNeed);
      const ssize_t clear_errs = H5Eget_num(H5E_DEFAULT);

      // 5. Restore address-space limit and release drained buffers + wrappers.
      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
      const herr_t unwrap_a = H5WB_unwrap(wb_actual);
      const herr_t unwrap_b = H5WB_unwrap(wb_clear);

      all_failed =
          (actual_oom == nullptr && actual_errs > 0 && clear_oom == nullptr &&
           clear_errs > 0 && unwrap_a >= 0 && unwrap_b >= 0);
    }
    std::exit(all_failed ? 0 : 6);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
