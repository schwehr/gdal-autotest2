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
// Tests global heap collection creation, object allocation, read, extension,
// link-count management, removal, and public variable-length dataset callers
// (third_party/hdf5/src/H5HG.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HG.c

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug/stats fields from H5AC_info_t
// (the first member of H5HG_heap_t). Match those struct layouts across both
// the prelude and internal headers.
#ifndef NDEBUG
#define H5HG_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5HG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HG_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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

TEST(H5HGTest, InsertReadGetObjSizeAndRemoveLifecycle) {
  ScopedFile file = CreateInMemoryFile("h5hg_lifecycle.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Insert a zero-byte object (NULL buffer allowed) and two non-zero
  //    objects into the same collection.
  H5HG_t hobj_zero{};
  ASSERT_GE(H5HG_insert(f, 0, nullptr, &hobj_zero), 0);
  EXPECT_EQ(hobj_zero.idx, 1u);

  const std::vector<uint8_t> data1 = {1,  2,  3,  4,  5,  6,  7,  8, 9,
                                      10, 11, 12, 13, 14, 15, 16, 17};
  const std::vector<uint8_t> data2(24, 0x7E);
  H5HG_t hobj1{};
  H5HG_t hobj2{};
  ASSERT_GE(H5HG_insert(f, data1.size(), data1.data(), &hobj1), 0);
  ASSERT_GE(H5HG_insert(f, data2.size(), data2.data(), &hobj2), 0);
  EXPECT_EQ(hobj1.addr, hobj_zero.addr);
  EXPECT_EQ(hobj2.addr, hobj_zero.addr);
  EXPECT_EQ(hobj1.idx, 2u);
  EXPECT_EQ(hobj2.idx, 3u);

  // 2. Verify H5HG_get_obj_size and H5HG_read (caller-supplied with and
  //    without buf_size, and library-allocated buffer freed via H5MM_free).
  size_t obj_size = 999;
  ASSERT_GE(H5HG_get_obj_size(f, &hobj_zero, &obj_size), 0);
  EXPECT_EQ(obj_size, 0u);
  ASSERT_GE(H5HG_get_obj_size(f, &hobj1, &obj_size), 0);
  EXPECT_EQ(obj_size, data1.size());

  std::vector<uint8_t> read_buf(data1.size(), 0);
  size_t read_sz = 0;
  EXPECT_EQ(H5HG_read(f, &hobj1, read_buf.data(), &read_sz), read_buf.data());
  EXPECT_EQ(read_sz, data1.size());
  EXPECT_EQ(read_buf, data1);

  std::fill(read_buf.begin(), read_buf.end(), 0);
  EXPECT_EQ(H5HG_read(f, &hobj1, read_buf.data(), nullptr), read_buf.data());
  EXPECT_EQ(read_buf, data1);

  void* alloc_buf = H5HG_read(f, &hobj2, nullptr, &read_sz);
  ASSERT_NE(alloc_buf, nullptr);
  absl::Cleanup free_alloc = [&] { H5MM_free(alloc_buf); };
  EXPECT_EQ(read_sz, data2.size());
  EXPECT_EQ(std::memcmp(alloc_buf, data2.data(), data2.size()), 0);

  void* zero_alloc = H5HG_read(f, &hobj_zero, nullptr, &read_sz);
  ASSERT_NE(zero_alloc, nullptr);
  absl::Cleanup free_zero = [&] { H5MM_free(zero_alloc); };
  EXPECT_EQ(read_sz, 0u);

  // 3. Remove middle object (shifts hobj2 and free-space object down).
  ASSERT_GE(H5HG_remove(f, &hobj1), 0);
  // Double-remove on an already-removed entry in a non-empty collection is a
  // no-op returning SUCCEED (HDFFV-10635 workaround).
  EXPECT_GE(H5HG_remove(f, &hobj1), 0);

  // Remaining objects still read intact after shift.
  std::vector<uint8_t> read_buf2(data2.size(), 0);
  ASSERT_NE(H5HG_read(f, &hobj2, read_buf2.data(), &read_sz), nullptr);
  EXPECT_EQ(read_sz, data2.size());
  EXPECT_EQ(read_buf2, data2);

  // 4. Remove the remaining objects; removing the last one deletes the entire
  //    collection from cache and file free space.
  ASSERT_GE(H5HG_remove(f, &hobj_zero), 0);
  ASSERT_GE(H5HG_remove(f, &hobj2), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5HG_read(f, &hobj2, read_buf2.data(), &read_sz), nullptr);
  }

  // 5. Large object (> H5HG_MINSIZE) allocates a collection without clamping
  //    up to H5HG_MINSIZE, and 4056 bytes in a fresh collection exercises the
  //    < H5HG_SIZEOF_OBJHDR trailing free-space branch.
  const std::vector<uint8_t> huge(70000, 0x33);
  H5HG_t hobj_huge{};
  ASSERT_GE(H5HG_insert(f, huge.size(), huge.data(), &hobj_huge), 0);

  const std::vector<uint8_t> tight(4056, 0x44);
  H5HG_t hobj_tight{};
  ASSERT_GE(H5HG_insert(f, tight.size(), tight.data(), &hobj_tight), 0);
  EXPECT_NE(hobj_tight.addr, hobj_huge.addr);

  std::vector<uint8_t> tight_out(tight.size(), 0);
  ASSERT_NE(H5HG_read(f, &hobj_tight, tight_out.data(), &read_sz), nullptr);
  EXPECT_EQ(read_sz, tight.size());
  EXPECT_EQ(tight_out, tight);
}

TEST(H5HGTest, ExactFitFullHeapReadExtendAndRemove) {
  // 1. Single 4064-byte object in a 4096-byte collection consumes all 4080
  //    bytes of free space (obj[0].begin == NULL, obj[0].size == 0).
  {
    ScopedFile file = CreateInMemoryFile("h5hg_full1.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const std::vector<uint8_t> full_payload(4064, 0x6B);
    H5HG_t hobj{};
    ASSERT_GE(H5HG_insert(f, full_payload.size(), full_payload.data(), &hobj),
              0);

    // Read while obj[0].begin == NULL (skips CWFS advance).
    std::vector<uint8_t> out(full_payload.size(), 0);
    size_t read_sz = 0;
    ASSERT_NE(H5HG_read(f, &hobj, out.data(), &read_sz), nullptr);
    EXPECT_EQ(read_sz, full_payload.size());
    EXPECT_EQ(out, full_payload);

    // Extend backing file space and heap while obj[0].begin == NULL (creates
    // a new free block at old_size).
    ASSERT_GT(H5MF_try_extend(f, H5FD_MEM_GHEAP, hobj.addr, H5HG_MINSIZE, 4096),
              0);
    ASSERT_GE(H5HG_extend(f, hobj.addr, 4096), 0);
    const std::vector<uint8_t> extra(1024, 0x91);
    H5HG_t hobj2{};
    ASSERT_GE(H5HG_insert(f, extra.size(), extra.data(), &hobj2), 0);
    EXPECT_EQ(hobj2.addr, hobj.addr);
    EXPECT_EQ(hobj2.idx, 2u);
  }

  // 2. Two objects totaling 4080 bytes (2000+16 and 2048+16) fill the heap
  //    completely; removing one while obj[0].begin == NULL creates a new free
  //    space entry at the end of the chunk.
  {
    ScopedFile file = CreateInMemoryFile("h5hg_full2.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const std::vector<uint8_t> part1(2000, 0x11);
    const std::vector<uint8_t> part2(2048, 0x22);
    H5HG_t h1{};
    H5HG_t h2{};
    ASSERT_GE(H5HG_insert(f, part1.size(), part1.data(), &h1), 0);
    ASSERT_GE(H5HG_insert(f, part2.size(), part2.data(), &h2), 0);
    ASSERT_EQ(h1.addr, h2.addr);

    ASSERT_GE(H5HG_remove(f, &h1), 0);
    std::vector<uint8_t> out(part2.size(), 0);
    size_t read_sz = 0;
    ASSERT_NE(H5HG_read(f, &h2, out.data(), &read_sz), nullptr);
    EXPECT_EQ(read_sz, part2.size());
    EXPECT_EQ(out, part2);
  }
}

TEST(H5HGTest, ExtendWithRemovedSlotsShiftsLiveObjectsAndRejectsBadAddr) {
  ScopedFile file = CreateInMemoryFile("h5hg_extend.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const std::vector<uint8_t> d1(32, 0xA1);
  const std::vector<uint8_t> d2(32, 0xB2);
  const std::vector<uint8_t> d3(32, 0xC3);
  H5HG_t h1{};
  H5HG_t h2{};
  H5HG_t h3{};
  ASSERT_GE(H5HG_insert(f, d1.size(), d1.data(), &h1), 0);
  ASSERT_GE(H5HG_insert(f, d2.size(), d2.data(), &h2), 0);
  ASSERT_GE(H5HG_insert(f, d3.size(), d3.data(), &h3), 0);
  ASSERT_EQ(h1.addr, h2.addr);
  ASSERT_EQ(h1.addr, h3.addr);

  // Remove h2 so heap->obj[2].begin == NULL while live slots and obj[0] exist,
  // then extend backing space and the heap.
  ASSERT_GE(H5HG_remove(f, &h2), 0);
  ASSERT_GT(H5MF_try_extend(f, H5FD_MEM_GHEAP, h1.addr, H5HG_MINSIZE, 4096), 0);
  ASSERT_GE(H5HG_extend(f, h1.addr, 4096), 0);

  std::vector<uint8_t> out(32, 0);
  size_t read_sz = 0;
  ASSERT_NE(H5HG_read(f, &h1, out.data(), &read_sz), nullptr);
  EXPECT_EQ(out, d1);
  ASSERT_NE(H5HG_read(f, &h3, out.data(), &read_sz), nullptr);
  EXPECT_EQ(out, d3);

  ScopedErrorSilence silence;
  EXPECT_LT(H5HG_extend(f, 0x800000ULL, 4096), 0);
}

TEST(H5HGTest, LinkCountAdjustmentsAndBoundsChecks) {
  ScopedFile file = CreateInMemoryFile("h5hg_link.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const std::vector<uint8_t> payload = {4, 3, 2, 1};
  H5HG_t hobj{};
  H5HG_t hkeep{};
  ASSERT_GE(H5HG_insert(f, payload.size(), payload.data(), &hobj), 0);
  ASSERT_GE(H5HG_insert(f, payload.size(), payload.data(), &hkeep), 0);

  EXPECT_EQ(H5HG_link(f, &hobj, 0), 0);
  EXPECT_EQ(H5HG_link(f, &hobj, 1), 1);
  EXPECT_EQ(H5HG_link(f, &hobj, 4), 5);
  EXPECT_EQ(H5HG_link(f, &hobj, -2), 3);
  EXPECT_EQ(H5HG_link(f, &hobj, -3), 0);

  {
    ScopedErrorSilence silence;
    // Negative link count underflow.
    EXPECT_LT(H5HG_link(f, &hobj, -1), 0);
  }

  // Max link count is 65535 (H5HG_MAXLINK); exceeding it fails.
  EXPECT_EQ(H5HG_link(f, &hobj, 65535), 65535);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5HG_link(f, &hobj, 1), 0);
    EXPECT_EQ(H5HG_link(f, &hobj, 0), 65535);

    // Invalid idx==0, idx>=nused, bad addr, and removed object.
    H5HG_t bad_zero = {hobj.addr, 0};
    H5HG_t bad_oob = {hobj.addr, 999};
    H5HG_t bad_addr = {0x800000ULL, 1};
    EXPECT_LT(H5HG_link(f, &bad_zero, 1), 0);
    EXPECT_LT(H5HG_link(f, &bad_oob, 1), 0);
    EXPECT_LT(H5HG_link(f, &bad_addr, 1), 0);
  }

  ASSERT_GE(H5HG_remove(f, &hobj), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5HG_link(f, &hobj, 1), 0);
  }
}

TEST(H5HGTest, ErrorPathsOnReadOnlyFileInvalidHandlesAndSmallSizes) {
  std::vector<uint8_t> image;
  H5HG_t h1{};
  H5HG_t h2{};
  const std::vector<uint8_t> d1 = {10, 20, 30, 40};
  const std::vector<uint8_t> d2 = {50, 60, 70, 80};

  // 1. Populate writable file, test invalid handle errors and corrupt removed
  //    slot check, then capture file image for read-only tests.
  {
    ScopedFile file = CreateInMemoryFile("h5hg_err_rw.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    ASSERT_GE(H5HG_insert(f, d1.size(), d1.data(), &h1), 0);
    ASSERT_GE(H5HG_insert(f, d2.size(), d2.data(), &h2), 0);
    H5HG_t h3{};
    ASSERT_GE(H5HG_insert(f, d1.size(), d1.data(), &h3), 0);
    ASSERT_GE(H5HG_remove(f, &h3), 0);

    {
      ScopedErrorSilence silence;
      H5HG_t bad_zero = {h1.addr, 0};
      H5HG_t bad_oob = {h1.addr, 999};
      H5HG_t bad_addr = {0x800000ULL, 1};
      uint8_t scratch[16]{};
      size_t sz = 0;

      EXPECT_EQ(H5HG_read(f, &bad_zero, scratch, &sz), nullptr);
      EXPECT_EQ(H5HG_read(f, &bad_oob, scratch, &sz), nullptr);
      EXPECT_EQ(H5HG_read(f, &bad_addr, scratch, &sz), nullptr);
      EXPECT_EQ(H5HG_read(f, &h3, scratch, &sz), nullptr);

      EXPECT_LT(H5HG_get_obj_size(f, &bad_zero, &sz), 0);
      EXPECT_LT(H5HG_get_obj_size(f, &bad_oob, &sz), 0);
      EXPECT_LT(H5HG_get_obj_size(f, &bad_addr, &sz), 0);
      EXPECT_LT(H5HG_get_obj_size(f, &h3, &sz), 0);

      EXPECT_LT(H5HG_remove(f, &bad_zero), 0);
      EXPECT_LT(H5HG_remove(f, &bad_oob), 0);
      EXPECT_LT(H5HG_remove(f, &bad_addr), 0);

      // Force begin==NULL with nrefs!=0 on removed slot h3 to reach the
      // H5HG_remove "bad heap pointer" check past the HDFFV-10635 shortcut.
      H5HG_heap_t* heap = H5HG__protect(f, h1.addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(heap, nullptr);
      heap->obj[h3.idx].nrefs = 1;
      ASSERT_GE(
          H5AC_unprotect(f, H5AC_GHEAP, h1.addr, heap, H5AC__NO_FLAGS_SET), 0);
      EXPECT_LT(H5HG_remove(f, &h3), 0);

      heap = H5HG__protect(f, h1.addr, H5AC__NO_FLAGS_SET);
      ASSERT_NE(heap, nullptr);
      heap->obj[h3.idx].nrefs = 0;
      ASSERT_GE(
          H5AC_unprotect(f, H5AC_GHEAP, h1.addr, heap, H5AC__NO_FLAGS_SET), 0);
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    image.resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_len);
  }

  // 2. Read-only file rejects insert, link, and remove while allowing reads.
  {
    ScopedPlist ro_fapl = MakeCoreFapl();
    ASSERT_TRUE(ro_fapl.valid());
    ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
    ScopedFile ro_file(
        H5Fopen("h5hg_err_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
    ASSERT_TRUE(ro_file.valid());
    H5F_t* rf = GetH5File(ro_file.get());
    ASSERT_NE(rf, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    size_t sz = 0;
    EXPECT_GE(H5HG_get_obj_size(rf, &h1, &sz), 0);
    EXPECT_EQ(sz, d1.size());
    std::vector<uint8_t> out(d1.size(), 0);
    EXPECT_NE(H5HG_read(rf, &h1, out.data(), &sz), nullptr);
    EXPECT_EQ(out, d1);

    ScopedErrorSilence silence;
    H5HG_t new_obj{};
    EXPECT_LT(H5HG_insert(rf, d1.size(), d1.data(), &new_obj), 0);
    EXPECT_LT(H5HG_link(rf, &h1, 1), 0);
    EXPECT_LT(H5HG_remove(rf, &h1), 0);
  }

  // 3. Small 2-byte address/size file tests header alignment padding and clean
  //    failure when an oversized object exceeds 16-bit address space.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), 2, 2), 0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile tiny_file(
        H5Fcreate("h5hg_2byte.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(tiny_file.valid());
    H5F_t* tf = GetH5File(tiny_file.get());
    ASSERT_NE(tf, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5HG_t small_obj{};
    ASSERT_GE(H5HG_insert(tf, d1.size(), d1.data(), &small_obj), 0);
    std::vector<uint8_t> out(d1.size(), 0);
    size_t sz = 0;
    ASSERT_NE(H5HG_read(tf, &small_obj, out.data(), &sz), nullptr);
    EXPECT_EQ(out, d1);

    // Requesting a size that causes eoa + size address overflow makes
    // H5MF_alloc return HADDR_UNDEF inside H5HG__create before obj is read.
    constexpr size_t kOverflowSize = std::numeric_limits<size_t>::max() - 127;
    H5HG_t fail_obj{};
    ScopedErrorSilence silence;
    EXPECT_LT(H5HG_insert(tf, kOverflowSize, d1.data(), &fail_obj), 0);
  }
}

TEST(H5HGTest, AllocGrowsObjectTableAndReusesIdsPastMaxIdx) {
  auto make_two_obj_image = [](haddr_t* out_addr,
                               std::vector<uint8_t>* out_img) {
    ScopedFile file = CreateInMemoryFile("h5hg_patch_src.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    const std::vector<uint8_t> d1 = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint8_t> d2 = {9, 10, 11, 12, 13, 14, 15, 16};
    H5HG_t h1{};
    H5HG_t h2{};
    ASSERT_GE(H5HG_insert(f, d1.size(), d1.data(), &h1), 0);
    ASSERT_GE(H5HG_insert(f, d2.size(), d2.data(), &h2), 0);
    ASSERT_EQ(h1.addr, h2.addr);
    ASSERT_EQ(h1.idx, 1u);
    ASSERT_EQ(h2.idx, 2u);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    out_img->resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), out_img->data(), out_img->size()),
              image_len);
    *out_addr = h1.addr;
  };

  // 1. Patch obj2 idx to 513 -> deserialize sets nalloc=514 and nused=514.
  //    Subsequent H5HG_insert hits idx >= heap->nalloc inside H5HG__alloc.
  {
    haddr_t addr = HADDR_UNDEF;
    std::vector<uint8_t> img;
    make_two_obj_image(&addr, &img);
    ASSERT_LT(addr + 42, img.size());
    ASSERT_EQ(std::memcmp(&img[addr], "GCOL", 4), 0);
    ASSERT_EQ(img[addr + 40], 2);
    ASSERT_EQ(img[addr + 41], 0);
    img[addr + 40] = static_cast<uint8_t>(513 & 0xff);
    img[addr + 41] = static_cast<uint8_t>((513 >> 8) & 0xff);

    ScopedPlist rw_fapl = MakeCoreFapl();
    ASSERT_TRUE(rw_fapl.valid());
    ASSERT_GE(H5Pset_file_image(rw_fapl.get(), img.data(), img.size()), 0);
    ScopedFile rw_file(H5Fopen("h5hg_grow.h5", H5F_ACC_RDWR, rw_fapl.get()));
    ASSERT_TRUE(rw_file.valid());
    H5F_t* f = GetH5File(rw_file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5HG_t patched = {addr, 513};
    std::vector<uint8_t> out(8, 0);
    size_t sz = 0;
    ASSERT_NE(H5HG_read(f, &patched, out.data(), &sz), nullptr);
    EXPECT_THAT(out, ElementsAre(9, 10, 11, 12, 13, 14, 15, 16));

    const std::vector<uint8_t> d3 = {21, 22, 23, 24, 25, 26, 27, 28};
    H5HG_t h3{};
    ASSERT_GE(H5HG_insert(f, d3.size(), d3.data(), &h3), 0);
    EXPECT_EQ(h3.addr, addr);
    EXPECT_EQ(h3.idx, 514u);
    ASSERT_NE(H5HG_read(f, &h3, out.data(), &sz), nullptr);
    EXPECT_EQ(out, d3);
  }

  // 2. Patch obj2 idx to 65535 (H5HG_MAXIDX) -> nused == 65536. Subsequent
  //    H5HG_insert enters the > H5HG_MAXIDX slot scan, skips occupied idx 1,
  //    and reuses empty idx 2.
  {
    haddr_t addr = HADDR_UNDEF;
    std::vector<uint8_t> img;
    make_two_obj_image(&addr, &img);
    ASSERT_LT(addr + 42, img.size());
    ASSERT_EQ(img[addr + 40], 2);
    ASSERT_EQ(img[addr + 41], 0);
    img[addr + 40] = 0xff;
    img[addr + 41] = 0xff;

    ScopedPlist rw_fapl = MakeCoreFapl();
    ASSERT_TRUE(rw_fapl.valid());
    ASSERT_GE(H5Pset_file_image(rw_fapl.get(), img.data(), img.size()), 0);
    ScopedFile rw_file(H5Fopen("h5hg_wrap.h5", H5F_ACC_RDWR, rw_fapl.get()));
    ASSERT_TRUE(rw_file.valid());
    H5F_t* f = GetH5File(rw_file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5HG_t patched = {addr, 65535};
    std::vector<uint8_t> out(8, 0);
    size_t sz = 0;
    ASSERT_NE(H5HG_read(f, &patched, out.data(), &sz), nullptr);

    const std::vector<uint8_t> d3 = {31, 32, 33, 34, 35, 36, 37, 38};
    H5HG_t h3{};
    ASSERT_GE(H5HG_insert(f, d3.size(), d3.data(), &h3), 0);
    EXPECT_EQ(h3.addr, addr);
    EXPECT_EQ(h3.idx, 2u);
    ASSERT_NE(H5HG_read(f, &h3, out.data(), &sz), nullptr);
    EXPECT_EQ(out, d3);
  }
}

TEST(H5HGTest, PublicVariableLengthDatasetExercisesOverwriteAndReopen) {
  std::vector<uint8_t> image;

  std::vector<int32_t> row0 = {1, 2, 3};
  std::vector<int32_t> row1(1200);
  std::iota(row1.begin(), row1.end(), 100);
  std::vector<int32_t> row2 = {42, 43};

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile file(
        H5Fcreate("h5hg_vlen_pub.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedType vl_type(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_type.valid());
    const hsize_t dims[1] = {3};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "vl_seqs", vl_type.get(),
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    hvl_t write_data[3] = {
        {.len = row0.size(), .p = row0.data()},
        {.len = row1.size(), .p = row1.data()},
        {.len = row2.size(), .p = row2.data()},
    };
    ASSERT_GE(H5Dwrite(dset.get(), vl_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write_data),
              0);

    // Overwrite dataset to trigger H5HG_remove + H5HG_insert on VL blobs.
    row0 = {7, 8, 9, 10};
    write_data[0] = {.len = row0.size(), .p = row0.data()};
    ASSERT_GE(H5Dwrite(dset.get(), vl_type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       write_data),
              0);

    dset.reset();
    space.reset();
    vl_type.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    const ssize_t image_len = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_len, 0);
    image.resize(static_cast<size_t>(image_len));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_len);
  }

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("h5hg_vlen_pub_ro.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedDataset dset(H5Dopen2(ro_file.get(), "vl_seqs", H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType dtype(H5Dget_type(dset.get()));
  ASSERT_TRUE(dtype.valid());
  ScopedSpace space(H5Dget_space(dset.get()));
  ASSERT_TRUE(space.valid());

  hvl_t read_data[3]{};
  absl::Cleanup reclaim = [&] {
    H5Dvlen_reclaim(dtype.get(), space.get(), H5P_DEFAULT, read_data);
  };
  ASSERT_GE(H5Dread(dset.get(), dtype.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_data),
            0);

  ASSERT_EQ(read_data[0].len, row0.size());
  EXPECT_EQ(
      std::memcmp(read_data[0].p, row0.data(), row0.size() * sizeof(int32_t)),
      0);
  ASSERT_EQ(read_data[1].len, row1.size());
  EXPECT_EQ(
      std::memcmp(read_data[1].p, row1.data(), row1.size() * sizeof(int32_t)),
      0);
  ASSERT_EQ(read_data[2].len, row2.size());
  EXPECT_EQ(
      std::memcmp(read_data[2].p, row2.data(), row2.size() * sizeof(int32_t)),
      0);
}

}  // namespace
}  // namespace hdf5
