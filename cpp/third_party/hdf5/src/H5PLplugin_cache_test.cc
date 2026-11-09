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
// Tests internal plugin cache creation, expansion, lookup, and teardown
// (third_party/hdf5/src/H5PLplugin_cache.c). In google3, cache lookup skips
// dlsym() via #if 0 patch.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5PLplugin_cache.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5PLCACHE_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5PLCACHE_TEST_HAS_SANITIZER) &&                        \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) || \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5PLCACHE_TEST_HAS_SANITIZER 1
#endif

extern "C" {
#define H5PL_FRIEND
#include "third_party/hdf5/src/H5PLpkg.h"
}  // extern "C"

namespace hdf5 {
namespace {

class ScopedPluginCacheGuard {
 public:
  ScopedPluginCacheGuard() {
    if (H5open() < 0) {
      ADD_FAILURE() << "H5open() failed";
      return;
    }
    unsigned int dummy = 0;
    if (H5PLsize(&dummy) < 0) {
      ADD_FAILURE() << "H5PLsize() failed";
    }
  }

  ~ScopedPluginCacheGuard() {
    bool already_closed = false;
    H5PL__close_plugin_cache(&already_closed);
    H5PL__create_plugin_cache();
  }

  ScopedPluginCacheGuard(const ScopedPluginCacheGuard&) = delete;
  ScopedPluginCacheGuard& operator=(const ScopedPluginCacheGuard&) = delete;
};

TEST(H5PLpluginCacheTest, CreateAndCloseCacheTracksAlreadyClosedState) {
  ScopedPluginCacheGuard guard;

  bool already_closed = true;
  ASSERT_GE(H5PL__close_plugin_cache(&already_closed), 0);
  EXPECT_FALSE(already_closed);

  // Closing an already-closed cache succeeds and sets already_closed = true.
  ASSERT_GE(H5PL__close_plugin_cache(&already_closed), 0);
  EXPECT_TRUE(already_closed);

  ASSERT_GE(H5PL__create_plugin_cache(), 0);
  already_closed = true;
  ASSERT_GE(H5PL__close_plugin_cache(&already_closed), 0);
  EXPECT_FALSE(already_closed);
}

TEST(H5PLpluginCacheTest, AddPluginsExpandsCacheAndClosesAllEntries) {
  ScopedPluginCacheGuard guard;

  // Add >16 plugins (H5PL_INITIAL_CACHE_CAPACITY == 16) across filter, VOL,
  // and VFD key kinds to exercise H5PL__expand_cache() and close loop.
  for (int i = 0; i < 10; ++i) {
    H5PL_key_t key{};
    key.id = 30000 + i;
    ASSERT_GE(H5PL__add_plugin(H5PL_TYPE_FILTER, &key, nullptr), 0);
  }
  for (int i = 0; i < 8; ++i) {
    H5PL_key_t key{};
    key.vol.kind = H5VL_GET_CONNECTOR_BY_VALUE;
    key.vol.u.value = static_cast<H5VL_class_value_t>(500 + i);
    ASSERT_GE(H5PL__add_plugin(H5PL_TYPE_VOL, &key, nullptr), 0);
  }
  for (int i = 0; i < 7; ++i) {
    H5PL_key_t key{};
    key.vfd.kind = H5FD_GET_DRIVER_BY_VALUE;
    key.vfd.u.value = static_cast<H5FD_class_value_t>(600 + i);
    ASSERT_GE(H5PL__add_plugin(H5PL_TYPE_VFD, &key, nullptr), 0);
  }

  bool already_closed = true;
  ASSERT_GE(H5PL__close_plugin_cache(&already_closed), 0);
  EXPECT_FALSE(already_closed);
}

TEST(H5PLpluginCacheTest, FindPluginInCacheReturnsNotFoundInGoogle3) {
  ScopedPluginCacheGuard guard;

  H5PL_key_t filter_key{};
  filter_key.id = 31234;
  ASSERT_GE(H5PL__add_plugin(H5PL_TYPE_FILTER, &filter_key, nullptr), 0);

  H5PL_search_params_t params{H5PL_TYPE_FILTER, &filter_key};
  bool found = true;
  const void* plugin_info = &found;
  ASSERT_GE(H5PL__find_plugin_in_cache(&params, &found, &plugin_info), 0);
  EXPECT_FALSE(found);
  EXPECT_EQ(plugin_info, nullptr);
}

#if !defined(H5PLCACHE_TEST_HAS_SANITIZER)
TEST(H5PLpluginCacheTest, ExpandCacheHandlesReallocFailureCleanly) {
  // Run in an isolated threadsafe death-test child process so RLIMIT_AS can be
  // tightened without affecting sanitizers or parent virtual address space.
  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    if (H5open() < 0) std::exit(1);
    unsigned int dummy = 0;
    if (H5PLsize(&dummy) < 0) std::exit(2);

    ScopedErrorSilence silence;
    (void)H5PLappend("");
    H5Eclear2(H5E_DEFAULT);

    // Pre-populate 32768 entries (1 MiB == kSmallAllocPages) before tightening
    // RLIMIT_AS so the next expansion requests 129 pages (> kSmallAllocPages),
    // entering AllocLarge/AllocRawHugepages while leaving >= 1 MiB mmap
    // headroom for runtime background threads.
    H5PL_key_t key{};
    for (int i = 0; i < 32768; ++i) {
      key.id = i;
      if (H5PL__add_plugin(H5PL_TYPE_FILTER, &key, nullptr) < 0) std::exit(3);
    }

    constexpr std::array<size_t, 8> kWarmSizes = {16,  32,  64,   128,
                                                  256, 512, 1024, 2048};
    for (size_t sz : kWarmSizes) {
      void* p = std::malloc(sz);
      std::free(p);
    }

    std::ifstream statm("/proc/self/statm");
    unsigned long vm_pages = 0;
    if (!(statm >> vm_pages)) std::exit(4);
    const long page_sz = ::sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) std::exit(5);
    const rlim_t vm_bytes =
        static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

    struct rlimit old_lim{};
    if (::getrlimit(RLIMIT_AS, &old_lim) != 0) std::exit(6);
    struct rlimit tight_lim = old_lim;
    tight_lim.rlim_cur = std::min<rlim_t>(
        old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(1ULL << 20));
    if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) std::exit(7);

    // Drain spans >= 1032 KiB (129 pages) across both scalar (alloc-token 0)
    // and pointer-containing (alloc-token 1) malloc partitions as well as raw
    // void* malloc so H5PL_cache_g's realloc partition is exhausted cleanly.
    struct PtrBlock {
      void* ptr;
    };
    std::vector<uint8_t*> drain_u8;
    std::vector<PtrBlock*> drain_ptr;
    std::vector<void*> drain_raw;
    drain_u8.reserve(4096);
    drain_ptr.reserve(4096);
    drain_raw.reserve(4096);

    constexpr std::array<size_t, 4> kDrainSizes = {16ULL << 20, 4ULL << 20,
                                                   2ULL << 20, 1032ULL << 10};
    for (size_t sz : kDrainSizes) {
      while (drain_u8.size() < 4096) {
        auto* p = static_cast<uint8_t*>(std::malloc(sz));
        if (p == nullptr) break;
        drain_u8.push_back(p);
      }
      while (drain_ptr.size() < 4096) {
        auto* p = static_cast<PtrBlock*>(std::malloc(sz));
        if (p == nullptr) break;
        drain_ptr.push_back(p);
      }
      while (drain_raw.size() < 4096) {
        void* p = std::malloc(sz);
        if (p == nullptr) break;
        drain_raw.push_back(p);
      }
    }

    herr_t rc = 0;
    for (int i = 0; i < 65536; ++i) {
      key.id = 40000 + i;
      rc = H5PL__add_plugin(H5PL_TYPE_FILTER, &key, nullptr);
      if (rc < 0) break;
    }

    ::setrlimit(RLIMIT_AS, &old_lim);
    for (uint8_t* p : drain_u8) std::free(p);
    for (PtrBlock* p : drain_ptr) std::free(p);
    for (void* p : drain_raw) std::free(p);
    if (rc >= 0) std::exit(8);
    if (H5PL__create_plugin_cache() < 0) std::exit(9);
    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
