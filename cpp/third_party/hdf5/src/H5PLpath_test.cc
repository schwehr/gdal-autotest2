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
// Tests internal plugin search path table management and directory scanning
// (third_party/hdf5/src/H5PLpath.c). On Linux google3, H5_HAVE_WIN32_API
// blocks are compiled out and H5PL__open unconditionally fails.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5PLpath.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define H5PLPATH_TEST_HAS_SANITIZER 1
#endif
#endif
#if !defined(H5PLPATH_TEST_HAS_SANITIZER) &&                         \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__) || defined(MEMORY_SANITIZER) || \
     defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__))
#define H5PLPATH_TEST_HAS_SANITIZER 1
#endif

extern "C" {
#define H5PL_FRIEND
#include "third_party/hdf5/src/H5PLpkg.h"
}  // extern "C"

namespace hdf5 {
namespace {

class ScopedPathTableGuard {
 public:
  ScopedPathTableGuard() {
    if (H5open() < 0) {
      ADD_FAILURE() << "H5open() failed";
      return;
    }
    unsigned int dummy = 0;
    if (H5PLsize(&dummy) < 0) {
      ADD_FAILURE() << "H5PLsize() failed";
    }
    const char* env = std::getenv(HDF5_PLUGIN_PATH);
    if (env != nullptr) {
      had_env_ = true;
      saved_env_ = env;
    }
  }

  ~ScopedPathTableGuard() {
    if (had_env_) {
      setenv(HDF5_PLUGIN_PATH, saved_env_.c_str(), 1);
    } else {
      unsetenv(HDF5_PLUGIN_PATH);
    }
    H5PL__close_path_table();
    H5PL__create_path_table();
  }

  ScopedPathTableGuard(const ScopedPathTableGuard&) = delete;
  ScopedPathTableGuard& operator=(const ScopedPathTableGuard&) = delete;

 private:
  bool had_env_ = false;
  std::string saved_env_;
};

herr_t CountingIterateOp(H5PL_type_t /*plugin_type*/,
                         const void* /*plugin_info*/, void* op_data) {
  if (op_data) {
    ++(*static_cast<int*>(op_data));
  }
  return H5_ITER_CONT;
}

TEST(H5PLpathTest, CreateAndClosePathTableRespectsEnvVarAndDefaults) {
  ScopedPathTableGuard guard;

  // 1. Close existing table, clear HDF5_PLUGIN_PATH, and recreate with default.
  ASSERT_GE(H5PL__close_path_table(), 0);
  EXPECT_EQ(H5PL__get_num_paths(), 0u);
  ASSERT_GE(H5PL__close_path_table(), 0);

  unsetenv(HDF5_PLUGIN_PATH);
  ASSERT_GE(H5PL__create_path_table(), 0);
  ASSERT_EQ(H5PL__get_num_paths(), 1u);
  EXPECT_STREQ(H5PL__get_path(0), H5PL_DEFAULT_PATH);

  // 2. Recreate with multiple colon-separated paths in HDF5_PLUGIN_PATH.
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "/dir/alpha:/dir/beta:/dir/gamma", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);
  ASSERT_EQ(H5PL__get_num_paths(), 3u);
  EXPECT_STREQ(H5PL__get_path(0), "/dir/alpha");
  EXPECT_STREQ(H5PL__get_path(1), "/dir/beta");
  EXPECT_STREQ(H5PL__get_path(2), "/dir/gamma");

  // 3. Recreate with empty HDF5_PLUGIN_PATH produces an empty table.
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);
  EXPECT_EQ(H5PL__get_num_paths(), 0u);
}

TEST(H5PLpathTest, MutatorsInsertReplaceRemoveAndValidateIndices) {
  ScopedPathTableGuard guard;
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);

  // 1. Insert into empty table, append, prepend, middle-insert, and replace.
  ASSERT_GE(H5PL__insert_path("/first", 0), 0);
  ASSERT_GE(H5PL__append_path("/last"), 0);
  ASSERT_GE(H5PL__prepend_path("/head"), 0);
  ASSERT_GE(H5PL__insert_path("/middle", 2), 0);
  ASSERT_EQ(H5PL__get_num_paths(), 4u);
  EXPECT_STREQ(H5PL__get_path(0), "/head");
  EXPECT_STREQ(H5PL__get_path(1), "/first");
  EXPECT_STREQ(H5PL__get_path(2), "/middle");
  EXPECT_STREQ(H5PL__get_path(3), "/last");

  ASSERT_GE(H5PL__replace_path("/first_updated", 1), 0);
  EXPECT_STREQ(H5PL__get_path(1), "/first_updated");

  // 2. Remove from middle and ends to verify shifting and NULL termination.
  ASSERT_GE(H5PL__remove_path(1), 0);
  ASSERT_EQ(H5PL__get_num_paths(), 3u);
  EXPECT_STREQ(H5PL__get_path(0), "/head");
  EXPECT_STREQ(H5PL__get_path(1), "/middle");
  EXPECT_STREQ(H5PL__get_path(2), "/last");

  ASSERT_GE(H5PL__remove_path(2), 0);
  ASSERT_EQ(H5PL__get_num_paths(), 2u);

  // 3. Exercise out-of-range / NULL-slot error paths cleanly within capacity.
  ScopedErrorSilence silence;
  const unsigned int num = H5PL__get_num_paths();
  EXPECT_EQ(H5PL__get_path(num), nullptr);
  EXPECT_LT(H5PL__replace_path("/unused", num), 0);
  EXPECT_LT(H5PL__remove_path(num), 0);
}

TEST(H5PLpathTest, ExpandPathTableOnAppendAndPrependAtCapacity) {
  ScopedPathTableGuard guard;
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);

  // Fill exactly to initial capacity (16) then prepend at idx 0 to trigger both
  // H5PL__expand_path_table() and H5PL__make_space_at(0) in the same call.
  for (unsigned int i = 0; i < 16; ++i) {
    const std::string p = "/cap_" + std::to_string(i);
    ASSERT_GE(H5PL__append_path(p.c_str()), 0);
  }
  ASSERT_GE(H5PL__prepend_path("/prepended_at_cap"), 0);
  EXPECT_EQ(H5PL__get_num_paths(), 17u);
  EXPECT_STREQ(H5PL__get_path(0), "/prepended_at_cap");
  EXPECT_STREQ(H5PL__get_path(1), "/cap_0");
  EXPECT_STREQ(H5PL__get_path(16), "/cap_15");

  // Append further beyond 32 entries to trigger a second expansion.
  for (unsigned int i = 17; i < 35; ++i) {
    const std::string p = "/extra_" + std::to_string(i);
    ASSERT_GE(H5PL__append_path(p.c_str()), 0);
  }
  EXPECT_EQ(H5PL__get_num_paths(), 35u);
  EXPECT_STREQ(H5PL__get_path(34), "/extra_34");
}

TEST(H5PLpathTest, PathTableIterateHandlesMissingNonPluginSubdirSymlinkAndLib) {
  ScopedPathTableGuard guard;
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);

  TempDir root("h5plpath_iter");
  const std::filesystem::path missing_dir = root.child("does_not_exist");
  const std::filesystem::path non_plugin_dir = root.child("non_plugins");
  const std::filesystem::path subdir_only_dir = root.child("subdir_only");
  const std::filesystem::path symlink_dir = root.child("dangling_link");
  const std::filesystem::path so_dir = root.child("has_so");
  const std::filesystem::path dylib_dir = root.child("has_dylib");

  std::filesystem::create_directories(non_plugin_dir);
  std::filesystem::create_directories(subdir_only_dir / "libdir.so");
  std::filesystem::create_directories(symlink_dir);
  std::filesystem::create_directories(so_dir);
  std::filesystem::create_directories(dylib_dir);

  {
    std::ofstream(non_plugin_dir / "readme.txt") << "ignored";
    std::ofstream(non_plugin_dir / "libwrong.txt") << "ignored";
    std::ofstream(non_plugin_dir / "notlib.so") << "ignored";
    std::ofstream(so_dir / "libfake.so") << "dummy";
    std::ofstream(dylib_dir / "libfake.dylib") << "dummy";
  }
  // Dangling symlink triggers HDstat() == -1 while matching "lib*.so".
  std::filesystem::create_symlink(root.child("missing_target"),
                                  symlink_dir / "libdangling.so");

  // 1. Missing dir, non-plugin files, and isolated subdirectory named
  // "libdir.so" (kept alone in subdir_only_dir so done: frees path cleanly)
  // all return H5_ITER_CONT.
  ASSERT_GE(H5PL__append_path(missing_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(non_plugin_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(subdir_only_dir.c_str()), 0);

  int count = 0;
  EXPECT_EQ(H5PL__path_table_iterate(H5PL_ITER_TYPE_ALL, CountingIterateOp,
                                     &count),
            H5_ITER_CONT);
  EXPECT_EQ(count, 0);

  // 2. Appending a dir with a regular lib*.so file hits H5PL__open failure.
  ScopedErrorSilence silence;
  ASSERT_GE(H5PL__append_path(so_dir.c_str()), 0);
  EXPECT_EQ(H5PL__path_table_iterate(H5PL_ITER_TYPE_FILTER, CountingIterateOp,
                                     &count),
            H5_ITER_ERROR);

  // 3. Test lib*.dylib matching and dangling symlink HDstat failure separately.
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);
  ASSERT_GE(H5PL__append_path(dylib_dir.c_str()), 0);
  EXPECT_EQ(H5PL__path_table_iterate(H5PL_ITER_TYPE_VOL, CountingIterateOp,
                                     &count),
            H5_ITER_ERROR);

  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);
  ASSERT_GE(H5PL__append_path(symlink_dir.c_str()), 0);
  EXPECT_EQ(H5PL__path_table_iterate(H5PL_ITER_TYPE_VFD, CountingIterateOp,
                                     &count),
            H5_ITER_ERROR);
}

TEST(H5PLpathTest, FindPluginInPathTableCoversDirectoryScanningBranches) {
  ScopedPathTableGuard guard;
  ASSERT_GE(H5PL__close_path_table(), 0);
  ASSERT_EQ(setenv(HDF5_PLUGIN_PATH, "", 1), 0);
  ASSERT_GE(H5PL__create_path_table(), 0);

  TempDir root("h5plpath_find");
  const std::filesystem::path missing_dir = root.child("missing");
  const std::filesystem::path mixed_dir = root.child("mixed");
  const std::filesystem::path symlink_dir = root.child("symlink");
  const std::filesystem::path so_dir = root.child("so");
  const std::filesystem::path dylib_dir = root.child("dylib");

  std::filesystem::create_directories(mixed_dir / "libdir.so");
  std::filesystem::create_directories(symlink_dir);
  std::filesystem::create_directories(so_dir);
  std::filesystem::create_directories(dylib_dir);
  {
    std::ofstream(mixed_dir / "note.txt") << "skip";
    std::ofstream(mixed_dir / "other.so") << "skip";
    std::ofstream(so_dir / "libtest.so") << "dummy";
    std::ofstream(dylib_dir / "libtest.dylib") << "dummy";
  }
  std::filesystem::create_symlink(root.child("missing_target"),
                                  symlink_dir / "libbroken.so");

  ASSERT_GE(H5PL__append_path(missing_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(mixed_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(symlink_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(so_dir.c_str()), 0);
  ASSERT_GE(H5PL__append_path(dylib_dir.c_str()), 0);

  ScopedErrorSilence silence;
  H5PL_key_t key{};
  key.id = 31000;
  H5PL_search_params_t params{H5PL_TYPE_FILTER, &key};
  bool found = true;
  const void* plugin_info = &found;

  // H5PL__find_plugin_in_path_table logs HERROR on path errors and keeps
  // scanning remaining entries, returning SUCCEED with found == false.
  EXPECT_GE(H5PL__find_plugin_in_path_table(&params, &found, &plugin_info), 0);
  EXPECT_FALSE(found);
  EXPECT_EQ(plugin_info, nullptr);
}

#if !defined(H5PLPATH_TEST_HAS_SANITIZER)
TEST(H5PLpathTest, AllocationFailuresPropagateErrorsAndCleanUp) {
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
    // Warm up HDF5 error reporting path so HGOTO_ERROR needs no large spans.
    (void)H5PLappend("");
    H5Eclear2(H5E_DEFAULT);

    constexpr size_t kLargeLen = 16ULL << 20;
    std::string large_str(kLargeLen, 'x');
    std::vector<void*> drain;
    drain.reserve(4096);

    constexpr std::array<size_t, 8> kWarmSizes = {16,  32,  64,   128,
                                                  256, 512, 1024, 2048};
    for (size_t sz : kWarmSizes) {
      void* p = std::malloc(sz);
      std::free(p);
    }

    const auto tighten_and_drain = [&]() -> rlimit {
      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) std::exit(3);
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) std::exit(4);
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) std::exit(5);
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) std::exit(6);

      // Exhaust only >16 MiB spans, leaving >= 4 MiB mmap headroom for runtime
      // background threads (e.g., sigaltstack allocation) during child startup.
      while (drain.size() < 4096) {
        void* p = std::malloc(kLargeLen + 1);
        if (p == nullptr) break;
        drain.push_back(p);
      }
      return old_lim;
    };

    const auto release_drain = [&](const rlimit& old_lim) {
      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
      drain.clear();
    };

    // 1. Trigger H5MM_strdup OOM in append, prepend, insert, and replace.
    if (H5PL__close_path_table() < 0) std::exit(7);
    if (setenv(HDF5_PLUGIN_PATH, "", 1) != 0) std::exit(8);
    if (H5PL__create_path_table() < 0) std::exit(9);
    if (H5PL__append_path("/seed") < 0) std::exit(10);

    rlimit old_lim = tighten_and_drain();
    const herr_t rc_app = H5PL__append_path(large_str.c_str());
    const herr_t rc_pre = H5PL__prepend_path(large_str.c_str());
    const herr_t rc_ins = H5PL__insert_path(large_str.c_str(), 0);
    const herr_t rc_rep = H5PL__replace_path(large_str.c_str(), 0);
    release_drain(old_lim);
    if (rc_app >= 0 || rc_pre >= 0 || rc_ins >= 0 || rc_rep >= 0) {
      std::exit(11);
    }

    // 2. Trigger H5MM_strdup(env_var) OOM in H5PL__create_path_table().
    if (H5PL__close_path_table() < 0) std::exit(12);
    if (setenv(HDF5_PLUGIN_PATH, large_str.c_str(), 1) != 0) std::exit(13);
    old_lim = tighten_and_drain();
    const herr_t rc_create = H5PL__create_path_table();
    release_drain(old_lim);
    unsetenv(HDF5_PLUGIN_PATH);
    if (rc_create >= 0) std::exit(14);
    if (H5PL__create_path_table() < 0) std::exit(15);

    std::exit(0);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
