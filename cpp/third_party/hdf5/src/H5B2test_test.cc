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
// Tests v2 B-tree H5B2_TEST and H5B2_TEST2 client callbacks (context lifecycle,
// store, compare, encode, decode, debug) as well as root address and node depth/
// info test introspection routines (third_party/hdf5/src/H5B2test.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2test.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` (`H5AC_info_t`). Match `NDEBUG` around the internal
// prelude and headers so internal struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5B2TEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma push_macro("try")
#undef try
#define try h5_try_param
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#include "third_party/hdf5/src/H5B2pkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#ifdef H5B2TEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2TEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

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

class ScopedBtree2 {
 public:
  ScopedBtree2() = default;
  explicit ScopedBtree2(H5B2_t* bt2) : bt2_(bt2) {}
  ~ScopedBtree2() { reset(); }

  ScopedBtree2(const ScopedBtree2&) = delete;
  ScopedBtree2& operator=(const ScopedBtree2&) = delete;

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

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

int CollectScalarOp(const void* record, void* op_data) {
  auto* out = static_cast<std::vector<hsize_t>*>(op_data);
  out->push_back(*static_cast<const hsize_t*>(record));
  return H5_ITER_CONT;
}

int CollectPairOp(const void* record, void* op_data) {
  auto* out = static_cast<std::vector<H5B2_test_rec_t>*>(op_data);
  out->push_back(*static_cast<const H5B2_test_rec_t*>(record));
  return H5_ITER_CONT;
}

herr_t FoundPairOp(const void* record, void* op_data) {
  *static_cast<H5B2_test_rec_t*>(op_data) =
      *static_cast<const H5B2_test_rec_t*>(record);
  return 0;
}

herr_t ModifyPairOp(void* record, void* op_data, bool* changed) {
  auto* rec = static_cast<H5B2_test_rec_t*>(record);
  rec->val = *static_cast<const hsize_t*>(op_data);
  *changed = true;
  return 0;
}

TEST(H5B2TestCallbacks, TestClassRoundTripFlushReopenAndDestruction) {
  TempFile temp_file("h5b2_test_roundtrip.h5");
  haddr_t bt2_addr = HADDR_UNDEF;

  // Create on disk, populate to depth >= 2, flush, and close to trigger encode
  // and context destruction.
  {
    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    H5B2_create_t cparam{};
    cparam.cls = H5B2_TEST;
    cparam.node_size = 128;
    cparam.rrec_size = H5F_SIZEOF_SIZE(f);
    cparam.split_percent = 100;
    cparam.merge_percent = 40;

    ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &bt2_addr), 0);
    ASSERT_NE(bt2_addr, HADDR_UNDEF);

    for (hsize_t k = 1; k <= 140; ++k) {
      ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
    }

    H5B2_stat_t stat{};
    ASSERT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
    EXPECT_GE(stat.depth, 2U);
    EXPECT_EQ(stat.nrecords, 140ULL);

    bt2.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // Reopen from disk to exercise H5B2__test_crt_context and decode callbacks.
  {
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedBtree2 bt2(H5B2_open(f, bt2_addr, f));
    ASSERT_NE(bt2.get(), nullptr);

    std::vector<hsize_t> values;
    ASSERT_GE(H5B2_iterate(bt2.get(), CollectScalarOp, &values), 0);
    ASSERT_EQ(values.size(), 140U);
    for (size_t i = 0; i < values.size(); ++i) {
      EXPECT_EQ(values[i], static_cast<hsize_t>(i + 1));
    }

    bool found = false;
    hsize_t query = 73;
    ASSERT_GE(H5B2_find(bt2.get(), &query, &found, nullptr, nullptr), 0);
    EXPECT_TRUE(found);
  }
}

TEST(H5B2TestCallbacks, Test2KeyValueRoundTripUpdateFlushAndReopen) {
  TempFile temp_file("h5b2_test2_roundtrip.h5");
  haddr_t bt2_addr = HADDR_UNDEF;

  {
    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    H5B2_create_t cparam{};
    cparam.cls = H5B2_TEST2;
    cparam.node_size = 128;
    cparam.rrec_size = 2 * H5F_SIZEOF_SIZE(f);
    cparam.split_percent = 100;
    cparam.merge_percent = 40;

    ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &bt2_addr), 0);

    for (hsize_t k = 1; k <= 60; ++k) {
      H5B2_test_rec_t rec{k, k * 100};
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }

    // Update one existing record using H5B2_update.
    H5B2_test_rec_t mod_key{25, 0};
    hsize_t new_val = 9999;
    ASSERT_GE(H5B2_update(bt2.get(), &mod_key, ModifyPairOp, &new_val), 0);

    H5B2_stat_t stat{};
    ASSERT_GE(H5B2_stat_info(bt2.get(), &stat), 0);
    EXPECT_GE(stat.depth, 2U);

    bt2.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  {
    ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    ScopedBtree2 bt2(H5B2_open(f, bt2_addr, f));
    ASSERT_NE(bt2.get(), nullptr);

    std::vector<H5B2_test_rec_t> records;
    ASSERT_GE(H5B2_iterate(bt2.get(), CollectPairOp, &records), 0);
    ASSERT_EQ(records.size(), 60U);

    bool found = false;
    H5B2_test_rec_t query{25, 0};
    H5B2_test_rec_t found_rec{};
    ASSERT_GE(H5B2_find(bt2.get(), &query, &found, FoundPairOp, &found_rec), 0);
    EXPECT_TRUE(found);
    EXPECT_EQ(found_rec.key, 25ULL);
    EXPECT_EQ(found_rec.val, 9999ULL);

    query.key = 999;
    found = true;
    ASSERT_GE(H5B2_find(bt2.get(), &query, &found, FoundPairOp, &found_rec), 0);
    EXPECT_FALSE(found);
  }
}

TEST(H5B2TestCallbacks, DebugCallbacksFormatSingleAndPairRecords) {
  TempFile dump_file("h5b2_debug_dump.txt");
  std::FILE* fp = std::fopen(dump_file.c_str(), "w+");
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_fp = [&]() { std::fclose(fp); };

  const hsize_t scalar_rec = 12345;
  ASSERT_GE((H5B2_TEST[0].debug)(fp, /*indent=*/2, /*fwidth=*/10, &scalar_rec,
                                 /*_udata=*/nullptr),
            0);

  const H5B2_test_rec_t pair_rec{42, 8400};
  ASSERT_GE((H5B2_TEST2[0].debug)(fp, /*indent=*/4, /*fwidth=*/10, &pair_rec,
                                  /*_udata=*/nullptr),
            0);
  ASSERT_EQ(std::fflush(fp), 0);
  std::rewind(fp);

  std::array<char, 512> buf{};
  const size_t nread = std::fread(buf.data(), 1, buf.size() - 1, fp);
  buf[nread] = '\0';
  const std::string text(buf.data(), nread);

  EXPECT_THAT(text, HasSubstr("Record:    12345"));
  EXPECT_THAT(text, HasSubstr("Record:    (42, 8400)"));
}

TEST(H5B2TestIntrospection, RootAddrNodeInfoAndDepthQueries) {
  ScopedFile file = CreateInMemoryFile("h5b2test_introspect.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 128;
  cparam.rrec_size = H5F_SIZEOF_SIZE(f);
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
  ASSERT_NE(bt2.get(), nullptr);

  // Empty tree has undefined root address and rejects node info/depth queries.
  haddr_t root_addr = 0;
  ASSERT_GE(H5B2__get_root_addr_test(bt2.get(), &root_addr), 0);
  EXPECT_EQ(root_addr, HADDR_UNDEF);

  {
    ScopedErrorSilence silence;
    H5B2_node_info_test_t ninfo{};
    hsize_t key = 1;
    EXPECT_LT(H5B2__get_node_info_test(bt2.get(), &key, &ninfo), 0);
    EXPECT_LT(H5B2__get_node_depth_test(bt2.get(), &key), 0);
  }

  // Insert 140 keys -> depth >= 2, root_addr becomes defined.
  for (hsize_t k = 1; k <= 140; ++k) {
    ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
  }
  ASSERT_GE(H5B2__get_root_addr_test(bt2.get(), &root_addr), 0);
  EXPECT_NE(root_addr, HADDR_UNDEF);

  bool saw_leaf = false;
  bool saw_internal_d1 = false;
  bool saw_internal_d2 = false;
  for (hsize_t k = 1; k <= 140; ++k) {
    H5B2_node_info_test_t ninfo{};
    ASSERT_GE(H5B2__get_node_info_test(bt2.get(), &k, &ninfo), 0);
    EXPECT_EQ(H5B2__get_node_depth_test(bt2.get(), &k),
              static_cast<int>(ninfo.depth));
    EXPECT_GT(ninfo.nrec, 0U);
    if (ninfo.depth == 0) saw_leaf = true;
    if (ninfo.depth == 1) saw_internal_d1 = true;
    if (ninfo.depth >= 2) saw_internal_d2 = true;
  }
  EXPECT_TRUE(saw_leaf);
  EXPECT_TRUE(saw_internal_d1);
  EXPECT_TRUE(saw_internal_d2);

  // Query missing keys below, between, and above all existing records to hit
  // both cmp < 0 and cmp > 0 leaf/internal branches and NOTFOUND error path.
  {
    ScopedErrorSilence silence;
    H5B2_node_info_test_t ninfo{};
    hsize_t low_key = 0;
    hsize_t high_key = 999;
    EXPECT_LT(H5B2__get_node_info_test(bt2.get(), &low_key, &ninfo), 0);
    EXPECT_LT(H5B2__get_node_info_test(bt2.get(), &high_key, &ninfo), 0);
    EXPECT_LT(H5B2__get_node_depth_test(bt2.get(), &high_key), 0);
  }
}

TEST(H5B2TestIntrospection, NodeInfoWalkWithSwmrParentTrackingAndErrorUnpin) {
  TempFile temp_file("h5b2test_swmr_walk.h5");
  haddr_t bt2_addr = HADDR_UNDEF;

  // Build and flush a depth >= 2 H5B2_TEST tree to disk first.
  {
    ScopedFile file(H5Fcreate(temp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    H5B2_create_t cparam{};
    cparam.cls = H5B2_TEST;
    cparam.node_size = 128;
    cparam.rrec_size = H5F_SIZEOF_SIZE(f);
    cparam.split_percent = 100;
    cparam.merge_percent = 40;

    ScopedBtree2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &bt2_addr), 0);

    for (hsize_t k = 1; k <= 140; ++k) {
      ASSERT_GE(H5B2_insert(bt2.get(), &k), 0);
    }
    bt2.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  }

  // Reopen with a clean metadata cache, enable SWMR parent-pinning behavior
  // before loading any nodes so AFTER_LOAD and BEFORE_EVICT flush dependencies
  // match cleanly across the entire walk, and exercise error cleanup unpinning.
  ScopedFile file(H5Fopen(temp_file.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  ScopedBtree2 bt2(H5B2_open(f, bt2_addr, f));
  ASSERT_NE(bt2.get(), nullptr);

  H5B2_hdr_t* hdr = bt2.get()->hdr;
  ASSERT_GE(hdr->depth, 2U);
  hdr->swmr_write = true;

  hsize_t leaf_key = 0;
  for (hsize_t k = 1; k <= 140; ++k) {
    H5B2_node_info_test_t ninfo{};
    ASSERT_GE(H5B2__get_node_info_test(bt2.get(), &k, &ninfo), 0);
    if (ninfo.depth == 0 && leaf_key == 0) {
      leaf_key = k;
    }
  }
  ASSERT_GT(leaf_key, 0ULL);

  // Locate leaf_key's depth-1 parent internal node and temporarily corrupt its
  // last/matching child leaf address so H5B2__protect_leaf() fails while
  // `parent` is a pinned internal node (exercising `done:` unpin cleanup).
  H5B2_node_ptr_t curr_ptr = hdr->root;
  void* parent_ptr = hdr;
  uint16_t d = hdr->depth;
  H5B2_internal_t* depth1_parent = nullptr;
  H5B2_node_ptr_t* corrupted_slot = nullptr;
  haddr_t saved_leaf_addr = HADDR_UNDEF;

  while (d > 0) {
    H5B2_internal_t* internal = H5B2__protect_internal(
        hdr, parent_ptr, &curr_ptr, d, /*shadow=*/false, H5AC__READ_ONLY_FLAG);
    ASSERT_NE(internal, nullptr);
    if (parent_ptr != hdr) {
      EXPECT_GE(H5AC_unpin_entry(parent_ptr), 0);
    }

    unsigned idx = 0;
    int cmp = 0;
    ASSERT_GE(H5B2__locate_record(hdr->cls, internal->nrec, hdr->nat_off,
                                  internal->int_native, &leaf_key, &idx, &cmp),
              0);
    ASSERT_NE(cmp, 0);
    if (cmp > 0) idx++;

    if (d == 1) {
      depth1_parent = internal;
      corrupted_slot = &internal->node_ptrs[idx];
      saved_leaf_addr = corrupted_slot->addr;
      corrupted_slot->addr = 0x7fffffff0000ULL;
      ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, curr_ptr.addr, internal,
                               H5AC__NO_FLAGS_SET),
                0);
      break;
    }
    const H5B2_node_ptr_t next_ptr = internal->node_ptrs[idx];
    ASSERT_GE(H5AC_unprotect(f, H5AC_BT2_INT, curr_ptr.addr, internal,
                             H5AC__PIN_ENTRY_FLAG),
              0);
    parent_ptr = internal;
    curr_ptr = next_ptr;
    --d;
  }
  ASSERT_NE(depth1_parent, nullptr);

  {
    ScopedErrorSilence silence;
    H5B2_node_info_test_t ninfo{};
    EXPECT_LT(H5B2__get_node_info_test(bt2.get(), &leaf_key, &ninfo), 0);
  }

  corrupted_slot->addr = saved_leaf_addr;
}

}  // namespace
}  // namespace hdf5
