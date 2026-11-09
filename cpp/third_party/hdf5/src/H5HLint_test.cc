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
// Tests internal local heap allocation, reference counting, and destruction
// routines (third_party/hdf5/src/H5HLint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HLint.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// inside H5HL_prfx_t / H5HL_dblk_t. Match those struct layouts across both the
// prelude and internal headers.
#ifndef NDEBUG
#define H5HLINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5HL_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5HLINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HLINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

TEST(H5HLintTest, NewInitializesFieldsAndDestFreesEmptyHeap) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                           /*prfx_size=*/32);
  ASSERT_NE(heap, nullptr);
  bool destroyed = false;
  absl::Cleanup guard = [&] {
    if (!destroyed) {
      H5HL__dest(heap);
    }
  };

  EXPECT_EQ(heap->rc, 0u);
  EXPECT_EQ(heap->prots, 0u);
  EXPECT_EQ(heap->sizeof_size, 8u);
  EXPECT_EQ(heap->sizeof_addr, 8u);
  EXPECT_EQ(heap->prfx_size, 32u);
  EXPECT_FALSE(heap->single_cache_obj);
  EXPECT_EQ(heap->freelist, nullptr);
  EXPECT_EQ(heap->prfx, nullptr);
  EXPECT_EQ(heap->dblk, nullptr);
  EXPECT_EQ(heap->dblk_image, nullptr);

  EXPECT_GE(H5HL__dest(heap), 0);
  destroyed = true;
}

TEST(H5HLintTest, IncAndDecRefCountDestroyHeapOnlyAtZero) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/4, /*sizeof_addr=*/4,
                           /*prfx_size=*/24);
  ASSERT_NE(heap, nullptr);

  ASSERT_GE(H5HL__inc_rc(heap), 0);
  EXPECT_EQ(heap->rc, 1u);
  ASSERT_GE(H5HL__inc_rc(heap), 0);
  EXPECT_EQ(heap->rc, 2u);

  // Decrementing from 2 -> 1 must keep the heap alive.
  ASSERT_GE(H5HL__dec_rc(heap), 0);
  EXPECT_EQ(heap->rc, 1u);
  EXPECT_EQ(heap->sizeof_size, 4u);
  EXPECT_EQ(heap->prfx_size, 24u);

  // Decrementing from 1 -> 0 destroys the heap via H5HL__dest().
  EXPECT_GE(H5HL__dec_rc(heap), 0);
}

TEST(H5HLintTest, DestFreesDataBlockImageAndMultiNodeFreelist) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                           /*prfx_size=*/32);
  ASSERT_NE(heap, nullptr);
  bool alive = true;
  absl::Cleanup cleanup = [&] {
    if (alive) {
      H5HL__dest(heap);
    }
  };

  heap->dblk_size = 128;
  heap->dblk_image = H5FL_BLK_CALLOC(lheap_chunk, 128);
  ASSERT_NE(heap->dblk_image, nullptr);

  auto* fl1 = H5FL_MALLOC(H5HL_free_t);
  ASSERT_NE(fl1, nullptr);
  auto* fl2 = H5FL_MALLOC(H5HL_free_t);
  ASSERT_NE(fl2, nullptr);

  fl1->offset = 16;
  fl1->size = 32;
  fl1->prev = nullptr;
  fl1->next = fl2;

  fl2->offset = 64;
  fl2->size = 64;
  fl2->prev = fl1;
  fl2->next = nullptr;
  heap->freelist = fl1;

  ASSERT_GE(H5HL__inc_rc(heap), 0);
  EXPECT_GE(H5HL__dec_rc(heap), 0);
  alive = false;
}

TEST(H5HLintTest, FileLocalHeapRoundTripExercisesNewRefCountAndDest) {
  ScopedFile file = CreateInMemoryFile("h5hlint_file.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &addr), 0);
    ASSERT_TRUE(H5_addr_defined(addr));

    H5HL_t* heap = H5HL_protect(f, addr, H5AC__NO_FLAGS_SET);
    ASSERT_NE(heap, nullptr);
    size_t offset = 0;
    constexpr char kPayload[] = "lheap_int_entry";
    ASSERT_GE(H5HL_insert(f, heap, sizeof(kPayload), kPayload, &offset), 0);
    EXPECT_STREQ(static_cast<const char*>(H5HL_offset_into(heap, offset)),
                 kPayload);
    ASSERT_GE(H5HL_unprotect(heap), 0);
  }

  // Closing the file evicts the prefix from cache, invoking H5HL__dec_rc ->
  // H5HL__dest.
  file.reset();
}

}  // namespace
}  // namespace hdf5
