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
// Tests SOHM index record comparison, encoding, and decoding (both in-heap and
// in-OH storage, fast-path matches, hash ordering, hash-collision bytewise
// comparisons, dirty OH message flushing, and error handling) in
// third_party/hdf5/src/H5SMmessage.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SMmessage.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t and
// H5O_t. Define NDEBUG around internal headers so struct offsets match `:hdf5`.
#ifndef NDEBUG
#define H5SMMESSAGE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try h5b2_try_param
extern "C" {
#define H5O_FRIEND
#define H5SM_FRIEND
#define H5S_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HFprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SMMESSAGE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SMMESSAGE_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SOHM_TAG) {
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_t* CreateTestHeap(H5F_t* f) {
  H5HF_create_t cparam{};
  cparam.managed.width = H5O_FHEAP_MAN_WIDTH;
  cparam.managed.start_block_size = H5O_FHEAP_MAN_START_BLOCK_SIZE;
  cparam.managed.max_direct_size = H5O_FHEAP_MAN_MAX_DIRECT_SIZE;
  cparam.managed.max_index = H5O_FHEAP_MAN_MAX_INDEX;
  cparam.managed.start_root_rows = H5O_FHEAP_MAN_START_ROOT_ROWS;
  cparam.checksum_dblocks = H5O_FHEAP_CHECKSUM_DBLOCKS;
  cparam.id_len = 0;
  cparam.max_man_size = H5O_FHEAP_MAX_MAN_SIZE;
  return H5HF_create(f, &cparam);
}

TEST(H5SMmessageTest, EncodeDecodeRoundTripHeapAndOhRecords) {
  for (const uint8_t addr_sz : {uint8_t{4}, uint8_t{8}}) {
    H5SM_bt2_ctx_t ctx{};
    ctx.sizeof_addr = addr_sz;

    H5SM_sohm_t heap_in{};
    heap_in.location = H5SM_IN_HEAP;
    heap_in.hash = 0xdeadbeefU;
    heap_in.u.heap_loc.ref_count = 42;
    heap_in.u.heap_loc.fheap_id.val = 0x0123456789abcdefULL;

    std::array<uint8_t, 64> raw{};
    ASSERT_GE(H5SM__message_encode(raw.data(), &heap_in, &ctx), 0);

    H5SM_sohm_t heap_out{};
    ASSERT_GE(H5SM__message_decode(raw.data(), &heap_out, &ctx), 0);
    EXPECT_EQ(heap_out.location, H5SM_IN_HEAP);
    EXPECT_EQ(heap_out.hash, heap_in.hash);
    EXPECT_EQ(heap_out.u.heap_loc.ref_count, 42u);
    EXPECT_EQ(heap_out.u.heap_loc.fheap_id.val, heap_in.u.heap_loc.fheap_id.val);

    H5SM_sohm_t oh_in{};
    oh_in.location = H5SM_IN_OH;
    oh_in.hash = 0xcafebabeU;
    oh_in.msg_type_id = H5O_FILL_NEW_ID;
    oh_in.u.mesg_loc.index = 7;
    oh_in.u.mesg_loc.oh_addr =
        (addr_sz == 8) ? haddr_t{0x0011223344556677ULL} : haddr_t{0x12345678U};

    raw.fill(0);
    ASSERT_GE(H5SM__message_encode(raw.data(), &oh_in, &ctx), 0);

    H5SM_sohm_t oh_out{};
    ASSERT_GE(H5SM__message_decode(raw.data(), &oh_out, &ctx), 0);
    EXPECT_EQ(oh_out.location, H5SM_IN_OH);
    EXPECT_EQ(oh_out.hash, oh_in.hash);
    EXPECT_EQ(oh_out.msg_type_id, H5O_FILL_NEW_ID);
    EXPECT_EQ(oh_out.u.mesg_loc.index, 7u);
    EXPECT_EQ(oh_out.u.mesg_loc.oh_addr, oh_in.u.mesg_loc.oh_addr);
  }
}

TEST(H5SMmessageTest, CompareFastPathsHashOrderingAndHeapCollisions) {
  ScopedFile file = CreateInMemoryFile("sohm_msg_cmp_heap.h5");
  ASSERT_TRUE(file.valid());
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  H5HF_t* fheap = CreateTestHeap(f);
  ASSERT_NE(fheap, nullptr);
  absl::Cleanup close_heap = [&] {
    if (fheap != nullptr) {
      H5HF_close(fheap);
    }
  };

  const std::vector<uint8_t> stored_payload = {
      0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  };
  H5O_fheap_id_t fheap_id{};
  ASSERT_GE(
      H5HF_insert(fheap, stored_payload.size(), stored_payload.data(), &fheap_id),
      0);

  H5SM_sohm_t heap_rec{};
  heap_rec.location = H5SM_IN_HEAP;
  heap_rec.hash = 1000U;
  heap_rec.u.heap_loc.ref_count = 2;
  heap_rec.u.heap_loc.fheap_id = fheap_id;

  // 1. Fast-path IN_HEAP match ignores hash when fheap_id matches.
  H5SM_mesg_key_t key{};
  key.file = f;
  key.fheap = fheap;
  key.message.location = H5SM_IN_HEAP;
  key.message.hash = 9999U;
  key.message.u.heap_loc.fheap_id = fheap_id;
  int cmp = -99;
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  // Mismatched fheap_id falls through to hash ordering.
  key.message.u.heap_loc.fheap_id.val = fheap_id.val ^ 0xffULL;
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, 1);

  key.message.hash = 500U;
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, -1);

  // 2. Fast-path IN_OH match & mismatch fallthrough.
  H5SM_sohm_t oh_rec{};
  oh_rec.location = H5SM_IN_OH;
  oh_rec.hash = 2000U;
  oh_rec.msg_type_id = H5O_SDSPACE_ID;
  oh_rec.u.mesg_loc.oh_addr = 0x5000U;
  oh_rec.u.mesg_loc.index = 1U;

  key.message.location = H5SM_IN_OH;
  key.message.hash = 9999U;
  key.message.msg_type_id = H5O_SDSPACE_ID;
  key.message.u.mesg_loc.oh_addr = 0x5000U;
  key.message.u.mesg_loc.index = 1U;
  ASSERT_GE(H5SM__message_compare(&key, &oh_rec, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  // Changing oh_addr, index, or msg_type_id bypasses fast path -> compares hash.
  key.message.u.mesg_loc.oh_addr = 0x6000U;
  ASSERT_GE(H5SM__message_compare(&key, &oh_rec, &cmp), 0);
  EXPECT_EQ(cmp, 1);

  key.message.u.mesg_loc.oh_addr = 0x5000U;
  key.message.u.mesg_loc.index = 2U;
  key.message.hash = 100U;
  ASSERT_GE(H5SM__message_compare(&key, &oh_rec, &cmp), 0);
  EXPECT_EQ(cmp, -1);

  key.message.u.mesg_loc.index = 1U;
  key.message.msg_type_id = H5O_DTYPE_ID;
  ASSERT_GE(H5SM__message_compare(&key, &oh_rec, &cmp), 0);
  EXPECT_EQ(cmp, -1);

  // 3. Hash collision against H5SM_IN_HEAP exercises H5SM__compare_cb branches.
  key.message.location = H5SM_NO_LOC;
  key.message.hash = heap_rec.hash;

  std::vector<uint8_t> same_bytes = stored_payload;
  key.encoding = same_bytes.data();
  key.encoding_size = same_bytes.size();
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  std::vector<uint8_t> diff_larger_byte = stored_payload;
  diff_larger_byte[5] = 0xff;
  key.encoding = diff_larger_byte.data();
  key.encoding_size = diff_larger_byte.size();
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_GT(cmp, 0);

  std::vector<uint8_t> larger_len(stored_payload.size() + 4, 0x10);
  key.encoding = larger_len.data();
  key.encoding_size = larger_len.size();
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, 1);

  std::vector<uint8_t> smaller_len(stored_payload.size() - 4, 0x10);
  key.encoding = smaller_len.data();
  key.encoding_size = smaller_len.size();
  ASSERT_GE(H5SM__message_compare(&key, &heap_rec, &cmp), 0);
  EXPECT_EQ(cmp, -1);

  // H5HF_op error on invalid fheap_id propagates cleanly.
  {
    ScopedErrorSilence silence;
    H5SM_sohm_t bad_heap_rec = heap_rec;
    bad_heap_rec.u.heap_loc.fheap_id.val = 0xffffffffffffffffULL;
    key.encoding = same_bytes.data();
    key.encoding_size = same_bytes.size();
    EXPECT_LT(H5SM__message_compare(&key, &bad_heap_rec, &cmp), 0);
  }
}

TEST(H5SMmessageTest, CompareOhHashCollisionsSequenceDirtyAndErrors) {
  for (const H5F_libver_t ver : {H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST}) {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    const H5F_libver_t high =
        (ver == H5F_LIBVER_EARLIEST) ? H5F_LIBVER_V18 : ver;
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), ver, high), 0);
    ScopedFile file(H5Fcreate(
        ver == H5F_LIBVER_LATEST ? "sohm_msg_oh_v2.h5" : "sohm_msg_oh_v1.h5",
        H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    // Create a dataset so its object header contains an SDSPACE message, then
    // encode an identical SDSPACE copy for hash-collision comparison.
    const std::array<hsize_t, 2> dims0 = {4, 5};
    ScopedSpace space0(H5Screate_simple(2, dims0.data(), nullptr));
    ASSERT_TRUE(space0.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/ds", H5T_NATIVE_INT,
                                  space0.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5O_loc_t* oloc = H5O_get_loc(dset.get());
    ASSERT_NE(oloc, nullptr);
    H5F_t* f = oloc->file;
    ASSERT_NE(f, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    // Read the SDSPACE message and encode it for exact bytewise matching.
    H5S_extent_t ext0{};
    ASSERT_NE(H5O_msg_read(oloc, H5O_SDSPACE_ID, &ext0), nullptr);
    absl::Cleanup reset_ext0 = [&] { H5O_msg_reset(H5O_SDSPACE_ID, &ext0); };
    const size_t raw_sz0 = H5O_msg_raw_size(f, H5O_SDSPACE_ID, true, &ext0);
    ASSERT_GT(raw_sz0, 0u);
    std::vector<uint8_t> enc0(raw_sz0, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, true, enc0.data(), &ext0), 0);

    // Append a second SDSPACE message of different size so sequence == 0 is
    // skipped (`sequence == udata->idx` false branch) when searching index 1,
    // and ensure a message stays dirty in cache (`mesg->dirty == true` branch)
    // before flushing.
    const std::array<hsize_t, 4> dims1 = {2, 3, 4, 5};
    ScopedSpace space1(H5Screate_simple(4, dims1.data(), nullptr));
    ASSERT_TRUE(space1.valid());
    H5O_loc_t* oloc_dset2 = nullptr;
    ScopedDataset dset2(H5Dcreate2(file.get(), "/ds_big", H5T_NATIVE_INT,
                                   space1.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(dset2.valid());
    oloc_dset2 = H5O_get_loc(dset2.get());
    ASSERT_NE(oloc_dset2, nullptr);

    H5S_extent_t ext1{};
    ASSERT_NE(H5O_msg_read(oloc_dset2, H5O_SDSPACE_ID, &ext1), nullptr);
    absl::Cleanup reset_ext1 = [&] { H5O_msg_reset(H5O_SDSPACE_ID, &ext1); };
    const size_t raw_sz1 = H5O_msg_raw_size(f, H5O_SDSPACE_ID, true, &ext1);
    ASSERT_GT(raw_sz1, raw_sz0);
    std::vector<uint8_t> enc1(raw_sz1, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, true, enc1.data(), &ext1), 0);

    // Append `ext1` as a second SDSPACE message onto `dset`'s OH, then leave
    // `mesg->dirty == true` un-flushed in cache so H5SM__compare_iter_op
    // flushes it via H5O_msg_flush(). Note: close dset handles only after test
    // comparisons; protect/unprotect with NO_FLAGS_SET preserves dirty flag on
    // the H5O_mesg_t until cache serialization.
    {
      H5O_t* oh = H5O_protect(oloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(oh, nullptr);
      const herr_t app_rc =
          H5O_msg_append_oh(f, oh, H5O_SDSPACE_ID, 0, 0, &ext1);
      // Mark the existing first SDSPACE message dirty as well before
      // releasing `oh` without resetting its messages.
      for (size_t m = 0; m < oh->nmesgs; ++m) {
        if (oh->mesg[m].type != nullptr &&
            oh->mesg[m].type->id == H5O_SDSPACE_ID) {
          oh->mesg[m].dirty = true;
        }
      }
      ASSERT_GE(H5O_unprotect(oloc, oh, H5AC__DIRTIED_FLAG), 0);
      ASSERT_GE(app_rc, 0);
    }

    // Compare against message at index 1 (`sequence == 0` skips to `sequence ==
    // 1`, hits dirty -> H5O_msg_flush(), and matches `enc1`).
    H5SM_sohm_t oh_rec1{};
    oh_rec1.location = H5SM_IN_OH;
    oh_rec1.hash = 0x42424242U;
    oh_rec1.msg_type_id = H5O_SDSPACE_ID;
    oh_rec1.u.mesg_loc.oh_addr = oloc->addr;
    oh_rec1.u.mesg_loc.index = 1U;

    H5SM_mesg_key_t key{};
    key.file = f;
    key.message.location = H5SM_NO_LOC;
    key.message.hash = oh_rec1.hash;
    key.encoding = enc1.data();
    key.encoding_size = enc1.size();

    int cmp = -99;
    ASSERT_GE(H5SM__message_compare(&key, &oh_rec1, &cmp), 0);
    EXPECT_EQ(cmp, 0);

    // Compare against message at index 0 (`enc0` vs `enc0` matches, `enc1` is
    // larger -> +1, and tiny encoding is smaller -> -1 against index 1).
    H5SM_sohm_t oh_rec0 = oh_rec1;
    oh_rec0.u.mesg_loc.index = 0U;

    key.encoding = enc0.data();
    key.encoding_size = enc0.size();
    ASSERT_GE(H5SM__message_compare(&key, &oh_rec0, &cmp), 0);
    EXPECT_EQ(cmp, 0);

    // Larger aligned_encoded_size > mesg->raw_size -> +1.
    std::vector<uint8_t> oversize(raw_sz1 + 64, 0xaa);
    key.encoding = oversize.data();
    key.encoding_size = oversize.size();
    ASSERT_GE(H5SM__message_compare(&key, &oh_rec0, &cmp), 0);
    EXPECT_EQ(cmp, 1);

    // Smaller aligned_encoded_size < mesg->raw_size -> -1.
    std::vector<uint8_t> undersize(2, 0x11);
    key.encoding = undersize.data();
    key.encoding_size = undersize.size();
    ASSERT_GE(H5SM__message_compare(&key, &oh_rec1, &cmp), 0);
    EXPECT_EQ(cmp, -1);

    // Equal size, different bytes -> memcmp non-zero.
    std::vector<uint8_t> diff_same_sz = enc0;
    diff_same_sz.back() ^= 0x01;
    key.encoding = diff_same_sz.data();
    key.encoding_size = diff_same_sz.size();
    ASSERT_GE(H5SM__message_compare(&key, &oh_rec0, &cmp), 0);
    EXPECT_NE(cmp, 0);

    // Invalid object header address -> H5O_msg_iterate error propagation.
    {
      ScopedErrorSilence silence;
      H5SM_sohm_t bad_oh = oh_rec0;
      bad_oh.u.mesg_loc.oh_addr = 0x7fffffff0ULL;
      key.encoding = enc0.data();
      key.encoding_size = enc0.size();
      EXPECT_LT(H5SM__message_compare(&key, &bad_oh, &cmp), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
