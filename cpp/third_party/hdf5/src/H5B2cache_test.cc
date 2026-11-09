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
// Tests v2 B-tree metadata cache callbacks (header, internal node, leaf node)
// including serialization, checksum verification, deserialization error paths,
// cache eviction/reload, SWMR proxy notifications, and ICR cleanup
// (third_party/hdf5/src/H5B2cache.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5B2cache.c

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t
// (first member of H5B2_hdr_t, H5B2_internal_t, H5B2_leaf_t). Define NDEBUG
// around internal headers so C++ struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5B2CACHE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5B2_FRIEND
#define H5B2_TESTING
#include "third_party/hdf5/src/H5ACprivate.h"
#pragma push_macro("try")
#undef try
#define try h5b2_try_param
#include "third_party/hdf5/src/H5B2pkg.h"
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5B2CACHE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5B2CACHE_TEST_RESTORE_UNDEF_NDEBUG
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

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

struct CodecControl {
  bool fail_encode = false;
  bool fail_decode = false;
};

void* CodecCrtContext(void* udata) { return udata; }
herr_t CodecDstContext(void* /*ctx*/) { return 0; }

herr_t CodecStore(void* nrecord, const void* udata) {
  *static_cast<hsize_t*>(nrecord) = *static_cast<const hsize_t*>(udata);
  return 0;
}

herr_t CodecCompare(const void* rec1, const void* rec2, int* result) {
  const hsize_t a = *static_cast<const hsize_t*>(rec1);
  const hsize_t b = *static_cast<const hsize_t*>(rec2);
  *result = (a < b) ? -1 : ((a > b) ? 1 : 0);
  return 0;
}

herr_t CodecEncode(uint8_t* raw, const void* nrecord, void* ctx) {
  auto* ctrl = static_cast<CodecControl*>(ctx);
  if (ctrl != nullptr && ctrl->fail_encode) {
    return -1;
  }
  const uint64_t v = static_cast<uint64_t>(*static_cast<const hsize_t*>(nrecord));
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffU);
  }
  return 0;
}

herr_t CodecDecode(const uint8_t* raw, void* nrecord, void* ctx) {
  auto* ctrl = static_cast<CodecControl*>(ctx);
  if (ctrl != nullptr && ctrl->fail_decode) {
    return -1;
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(raw[i]) << (i * 8);
  }
  *static_cast<hsize_t*>(nrecord) = static_cast<hsize_t>(v);
  return 0;
}

H5B2_class_t MakeCodecClass(H5B2_subid_t id = H5B2_TEST_ID) {
  H5B2_class_t cls{};
  cls.id = id;
  cls.name = "H5B2cache_custom";
  cls.nrec_size = sizeof(hsize_t);
  cls.crt_context = CodecCrtContext;
  cls.dst_context = CodecDstContext;
  cls.store = CodecStore;
  cls.compare = CodecCompare;
  cls.encode = CodecEncode;
  cls.decode = CodecDecode;
  cls.debug = nullptr;
  return cls;
}

int CollectRecordsOp(const void* record, void* op_data) {
  auto* out = static_cast<std::vector<hsize_t>*>(op_data);
  out->push_back(*static_cast<const hsize_t*>(record));
  return H5_ITER_CONT;
}

herr_t ModifyNoopOrUpdateOp(void* record, void* op_data, bool* changed) {
  *static_cast<hsize_t*>(record) = *static_cast<const hsize_t*>(op_data);
  *changed = true;
  return 0;
}

TEST(H5B2cacheTest, EndToEndFlushEvictAndReloadMultiDepthTree) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  haddr_t bt2_addr = HADDR_UNDEF;
  constexpr hsize_t kCount = 150;
  {
    ScopedB2 bt2(H5B2_create(f, &cparam, f));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_GE(H5B2_get_addr(bt2.get(), &bt2_addr), 0);
    for (hsize_t i = 0; i < kCount; ++i) {
      hsize_t rec = (i * 41) % kCount;
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }
    EXPECT_GE(bt2.get()->hdr->depth, 2);
  }

  // Flush and evict all cached metadata so subsequent open+iterate loads
  // header, depth=2 internal, depth=1 internal, and leaf nodes from disk.
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  ScopedB2 reopened(H5B2_open(f, bt2_addr, f));
  ASSERT_NE(reopened.get(), nullptr);
  EXPECT_GE(reopened.get()->hdr->depth, 2);

  std::vector<hsize_t> visited;
  ASSERT_GE(H5B2_iterate(reopened.get(), CollectRecordsOp, &visited), 0);
  ASSERT_EQ(visited.size(), kCount);
  for (hsize_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(visited[i], i);
  }
}

TEST(H5B2cacheTest, SwmrFlushDependencyShadowingAndEviction) {
  TempFile tmp_file("h5b2cache_swmr.h5");

  // Phase 1: Custom SWMR tree with node_size=128 (depth >= 2) attached to a
  // proxy parent so flush, update (shadowing), eviction, and BEFORE_EVICT run
  // for header, internal nodes, and leaf nodes.
  CodecControl ctrl;
  H5B2_class_t swmr_cls = MakeCodecClass(H5B2_CDSET_ID);
  H5AC_proxy_entry_t* parent_proxy = nullptr;
  auto destroy_proxy = absl::MakeCleanup([&parent_proxy] {
    if (parent_proxy != nullptr) {
      H5AC_proxy_entry_dest(parent_proxy);
      parent_proxy = nullptr;
    }
  });
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());
    H5F_t* f = GetH5File(file.get());
    ASSERT_NE(f, nullptr);

    parent_proxy = H5AC_proxy_entry_create();
    ASSERT_NE(parent_proxy, nullptr);

    H5B2_create_t cparam{};
    cparam.cls = &swmr_cls;
    cparam.node_size = 128;
    cparam.rrec_size = 8;
    cparam.split_percent = 100;
    cparam.merge_percent = 40;

    ScopedB2 bt2(H5B2_create(f, &cparam, &ctrl));
    ASSERT_NE(bt2.get(), nullptr);
    ASSERT_TRUE(bt2.get()->hdr->swmr_write);
    ASSERT_GE(H5B2_depend(bt2.get(), parent_proxy), 0);

    for (hsize_t i = 0; i < 150; ++i) {
      hsize_t rec = i;
      ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
    }

    // Flush advances hdr->shadow_epoch via H5B2__cache_hdr_notify(AFTER_FLUSH).
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    EXPECT_GE(bt2.get()->hdr->shadow_epoch, 1u);

    // Modify records via H5B2_update to trigger SWMR node shadowing after flush.
    hsize_t key0 = 0;
    hsize_t key_mid = 75;
    hsize_t key_last = 149;
    ASSERT_GE(H5B2_update(bt2.get(), &key0, ModifyNoopOrUpdateOp, &key0), 0);
    ASSERT_GE(H5B2_update(bt2.get(), &key_mid, ModifyNoopOrUpdateOp, &key_mid),
              0);
    ASSERT_GE(
        H5B2_update(bt2.get(), &key_last, ModifyNoopOrUpdateOp, &key_last), 0);

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    bt2.reset();

    // Evict everything cleanly so BEFORE_EVICT detaches top_proxy and parent.
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::move(destroy_proxy).Invoke();

  // Phase 2: Public 2D chunked dataset with >1 unlimited dims in SWMR mode
  // closed & reopened with H5F_ACC_SWMR_WRITE so AFTER_LOAD runs on B-tree
  // header, internal node, and leaf nodes.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fstart_swmr_write(file.get()), 0);

    hsize_t dims[2] = {16, 16};
    hsize_t maxdims[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
    hsize_t chunk[2] = {1, 1};
    ScopedSpace space(H5Screate_simple(2, dims, maxdims));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "chunks", H5T_NATIVE_INT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int> vals(256, 42);
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals.data()),
              0);
    ASSERT_GE(H5Dflush(dset.get()), 0);
    // Re-write a subset to update chunks under SWMR shadow epoch.
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals.data()),
              0);
  }

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
        0);
    ScopedFile file(H5Fopen(tmp_file.c_str(),
                            H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedDataset dset(H5Dopen2(file.get(), "chunks", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int> out(256, 0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    EXPECT_EQ(out[0], 42);
    EXPECT_EQ(out[255], 42);
  }
}

TEST(H5B2cacheTest, HeaderCacheCallbacksSerializeVerifyDeserializeErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = H5B2_TEST;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  H5B2_hdr_cache_ud_t udata{};
  udata.f = f;
  udata.addr = 0x2000;
  udata.ctx_udata = f;

  size_t init_len = 0;
  ASSERT_GE(H5AC_BT2_HDR[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, H5B2_HEADER_SIZE_FILE(f));

  H5B2_hdr_t* src_hdr = H5B2__hdr_alloc(f);
  ASSERT_NE(src_hdr, nullptr);
  ASSERT_GE(H5B2__hdr_init(src_hdr, &cparam, f, 1), 0);
  src_hdr->addr = 0x2000;
  src_hdr->root.addr = 0x3000;
  src_hdr->root.node_nrec = 3;
  src_hdr->root.all_nrec = 17;
  auto free_src_hdr = absl::MakeCleanup([&src_hdr] {
    if (src_hdr != nullptr) {
      H5B2__hdr_free(src_hdr);
    }
  });

  size_t img_len = 0;
  ASSERT_GE(H5AC_BT2_HDR[0].image_len(src_hdr, &img_len), 0);
  EXPECT_EQ(img_len, src_hdr->hdr_size);

  std::vector<uint8_t> image(img_len, 0);
  ASSERT_GE(H5AC_BT2_HDR[0].serialize(f, image.data(), img_len, src_hdr), 0);

  // Verify valid checksum, corrupt bit, and too-short buffer failure.
  EXPECT_EQ(H5AC_BT2_HDR[0].verify_chksum(image.data(), img_len, &udata), 1);
  {
    std::vector<uint8_t> bad_chk = image;
    bad_chk[0] ^= 0x01;
    EXPECT_EQ(H5AC_BT2_HDR[0].verify_chksum(bad_chk.data(), img_len, &udata),
              0);
  }
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_BT2_HDR[0].verify_chksum(image.data(), 2, &udata), 0);
  }

  // Deserialize valid header and destroy via free_icr.
  bool dirty = false;
  void* deser_raw =
      H5AC_BT2_HDR[0].deserialize(image.data(), img_len, &udata, &dirty);
  ASSERT_NE(deser_raw, nullptr);
  auto* deser_hdr = static_cast<H5B2_hdr_t*>(deser_raw);
  EXPECT_EQ(deser_hdr->depth, 1);
  EXPECT_EQ(deser_hdr->root.addr, 0x3000u);
  EXPECT_EQ(deser_hdr->root.node_nrec, 3);
  EXPECT_EQ(deser_hdr->root.all_nrec, 17u);
  EXPECT_GE(H5AC_BT2_HDR[0].free_icr(deser_hdr), 0);

  // Negative deserialization cases: magic, version, invalid class ID.
  {
    ScopedErrorSilence silence;

    std::vector<uint8_t> bad_magic = image;
    bad_magic[0] = 'Z';
    EXPECT_EQ(
        H5AC_BT2_HDR[0].deserialize(bad_magic.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_ver = image;
    bad_ver[H5_SIZEOF_MAGIC] = 99;
    EXPECT_EQ(
        H5AC_BT2_HDR[0].deserialize(bad_ver.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_id = image;
    bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(H5B2_NUM_BTREE_ID);
    EXPECT_EQ(
        H5AC_BT2_HDR[0].deserialize(bad_id.data(), img_len, &udata, &dirty),
        nullptr);
  }

  // Header notify callback paths: non-SWMR and SWMR (without active proxy).
  src_hdr->swmr_write = false;
  src_hdr->parent = nullptr;
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, src_hdr), 0);

  src_hdr->swmr_write = true;
  EXPECT_GE(H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, src_hdr),
            0);
  EXPECT_GE(H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_LOAD, src_hdr), 0);
  const uint64_t prev_epoch = src_hdr->shadow_epoch;
  EXPECT_GE(H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_AFTER_FLUSH, src_hdr),
            0);
  EXPECT_EQ(src_hdr->shadow_epoch, prev_epoch + 1);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, src_hdr), 0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_ENTRY_CLEANED, src_hdr), 0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_CHILD_DIRTIED, src_hdr), 0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_CHILD_CLEANED, src_hdr), 0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED, src_hdr),
      0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_CHILD_SERIALIZED, src_hdr), 0);
  EXPECT_GE(
      H5AC_BT2_HDR[0].notify(H5AC_NOTIFY_ACTION_BEFORE_EVICT, src_hdr), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5AC_BT2_HDR[0].notify(static_cast<H5AC_notify_action_t>(999), src_hdr),
        0);
  }
}

TEST(H5B2cacheTest, InternalNodeCallbacksAllDepthsAndErrors) {
  CodecControl ctrl;
  H5B2_class_t cls = MakeCodecClass(H5B2_TEST_ID);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = &cls;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  ScopedB2 bt2(H5B2_create(f, &cparam, &ctrl));
  ASSERT_NE(bt2.get(), nullptr);
  for (hsize_t i = 0; i < 150; ++i) {
    hsize_t rec = i;
    ASSERT_GE(H5B2_insert(bt2.get(), &rec), 0);
  }
  H5B2_hdr_t* hdr = bt2.get()->hdr;
  ASSERT_GE(hdr->depth, 2);

  // Test both depth == 1 and depth == 2 internal node serialization/deser.
  for (uint16_t test_depth : {uint16_t{1}, uint16_t{2}}) {
    std::vector<hsize_t> recs = {11, 22};
    std::vector<uint8_t> nat_buf(hdr->node_info[test_depth].max_nrec *
                                 sizeof(hsize_t));
    std::memcpy(nat_buf.data(), recs.data(), recs.size() * sizeof(hsize_t));

    std::vector<H5B2_node_ptr_t> ptrs(hdr->node_info[test_depth].max_nrec + 1);
    ptrs[0] = {0x3000, 3, test_depth > 1 ? 15u : 3u};
    ptrs[1] = {0x4000, 4, test_depth > 1 ? 20u : 4u};
    ptrs[2] = {0x5000, 2, test_depth > 1 ? 10u : 2u};

    H5B2_internal_t src_int{};
    src_int.hdr = hdr;
    src_int.nrec = 2;
    src_int.depth = test_depth;
    src_int.int_native = nat_buf.data();
    src_int.node_ptrs = ptrs.data();

    H5B2_internal_cache_ud_t udata{};
    udata.f = f;
    udata.hdr = hdr;
    udata.parent = nullptr;
    udata.nrec = 2;
    udata.depth = test_depth;

    size_t init_len = 0;
    ASSERT_GE(H5AC_BT2_INT[0].get_initial_load_size(&udata, &init_len), 0);
    EXPECT_EQ(init_len, hdr->node_size);

    size_t img_len = 0;
    ASSERT_GE(H5AC_BT2_INT[0].image_len(&src_int, &img_len), 0);
    EXPECT_EQ(img_len, hdr->node_size);

    std::vector<uint8_t> image(img_len, 0);
    ASSERT_GE(H5AC_BT2_INT[0].serialize(f, image.data(), img_len, &src_int),
              0);

    EXPECT_EQ(H5AC_BT2_INT[0].verify_chksum(image.data(), img_len, &udata), 1);
    {
      std::vector<uint8_t> flipped = image;
      flipped[0] ^= 0x04;
      EXPECT_EQ(H5AC_BT2_INT[0].verify_chksum(flipped.data(), img_len, &udata),
                0);
    }

    bool dirty = false;
    void* deser_raw =
        H5AC_BT2_INT[0].deserialize(image.data(), img_len, &udata, &dirty);
    ASSERT_NE(deser_raw, nullptr);
    auto* deser_int = static_cast<H5B2_internal_t*>(deser_raw);
    EXPECT_EQ(deser_int->nrec, 2);
    EXPECT_EQ(deser_int->depth, test_depth);
    EXPECT_EQ(deser_int->node_ptrs[1].addr, 0x4000u);
    EXPECT_EQ(deser_int->node_ptrs[1].all_nrec,
              test_depth > 1 ? 20u : 4u);

    if (test_depth == 1) {
      // Negative deserialize cases: magic, version, type, decode failure.
      {
        ScopedErrorSilence silence;

        std::vector<uint8_t> bad_magic = image;
        bad_magic[0] = 'X';
        EXPECT_EQ(H5AC_BT2_INT[0].deserialize(bad_magic.data(), img_len, &udata,
                                              &dirty),
                  nullptr);

        std::vector<uint8_t> bad_ver = image;
        bad_ver[H5_SIZEOF_MAGIC] = 5;
        EXPECT_EQ(H5AC_BT2_INT[0].deserialize(bad_ver.data(), img_len, &udata,
                                              &dirty),
                  nullptr);

        std::vector<uint8_t> bad_id = image;
        bad_id[H5_SIZEOF_MAGIC + 1] =
            static_cast<uint8_t>(cls.id) ^ 1U;
        EXPECT_EQ(H5AC_BT2_INT[0].deserialize(bad_id.data(), img_len, &udata,
                                              &dirty),
                  nullptr);

        ctrl.fail_decode = true;
        EXPECT_EQ(
            H5AC_BT2_INT[0].deserialize(image.data(), img_len, &udata, &dirty),
            nullptr);
        ctrl.fail_decode = false;

        ctrl.fail_encode = true;
        std::vector<uint8_t> tmp_img(img_len, 0);
        EXPECT_LT(H5AC_BT2_INT[0].serialize(f, tmp_img.data(), img_len,
                                            &src_int),
                  0);
        ctrl.fail_encode = false;
      }

      // Notify callback branches (non-SWMR and SWMR non-evict/unknown).
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, deser_int),
          0);
      const bool saved_swmr = hdr->swmr_write;
      auto restore_swmr =
          absl::MakeCleanup([hdr, saved_swmr] { hdr->swmr_write = saved_swmr; });
      hdr->swmr_write = true;
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_AFTER_FLUSH, deser_int),
          0);
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, deser_int),
          0);
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_ENTRY_CLEANED, deser_int),
          0);
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_CHILD_DIRTIED, deser_int),
          0);
      EXPECT_GE(
          H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_CHILD_CLEANED, deser_int),
          0);
      EXPECT_GE(H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
                                       deser_int),
                0);
      EXPECT_GE(H5AC_BT2_INT[0].notify(H5AC_NOTIFY_ACTION_CHILD_SERIALIZED,
                                       deser_int),
                0);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5AC_BT2_INT[0].notify(static_cast<H5AC_notify_action_t>(999),
                                         deser_int),
                  0);
      }
    }

    EXPECT_GE(H5AC_BT2_INT[0].free_icr(deser_int), 0);
  }
}

TEST(H5B2cacheTest, LeafNodeCallbacksAndErrors) {
  CodecControl ctrl;
  H5B2_class_t cls = MakeCodecClass(H5B2_TEST_ID);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());
  H5F_t* f = GetH5File(file.get());
  ASSERT_NE(f, nullptr);

  H5B2_create_t cparam{};
  cparam.cls = &cls;
  cparam.node_size = 128;
  cparam.rrec_size = 8;
  cparam.split_percent = 100;
  cparam.merge_percent = 40;

  ScopedB2 bt2(H5B2_create(f, &cparam, &ctrl));
  ASSERT_NE(bt2.get(), nullptr);
  H5B2_hdr_t* hdr = bt2.get()->hdr;

  std::vector<hsize_t> recs = {100, 200, 300};
  std::vector<uint8_t> nat_buf(hdr->node_info[0].max_nrec * sizeof(hsize_t));
  std::memcpy(nat_buf.data(), recs.data(), recs.size() * sizeof(hsize_t));

  H5B2_leaf_t src_leaf{};
  src_leaf.hdr = hdr;
  src_leaf.nrec = 3;
  src_leaf.leaf_native = nat_buf.data();

  H5B2_leaf_cache_ud_t udata{};
  udata.f = f;
  udata.hdr = hdr;
  udata.parent = nullptr;
  udata.nrec = 3;

  size_t init_len = 0;
  ASSERT_GE(H5AC_BT2_LEAF[0].get_initial_load_size(&udata, &init_len), 0);
  EXPECT_EQ(init_len, hdr->node_size);

  size_t img_len = 0;
  ASSERT_GE(H5AC_BT2_LEAF[0].image_len(&src_leaf, &img_len), 0);
  EXPECT_EQ(img_len, hdr->node_size);

  std::vector<uint8_t> image(img_len, 0);
  ASSERT_GE(H5AC_BT2_LEAF[0].serialize(f, image.data(), img_len, &src_leaf), 0);

  EXPECT_EQ(H5AC_BT2_LEAF[0].verify_chksum(image.data(), img_len, &udata), 1);
  {
    std::vector<uint8_t> flipped = image;
    flipped[1] ^= 0x08;
    EXPECT_EQ(H5AC_BT2_LEAF[0].verify_chksum(flipped.data(), img_len, &udata),
              0);
  }

  bool dirty = false;
  void* deser_raw =
      H5AC_BT2_LEAF[0].deserialize(image.data(), img_len, &udata, &dirty);
  ASSERT_NE(deser_raw, nullptr);
  auto* deser_leaf = static_cast<H5B2_leaf_t*>(deser_raw);
  EXPECT_EQ(deser_leaf->nrec, 3);

  {
    ScopedErrorSilence silence;

    std::vector<uint8_t> bad_magic = image;
    bad_magic[0] = 'Q';
    EXPECT_EQ(
        H5AC_BT2_LEAF[0].deserialize(bad_magic.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_ver = image;
    bad_ver[H5_SIZEOF_MAGIC] = 42;
    EXPECT_EQ(
        H5AC_BT2_LEAF[0].deserialize(bad_ver.data(), img_len, &udata, &dirty),
        nullptr);

    std::vector<uint8_t> bad_id = image;
    bad_id[H5_SIZEOF_MAGIC + 1] = static_cast<uint8_t>(cls.id) ^ 1U;
    EXPECT_EQ(
        H5AC_BT2_LEAF[0].deserialize(bad_id.data(), img_len, &udata, &dirty),
        nullptr);

    ctrl.fail_decode = true;
    EXPECT_EQ(
        H5AC_BT2_LEAF[0].deserialize(image.data(), img_len, &udata, &dirty),
        nullptr);
    ctrl.fail_decode = false;

    ctrl.fail_encode = true;
    std::vector<uint8_t> tmp_img(img_len, 0);
    EXPECT_LT(
        H5AC_BT2_LEAF[0].serialize(f, tmp_img.data(), img_len, &src_leaf), 0);
    ctrl.fail_encode = false;
  }

  // Notify callback branches (non-SWMR and SWMR non-evict/unknown).
  EXPECT_GE(H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_AFTER_INSERT, deser_leaf),
            0);
  const bool saved_swmr = hdr->swmr_write;
  auto restore_swmr =
      absl::MakeCleanup([hdr, saved_swmr] { hdr->swmr_write = saved_swmr; });
  hdr->swmr_write = true;
  EXPECT_GE(H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_AFTER_FLUSH, deser_leaf),
            0);
  EXPECT_GE(
      H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_ENTRY_DIRTIED, deser_leaf), 0);
  EXPECT_GE(
      H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_ENTRY_CLEANED, deser_leaf), 0);
  EXPECT_GE(
      H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_CHILD_DIRTIED, deser_leaf), 0);
  EXPECT_GE(
      H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_CHILD_CLEANED, deser_leaf), 0);
  EXPECT_GE(H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_CHILD_UNSERIALIZED,
                                    deser_leaf),
            0);
  EXPECT_GE(
      H5AC_BT2_LEAF[0].notify(H5AC_NOTIFY_ACTION_CHILD_SERIALIZED, deser_leaf),
      0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5AC_BT2_LEAF[0].notify(static_cast<H5AC_notify_action_t>(999),
                                      deser_leaf),
              0);
  }

  EXPECT_GE(H5AC_BT2_LEAF[0].free_icr(deser_leaf), 0);
}

}  // namespace
}  // namespace hdf5
