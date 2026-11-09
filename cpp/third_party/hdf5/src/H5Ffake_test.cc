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
// Tests internal "fake" file allocation and deallocation used for standalone
// datatype/dataspace header encode/decode operations
// (third_party/hdf5/src/H5Ffake.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ffake.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5FFAKE_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5FFAKE_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5FFAKE_TEST_HAS_ASAN_OR_LSAN) &&                       \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5FFAKE_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5FFAKE_TEST_HAS_OTHER_SAN) &&                     \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5FFAKE_TEST_HAS_OTHER_SAN 1
#endif

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields in internal
// metadata cache structs embedded inside file structures.
#ifndef NDEBUG
#define H5FFAKE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "third_party/hdf5/src/H5FLprivate.h"
}  // extern "C"
#ifdef H5FFAKE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FFAKE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5FfakeTest, AllocZeroUsesDefaultSizeofSize) {
  ASSERT_GE(H5open(), 0);

  H5F_t* fake = H5F_fake_alloc(0);
  ASSERT_NE(fake, nullptr);
  absl::Cleanup free_fake = [&] { H5F_fake_free(fake); };

  ASSERT_NE(fake->shared, nullptr);
  EXPECT_EQ(fake->shared->sizeof_size, H5F_OBJ_SIZE_SIZE);
  EXPECT_EQ(H5F_sizeof_size(fake), H5F_OBJ_SIZE_SIZE);
}

TEST(H5FfakeTest, AllocExplicitSizeofSizePreservesValue) {
  ASSERT_GE(H5open(), 0);

  constexpr auto kSizes = std::to_array<uint8_t>({1, 2, 4, 8});
  for (uint8_t size : kSizes) {
    H5F_t* fake = H5F_fake_alloc(size);
    ASSERT_NE(fake, nullptr);
    absl::Cleanup free_fake = [&] { H5F_fake_free(fake); };

    ASSERT_NE(fake->shared, nullptr);
    EXPECT_EQ(fake->shared->sizeof_size, size);
    EXPECT_EQ(H5F_sizeof_size(fake), size);
  }
}

TEST(H5FfakeTest, FreeHandlesNullAndMissingSharedStruct) {
  ASSERT_GE(H5open(), 0);

  EXPECT_GE(H5F_fake_free(nullptr), 0);

  H5F_t* fake = H5F_fake_alloc(4);
  ASSERT_NE(fake, nullptr);
  ASSERT_NE(fake->shared, nullptr);
  fake->shared = H5FL_FREE(H5F_shared_t, fake->shared);
  EXPECT_EQ(fake->shared, nullptr);

  EXPECT_GE(H5F_fake_free(fake), 0);
}

#if !defined(H5FFAKE_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5FFAKE_TEST_HAS_OTHER_SAN)
TEST(H5FfakeTest, AllocReturnsNullWhenTopOrSharedAllocationFails) {
  ASSERT_GE(H5open(), 0);

  // Warm up and initialize both free lists, then drain cached nodes so
  // H5FL_CALLOC calls H5FL__malloc(head->size).
  {
    H5F_t* warmup = H5F_fake_alloc(4);
    ASSERT_NE(warmup, nullptr);
    EXPECT_GE(H5F_fake_free(warmup), 0);
  }
  ASSERT_GE(H5FL_garbage_coll(), 0);

  ScopedErrorSilence silence;

  // 1. Force top H5F_t allocation failure via impossibly large size.
  {
    const size_t orig_f_size = H5FL_REG_NAME(H5F_t).size;
    absl::Cleanup restore_f_size = [&] {
      H5FL_REG_NAME(H5F_t).size = orig_f_size;
    };
    H5FL_REG_NAME(H5F_t).size = std::numeric_limits<size_t>::max();
    EXPECT_EQ(H5F_fake_alloc(0), nullptr);
  }

  // 2. Allow H5F_t allocation to succeed and force f->shared allocation
  // failure, exercising H5F_fake_free cleanup in done:.
  ASSERT_GE(H5FL_garbage_coll(), 0);
  {
    const size_t orig_shared_size = H5FL_REG_NAME(H5F_shared_t).size;
    absl::Cleanup restore_shared_size = [&] {
      H5FL_REG_NAME(H5F_shared_t).size = orig_shared_size;
    };
    H5FL_REG_NAME(H5F_shared_t).size = std::numeric_limits<size_t>::max();
    EXPECT_EQ(H5F_fake_alloc(4), nullptr);
  }

  // Normal allocation continues to work cleanly once sizes are restored.
  H5F_t* fake = H5F_fake_alloc(8);
  ASSERT_NE(fake, nullptr);
  EXPECT_EQ(H5F_sizeof_size(fake), 8u);
  EXPECT_GE(H5F_fake_free(fake), 0);
  ASSERT_GE(H5FL_garbage_coll(), 0);
}
#endif

}  // namespace
}  // namespace hdf5
