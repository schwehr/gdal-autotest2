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
// Tests public HDF5 plugin loading state and search path list APIs
// (third_party/hdf5/src/H5PL.c). In google3, dynamic dlopen/dlsym plugin
// loading is disabled by patch and H5_DEFAULT_PLUGINDIR is "/does/not/exist".
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5PL.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Captures the global H5PL loading mask and path list, restoring both on exit.
class ScopedPluginState {
 public:
  ScopedPluginState() {
    if (H5open() < 0) {
      ADD_FAILURE() << "H5open() failed";
      return;
    }
    if (H5PLget_loading_state(&saved_mask_) >= 0) {
      has_mask_ = true;
    }
    unsigned int count = 0;
    if (H5PLsize(&count) >= 0) {
      has_paths_ = true;
      saved_paths_.reserve(count);
      for (unsigned int i = 0; i < count; ++i) {
        const ssize_t len = H5PLget(i, nullptr, 0);
        if (len >= 0) {
          std::string path(static_cast<size_t>(len) + 1, '\0');
          if (H5PLget(i, path.data(), path.size()) >= 0) {
            path.resize(static_cast<size_t>(len));
            saved_paths_.push_back(std::move(path));
          }
        }
      }
    }
  }

  ~ScopedPluginState() {
    if (has_mask_) {
      H5PLset_loading_state(saved_mask_);
    }
    if (has_paths_) {
      ClearAllPaths();
      for (const std::string& p : saved_paths_) {
        if (!p.empty()) {
          H5PLappend(p.c_str());
        }
      }
    }
  }

  ScopedPluginState(const ScopedPluginState&) = delete;
  ScopedPluginState& operator=(const ScopedPluginState&) = delete;

  static void ClearAllPaths() {
    unsigned int count = 0;
    while (H5PLsize(&count) >= 0 && count > 0) {
      if (H5PLremove(0) < 0) {
        break;
      }
    }
  }

 private:
  unsigned int saved_mask_ = 0;
  bool has_mask_ = false;
  std::vector<std::string> saved_paths_;
  bool has_paths_ = false;
};

std::vector<std::string> ReadAllPaths() {
  unsigned int count = 0;
  EXPECT_GE(H5PLsize(&count), 0);
  std::vector<std::string> result;
  result.reserve(count);
  for (unsigned int i = 0; i < count; ++i) {
    const ssize_t len = H5PLget(i, nullptr, 0);
    EXPECT_GE(len, 0);
    if (len < 0) {
      continue;
    }
    std::string buf(static_cast<size_t>(len) + 1, '\0');
    const ssize_t copied = H5PLget(i, buf.data(), buf.size());
    EXPECT_EQ(copied, len);
    buf.resize(static_cast<size_t>(len));
    result.push_back(std::move(buf));
  }
  return result;
}

TEST(H5PLTest, PublicConstantsAndDefaultPathAreDefined) {
  EXPECT_STREQ(H5PL_NO_PLUGIN, "::");
  EXPECT_EQ(H5PL_TYPE_ERROR, -1);
  EXPECT_EQ(H5PL_TYPE_FILTER, 0);
  EXPECT_EQ(H5PL_TYPE_VOL, 1);
  EXPECT_EQ(H5PL_TYPE_VFD, 2);
  EXPECT_EQ(H5PL_TYPE_NONE, 3);
  EXPECT_EQ(H5PL_FILTER_PLUGIN, 0x0001u);
  EXPECT_EQ(H5PL_VOL_PLUGIN, 0x0002u);
  EXPECT_EQ(H5PL_VFD_PLUGIN, 0x0004u);
  EXPECT_EQ(H5PL_ALL_PLUGIN, 0xFFFFu);

  ScopedPluginState guard;
  unsigned int mask = 0;
  ASSERT_GE(H5PLget_loading_state(&mask), 0);
  EXPECT_EQ(mask, H5PL_ALL_PLUGIN);

  unsigned int count = 0;
  ASSERT_GE(H5PLsize(&count), 0);
  EXPECT_GE(count, 1u);
}

TEST(H5PLTest, SetAndGetLoadingStateTogglesBitmasksAndRejectsNull) {
  ScopedPluginState guard;

  constexpr auto kMasks = std::to_array<unsigned int>({
      0u,
      H5PL_FILTER_PLUGIN,
      H5PL_VOL_PLUGIN,
      H5PL_VFD_PLUGIN,
      H5PL_FILTER_PLUGIN | H5PL_VFD_PLUGIN,
      H5PL_ALL_PLUGIN,
      static_cast<unsigned int>(-1),
  });

  for (unsigned int expected : kMasks) {
    ASSERT_GE(H5PLset_loading_state(expected), 0);
    unsigned int actual = ~expected;
    ASSERT_GE(H5PLget_loading_state(&actual), 0);
    EXPECT_EQ(actual, expected);
  }

  ScopedErrorSilence silence;
  EXPECT_LT(H5PLget_loading_state(nullptr), 0);
}

TEST(H5PLTest, SizeRejectsNullOutput) {
  ScopedErrorSilence silence;
  EXPECT_LT(H5PLsize(nullptr), 0);
}

TEST(H5PLTest, AppendPrependInsertReplaceRemoveAndGetLifecycle) {
  ScopedPluginState guard;
  ScopedPluginState::ClearAllPaths();

  unsigned int count = 99;
  ASSERT_GE(H5PLsize(&count), 0);
  EXPECT_EQ(count, 0u);

  // 1. Insert into an empty path table (idx == 0 is permitted when empty).
  constexpr char kFirst[] = "/opt/hdf5/first";
  ASSERT_GE(H5PLinsert(kFirst, 0), 0);
  EXPECT_THAT(ReadAllPaths(), ElementsAre("/opt/hdf5/first"));

  // 2. Append, prepend, insert in the middle, and replace an entry.
  constexpr char kThird[] = "/opt/hdf5/third";
  constexpr char kZero[] = "/opt/hdf5/zero";
  constexpr char kSecond[] = "/opt/hdf5/second";
  constexpr char kReplaced[] = "/opt/hdf5/first_v2";
  ASSERT_GE(H5PLappend(kThird), 0);
  ASSERT_GE(H5PLprepend(kZero), 0);
  ASSERT_GE(H5PLinsert(kSecond, 2), 0);
  EXPECT_THAT(ReadAllPaths(),
              ElementsAre("/opt/hdf5/zero", "/opt/hdf5/first",
                          "/opt/hdf5/second", "/opt/hdf5/third"));

  ASSERT_GE(H5PLreplace(kReplaced, 1), 0);
  EXPECT_THAT(ReadAllPaths(),
              ElementsAre("/opt/hdf5/zero", "/opt/hdf5/first_v2",
                          "/opt/hdf5/second", "/opt/hdf5/third"));

  // 3. Query length with NULL buffer and truncation with a short buffer.
  const ssize_t full_len = H5PLget(1, nullptr, 0);
  ASSERT_EQ(full_len, static_cast<ssize_t>(std::string_view(kReplaced).size()));

  std::array<char, 8> short_buf{};
  short_buf.fill('X');
  const ssize_t trunc_ret = H5PLget(1, short_buf.data(), short_buf.size());
  EXPECT_EQ(trunc_ret, full_len);
  EXPECT_STREQ(short_buf.data(), "/opt/hd");

  // 4. Remove from the middle and ends, verifying compaction.
  ASSERT_GE(H5PLremove(1), 0);
  EXPECT_THAT(ReadAllPaths(),
              ElementsAre("/opt/hdf5/zero", "/opt/hdf5/second",
                          "/opt/hdf5/third"));
  ASSERT_GE(H5PLremove(2), 0);
  EXPECT_THAT(ReadAllPaths(),
              ElementsAre("/opt/hdf5/zero", "/opt/hdf5/second"));
  ASSERT_GE(H5PLremove(0), 0);
  EXPECT_THAT(ReadAllPaths(), ElementsAre("/opt/hdf5/second"));
  ASSERT_GE(H5PLremove(0), 0);
  EXPECT_THAT(ReadAllPaths(), ElementsAre());
}

TEST(H5PLTest, AppendGrowsPathTableBeyondInitialCapacity) {
  ScopedPluginState guard;
  ScopedPluginState::ClearAllPaths();

  // Default initial path table capacity in H5PLpath.c is 16.
  constexpr unsigned int kNumPaths = 20;
  for (unsigned int i = 0; i < kNumPaths; ++i) {
    const std::string p = "/plugin/dir_" + std::to_string(i);
    ASSERT_GE(H5PLappend(p.c_str()), 0);
  }

  unsigned int count = 0;
  ASSERT_GE(H5PLsize(&count), 0);
  EXPECT_EQ(count, kNumPaths);

  const std::vector<std::string> paths = ReadAllPaths();
  ASSERT_EQ(paths.size(), kNumPaths);
  for (unsigned int i = 0; i < kNumPaths; ++i) {
    EXPECT_EQ(paths[i], "/plugin/dir_" + std::to_string(i));
  }
}

TEST(H5PLTest, InvalidArgumentsAndOutOfBoundsOperationsFail) {
  ScopedPluginState guard;
  ScopedPluginState::ClearAllPaths();
  ScopedErrorSilence silence;

  // Null and empty string path arguments must fail across all mutators.
  constexpr char kEmpty[] = "";
  EXPECT_LT(H5PLappend(nullptr), 0);
  EXPECT_LT(H5PLappend(kEmpty), 0);
  EXPECT_LT(H5PLprepend(nullptr), 0);
  EXPECT_LT(H5PLprepend(kEmpty), 0);
  EXPECT_LT(H5PLreplace(nullptr, 0), 0);
  EXPECT_LT(H5PLreplace(kEmpty, 0), 0);
  EXPECT_LT(H5PLinsert(nullptr, 0), 0);
  EXPECT_LT(H5PLinsert(kEmpty, 0), 0);

  // Operations requiring at least one entry fail when the table is empty,
  // and H5PLget leaves the caller buffer untouched.
  constexpr char kValid[] = "/opt/hdf5/valid";
  std::array<char, 16> sentinel{};
  sentinel.fill('S');
  sentinel.back() = '\0';
  const std::array<char, 16> expected_sentinel = sentinel;

  EXPECT_LT(H5PLreplace(kValid, 0), 0);
  EXPECT_LT(H5PLremove(0), 0);
  EXPECT_LT(H5PLget(0, sentinel.data(), sentinel.size()), 0);
  EXPECT_EQ(sentinel, expected_sentinel);

  // Out-of-bounds indices fail when the table is non-empty.
  ASSERT_GE(H5PLappend(kValid), 0);
  EXPECT_LT(H5PLreplace(kValid, 1), 0);
  EXPECT_LT(H5PLinsert(kValid, 1), 0);
  EXPECT_LT(H5PLremove(1), 0);
  EXPECT_LT(H5PLget(1, sentinel.data(), sentinel.size()), 0);
  EXPECT_EQ(sentinel, expected_sentinel);
}

}  // namespace
}  // namespace hdf5
