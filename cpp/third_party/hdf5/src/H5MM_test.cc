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
// Tests internal and public memory management helpers (H5MM_realloc,
// H5MM_xstrdup, H5MM_strdup, H5MM_strndup, H5MM_xfree, H5MM_xfree_const,
// H5MMprivate.h macros, and H5MMpublic.h allocator typedefs)
// (third_party/hdf5/src/H5MM.c).
//
// Note: H5MM_DEBUG is not defined in the default google3 build, so
// H5MM_memcpy() in H5MM.c is compiled out and expands to memcpy().
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MM.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MMprivate.h
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5MMpublic.h

// Include prelude first to satisfy H5private.h / GoogleTest macro ordering.
// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5MMprivate.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

TEST(H5MmTest, ReallocNullAndZeroSizesReturnNull) {
  // H5MM_realloc(NULL, 0) takes the explicit fast-path returning NULL rather
  // than calling realloc(NULL, 0).
  EXPECT_EQ(nullptr, H5MM_realloc(nullptr, 0));

  // H5MM_realloc(NULL, size > 0) allocates writable storage.
  constexpr size_t kInitialSize = 24;
  void* ptr = H5MM_realloc(nullptr, kInitialSize);
  ASSERT_NE(nullptr, ptr);
  absl::Cleanup free_ptr = [&ptr] { ptr = H5MM_xfree(ptr); };
  std::memset(ptr, 0xa5, kInitialSize);

  // H5MM_realloc(ptr, 0) frees ptr and guarantees returning NULL even on
  // platforms where realloc(ptr, 0) returns a non-NULL zero-size pointer.
  ptr = H5MM_realloc(ptr, 0);
  EXPECT_EQ(nullptr, ptr);

  // Public H5resize_memory() delegates to H5MM_realloc() and H5free_memory()
  // delegates to H5MM_xfree().
  EXPECT_EQ(nullptr, H5resize_memory(nullptr, 0));
  void* pub_ptr = H5allocate_memory(16, /*clear=*/false);
  ASSERT_NE(nullptr, pub_ptr);
  absl::Cleanup free_pub = [&pub_ptr] { H5free_memory(pub_ptr); };
  pub_ptr = H5resize_memory(pub_ptr, 0);
  EXPECT_EQ(nullptr, pub_ptr);
}

TEST(H5MmTest, ReallocGrowAndShrinkPreservesPrefixBytes) {
  constexpr size_t kInitialSize = 16;
  constexpr size_t kGrownSize = 64;
  constexpr size_t kShrunkSize = 8;

  uint8_t* buf = static_cast<uint8_t*>(H5MM_realloc(nullptr, kInitialSize));
  ASSERT_NE(nullptr, buf);
  absl::Cleanup free_buf = [&buf] {
    buf = static_cast<uint8_t*>(H5MM_xfree(buf));
  };

  for (size_t i = 0; i < kInitialSize; ++i) {
    buf[i] = static_cast<uint8_t>(0x30 + i);
  }

  // Grow allocation and verify original prefix bytes are preserved.
  uint8_t* grown = static_cast<uint8_t*>(H5MM_realloc(buf, kGrownSize));
  ASSERT_NE(nullptr, grown);
  buf = grown;
  for (size_t i = 0; i < kInitialSize; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(0x30 + i), buf[i]);
  }
  std::memset(buf + kInitialSize, 0x55, kGrownSize - kInitialSize);

  // Shrink allocation and verify the retained prefix remains intact.
  uint8_t* shrunk = static_cast<uint8_t*>(H5MM_realloc(buf, kShrunkSize));
  ASSERT_NE(nullptr, shrunk);
  buf = shrunk;
  for (size_t i = 0; i < kShrunkSize; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(0x30 + i), buf[i]);
  }
}

TEST(H5MmTest, XstrdupAllowsNullAndDuplicatesStrings) {
  ASSERT_GE(H5open(), 0);
  EXPECT_EQ(nullptr, H5MM_xstrdup(nullptr));
  EXPECT_EQ(0, H5Eget_num(H5E_DEFAULT));

  constexpr auto kInputs = std::to_array<const char*>({
      "",
      "x",
      "hdf5-memory-management",
      "embedded\twhitespace\nand symbols!@#",
  });
  for (const char* input : kInputs) {
    char* dup = H5MM_xstrdup(input);
    absl::Cleanup free_dup = [&dup] {
      dup = static_cast<char*>(H5MM_xfree(dup));
    };
    ASSERT_NE(nullptr, dup);
    EXPECT_NE(input, dup);
    EXPECT_STREQ(input, dup);
  }
}

TEST(H5MmTest, StrdupRejectsNullAndDuplicatesStrings) {
  ASSERT_GE(H5open(), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5MM_strdup(nullptr));
    EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  }
  EXPECT_EQ(0, H5Eget_num(H5E_DEFAULT));

  constexpr auto kInputs = std::to_array<const char*>({
      "",
      "a",
      "H5MM_strdup_test_string",
  });
  for (const char* input : kInputs) {
    char* dup = H5MM_strdup(input);
    absl::Cleanup free_dup = [&dup] {
      dup = static_cast<char*>(H5MM_xfree(dup));
    };
    ASSERT_NE(nullptr, dup);
    EXPECT_NE(input, dup);
    EXPECT_STREQ(input, dup);
  }
}

TEST(H5MmTest, StrndupRejectsNullAndBoundsCopiedLength) {
  ASSERT_GE(H5open(), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5MM_strndup(nullptr, 0));
    EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);

    H5Eclear2(H5E_DEFAULT);
    EXPECT_EQ(nullptr, H5MM_strndup(nullptr, 16));
    EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
  }
  EXPECT_EQ(0, H5Eget_num(H5E_DEFAULT));

  struct StrndupCase {
    const char* input;
    size_t n;
    const char* expected;
  };
  constexpr auto kCases = std::to_array<StrndupCase>({
      {.input = "abcdef", .n = 0, .expected = ""},
      {.input = "abcdef", .n = 3, .expected = "abc"},
      {.input = "abcdef", .n = 6, .expected = "abcdef"},
      {.input = "abcdef", .n = 64, .expected = "abcdef"},
      {.input = "", .n = 0, .expected = ""},
      {.input = "", .n = 8, .expected = ""},
  });
  for (const StrndupCase& tc : kCases) {
    char* dup = H5MM_strndup(tc.input, tc.n);
    absl::Cleanup free_dup = [&dup] {
      dup = static_cast<char*>(H5MM_xfree(dup));
    };
    ASSERT_NE(nullptr, dup);
    EXPECT_NE(tc.input, dup);
    EXPECT_STREQ(tc.expected, dup);
  }

  // Verify HDstrndup stops at `n` bytes even when the source lacks a NUL byte.
  constexpr auto kUnterminated = std::to_array<char>({'h', '5', 'm', 'm'});
  char* bounded = H5MM_strndup(kUnterminated.data(), kUnterminated.size());
  absl::Cleanup free_bounded = [&bounded] {
    bounded = static_cast<char*>(H5MM_xfree(bounded));
  };
  ASSERT_NE(nullptr, bounded);
  EXPECT_STREQ("h5mm", bounded);
}

TEST(H5MmTest, XfreeAndXfreeConstAcceptNullAndReturnNull) {
  EXPECT_EQ(nullptr, H5MM_xfree(nullptr));
  EXPECT_EQ(nullptr, H5MM_xfree_const(nullptr));

  void* mut_buf = H5MM_strdup("mutable_copy");
  ASSERT_NE(nullptr, mut_buf);
  mut_buf = H5MM_xfree(mut_buf);
  EXPECT_EQ(nullptr, mut_buf);

  const char* const_str = H5MM_strdup("const_copy");
  ASSERT_NE(nullptr, const_str);
  const_str = static_cast<const char*>(H5MM_xfree_const(const_str));
  EXPECT_EQ(nullptr, const_str);

  const uint8_t* const_bytes =
      static_cast<const uint8_t*>(H5MM_realloc(nullptr, 32));
  ASSERT_NE(nullptr, const_bytes);
  const_bytes = static_cast<const uint8_t*>(H5MM_xfree_const(const_bytes));
  EXPECT_EQ(nullptr, const_bytes);
}

TEST(H5MmTest, PrivateMacrosAllocateZeroCopyAndFree) {
  constexpr size_t kCount = 16;
  uint8_t* zeroed = static_cast<uint8_t*>(H5MM_calloc(kCount));
  ASSERT_NE(nullptr, zeroed);
  absl::Cleanup free_zeroed = [&zeroed] { H5MM_free(zeroed); };
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(0, zeroed[i]);
  }

  constexpr auto kSource = std::to_array<uint8_t>({0x11, 0x22, 0x33, 0x44});
  uint8_t* copied = static_cast<uint8_t*>(H5MM_malloc(kSource.size()));
  ASSERT_NE(nullptr, copied);
  absl::Cleanup free_copied = [&copied] { H5MM_free(copied); };

  H5MM_memcpy(copied, kSource.data(), kSource.size());
  std::array<uint8_t, kSource.size()> actual{};
  std::memcpy(actual.data(), copied, actual.size());
  EXPECT_THAT(actual, ElementsAreArray(kSource));
}

TEST(H5MmTest, PublicAllocatorTypedefsWorkWithVlenMemManager) {
  struct AllocStats {
    size_t alloc_calls = 0;
    size_t free_calls = 0;
    size_t live_allocs = 0;
  } stats;

  H5MM_allocate_t custom_alloc = [](size_t size, void* alloc_info) -> void* {
    auto* st = static_cast<AllocStats*>(alloc_info);
    ++st->alloc_calls;
    ++st->live_allocs;
    return H5MM_realloc(nullptr, size);
  };
  H5MM_free_t custom_free = [](void* mem, void* free_info) {
    auto* st = static_cast<AllocStats*>(free_info);
    ++st->free_calls;
    if (mem != nullptr) {
      --st->live_allocs;
    }
    H5MM_xfree(mem);
  };

  // 1. Register custom H5MM_allocate_t / H5MM_free_t callbacks on a DXPL and
  //    verify round-trip retrieval.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), custom_alloc, &stats,
                                    custom_free, &stats),
            0);

  H5MM_allocate_t got_alloc = nullptr;
  H5MM_free_t got_free = nullptr;
  void* got_alloc_info = nullptr;
  void* got_free_info = nullptr;
  ASSERT_GE(H5Pget_vlen_mem_manager(dxpl.get(), &got_alloc, &got_alloc_info,
                                    &got_free, &got_free_info),
            0);
  EXPECT_EQ(custom_alloc, got_alloc);
  EXPECT_EQ(&stats, got_alloc_info);
  EXPECT_EQ(custom_free, got_free);
  EXPECT_EQ(&stats, got_free_info);

  // 2. Write and read a variable-length sequence dataset in memory so HDF5
  //    exercises the registered H5MM_allocate_t and H5MM_free_t callbacks.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType vl_type(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_type.valid());

  constexpr hsize_t kDim = 2;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file.get(), "vl_ints", vl_type.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<int32_t, 2> row0 = {10, 20};
  std::array<int32_t, 3> row1 = {30, 40, 50};
  std::array<hvl_t, kDim> write_data = {{
      {.len = row0.size(), .p = row0.data()},
      {.len = row1.size(), .p = row1.data()},
  }};
  ASSERT_GE(H5Dwrite(dset.get(), vl_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     write_data.data()),
            0);

  std::array<hvl_t, kDim> read_data{};
  absl::Cleanup reclaim_vlen = [&] {
    H5Treclaim(vl_type.get(), space.get(), dxpl.get(), read_data.data());
  };
  ASSERT_GE(H5Dread(dset.get(), vl_type.get(), H5S_ALL, H5S_ALL, dxpl.get(),
                    read_data.data()),
            0);

  ASSERT_EQ(row0.size(), read_data[0].len);
  ASSERT_EQ(row1.size(), read_data[1].len);
  std::vector<int32_t> got0(
      static_cast<const int32_t*>(read_data[0].p),
      static_cast<const int32_t*>(read_data[0].p) + read_data[0].len);
  std::vector<int32_t> got1(
      static_cast<const int32_t*>(read_data[1].p),
      static_cast<const int32_t*>(read_data[1].p) + read_data[1].len);
  EXPECT_THAT(got0, ElementsAreArray(row0));
  EXPECT_THAT(got1, ElementsAreArray(row1));

  std::move(reclaim_vlen).Invoke();
  EXPECT_EQ(kDim, stats.alloc_calls);
  EXPECT_EQ(kDim, stats.free_calls);
  EXPECT_EQ(0U, stats.live_allocs);
}

}  // namespace
}  // namespace hdf5
