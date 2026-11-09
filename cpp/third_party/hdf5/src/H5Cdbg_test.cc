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
// Tests metadata cache debugging, statistics, prefix configuration, and
// auto-resize reporting helpers (third_party/hdf5/src/H5Cdbg.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG, so NDEBUG-gated
// debugging dump/validation routines and H5C_COLLECT_CACHE_STATS bodies are
// compiled out; NDEBUG is temporarily aligned around internal headers so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t ABI layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cdbg.c

#ifndef NDEBUG
#define H5C_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5C_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5C_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5C_t* GetCachePtr(hid_t file_id) {
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
  if (f == nullptr || f->shared == nullptr) {
    return nullptr;
  }
  return static_cast<H5C_t*>(f->shared->cache);
}

// Redirects POSIX stdout into a pipe so H5C_def_auto_resize_rpt_fcn output can
// be verified deterministically without polluting test logs.
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

TEST(H5CdbgTest, SetPrefixRejectsInvalidInputsAndCopiesValidPrefix) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5C_t* cache = GetCachePtr(file.get());
  ASSERT_NE(nullptr, cache);

  // 1. Validate error paths for null pointers and overly long prefixes.
  std::string short_prefix = "dbg:";
  std::string exact_limit_prefix(H5C__PREFIX_LEN, 'a');
  std::string too_long_prefix(H5C__PREFIX_LEN + 8, 'b');
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_set_prefix(nullptr, short_prefix.data()), 0);
    EXPECT_LT(H5C_set_prefix(cache, nullptr), 0);
    EXPECT_LT(H5C_set_prefix(cache, exact_limit_prefix.data()), 0);
    EXPECT_LT(H5C_set_prefix(cache, too_long_prefix.data()), 0);
  }

  // 2. Restore an empty prefix after testing valid prefix strings.
  std::string empty_prefix;
  auto restore = absl::MakeCleanup([&]() {
    H5C_set_prefix(cache, empty_prefix.data());
  });

  EXPECT_GE(H5C_set_prefix(cache, short_prefix.data()), 0);
  EXPECT_STREQ("dbg:", cache->prefix);

  std::string max_valid_prefix(H5C__PREFIX_LEN - 1, 'x');
  EXPECT_GE(H5C_set_prefix(cache, max_valid_prefix.data()), 0);
  EXPECT_STREQ(max_valid_prefix.c_str(), cache->prefix);

  EXPECT_GE(H5C_set_prefix(cache, empty_prefix.data()), 0);
  EXPECT_STREQ("", cache->prefix);
}

TEST(H5CdbgTest, StatsResetAndImageStatsHandleValidAndInvalidInputs) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5C_t* cache = GetCachePtr(file.get());
  ASSERT_NE(nullptr, cache);

  // 1. Reject null arguments in stats helpers.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_stats(nullptr, "cache", false), 0);
    EXPECT_LT(H5C_stats(cache, nullptr, true), 0);
    EXPECT_LT(H5C__image_stats(nullptr, true), 0);
  }

  // 2. Exercise valid invocations (no-op bodies when cache stats are off).
  EXPECT_GE(H5C_stats(cache, "in_memory_cache", false), 0);
  EXPECT_GE(H5C_stats(cache, "in_memory_cache", true), 0);
  H5C_stats__reset(cache);
  EXPECT_GE(H5C__image_stats(cache, false), 0);
  EXPECT_GE(H5C__image_stats(cache, true), 0);
}

TEST(H5CdbgTest, DefAutoResizeRptFcnFormatsAllStatusesAndDecrementModes) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  H5C_t* cache = GetCachePtr(file.get());
  ASSERT_NE(nullptr, cache);

  // Save prefix and resize configuration to keep cache state clean.
  std::string saved_prefix(cache->prefix);
  const H5C_auto_size_ctl_t orig_ctl = cache->resize_ctl;
  const size_t orig_flash_thresh = cache->flash_size_increase_threshold;
  auto restore = absl::MakeCleanup([&]() {
    H5C_set_prefix(cache, saved_prefix.data());
    cache->resize_ctl = orig_ctl;
    cache->flash_size_increase_threshold = orig_flash_thresh;
  });

  std::string prefix = "mdc: ";
  ASSERT_GE(H5C_set_prefix(cache, prefix.data()), 0);
  cache->resize_ctl.lower_hr_threshold = 0.80;
  cache->resize_ctl.upper_hr_threshold = 0.95;
  cache->resize_ctl.flash_incr_mode = H5C_flash_incr__add_space;
  cache->flash_size_increase_threshold = 2048;

  StdoutCapture capture;

  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.90,
                              in_spec, 4096, 4096, 2048, 2048);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              increase, 4096, 8192, 2048, 4096);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              flash_increase, 4096, 16384, 2048, 8192);

  constexpr auto kDecrModes = std::to_array<enum H5C_cache_decr_mode>({
      H5C_decr__off,
      H5C_decr__threshold,
      H5C_decr__age_out,
      H5C_decr__age_out_with_threshold,
  });
  for (enum H5C_cache_decr_mode mode : kDecrModes) {
    cache->resize_ctl.decr_mode = mode;
    H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.98,
                                decrease, 8192, 4096, 4096, 2048);
  }

  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              at_max_size, 8192, 8192, 4096, 4096);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.98,
                              at_min_size, 4096, 4096, 2048, 2048);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              increase_disabled, 4096, 4096, 2048, 2048);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.98,
                              decrease_disabled, 4096, 4096, 2048, 2048);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              not_full, 4096, 4096, 2048, 2048);

#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  // Exercises defensive default branches on corrupted enum values.
  cache->resize_ctl.decr_mode = static_cast<enum H5C_cache_decr_mode>(99);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.98,
                              decrease, 8192, 4096, 4096, 2048);
  H5C_def_auto_resize_rpt_fcn(cache, H5C__CURR_AUTO_RESIZE_RPT_FCN_VER, 0.50,
                              static_cast<enum H5C_resize_status>(99), 4096,
                              4096, 2048, 2048);
#endif

  const std::string output = capture.Finish();
  EXPECT_THAT(output, HasSubstr("mdc: Auto cache resize -- no change."));
  EXPECT_THAT(output, HasSubstr("cache size increased from (4096/2048) to (8192/4096)."));
  EXPECT_THAT(output, HasSubstr("mdc: flash cache resize(1) -- size threshold = 2048."));
  EXPECT_THAT(output, HasSubstr("decrease off."));
  EXPECT_THAT(output, HasSubstr("decrease by threshold."));
  EXPECT_THAT(output, HasSubstr("decrease by ageout."));
  EXPECT_THAT(output, HasSubstr("decrease by ageout with threshold."));
  EXPECT_THAT(output, HasSubstr("cache already at maximum size"));
  EXPECT_THAT(output, HasSubstr("cache already at minimum size."));
  EXPECT_THAT(output, HasSubstr("increase disabled"));
  EXPECT_THAT(output, HasSubstr("decrease disabled"));
  EXPECT_THAT(output, HasSubstr("cache not full so no increase in size."));
}

}  // namespace
}  // namespace hdf5
