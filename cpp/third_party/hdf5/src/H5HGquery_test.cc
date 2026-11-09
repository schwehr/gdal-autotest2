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
// Tests internal global heap query accessors H5HG_get_addr, H5HG_get_size, and
// H5HG_get_free_size (third_party/hdf5/src/H5HGquery.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HGquery.c

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields from H5AC_info_t
// (the first member of H5HG_heap_t). Match those struct layouts across both
// the prelude and internal headers.
#ifndef NDEBUG
#define H5HGQUERY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5HG_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HGpkg.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5HGQUERY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HGQUERY_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__GLOBALHEAP_TAG) {
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

TEST(H5HGqueryTest, SyntheticHeapQueriesReturnAddressSizeAndFreeSize) {
  struct Case {
    haddr_t addr;
    size_t size;
    size_t free_size;
  };
  constexpr Case kCases[] = {
      {.addr = 0, .size = 4096, .free_size = 4080},
      {.addr = 0x2000, .size = 8192, .free_size = 128},
      {.addr = 0x123456, .size = 65536, .free_size = 0},
      {.addr = HADDR_UNDEF, .size = 4096, .free_size = 4096},
  };

  for (const Case& tc : kCases) {
    H5HG_obj_t obj0{};
    obj0.size = tc.free_size;

    H5HG_heap_t heap{};
    heap.addr = tc.addr;
    heap.size = tc.size;
    heap.obj = &obj0;

    EXPECT_EQ(H5HG_get_addr(&heap), tc.addr);
    EXPECT_EQ(H5HG_get_size(&heap), tc.size);
    EXPECT_EQ(H5HG_get_free_size(&heap), tc.free_size);

    // Outside H5HG_MODULE, H5HG_* query macros dispatch to H5HG_get_*.
    EXPECT_EQ(H5HG_ADDR(&heap), tc.addr);
    EXPECT_EQ(H5HG_SIZE(&heap), tc.size);
    EXPECT_EQ(H5HG_FREE_SIZE(&heap), tc.free_size);
  }
}

TEST(H5HGqueryTest, LiveProtectedHeapReflectsInsertExtendAndRemove) {
  ScopedFile file = CreateInMemoryFile("h5hgquery_live.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Insert two objects into a new 4096-byte collection and query its layout.
  const std::vector<uint8_t> payload1(100, 0x1A);
  const std::vector<uint8_t> payload2(200, 0x2B);
  H5HG_t hobj1{};
  H5HG_t hobj2{};
  ASSERT_GE(H5HG_insert(f, payload1.size(), payload1.data(), &hobj1), 0);
  ASSERT_GE(H5HG_insert(f, payload2.size(), payload2.data(), &hobj2), 0);
  ASSERT_EQ(hobj2.addr, hobj1.addr);

  const size_t hdr_size = H5HG_SIZEOF_HDR(f);
  const size_t obj1_need = H5HG_SIZEOF_OBJHDR(f) + H5HG_ALIGN(payload1.size());
  const size_t obj2_need = H5HG_SIZEOF_OBJHDR(f) + H5HG_ALIGN(payload2.size());

  size_t free_after_insert = 0;
  {
    H5HG_heap_t* heap = H5HG__protect(f, hobj1.addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprotect = [&] {
      EXPECT_GE(
          H5AC_unprotect(f, H5AC_GHEAP, hobj1.addr, heap, H5AC__NO_FLAGS_SET),
          0);
    };
    EXPECT_EQ(H5HG_get_addr(heap), hobj1.addr);
    EXPECT_EQ(H5HG_get_size(heap), size_t{H5HG_MINSIZE});
    free_after_insert = H5HG_get_free_size(heap);
    EXPECT_EQ(free_after_insert,
              size_t{H5HG_MINSIZE} - hdr_size - obj1_need - obj2_need);
  }

  // 2. Extend the backing file space and collection and verify size and
  //    free_size grow accordingly.
  constexpr size_t kExtendBytes = 4096;
  ASSERT_GT(H5MF_try_extend(f, H5FD_MEM_GHEAP, hobj1.addr, H5HG_MINSIZE,
                            kExtendBytes),
            0);
  ASSERT_GE(H5HG_extend(f, hobj1.addr, kExtendBytes), 0);
  {
    H5HG_heap_t* heap = H5HG__protect(f, hobj1.addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprotect = [&] {
      EXPECT_GE(
          H5AC_unprotect(f, H5AC_GHEAP, hobj1.addr, heap, H5AC__NO_FLAGS_SET),
          0);
    };
    EXPECT_EQ(H5HG_get_addr(heap), hobj1.addr);
    EXPECT_EQ(H5HG_get_size(heap), size_t{H5HG_MINSIZE} + kExtendBytes);
    EXPECT_EQ(H5HG_get_free_size(heap), free_after_insert + kExtendBytes);
  }

  // 3. Remove the first object (keeping the second so the heap stays alive) and
  //    verify free_size reclaims obj1_need bytes.
  ASSERT_GE(H5HG_remove(f, &hobj1), 0);
  {
    H5HG_heap_t* heap = H5HG__protect(f, hobj2.addr, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(heap, nullptr);
    absl::Cleanup unprotect = [&] {
      EXPECT_GE(
          H5AC_unprotect(f, H5AC_GHEAP, hobj2.addr, heap, H5AC__NO_FLAGS_SET),
          0);
    };
    EXPECT_EQ(H5HG_get_addr(heap), hobj2.addr);
    EXPECT_EQ(H5HG_get_size(heap), size_t{H5HG_MINSIZE} + kExtendBytes);
    EXPECT_EQ(H5HG_get_free_size(heap),
              free_after_insert + kExtendBytes + obj1_need);
  }
}

}  // namespace
}  // namespace hdf5
