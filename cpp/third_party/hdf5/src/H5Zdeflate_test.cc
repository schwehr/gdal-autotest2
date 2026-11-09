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
// Tests the zlib deflate compression filter (third_party/hdf5/src/H5Zdeflate.c).
// Note: google3 enables H5_HAVE_FILTER_DEFLATE via zlib (not zlib-ng).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Zdeflate.c

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5ZDEFLATE_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5ZDEFLATE_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5ZDEFLATE_TEST_HAS_ASAN_OR_LSAN) &&                   \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5ZDEFLATE_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5ZDEFLATE_TEST_HAS_OTHER_SAN) &&                 \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5ZDEFLATE_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` compiles with -DNDEBUG; match its layout across internal headers.
#ifndef NDEBUG
#define H5ZDEFLATE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZDEFLATE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZDEFLATE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

TEST(H5ZdeflateTest, FilterAvailableAndClassMetadata) {
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_DEFLATE), 0);

  unsigned flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_DEFLATE, &flags), 0);
  EXPECT_EQ(
      flags,
      static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED |
                            H5Z_FILTER_CONFIG_DECODE_ENABLED));

  EXPECT_EQ(H5Z_DEFLATE[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_DEFLATE[0].id, H5Z_FILTER_DEFLATE);
  EXPECT_EQ(H5Z_DEFLATE[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_DEFLATE[0].decoder_present, 1U);
  EXPECT_EQ(std::string_view(H5Z_DEFLATE[0].name), "deflate");
  EXPECT_EQ(H5Z_DEFLATE[0].can_apply, nullptr);
  EXPECT_EQ(H5Z_DEFLATE[0].set_local, nullptr);
  ASSERT_NE(H5Z_DEFLATE[0].filter, nullptr);
}

TEST(H5ZdeflateTest, ChunkedDatasetRoundTripAcrossCompressionLevels) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kNumElems = 1024;
  constexpr hsize_t kChunkDims[1] = {256};
  std::vector<int32_t> expected(kNumElems);
  for (hsize_t i = 0; i < kNumElems; ++i) {
    expected[i] = static_cast<int32_t>(i % 16);
  }

  ScopedSpace space(H5Screate_simple(1, &kNumElems, nullptr));
  ASSERT_TRUE(space.valid());

  for (unsigned level : {0U, 1U, 6U, 9U}) {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims), 0);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), level), 0);

    const std::string dset_name = "dset_" + std::to_string(level);
    ScopedDataset dset(H5Dcreate2(file.get(), dset_name.c_str(),
                                  H5T_NATIVE_INT32, space.get(), H5P_DEFAULT,
                                  dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, expected.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

    if (level > 0) {
      const hsize_t storage_size = H5Dget_storage_size(dset.get());
      EXPECT_GT(storage_size, 0U);
      EXPECT_LT(storage_size, kNumElems * sizeof(int32_t));
    }

    std::vector<int32_t> actual(kNumElems, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, actual.data()),
              0);
    EXPECT_THAT(actual, ElementsAreArray(expected));
  }

  // Invalid compression level (> 9) is rejected by H5Pset_deflate.
  ScopedPlist bad_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(bad_dcpl.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_deflate(bad_dcpl.get(), 10), 0);
}

TEST(H5ZdeflateTest, DirectFilterCompressAndDecompressGrowsOutputBuffer) {
  constexpr size_t kOrigBytes = 4096;
  std::vector<uint8_t> original(kOrigBytes);
  for (size_t i = 0; i < kOrigBytes; ++i) {
    original[i] = static_cast<uint8_t>((i / 8) & 0xff);
  }

  void* buf = H5allocate_memory(kOrigBytes, false);
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
  std::memcpy(buf, original.data(), kOrigBytes);

  size_t buf_size = kOrigBytes;
  const unsigned level = 6;
  const size_t compressed_bytes =
      H5Z_DEFLATE[0].filter(0, 1, &level, kOrigBytes, &buf_size, &buf);
  ASSERT_GT(compressed_bytes, 0U);
  EXPECT_LT(compressed_bytes, kOrigBytes);
  EXPECT_EQ(buf_size, kOrigBytes);

  // Force inflate's output buffer to start small so it hits the realloc growth
  // loop multiple times (8 -> 16 -> ... -> 4096).
  buf_size = 8;
  const size_t decompressed_bytes = H5Z_DEFLATE[0].filter(
      H5Z_FLAG_REVERSE, 1, &level, compressed_bytes, &buf_size, &buf);
  ASSERT_EQ(decompressed_bytes, kOrigBytes);
  EXPECT_GE(buf_size, kOrigBytes);

  std::vector<uint8_t> decoded(kOrigBytes);
  std::memcpy(decoded.data(), buf, kOrigBytes);
  EXPECT_THAT(decoded, ElementsAreArray(original));
}

TEST(H5ZdeflateTest, DirectFilterRejectsInvalidParamsAndCorruptStreams) {
  constexpr size_t kBufBytes = 16;
  void* buf = H5allocate_memory(kBufBytes, true);
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };

  ScopedErrorSilence silence;
  size_t buf_size = kBufBytes;

  // Invalid cd_nelmts (0 and 2).
  const unsigned valid_level = 6;
  EXPECT_EQ(H5Z_DEFLATE[0].filter(0, 0, &valid_level, kBufBytes, &buf_size, &buf),
            0U);
  const std::array<unsigned, 2> two_params = {6, 1};
  EXPECT_EQ(
      H5Z_DEFLATE[0].filter(0, 2, two_params.data(), kBufBytes, &buf_size, &buf),
      0U);

  // Invalid aggression level (> 9).
  const unsigned invalid_level = 10;
  EXPECT_EQ(
      H5Z_DEFLATE[0].filter(0, 1, &invalid_level, kBufBytes, &buf_size, &buf),
      0U);

  // Corrupt zlib stream during decompression hits inflate error + outbuf free.
  auto* bytes = static_cast<uint8_t*>(buf);
  bytes[0] = 0x78;
  bytes[1] = 0x9c;
  for (size_t i = 2; i < kBufBytes; ++i) {
    bytes[i] = static_cast<uint8_t>(0xff - i);
  }
  EXPECT_EQ(H5Z_DEFLATE[0].filter(H5Z_FLAG_REVERSE, 1, &valid_level, kBufBytes,
                                  &buf_size, &buf),
            0U);

  // Passing a null input buffer with nbytes > 0 hits compress2 Z_STREAM_ERROR
  // (compiled with -DNDEBUG in `:hdf5`, so assert(*buf) is elided).
  void* null_buf = nullptr;
  size_t null_buf_size = kBufBytes;
  EXPECT_EQ(H5Z_DEFLATE[0].filter(0, 1, &valid_level, kBufBytes, &null_buf_size,
                                  &null_buf),
            0U);
}

#if !defined(H5ZDEFLATE_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5ZDEFLATE_TEST_HAS_OTHER_SAN)
TEST(H5ZdeflateTest, DirectFilterHandlesAllocationFailuresCleanly) {
  ASSERT_GE(H5open(), 0);

  // Run in an isolated threadsafe child process with tightened RLIMIT_AS so
  // large malloc/realloc requests return nullptr without tripping sanitizer OOM
  // aborts.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    bool all_ok = false;
    {
      ScopedErrorSilence silence;
      constexpr size_t kLargeBytes = 16ULL << 20;
      const unsigned level = 6;

      // 1. Pre-compress a 16 MiB zero buffer before limiting address space so
      // we can test inflate realloc OOM when growing past 8 MiB.
      void* compressed = H5allocate_memory(kLargeBytes, true);
      if (compressed == nullptr) {
        std::exit(1);
      }
      size_t comp_buf_size = kLargeBytes;
      const size_t comp_len = H5Z_DEFLATE[0].filter(
          0, 1, &level, kLargeBytes, &comp_buf_size, &compressed);
      if (comp_len == 0) {
        H5free_memory(compressed);
        std::exit(2);
      }

      // Warm small allocations for error stack entries.
      void* small_in = H5allocate_memory(64, true);
      if (small_in == nullptr) {
        H5free_memory(compressed);
        std::exit(3);
      }

      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(4);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(5);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(6);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(7);
      }

      std::vector<void*> drain;
      drain.reserve(256);
      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeBytes);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      // 2. Forward malloc OOM.
      size_t fwd_buf_size = kLargeBytes;
      const size_t fwd_rc = H5Z_DEFLATE[0].filter(
          0, 1, &level, kLargeBytes, &fwd_buf_size, &small_in);

      // 3. Reverse initial malloc OOM.
      size_t rev_malloc_size = kLargeBytes;
      const size_t rev_malloc_rc = H5Z_DEFLATE[0].filter(
          H5Z_FLAG_REVERSE, 1, &level, 64, &rev_malloc_size, &small_in);

      // 4. Reverse realloc OOM during growth (starts at 1 MiB, fails at 16 MiB
      // or earlier when doubling).
      size_t rev_realloc_size = 1ULL << 20;
      const size_t rev_realloc_rc =
          H5Z_DEFLATE[0].filter(H5Z_FLAG_REVERSE, 1, &level, comp_len,
                                &rev_realloc_size, &compressed);

      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
      H5free_memory(small_in);
      H5free_memory(compressed);

      all_ok = (fwd_rc == 0 && rev_malloc_rc == 0 && rev_realloc_rc == 0);
    }
    std::exit(all_ok ? 0 : 8);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
