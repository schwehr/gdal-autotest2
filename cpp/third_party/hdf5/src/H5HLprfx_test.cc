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
// Tests internal local heap prefix allocation and destruction routines
// (third_party/hdf5/src/H5HLprfx.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HLprfx.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5HL_prfx_t). Match those struct layouts across both the
// prelude and internal headers.
#ifndef NDEBUG
#define H5HLPRFX_TEST_RESTORE_UNDEF_NDEBUG 1
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
#ifdef H5HLPRFX_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HLPRFX_TEST_RESTORE_UNDEF_NDEBUG
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

TEST(H5HLprfxTest, PrfxNewAndDestLinkAndDestroyHeapOnLastRef) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                           /*prfx_size=*/32);
  ASSERT_NE(heap, nullptr);

  H5HL_prfx_t* prfx = H5HL__prfx_new(heap);
  ASSERT_NE(prfx, nullptr);
  EXPECT_EQ(prfx->heap, heap);
  EXPECT_EQ(heap->prfx, prfx);
  EXPECT_EQ(heap->rc, 1u);

  // Destroying the sole prefix decrements heap->rc to 0 and destroys heap.
  EXPECT_GE(H5HL__prfx_dest(prfx), 0);
}

TEST(H5HLprfxTest, PrfxDestPreservesHeapWhileOtherRefsRemain) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                           /*prfx_size=*/32);
  ASSERT_NE(heap, nullptr);
  ASSERT_GE(H5HL__inc_rc(heap), 0);
  absl::Cleanup dec_heap = [&] {
    if (heap != nullptr) {
      H5HL__dec_rc(heap);
    }
  };

  H5HL_prfx_t* prfx = H5HL__prfx_new(heap);
  ASSERT_NE(prfx, nullptr);
  EXPECT_EQ(heap->rc, 2u);

  ASSERT_GE(H5HL__prfx_dest(prfx), 0);
  EXPECT_EQ(heap->prfx, nullptr);
  EXPECT_EQ(heap->rc, 1u);

  ASSERT_GE(H5HL__dec_rc(heap), 0);
  heap = nullptr;
}

TEST(H5HLprfxTest, PrfxDestHandlesUnlinkedPrefixWithoutHeap) {
  ASSERT_GE(H5open(), 0);

  H5HL_t* heap = H5HL__new(/*sizeof_size=*/8, /*sizeof_addr=*/8,
                           /*prfx_size=*/32);
  ASSERT_NE(heap, nullptr);
  ASSERT_GE(H5HL__inc_rc(heap), 0);
  absl::Cleanup dec_heap = [&] {
    if (heap != nullptr) {
      H5HL__dec_rc(heap);
    }
  };

  H5HL_prfx_t* prfx = H5HL__prfx_new(heap);
  ASSERT_NE(prfx, nullptr);
  ASSERT_EQ(heap->rc, 2u);

  // Unlink manually so H5HL__prfx_dest exercises its `prfx->heap == nullptr`
  // branch cleanly without leaking heap or refcounts.
  heap->prfx = nullptr;
  prfx->heap = nullptr;
  ASSERT_GE(H5HL__dec_rc(heap), 0);
  EXPECT_EQ(heap->rc, 1u);

  EXPECT_GE(H5HL__prfx_dest(prfx), 0);

  ASSERT_GE(H5HL__dec_rc(heap), 0);
  heap = nullptr;
}

TEST(H5HLprfxTest, FileLocalHeapCreateAndCloseExercisesPrfxLifecycle) {
  ScopedFile file = CreateInMemoryFile("h5hlprfx_file.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  {
    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    haddr_t addr = HADDR_UNDEF;
    ASSERT_GE(H5HL_create(f, /*size_hint=*/64, &addr), 0);
    ASSERT_TRUE(H5_addr_defined(addr));

    H5HL_t* heap = H5HL_protect(f, addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(heap, nullptr);
    EXPECT_NE(heap->prfx, nullptr);
    ASSERT_GE(H5HL_unprotect(heap), 0);
  }

  file.reset();
}

}  // namespace
}  // namespace hdf5
