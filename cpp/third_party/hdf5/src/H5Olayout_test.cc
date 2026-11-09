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
// Tests data layout object header messages: encode/decode across v1-v5 and all
// storage/chunk index classes, malformed decode rejection, native copy/reset/
// free, dataset unlinking/deletion, H5Ocopy across versions/layouts/caches,
// and debug output including VDS mappings (third_party/hdf5/src/H5Olayout.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Olayout.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields in cache headers and
// object headers pulled in via `H5Opkg.h`.
#ifndef NDEBUG
#define H5O_LAYOUT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
// `H5B2private.h` (pulled via `H5Dpkg.h`) uses C parameter name `try`.
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
#include "third_party/hdf5/src/H5Dpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5O_LAYOUT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_LAYOUT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__COPIED_TAG) {
    std::memset(&node_, 0, sizeof(node_));
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
  H5CX_node_t node_;
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

template <typename T>
T ZeroStruct() {
  T val;
  std::memset(&val, 0, sizeof(val));
  return val;
}

H5F_t* GetNativeFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void AppendUint16Le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void AppendUint32Le(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void AppendUint64Le(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

std::string ReadWholeFile(const TempFile& file) {
  std::FILE* fp = std::fopen(file.c_str(), "rb");
  if (fp == nullptr) {
    return "";
  }
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  std::string out;
  char buf[512];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
    out.append(buf, n);
  }
  return out;
}

TEST(H5OlayoutTest, MessageClassMetadataMatches) {
  EXPECT_EQ(H5O_MSG_LAYOUT[0].id, H5O_LAYOUT_ID);
  EXPECT_STREQ(H5O_MSG_LAYOUT[0].name, "layout");
  EXPECT_EQ(H5O_MSG_LAYOUT[0].native_size, sizeof(H5O_layout_t));
}

TEST(H5OlayoutTest, EncodeDecodeCopySizeAndResetAcrossVersionsAndClasses) {
  ScopedFile file = CreateInMemoryFile("layout_rt.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Compact layout: empty and populated buffers, plus NULL-buffer encode.
  {
    H5O_layout_t compact_empty = ZeroStruct<H5O_layout_t>();
    compact_empty.version = H5O_LAYOUT_VERSION_3;
    compact_empty.type = H5D_COMPACT;
    compact_empty.storage.type = H5D_COMPACT;
    compact_empty.storage.u.compact.size = 0;
    compact_empty.storage.u.compact.buf = nullptr;

    const size_t sz = H5O_msg_raw_size(f, H5O_LAYOUT_ID, false, &compact_empty);
    EXPECT_EQ(sz, 4u);
    std::vector<uint8_t> buf(sz, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, buf.data(), &compact_empty),
              0);
    H5O_layout_t* dec = static_cast<H5O_layout_t*>(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, buf.size(), buf.data()));
    ASSERT_NE(dec, nullptr);
    absl::Cleanup free_dec = [dec] { H5O_msg_free(H5O_LAYOUT_ID, dec); };
    EXPECT_EQ(dec->storage.u.compact.size, 0u);
    EXPECT_EQ(dec->storage.u.compact.buf, nullptr);

    H5O_layout_t* cp =
        static_cast<H5O_layout_t*>(H5O_msg_copy(H5O_LAYOUT_ID, dec, nullptr));
    ASSERT_NE(cp, nullptr);
    H5O_msg_free(H5O_LAYOUT_ID, cp);
  }

  {
    uint8_t payload[] = {10, 20, 30, 40};
    H5O_layout_t compact_full = ZeroStruct<H5O_layout_t>();
    compact_full.version = H5O_LAYOUT_VERSION_3;
    compact_full.type = H5D_COMPACT;
    compact_full.storage.type = H5D_COMPACT;
    compact_full.storage.u.compact.size = sizeof(payload);
    compact_full.storage.u.compact.buf = payload;

    const size_t sz = H5O_msg_raw_size(f, H5O_LAYOUT_ID, false, &compact_full);
    EXPECT_EQ(sz, 4u + sizeof(payload));
    std::vector<uint8_t> buf(sz, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, buf.data(), &compact_full),
              0);

    H5O_layout_t* dec = static_cast<H5O_layout_t*>(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, buf.size(), buf.data()));
    ASSERT_NE(dec, nullptr);
    absl::Cleanup free_dec = [dec] { H5O_msg_free(H5O_LAYOUT_ID, dec); };
    EXPECT_EQ(dec->storage.u.compact.size, sizeof(payload));
    EXPECT_EQ(std::memcmp(dec->storage.u.compact.buf, payload, sizeof(payload)),
              0);

    H5O_layout_t cp_inplace = ZeroStruct<H5O_layout_t>();
    ASSERT_EQ(H5O_msg_copy(H5O_LAYOUT_ID, dec, &cp_inplace), &cp_inplace);
    absl::Cleanup reset_cp = [&cp_inplace] {
      H5O_msg_reset(H5O_LAYOUT_ID, &cp_inplace);
    };
    EXPECT_NE(cp_inplace.storage.u.compact.buf, dec->storage.u.compact.buf);

    // Compact with size > 0 and buf == nullptr zero-fills on encode.
    H5O_layout_t compact_zero_fill = ZeroStruct<H5O_layout_t>();
    compact_zero_fill.version = H5O_LAYOUT_VERSION_3;
    compact_zero_fill.type = H5D_COMPACT;
    compact_zero_fill.storage.type = H5D_COMPACT;
    compact_zero_fill.storage.u.compact.size = 4;
    compact_zero_fill.storage.u.compact.buf = nullptr;
    std::vector<uint8_t> zbuf(8, 0xaa);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, zbuf.data(),
                             &compact_zero_fill),
              0);
    EXPECT_EQ(zbuf[4], 0);
    EXPECT_EQ(zbuf[7], 0);
  }

  // 2. Contiguous layout: v1 upgrade on encode + v3 round-trip.
  {
    H5O_layout_t contig = ZeroStruct<H5O_layout_t>();
    contig.version = H5O_LAYOUT_VERSION_1;
    contig.type = H5D_CONTIGUOUS;
    contig.storage.type = H5D_CONTIGUOUS;
    contig.storage.u.contig.addr = 0x1000;
    contig.storage.u.contig.size = 4096;

    const size_t sz = H5O_msg_raw_size(f, H5O_LAYOUT_ID, false, &contig);
    std::vector<uint8_t> buf(sz, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, buf.data(), &contig), 0);
    EXPECT_EQ(buf[0], H5O_LAYOUT_VERSION_3);

    H5O_layout_t* dec = static_cast<H5O_layout_t*>(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, buf.size(), buf.data()));
    ASSERT_NE(dec, nullptr);
    absl::Cleanup free_dec = [dec] { H5O_msg_free(H5O_LAYOUT_ID, dec); };
    EXPECT_EQ(dec->storage.u.contig.addr, 0x1000u);
    EXPECT_EQ(dec->storage.u.contig.size, 4096u);

    H5O_layout_t* cp =
        static_cast<H5O_layout_t*>(H5O_msg_copy(H5O_LAYOUT_ID, dec, nullptr));
    ASSERT_NE(cp, nullptr);
    H5O_msg_free(H5O_LAYOUT_ID, cp);
  }

  // 3. Chunked v3 (BTREE) and v4/v5 (NONE, SINGLE unfiltered, SINGLE filtered,
  //    FARRAY, EARRAY, BT2).
  auto check_chunked_roundtrip = [&](const H5O_layout_t& src) {
    const size_t sz = H5O_msg_raw_size(f, H5O_LAYOUT_ID, false, &src);
    ASSERT_GT(sz, 0u);
    std::vector<uint8_t> buf(sz, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, buf.data(), &src), 0);
    H5O_layout_t* dec = static_cast<H5O_layout_t*>(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, buf.size(), buf.data()));
    ASSERT_NE(dec, nullptr);
    absl::Cleanup free_dec = [dec] { H5O_msg_free(H5O_LAYOUT_ID, dec); };
    EXPECT_EQ(dec->u.chunk.idx_type, src.u.chunk.idx_type);
    EXPECT_EQ(dec->storage.u.chunk.idx_addr, src.storage.u.chunk.idx_addr);

    H5O_layout_t* cp =
        static_cast<H5O_layout_t*>(H5O_msg_copy(H5O_LAYOUT_ID, dec, nullptr));
    ASSERT_NE(cp, nullptr);
    H5O_msg_free(H5O_LAYOUT_ID, cp);
  };

  {
    H5O_layout_t btree3 = ZeroStruct<H5O_layout_t>();
    btree3.version = H5O_LAYOUT_VERSION_3;
    btree3.type = H5D_CHUNKED;
    btree3.storage.type = H5D_CHUNKED;
    btree3.u.chunk.idx_type = H5D_CHUNK_IDX_BTREE;
    btree3.storage.u.chunk.idx_type = H5D_CHUNK_IDX_BTREE;
    btree3.storage.u.chunk.idx_addr = 0x2200;
    btree3.storage.u.chunk.ops = H5D_COPS_BTREE;
    btree3.u.chunk.ndims = 3;
    btree3.u.chunk.dim[0] = 4;
    btree3.u.chunk.dim[1] = 8;
    btree3.u.chunk.dim[2] = 4;
    check_chunked_roundtrip(btree3);

    H5O_layout_t none4 = ZeroStruct<H5O_layout_t>();
    none4.version = H5O_LAYOUT_VERSION_4;
    none4.type = H5D_CHUNKED;
    none4.storage.type = H5D_CHUNKED;
    none4.u.chunk.idx_type = H5D_CHUNK_IDX_NONE;
    none4.storage.u.chunk.idx_type = H5D_CHUNK_IDX_NONE;
    none4.storage.u.chunk.idx_addr = 0x3300;
    none4.storage.u.chunk.ops = nullptr;
    none4.u.chunk.ndims = 2;
    none4.u.chunk.enc_bytes_per_dim = 2;
    none4.u.chunk.dim[0] = 10;
    none4.u.chunk.dim[1] = 20;
    check_chunked_roundtrip(none4);

    H5O_layout_t single4 = none4;
    single4.u.chunk.idx_type = H5D_CHUNK_IDX_SINGLE;
    single4.storage.u.chunk.idx_type = H5D_CHUNK_IDX_SINGLE;
    check_chunked_roundtrip(single4);

    H5O_layout_t single5_filt = single4;
    single5_filt.version = H5O_LAYOUT_VERSION_5;
    single5_filt.u.chunk.flags = H5O_LAYOUT_CHUNK_SINGLE_INDEX_WITH_FILTER;
    single5_filt.storage.u.chunk.u.single.nbytes = 512;
    single5_filt.storage.u.chunk.u.single.filter_mask = 0x1;
    check_chunked_roundtrip(single5_filt);

    H5O_layout_t farray4 = none4;
    farray4.u.chunk.idx_type = H5D_CHUNK_IDX_FARRAY;
    farray4.storage.u.chunk.idx_type = H5D_CHUNK_IDX_FARRAY;
    farray4.u.chunk.u.farray.cparam.max_dblk_page_nelmts_bits = 10;
    check_chunked_roundtrip(farray4);

    H5O_layout_t earray4 = none4;
    earray4.u.chunk.idx_type = H5D_CHUNK_IDX_EARRAY;
    earray4.storage.u.chunk.idx_type = H5D_CHUNK_IDX_EARRAY;
    earray4.u.chunk.u.earray.cparam.max_nelmts_bits = 32;
    earray4.u.chunk.u.earray.cparam.idx_blk_elmts = 4;
    earray4.u.chunk.u.earray.cparam.sup_blk_min_data_ptrs = 4;
    earray4.u.chunk.u.earray.cparam.data_blk_min_elmts = 16;
    earray4.u.chunk.u.earray.cparam.max_dblk_page_nelmts_bits = 10;
    check_chunked_roundtrip(earray4);

    H5O_layout_t bt2_4 = none4;
    bt2_4.u.chunk.idx_type = H5D_CHUNK_IDX_BT2;
    bt2_4.storage.u.chunk.idx_type = H5D_CHUNK_IDX_BT2;
    bt2_4.u.chunk.u.btree2.cparam.node_size = 2048;
    bt2_4.u.chunk.u.btree2.cparam.split_percent = 100;
    bt2_4.u.chunk.u.btree2.cparam.merge_percent = 40;
    check_chunked_roundtrip(bt2_4);
  }

  // 4. Virtual layout encode/decode/copy/reset round-trip.
  {
    H5O_layout_t virt = ZeroStruct<H5O_layout_t>();
    virt.version = H5O_LAYOUT_VERSION_4;
    virt.type = H5D_VIRTUAL;
    virt.storage.type = H5D_VIRTUAL;
    virt.storage.u.virt.serial_list_hobjid.addr = 0x4400;
    virt.storage.u.virt.serial_list_hobjid.idx = 3;
    virt.storage.u.virt.source_fapl = H5I_INVALID_HID;
    virt.storage.u.virt.source_dapl = H5I_INVALID_HID;

    const size_t sz = H5O_msg_raw_size(f, H5O_LAYOUT_ID, false, &virt);
    EXPECT_EQ(sz, 2u + H5F_SIZEOF_ADDR(f) + 4u);
    std::vector<uint8_t> buf(sz, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_LAYOUT_ID, false, buf.data(), &virt), 0);
    H5O_layout_t* dec = static_cast<H5O_layout_t*>(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, buf.size(), buf.data()));
    ASSERT_NE(dec, nullptr);
    absl::Cleanup free_dec = [dec] { H5O_msg_free(H5O_LAYOUT_ID, dec); };
    EXPECT_EQ(dec->storage.u.virt.serial_list_hobjid.addr, 0x4400u);
    EXPECT_EQ(dec->storage.u.virt.serial_list_hobjid.idx, 3u);

    H5O_layout_t* cp =
        static_cast<H5O_layout_t*>(H5O_msg_copy(H5O_LAYOUT_ID, dec, nullptr));
    ASSERT_NE(cp, nullptr);
    H5O_msg_free(H5O_LAYOUT_ID, cp);
  }

  // 5. Legacy v1 & v2 decodes across CONTIGUOUS, CHUNKED, and COMPACT.
  {
    // v1 contiguous (ndims = 2, 5 reserved, 8-byte addr, 2 * 4-byte dims).
    std::vector<uint8_t> v1_contig = {H5O_LAYOUT_VERSION_1, 2, H5D_CONTIGUOUS,
                                      0, 0, 0, 0, 0};
    AppendUint64Le(v1_contig, 0x5000);
    AppendUint32Le(v1_contig, 10);
    AppendUint32Le(v1_contig, 20);
    H5O_layout_t* dec = static_cast<H5O_layout_t*>(H5O_msg_decode(
        f, nullptr, H5O_LAYOUT_ID, v1_contig.size(), v1_contig.data()));
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->version, H5O_LAYOUT_VERSION_1);
    EXPECT_EQ(dec->type, H5D_CONTIGUOUS);
    EXPECT_EQ(dec->storage.u.contig.addr, 0x5000u);
    H5O_msg_free(H5O_LAYOUT_ID, dec);

    // v2 chunked (ndims = 3, 5 reserved, 8-byte idx_addr, 3 * 4-byte dims).
    std::vector<uint8_t> v2_chunk = {H5O_LAYOUT_VERSION_2, 3, H5D_CHUNKED,
                                     0, 0, 0, 0, 0};
    AppendUint64Le(v2_chunk, 0x6000);
    AppendUint32Le(v2_chunk, 2);
    AppendUint32Le(v2_chunk, 3);
    AppendUint32Le(v2_chunk, 4);
    H5O_layout_t* dec_c = static_cast<H5O_layout_t*>(H5O_msg_decode(
        f, nullptr, H5O_LAYOUT_ID, v2_chunk.size(), v2_chunk.data()));
    ASSERT_NE(dec_c, nullptr);
    EXPECT_EQ(dec_c->version, H5O_LAYOUT_VERSION_2);
    EXPECT_EQ(dec_c->u.chunk.size, 24u);
    H5O_msg_free(H5O_LAYOUT_ID, dec_c);

    // v1 compact with size == 0 and v2 compact with size == 4.
    std::vector<uint8_t> v1_comp0 = {H5O_LAYOUT_VERSION_1, 1, H5D_COMPACT,
                                     0, 0, 0, 0, 0};
    AppendUint32Le(v1_comp0, 1);
    AppendUint32Le(v1_comp0, 0);
    H5O_layout_t* dec_k0 = static_cast<H5O_layout_t*>(H5O_msg_decode(
        f, nullptr, H5O_LAYOUT_ID, v1_comp0.size(), v1_comp0.data()));
    ASSERT_NE(dec_k0, nullptr);
    EXPECT_EQ(dec_k0->storage.u.compact.size, 0u);
    H5O_msg_free(H5O_LAYOUT_ID, dec_k0);

    std::vector<uint8_t> v2_comp4 = {H5O_LAYOUT_VERSION_2, 1, H5D_COMPACT,
                                     0, 0, 0, 0, 0};
    AppendUint32Le(v2_comp4, 1);
    AppendUint32Le(v2_comp4, 4);
    v2_comp4.insert(v2_comp4.end(), {1, 2, 3, 4});
    H5O_layout_t* dec_k4 = static_cast<H5O_layout_t*>(H5O_msg_decode(
        f, nullptr, H5O_LAYOUT_ID, v2_comp4.size(), v2_comp4.data()));
    ASSERT_NE(dec_k4, nullptr);
    EXPECT_EQ(dec_k4->storage.u.compact.size, 4u);
    H5O_msg_free(H5O_LAYOUT_ID, dec_k4);
  }

  EXPECT_GE(H5O_msg_reset(H5O_LAYOUT_ID, nullptr), 0);
}

TEST(H5OlayoutTest, DecodeEncodeCopyAndResetRejectInvalidInputs) {
  ScopedFile file = CreateInMemoryFile("layout_bad.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetNativeFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  auto expect_bad_decode = [&](const std::vector<uint8_t>& bytes) {
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, bytes.size(), bytes.data()),
        nullptr);
  };

  // 0-byte buffer with valid end sentinel (tiny + 0 - 1 == tiny).
  uint8_t tiny[2] = {0, 0};
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_LAYOUT_ID, 0, tiny + 1), nullptr);

  // Version out of range.
  expect_bad_decode({0, 1, H5D_CONTIGUOUS});
  expect_bad_decode({H5O_LAYOUT_VERSION_LATEST + 1, 1, H5D_CONTIGUOUS});

  // v1/v2 bad ndims, truncated fields, bad layout types, 0 dims.
  expect_bad_decode({H5O_LAYOUT_VERSION_1});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, H5O_LAYOUT_NDIMS + 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 2});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 2, H5D_VIRTUAL, 0, 0, 0, 0, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 2, H5D_CONTIGUOUS, 0, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 2, H5D_CONTIGUOUS, 0, 0, 0, 0, 0, 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_1, 2, H5D_CHUNKED, 0, 0, 0, 0, 0, 1});

  // v1 contiguous truncated dims.
  std::vector<uint8_t> v1_trunc_dims = {H5O_LAYOUT_VERSION_1, 2, H5D_CONTIGUOUS,
                                        0, 0, 0, 0, 0};
  AppendUint64Le(v1_trunc_dims, 0x1000);
  v1_trunc_dims.push_back(1);
  expect_bad_decode(v1_trunc_dims);

  // v1 chunked ndims < 2.
  std::vector<uint8_t> v1_chunk_nd1 = {H5O_LAYOUT_VERSION_1, 1, H5D_CHUNKED,
                                       0, 0, 0, 0, 0};
  AppendUint64Le(v1_chunk_nd1, 0x1000);
  AppendUint32Le(v1_chunk_nd1, 4);
  expect_bad_decode(v1_chunk_nd1);

  // v1 chunked truncated dims & dim == 0.
  std::vector<uint8_t> v1_chunk_trunc = {H5O_LAYOUT_VERSION_1, 2, H5D_CHUNKED,
                                         0, 0, 0, 0, 0};
  AppendUint64Le(v1_chunk_trunc, 0x1000);
  v1_chunk_trunc.push_back(4);
  expect_bad_decode(v1_chunk_trunc);

  std::vector<uint8_t> v1_chunk_zero = {H5O_LAYOUT_VERSION_1, 2, H5D_CHUNKED,
                                        0, 0, 0, 0, 0};
  AppendUint64Le(v1_chunk_zero, 0x1000);
  AppendUint32Le(v1_chunk_zero, 4);
  AppendUint32Le(v1_chunk_zero, 0);
  expect_bad_decode(v1_chunk_zero);

  // v1 compact truncated size & overflow payload.
  std::vector<uint8_t> v1_comp_trunc = {H5O_LAYOUT_VERSION_1, 1, H5D_COMPACT,
                                        0, 0, 0, 0, 0};
  AppendUint32Le(v1_comp_trunc, 1);
  v1_comp_trunc.push_back(1);
  expect_bad_decode(v1_comp_trunc);

  std::vector<uint8_t> v1_comp_over = {H5O_LAYOUT_VERSION_1, 1, H5D_COMPACT,
                                       0, 0, 0, 0, 0};
  AppendUint32Le(v1_comp_over, 1);
  AppendUint32Le(v1_comp_over, 16);
  v1_comp_over.push_back(7);
  expect_bad_decode(v1_comp_over);

  // v3+ missing class, invalid class, compact/contiguous errors.
  expect_bad_decode({H5O_LAYOUT_VERSION_3});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_NLAYOUTS});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_COMPACT, 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_COMPACT, 10, 0, 1, 2});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_CONTIGUOUS, 1, 2});
  expect_bad_decode(
      {H5O_LAYOUT_VERSION_3, H5D_CONTIGUOUS, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2});

  // v3 chunked errors: truncated ndims, too big ndims, ndims < 2, trunc addr,
  // trunc dims, zero dim.
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_CHUNKED});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_CHUNKED, H5O_LAYOUT_NDIMS + 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_CHUNKED, 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_CHUNKED, 2, 1});
  std::vector<uint8_t> v3_trunc_dims = {H5O_LAYOUT_VERSION_3, H5D_CHUNKED, 2};
  AppendUint64Le(v3_trunc_dims, 0x1000);
  v3_trunc_dims.push_back(1);
  expect_bad_decode(v3_trunc_dims);
  std::vector<uint8_t> v3_zero_dim = {H5O_LAYOUT_VERSION_3, H5D_CHUNKED, 2};
  AppendUint64Le(v3_zero_dim, 0x1000);
  AppendUint32Le(v3_zero_dim, 4);
  AppendUint32Le(v3_zero_dim, 0);
  expect_bad_decode(v3_zero_dim);

  // v4 chunked errors: trunc flags, bad flags, trunc ndims, large ndims,
  // trunc enc_bytes_per_dim, enc_bytes == 0 / 9, trunc dims, dim == 0.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0x80});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, H5O_LAYOUT_NDIMS + 1});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 9});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 0});

  // v4 chunked idx_type errors: trunc idx_type, unknown idx_type, BTREE in v4,
  // trunc idx_addr.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_NTYPES});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BTREE});
  expect_bad_decode(
      {H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4, H5D_CHUNK_IDX_NONE, 1});

  // v4 SINGLE filtered truncated.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED,
                     H5O_LAYOUT_CHUNK_SINGLE_INDEX_WITH_FILTER, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_SINGLE, 1, 2});

  // v4 FARRAY truncated / zero cparam.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_FARRAY});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_FARRAY, 0});

  // v4 EARRAY truncated & zero on each of the 5 bytes.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4, 4});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4, 4, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4, 4, 16});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_EARRAY, 32, 4, 4, 16, 0});

  // v4 BT2 truncated & bad split/merge percents.
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 1, 2});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0, 101});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0, 100});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0, 100, 0});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_CHUNKED, 0, 2, 1, 4, 4,
                     H5D_CHUNK_IDX_BT2, 0, 8, 0, 0, 100, 101});

  // Virtual layout errors: v3 (also hits done: H5D__virtual_reset_layout with
  // zeroed fapl=0), truncated addr, truncated idx.
  expect_bad_decode({H5O_LAYOUT_VERSION_3, H5D_VIRTUAL});
  expect_bad_decode({H5O_LAYOUT_VERSION_4, H5D_VIRTUAL, 1, 2});
  expect_bad_decode(
      {H5O_LAYOUT_VERSION_4, H5D_VIRTUAL, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2});

  // Encode/copy/reset invalid inputs.
  std::vector<uint8_t> scratch(64, 0);
  H5O_layout_t bad_class = ZeroStruct<H5O_layout_t>();
  bad_class.version = H5O_LAYOUT_VERSION_4;
  bad_class.type = H5D_NLAYOUTS;
  EXPECT_LT(H5O_msg_encode(f, H5O_LAYOUT_ID, false, scratch.data(), &bad_class),
            0);
  EXPECT_EQ(H5O_msg_copy(H5O_LAYOUT_ID, &bad_class, nullptr), nullptr);

  H5O_layout_t bad_v4_btree = ZeroStruct<H5O_layout_t>();
  bad_v4_btree.version = H5O_LAYOUT_VERSION_4;
  bad_v4_btree.type = H5D_CHUNKED;
  bad_v4_btree.u.chunk.ndims = 2;
  bad_v4_btree.u.chunk.enc_bytes_per_dim = 1;
  bad_v4_btree.u.chunk.dim[0] = 2;
  bad_v4_btree.u.chunk.dim[1] = 2;
  bad_v4_btree.u.chunk.idx_type = H5D_CHUNK_IDX_BTREE;
  EXPECT_LT(
      H5O_msg_encode(f, H5O_LAYOUT_ID, false, scratch.data(), &bad_v4_btree),
      0);

  bad_v4_btree.u.chunk.idx_type = H5D_CHUNK_IDX_NTYPES;
  EXPECT_LT(
      H5O_msg_encode(f, H5O_LAYOUT_ID, false, scratch.data(), &bad_v4_btree),
      0);

  // H5O__layout_reset error path when virtual layout holds an invalid source
  // FAPL ID >= 0.
  H5O_layout_t bad_virt_reset = ZeroStruct<H5O_layout_t>();
  bad_virt_reset.type = H5D_VIRTUAL;
  bad_virt_reset.storage.type = H5D_VIRTUAL;
  bad_virt_reset.storage.u.virt.source_fapl = 999999;
  bad_virt_reset.storage.u.virt.source_dapl = H5I_INVALID_HID;
  EXPECT_LT(H5O_msg_reset(H5O_LAYOUT_ID, &bad_virt_reset), 0);
}

TEST(H5OlayoutTest, PublicDatasetsCopyVersionBoundsDeleteAndVirtual) {
  TempDir dir("layout_vds");
  const std::string src_path = dir.child("src.h5").native();
  const std::string dst_latest_path = dir.child("dst_latest.h5").native();

  ScopedPlist fapl_latest(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_latest.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
      0);

  ScopedPlist fapl_v18 = MakeCoreFapl();
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v18.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);

  ScopedFile src(H5Fcreate(src_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl_latest.get()));
  ScopedFile dst_latest(H5Fcreate(dst_latest_path.c_str(), H5F_ACC_TRUNC,
                                  H5P_DEFAULT, fapl_latest.get()));
  ScopedFile dst_v18(
      H5Fcreate("v18_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v18.get()));
  ScopedFile src_v18(
      H5Fcreate("v18_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v18.get()));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst_latest.valid());
  ASSERT_TRUE(dst_v18.valid());
  ASSERT_TRUE(src_v18.valid());

  // Compact datasets: empty NULL space & populated space.
  {
    ScopedSpace space_null(H5Screate(H5S_NULL));
    ScopedSpace space4(H5Screate_simple(1, std::vector<hsize_t>{4}.data(),
                                         nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl.get(), H5D_COMPACT), 0);
    ScopedDataset d_null(H5Dcreate2(src.get(), "/compact_null", H5T_NATIVE_INT32,
                                 space_null.get(), H5P_DEFAULT, dcpl.get(),
                                 H5P_DEFAULT));
    ScopedDataset d_pop(H5Dcreate2(src.get(), "/compact_pop", H5T_NATIVE_INT32,
                                space4.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d_null.valid());
    ASSERT_TRUE(d_pop.valid());
    const int32_t data[4] = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d_pop.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data),
              0);
  }
  ASSERT_GE(H5Ocopy(src.get(), "/compact_null", dst_latest.get(),
                    "/compact_null", H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/compact_pop", dst_latest.get(),
                    "/compact_pop", H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Contiguous datasets: allocated, unallocated (ALLOC_TIME_LATE), and open
  // with cached sieve buffer.
  {
    const hsize_t dims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_alloc_time(dcpl_late.get(), H5D_ALLOC_TIME_LATE), 0);

    ScopedDataset d_late(H5Dcreate2(src.get(), "/contig_unalloc", H5T_NATIVE_INT32,
                                 space.get(), H5P_DEFAULT, dcpl_late.get(),
                                 H5P_DEFAULT));
    ScopedDataset d_open(H5Dcreate2(src.get(), "/contig_open", H5T_NATIVE_INT32,
                                 space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(d_late.valid());
    ASSERT_TRUE(d_open.valid());

    // Copy both unallocated and open contiguous datasets.
    ASSERT_GE(H5Ocopy(src.get(), "/contig_unalloc", dst_latest.get(),
                      "/contig_unalloc", H5P_DEFAULT, H5P_DEFAULT),
              0);
    const int32_t vals[4] = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(d_open.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/contig_open", dst_latest.get(),
                      "/contig_open", H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Chunked datasets across all index types + cached open chunk copy.
  {
    const hsize_t dims2[2] = {4, 4};
    const hsize_t chunk2[2] = {2, 2};
    const hsize_t unlim2[2] = {H5S_UNLIMITED, H5S_UNLIMITED};
    const hsize_t one_unlim[2] = {H5S_UNLIMITED, 4};

    ScopedSpace fixed_sp(H5Screate_simple(2, dims2, nullptr));
    ScopedSpace earray_sp(H5Screate_simple(2, dims2, one_unlim));
    ScopedSpace bt2_sp(H5Screate_simple(2, dims2, unlim2));

    // v1 B-tree (v18 file) + open cached write.
    ScopedPlist dcpl_bt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl_bt.get(), 2, chunk2), 0);
    ScopedDataset d_bt(H5Dcreate2(src_v18.get(), "/chunk_btree", H5T_NATIVE_INT32,
                               fixed_sp.get(), H5P_DEFAULT, dcpl_bt.get(),
                               H5P_DEFAULT));
    ASSERT_TRUE(d_bt.valid());
    const int32_t data16[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(d_bt.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data16),
              0);
    ASSERT_GE(H5Ocopy(src_v18.get(), "/chunk_btree", dst_v18.get(),
                      "/chunk_btree", H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Implicit (NONE), SINGLE unfiltered, SINGLE filtered (Fletcher32), FARRAY,
    // EARRAY, BT2, plus unallocated FARRAY copy.
    ScopedPlist dcpl_none(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl_none.get(), 2, chunk2), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_none.get(), H5D_ALLOC_TIME_EARLY), 0);

    ScopedPlist dcpl_single(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl_single.get(), 2, dims2), 0);

    ScopedPlist dcpl_single_filt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl_single_filt.get(), 2, dims2), 0);
    ASSERT_GE(H5Pset_fletcher32(dcpl_single_filt.get()), 0);

    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl_late.get(), 2, chunk2), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_late.get(), H5D_ALLOC_TIME_LATE), 0);

    ScopedDataset d_none(H5Dcreate2(src.get(), "/chunk_none", H5T_NATIVE_INT32,
                                 fixed_sp.get(), H5P_DEFAULT, dcpl_none.get(),
                                 H5P_DEFAULT));
    ScopedDataset d_single(H5Dcreate2(src.get(), "/chunk_single", H5T_NATIVE_INT32,
                                   fixed_sp.get(), H5P_DEFAULT,
                                   dcpl_single.get(), H5P_DEFAULT));
    ScopedDataset d_sfilt(H5Dcreate2(src.get(), "/chunk_sfilt", H5T_NATIVE_INT32,
                                  fixed_sp.get(), H5P_DEFAULT,
                                  dcpl_single_filt.get(), H5P_DEFAULT));
    ScopedDataset d_farray(H5Dcreate2(src.get(), "/chunk_farray", H5T_NATIVE_INT32,
                                   fixed_sp.get(), H5P_DEFAULT, dcpl_late.get(),
                                   H5P_DEFAULT));
    ScopedDataset d_earray(H5Dcreate2(src.get(), "/chunk_earray", H5T_NATIVE_INT32,
                                   earray_sp.get(), H5P_DEFAULT,
                                   dcpl_late.get(), H5P_DEFAULT));
    ScopedDataset d_bt2(H5Dcreate2(src.get(), "/chunk_bt2", H5T_NATIVE_INT32,
                                bt2_sp.get(), H5P_DEFAULT, dcpl_late.get(),
                                H5P_DEFAULT));
    ScopedDataset d_unalloc(H5Dcreate2(src.get(), "/chunk_unalloc",
                                    H5T_NATIVE_INT32, fixed_sp.get(),
                                    H5P_DEFAULT, dcpl_late.get(), H5P_DEFAULT));
    ASSERT_TRUE(d_none.valid());
    ASSERT_TRUE(d_single.valid());
    ASSERT_TRUE(d_sfilt.valid());
    ASSERT_TRUE(d_farray.valid());
    ASSERT_TRUE(d_earray.valid());
    ASSERT_TRUE(d_bt2.valid());
    ASSERT_TRUE(d_unalloc.valid());

    for (hid_t did : {d_single.get(), d_sfilt.get(), d_farray.get(),
                      d_earray.get(), d_bt2.get()}) {
      ASSERT_GE(H5Dwrite(did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         data16),
                0);
    }

    for (const char* name : {"/chunk_none", "/chunk_single", "/chunk_sfilt",
                             "/chunk_farray", "/chunk_earray", "/chunk_bt2",
                             "/chunk_unalloc"}) {
      ASSERT_GE(
          H5Ocopy(src.get(), name, dst_latest.get(), name, H5P_DEFAULT, H5P_DEFAULT),
          0);
    }

    // Copying a v4+ chunked layout into a v18-bound file fails version check
    // in H5O__layout_pre_copy_file.
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(src.get(), "/chunk_farray", dst_v18.get(), "/bad_ver",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Virtual dataset: write mapping, copy to dst_latest, inspect debug with
  // mappings loaded, and delete VDS dataset.
  {
    const hsize_t sdims[1] = {4};
    ScopedSpace src_sp(H5Screate_simple(1, sdims, nullptr));
    ScopedSpace vds_sp(H5Screate_simple(1, sdims, nullptr));
    ScopedDataset src_dset(H5Dcreate2(src.get(), "/vds_src", H5T_NATIVE_INT32,
                                   src_sp.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(src_dset.valid());
    const int32_t vals[4] = {100, 200, 300, 400};
    ASSERT_GE(H5Dwrite(src_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, vals),
              0);

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_vds.get(), vds_sp.get(), ".", "/vds_src",
                             src_sp.get()),
              0);
    ScopedDataset vds(H5Dcreate2(src.get(), "/vds_dset", H5T_NATIVE_INT32,
                              vds_sp.get(), H5P_DEFAULT, dcpl_vds.get(),
                              H5P_DEFAULT));
    ASSERT_TRUE(vds.valid());

    ASSERT_GE(H5Ocopy(src.get(), "/vds_dset", dst_latest.get(), "/vds_copy",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Load virtual layout and dump via H5O_debug_id to cover mapping loop.
    H5F_t* f_src = GetNativeFile(src.get());
    H5O_loc_t* vds_loc = H5O_get_loc(vds.get());
    ASSERT_NE(f_src, nullptr);
    ASSERT_NE(vds_loc, nullptr);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());

    H5O_layout_t vlayout = ZeroStruct<H5O_layout_t>();
    ASSERT_NE(H5O_msg_read(vds_loc, H5O_LAYOUT_ID, &vlayout), nullptr);
    absl::Cleanup reset_vlayout = [&vlayout] {
      H5O_msg_reset(H5O_LAYOUT_ID, &vlayout);
    };
    ASSERT_GE(H5D__virtual_load_layout(f_src, &vlayout), 0);

    TempFile dbg_vds("layout_vds_debug.txt");
    std::FILE* fp = std::fopen(dbg_vds.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    EXPECT_GE(H5O_debug_id(H5O_LAYOUT_ID, f_src, &vlayout, fp, 0, 40), 0);
    std::fclose(fp);
    const std::string vds_dbg = ReadWholeFile(dbg_vds);
    EXPECT_THAT(vds_dbg, HasSubstr("Virtual"));
    EXPECT_THAT(vds_dbg, HasSubstr("Mapping 0:"));
    EXPECT_THAT(vds_dbg, HasSubstr("/vds_src"));
  }

  // Delete all layout classes to exercise H5O__layout_delete.
  for (const char* name :
       {"/compact_pop", "/contig_unalloc", "/contig_open", "/chunk_none",
        "/chunk_single", "/chunk_sfilt", "/chunk_farray", "/chunk_earray",
        "/chunk_bt2", "/chunk_unalloc", "/vds_dset"}) {
    ASSERT_GE(H5Ldelete(src.get(), name, H5P_DEFAULT), 0);
  }
}

TEST(H5OlayoutTest, DirectDeleteAndCopyFileEdgeCasesAndDebugVariants) {
  ScopedFile src_file = CreateInMemoryFile("layout_edge_src.h5");
  ScopedFile dst_file = CreateInMemoryFile("layout_edge_dst.h5");
  ASSERT_TRUE(src_file.valid());
  ASSERT_TRUE(dst_file.valid());

  const hsize_t dims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ScopedDataset dset(H5Dcreate2(src_file.get(), "/dset", H5T_NATIVE_INT32,
                             space.get(), H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5F_t* f_src = GetNativeFile(src_file.get());
  H5F_t* f_dst = GetNativeFile(dst_file.get());
  H5O_loc_t* dset_loc = H5O_get_loc(dset.get());
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);
  ASSERT_NE(dset_loc, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // Prepare H5D_copy_file_ud_t using real extent and native int32 datatype.
  const H5S_t* s_obj =
      static_cast<const H5S_t*>(H5I_object_verify(space.get(), H5I_DATASPACE));
  ASSERT_NE(s_obj, nullptr);
  const H5S_extent_t* ext = H5S_get_simple_extent(s_obj);
  ASSERT_NE(ext, nullptr);
  H5S_extent_t* ext_copy = static_cast<H5S_extent_t*>(
      H5O_msg_copy(H5O_SDSPACE_ID, ext, nullptr));
  ASSERT_NE(ext_copy, nullptr);
  absl::Cleanup free_ext = [ext_copy] {
    H5O_msg_free(H5O_SDSPACE_ID, ext_copy);
  };

  H5T_t* dt = static_cast<H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(dt, nullptr);

  H5D_copy_file_ud_t d_udata = ZeroStruct<H5D_copy_file_ud_t>();
  d_udata.src_space_extent = ext_copy;
  d_udata.src_dtype = dt;

  H5O_copy_t cpy_info = ZeroStruct<H5O_copy_t>();
  cpy_info.file_dst = f_dst;
  bool recompute_size = false;
  unsigned mesg_flags = 0;

  // v1 contiguous layout copy_file recomputes size as nelmts * dt_size.
  H5O_layout_t v1_contig = ZeroStruct<H5O_layout_t>();
  v1_contig.version = H5O_LAYOUT_VERSION_1;
  v1_contig.type = H5D_CONTIGUOUS;
  v1_contig.storage.type = H5D_CONTIGUOUS;
  v1_contig.storage.u.contig.addr = HADDR_UNDEF;
  v1_contig.storage.u.contig.size = 0;
  H5O_layout_t* v1_copied = static_cast<H5O_layout_t*>(
      H5O_MSG_LAYOUT[0].copy_file(f_src, &v1_contig, f_dst, &recompute_size,
                                  &mesg_flags, &cpy_info, &d_udata));
  ASSERT_NE(v1_copied, nullptr);
  EXPECT_EQ(v1_copied->storage.u.contig.size, 16u);
  H5O_msg_free(H5O_LAYOUT_ID, v1_copied);

  // Protect oh once for direct del() checks and silence expected failures.
  H5O_t* oh = H5O_protect(dset_loc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(oh, nullptr);
  absl::Cleanup unprot_oh = [&] {
    if (oh != nullptr) {
      H5O_unprotect(dset_loc, oh, H5AC__NO_FLAGS_SET);
    }
  };

  {
    ScopedErrorSilence silence;

    // v3 contiguous with wrong storage size fails sanity check and frees
    // layout_dst in done:.
    H5O_layout_t bad_contig_sz = v1_contig;
    bad_contig_sz.version = H5O_LAYOUT_VERSION_3;
    bad_contig_sz.storage.u.contig.size = 999;
    EXPECT_EQ(H5O_MSG_LAYOUT[0].copy_file(f_src, &bad_contig_sz, f_dst,
                                          &recompute_size, &mesg_flags,
                                          &cpy_info, &d_udata),
              nullptr);

    // Virtual layout with bogus heap object address fails in copy_file.
    H5O_layout_t bad_virt = ZeroStruct<H5O_layout_t>();
    bad_virt.version = H5O_LAYOUT_VERSION_4;
    bad_virt.type = H5D_VIRTUAL;
    bad_virt.storage.type = H5D_VIRTUAL;
    bad_virt.storage.u.virt.serial_list_hobjid.addr = 0xdeadbeef00ULL;
    bad_virt.storage.u.virt.serial_list_hobjid.idx = 1;
    bad_virt.storage.u.virt.source_fapl = H5I_INVALID_HID;
    bad_virt.storage.u.virt.source_dapl = H5I_INVALID_HID;
    EXPECT_EQ(H5O_MSG_LAYOUT[0].copy_file(f_src, &bad_virt, f_dst,
                                          &recompute_size, &mesg_flags,
                                          &cpy_info, &d_udata),
              nullptr);

    // Invalid layout class in copy_file and del.
    H5O_layout_t bad_cls = ZeroStruct<H5O_layout_t>();
    bad_cls.version = H5O_LAYOUT_VERSION_4;
    bad_cls.type = H5D_NLAYOUTS;
    bad_cls.storage.type = H5D_NLAYOUTS;
    EXPECT_EQ(H5O_MSG_LAYOUT[0].copy_file(f_src, &bad_cls, f_dst,
                                          &recompute_size, &mesg_flags,
                                          &cpy_info, &d_udata),
              nullptr);
    EXPECT_LT(H5O_MSG_LAYOUT[0].del(f_src, oh, &bad_cls), 0);

    // Temporary-address contiguous storage fails H5MF_xfree via del.
    const haddr_t tmp_addr = H5MF_alloc_tmp(f_src, 64);
    ASSERT_TRUE(H5_addr_defined(tmp_addr));
    H5O_layout_t bad_contig_del = ZeroStruct<H5O_layout_t>();
    bad_contig_del.version = H5O_LAYOUT_VERSION_3;
    bad_contig_del.type = H5D_CONTIGUOUS;
    bad_contig_del.storage.type = H5D_CONTIGUOUS;
    bad_contig_del.storage.u.contig.addr = tmp_addr;
    bad_contig_del.storage.u.contig.size = 64;
    EXPECT_LT(H5O_MSG_LAYOUT[0].del(f_src, oh, &bad_contig_del), 0);

    // Bogus virtual global heap object fails H5D__virtual_delete via del.
    bad_virt.storage.u.virt.serial_list_hobjid.addr = 0xdeadbeef00ULL;
    EXPECT_LT(H5O_MSG_LAYOUT[0].del(f_src, oh, &bad_virt), 0);
  }

  ASSERT_GE(H5O_unprotect(dset_loc, oh, H5AC__NO_FLAGS_SET), 0);
  oh = nullptr;

  // Debug output for all layout classes and all chunk index types.
  TempFile dbg_file("layout_debug.txt");
  std::FILE* fp = std::fopen(dbg_file.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  {
    absl::Cleanup close_fp = [&fp] {
      if (fp != nullptr) {
        std::fclose(fp);
      }
    };

    H5O_layout_t msg = ZeroStruct<H5O_layout_t>();
    msg.version = H5O_LAYOUT_VERSION_4;
    msg.type = H5D_CONTIGUOUS;
    msg.storage.u.contig.addr = 0x100;
    msg.storage.u.contig.size = 64;
    EXPECT_GE(H5O_debug_id(H5O_LAYOUT_ID, f_src, &msg, fp, 0, 40), 0);

    msg.type = H5D_COMPACT;
    msg.storage.u.compact.size = 12;
    EXPECT_GE(H5O_debug_id(H5O_LAYOUT_ID, f_src, &msg, fp, 0, 40), 0);

    msg.type = H5D_CHUNKED;
    msg.u.chunk.ndims = 2;
    msg.u.chunk.dim[0] = 4;
    msg.u.chunk.dim[1] = 8;
    msg.storage.u.chunk.idx_addr = 0x200;
    for (H5D_chunk_index_t idx :
         {H5D_CHUNK_IDX_BTREE, H5D_CHUNK_IDX_NONE, H5D_CHUNK_IDX_SINGLE,
          H5D_CHUNK_IDX_FARRAY, H5D_CHUNK_IDX_EARRAY, H5D_CHUNK_IDX_BT2,
          H5D_CHUNK_IDX_NTYPES}) {
      msg.u.chunk.idx_type = idx;
      EXPECT_GE(H5O_debug_id(H5O_LAYOUT_ID, f_src, &msg, fp, 2, 40), 0);
    }

    H5O_layout_t unknown_msg = ZeroStruct<H5O_layout_t>();
    unknown_msg.version = H5O_LAYOUT_VERSION_4;
    unknown_msg.type = H5D_NLAYOUTS;
    EXPECT_GE(H5O_debug_id(H5O_LAYOUT_ID, f_src, &unknown_msg, fp, 0, 40), 0);

    std::fclose(fp);
    fp = nullptr;
  }

  const std::string output = ReadWholeFile(dbg_file);
  EXPECT_THAT(output, HasSubstr("Contiguous"));
  EXPECT_THAT(output, HasSubstr("Compact"));
  EXPECT_THAT(output, HasSubstr("v1 B-tree"));
  EXPECT_THAT(output, HasSubstr("Implicit"));
  EXPECT_THAT(output, HasSubstr("Single Chunk"));
  EXPECT_THAT(output, HasSubstr("Fixed Array"));
  EXPECT_THAT(output, HasSubstr("Extensible Array"));
  EXPECT_THAT(output, HasSubstr("v2 B-tree"));
  EXPECT_THAT(output, HasSubstr("Unknown"));
}

}  // namespace
}  // namespace hdf5
