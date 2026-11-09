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
// Tests the SOHM v2 B-tree index class callbacks (context creation/destruction,
// record storage, and heap/OH record debug formatting) in
// third_party/hdf5/src/H5SMbtree2.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SMbtree2.c

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from internal cache
// and object header structs. Define NDEBUG around internal headers so struct
// layouts match `:hdf5`.
#ifndef NDEBUG
#define H5SMBTREE2_TEST_RESTORE_UNDEF_NDEBUG 1
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
#define H5SM_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5B2private.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SMBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SMBTREE2_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

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

class ScopedB2 {
 public:
  ScopedB2() = default;
  explicit ScopedB2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedB2() { reset(); }

  ScopedB2(const ScopedB2&) = delete;
  ScopedB2& operator=(const ScopedB2&) = delete;

  void reset(H5B2_t* next = nullptr) {
    if (bt2_ != nullptr) {
      H5B2_close(bt2_);
    }
    bt2_ = next;
  }

  H5B2_t* get() const { return bt2_; }

 private:
  H5B2_t* bt2_ = nullptr;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string CaptureBtreeDebug(const H5SM_sohm_t& record, const void* ctx) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  const herr_t rc =
      H5SM_INDEX[0].debug(stream, /*indent=*/2, /*fwidth=*/24, &record, ctx);
  std::fclose(stream);
  if (rc < 0) {
    return "";
  }
  return std::string(buf != nullptr ? buf : "", len);
}

TEST(H5SMbtree2Test, ClassMetadataAndDirectCallbacks) {
  EXPECT_EQ(H5SM_INDEX[0].id, H5B2_SOHM_INDEX_ID);
  EXPECT_STREQ(H5SM_INDEX[0].name, "H5B2_SOHM_INDEX_ID");
  EXPECT_EQ(H5SM_INDEX[0].nrec_size, sizeof(H5SM_sohm_t));
  EXPECT_NE(H5SM_INDEX[0].crt_context, nullptr);
  EXPECT_NE(H5SM_INDEX[0].dst_context, nullptr);
  EXPECT_NE(H5SM_INDEX[0].store, nullptr);
  EXPECT_NE(H5SM_INDEX[0].compare, nullptr);
  EXPECT_NE(H5SM_INDEX[0].encode, nullptr);
  EXPECT_NE(H5SM_INDEX[0].decode, nullptr);
  EXPECT_NE(H5SM_INDEX[0].debug, nullptr);

  for (const size_t addr_sz : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_sz, addr_sz), 0);

    ScopedFile file =
        CreateInMemoryFile("sohm_bt2_direct.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    void* bt2_ctx = H5SM_INDEX[0].crt_context(f);
    ASSERT_NE(bt2_ctx, nullptr);
    absl::Cleanup dst_ctx = [&] {
      if (bt2_ctx != nullptr) {
        EXPECT_GE(H5SM_INDEX[0].dst_context(bt2_ctx), 0);
        bt2_ctx = nullptr;
      }
    };

    EXPECT_EQ(static_cast<const H5SM_bt2_ctx_t*>(bt2_ctx)->sizeof_addr,
              static_cast<uint8_t>(addr_sz));

    H5SM_mesg_key_t key{};
    key.message.location = H5SM_IN_HEAP;
    key.message.hash = 0x12345678U;
    key.message.msg_type_id = H5O_SDSPACE_ID;
    key.message.u.heap_loc.ref_count = 7;
    key.message.u.heap_loc.fheap_id.val = 0x0102030405060708ULL;

    H5SM_sohm_t stored{};
    ASSERT_GE(H5SM_INDEX[0].store(&stored, &key), 0);
    EXPECT_EQ(stored.location, H5SM_IN_HEAP);
    EXPECT_EQ(stored.hash, 0x12345678U);
    EXPECT_EQ(stored.u.heap_loc.ref_count, 7u);
    EXPECT_EQ(stored.u.heap_loc.fheap_id.val, 0x0102030405060708ULL);

    const std::string heap_dbg = CaptureBtreeDebug(stored, bt2_ctx);
    EXPECT_THAT(heap_dbg, HasSubstr("Shared Message in heap:"));
    EXPECT_THAT(heap_dbg, HasSubstr("7"));

    H5SM_mesg_key_t oh_key{};
    oh_key.message.location = H5SM_IN_OH;
    oh_key.message.hash = 0x87654321U;
    oh_key.message.msg_type_id = H5O_DTYPE_ID;
    oh_key.message.u.mesg_loc.oh_addr = 0x4096U;
    oh_key.message.u.mesg_loc.index = 2U;

    H5SM_sohm_t oh_stored{};
    ASSERT_GE(H5SM_INDEX[0].store(&oh_stored, &oh_key), 0);
    EXPECT_EQ(oh_stored.location, H5SM_IN_OH);
    EXPECT_EQ(oh_stored.u.mesg_loc.oh_addr, 0x4096U);
    EXPECT_EQ(oh_stored.u.mesg_loc.index, 2U);

    const std::string oh_dbg = CaptureBtreeDebug(oh_stored, bt2_ctx);
    EXPECT_THAT(oh_dbg, HasSubstr("Shared Message in OH:"));
    EXPECT_THAT(oh_dbg, HasSubstr("16534"));

    EXPECT_GE(H5SM_INDEX[0].dst_context(bt2_ctx), 0);
    bt2_ctx = nullptr;
  }
}

TEST(H5SMbtree2Test, BtreeCreateInsertFlushEvictReloadFindAndDelete) {
  for (const size_t addr_sz : {size_t{4}, size_t{8}}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_sz, addr_sz), 0);

    ScopedFile file =
        CreateInMemoryFile("sohm_bt2_tree.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    H5B2_create_t cparam{};
    cparam.cls = H5SM_INDEX;
    cparam.node_size = H5SM_B2_NODE_SIZE;
    cparam.rrec_size = static_cast<uint32_t>(H5SM_SOHM_ENTRY_SIZE(f));
    cparam.split_percent = H5SM_B2_SPLIT_PERCENT;
    cparam.merge_percent = H5SM_B2_MERGE_PERCENT;

    haddr_t bt2_addr = HADDR_UNDEF;
    {
      ScopedB2 bt2(H5B2_create(f, &cparam, f));
      ASSERT_NE(bt2.get(), nullptr);
      ASSERT_GE(H5B2_get_addr(bt2.get(), &bt2_addr), 0);

      for (uint32_t i = 0; i < 40; ++i) {
        H5SM_mesg_key_t key{};
        key.file = f;
        // Upstream H5B2__cache_leaf_serialize checksums leaf buffers before
        // zeroing unused trailing bytes; with 4-byte addresses H5SM_IN_OH writes
        // 13 of 17 bytes per slot, so keep 4-byte trees IN_HEAP (17 bytes) and
        // exercise mixed IN_HEAP/IN_OH entries with 8-byte addresses (both 17).
        if (addr_sz == 4 || i % 2 == 0) {
          key.message.location = H5SM_IN_HEAP;
          key.message.hash = 0x1000U + i * 17U;
          key.message.msg_type_id = H5O_SDSPACE_ID;
          key.message.u.heap_loc.ref_count = i + 1U;
          key.message.u.heap_loc.fheap_id.val = 0x200000ULL + i;
        } else {
          key.message.location = H5SM_IN_OH;
          key.message.hash = 0x1000U + i * 17U;
          key.message.msg_type_id = H5O_DTYPE_ID;
          key.message.u.mesg_loc.oh_addr = 0x800U + i * 16U;
          key.message.u.mesg_loc.index = static_cast<H5O_msg_crt_idx_t>(i % 4);
        }
        ASSERT_GE(H5B2_insert(bt2.get(), &key), 0);
      }
    }

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ASSERT_GE(H5AC_evict(f), 0);

    {
      ScopedB2 reopened(H5B2_open(f, bt2_addr, f));
      ASSERT_NE(reopened.get(), nullptr);

      hsize_t nrec = 0;
      ASSERT_GE(H5B2_get_nrec(reopened.get(), &nrec), 0);
      EXPECT_EQ(nrec, 40u);

      struct DebugVisit {
        std::vector<std::string> lines;
      } visit;

      auto iter_cb = [](const void* record, void* op_data) -> int {
        auto* out = static_cast<DebugVisit*>(op_data);
        out->lines.push_back(CaptureBtreeDebug(
            *static_cast<const H5SM_sohm_t*>(record), nullptr));
        return H5_ITER_CONT;
      };
      ASSERT_GE(H5B2_iterate(reopened.get(), iter_cb, &visit), 0);
      ASSERT_EQ(visit.lines.size(), 40u);

      // Verify fast-path heap ID lookup works after decode.
      H5SM_mesg_key_t query{};
      query.file = f;
      query.message.location = H5SM_IN_HEAP;
      query.message.hash = 0x1000U + 4U * 17U;
      query.message.msg_type_id = H5O_SDSPACE_ID;
      query.message.u.heap_loc.fheap_id.val = 0x200004ULL;
      bool found = false;
      H5SM_sohm_t found_rec{};
      auto find_cb = [](const void* rec, void* op_data) -> herr_t {
        *static_cast<H5SM_sohm_t*>(op_data) =
            *static_cast<const H5SM_sohm_t*>(rec);
        return 0;
      };
      ASSERT_GE(H5B2_find(reopened.get(), &query, &found, find_cb, &found_rec),
                0);
      EXPECT_TRUE(found);
      EXPECT_EQ(found_rec.u.heap_loc.ref_count, 5u);
    }

    ASSERT_GE(H5B2_delete(f, bt2_addr, f, nullptr, nullptr), 0);
  }
}

}  // namespace
}  // namespace hdf5
