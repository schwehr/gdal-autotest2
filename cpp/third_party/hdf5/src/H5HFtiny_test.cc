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
// Tests tiny object initialization, ID length encoding boundaries (short and
// extended formats), insert/read/op/remove operations, and error paths for
// third_party/hdf5/src/H5HFtiny.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5HFtiny.c

#include <array>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t (H5AC_info_t) inside internal fractal heap structs.
#ifndef NDEBUG
#define H5HF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5HFpkg.h pulls in H5B2private.h, which uses `try` as a C parameter name,
// colliding with both the C++ keyword and Google3's `-fno-exceptions` macro.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_param_try
extern "C" {
#define H5HF_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5HFpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5HF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

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

H5F_t* GetFilePtr(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5HF_create_t MakeHeapCreateParams(uint16_t id_len) {
  H5HF_create_t cparam{};
  cparam.managed.width = 4;
  cparam.managed.start_block_size = 512;
  cparam.managed.max_direct_size = 4096;
  cparam.managed.max_index = 20;
  cparam.managed.start_root_rows = 1;
  cparam.checksum_dblocks = true;
  cparam.max_man_size = 512;
  cparam.id_len = id_len;
  return cparam;
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((seed + i * 29U) & 0xffU);
  }
  return data;
}

TEST(H5HFtinyTest, TinyInitBoundariesCoverShortThresholdAndExtended) {
  struct InitCase {
    unsigned id_len;
    size_t expected_max_len;
    bool expected_extended;
  };
  constexpr auto kCases = std::to_array<InitCase>({
      // (id_len - 1) <= 16: short length encoding in low 4 bits of first byte.
      {.id_len = 5, .expected_max_len = 4, .expected_extended = false},
      {.id_len = 16, .expected_max_len = 15, .expected_extended = false},
      {.id_len = 17, .expected_max_len = 16, .expected_extended = false},
      // (id_len - 1) == 17: boundary avoids wasted 2nd length byte.
      {.id_len = 18, .expected_max_len = 16, .expected_extended = false},
      // (id_len - 1) > 17: extended 12-bit length spans two header bytes.
      {.id_len = 19, .expected_max_len = 17, .expected_extended = true},
      {.id_len = 300, .expected_max_len = 298, .expected_extended = true},
      {.id_len = H5HF_MAX_ID_LEN,
       .expected_max_len = H5HF_MAX_ID_LEN - 2,
       .expected_extended = true},
  });

  for (const InitCase& tc : kCases) {
    H5HF_hdr_t hdr{};
    hdr.id_len = tc.id_len;
    EXPECT_GE(H5HF__tiny_init(&hdr), 0);
    EXPECT_EQ(hdr.tiny_max_len, tc.expected_max_len);
    EXPECT_EQ(hdr.tiny_len_extended, tc.expected_extended);
  }
}

TEST(H5HFtinyTest, ShortFormatInsertGetLenReadOpRemoveAndBoundaryId18) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = GetFilePtr(file.get());
  ASSERT_NE(f, nullptr);

  struct OpCapture {
    std::vector<uint8_t> bytes;
    bool fail = false;
  };
  auto op_cb = [](const void* obj, size_t obj_len, void* op_data) -> herr_t {
    auto* cap = static_cast<OpCapture*>(op_data);
    if (cap->fail) {
      return -1;
    }
    const auto* ptr = static_cast<const uint8_t*>(obj);
    cap->bytes.assign(ptr, ptr + obj_len);
    return 0;
  };

  // id_len = 17 => max_len = 16 (extended=false).
  // id_len = 18 => boundary case: still max_len = 16 (extended=false).
  for (uint16_t id_len : {uint16_t{17}, uint16_t{18}}) {
    const H5HF_create_t cparam = MakeHeapCreateParams(id_len);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() { H5HF_close(fh); });

    EXPECT_EQ(fh->hdr->tiny_max_len, 16U);
    EXPECT_FALSE(fh->hdr->tiny_len_extended);

    for (size_t obj_size : {size_t{1}, size_t{7}, size_t{16}}) {
      const std::vector<uint8_t> payload =
          MakePattern(obj_size, static_cast<uint8_t>(id_len + obj_size));
      // Pre-fill ID with 0xEE to verify zero-padding of trailing unused bytes.
      std::vector<uint8_t> id(id_len, 0xeeU);

      ASSERT_GE(H5HF_insert(fh, payload.size(), payload.data(), id.data()), 0);
      EXPECT_EQ(id[0], static_cast<uint8_t>(H5HF_ID_VERS_CURR |
                                            H5HF_ID_TYPE_TINY |
                                            ((obj_size - 1) & 0x0fU)));
      for (size_t tail = 1 + obj_size; tail < id_len; ++tail) {
        EXPECT_EQ(id[tail], 0U);
      }

      size_t got_len = 0;
      EXPECT_GE(H5HF_get_obj_len(fh, id.data(), &got_len), 0);
      EXPECT_EQ(got_len, obj_size);

      size_t direct_len = 0;
      EXPECT_GE(H5HF__tiny_get_obj_len(fh->hdr, id.data(), &direct_len), 0);
      EXPECT_EQ(direct_len, obj_size);

      std::vector<uint8_t> read_buf(obj_size, 0xa5U);
      EXPECT_GE(H5HF__tiny_read(fh->hdr, id.data(), read_buf.data()), 0);
      EXPECT_THAT(read_buf, ElementsAreArray(payload));

      std::fill(read_buf.begin(), read_buf.end(), 0x5aU);
      EXPECT_GE(H5HF_read(fh, id.data(), read_buf.data()), 0);
      EXPECT_THAT(read_buf, ElementsAreArray(payload));

      OpCapture cap;
      EXPECT_GE(H5HF__tiny_op(fh->hdr, id.data(), op_cb, &cap), 0);
      EXPECT_THAT(cap.bytes, ElementsAreArray(payload));

      {
        ScopedErrorSilence silence;
        cap.fail = true;
        EXPECT_LT(H5HF__tiny_op(fh->hdr, id.data(), op_cb, &cap), 0);
        EXPECT_LT(H5HF_op(fh, id.data(), op_cb, &cap), 0);
      }

      H5HF_stat_t stats{};
      EXPECT_GE(H5HF_stat_info(fh, &stats), 0);
      EXPECT_EQ(stats.tiny_nobjs, 1U);
      EXPECT_EQ(stats.tiny_size, obj_size);

      EXPECT_GE(H5HF_remove(fh, id.data()), 0);
    }

    // Exercise H5HF__tiny_insert and H5HF__tiny_remove directly.
    const std::vector<uint8_t> direct_payload = MakePattern(10, 0x41U);
    std::vector<uint8_t> direct_id(id_len, 0xffU);
    ASSERT_GE(H5HF__tiny_insert(fh->hdr, direct_payload.size(),
                                direct_payload.data(), direct_id.data()),
              0);
    EXPECT_EQ(fh->hdr->tiny_nobjs, 1U);
    EXPECT_EQ(fh->hdr->tiny_size, 10U);
    EXPECT_GE(H5HF__tiny_remove(fh->hdr, direct_id.data()), 0);
    EXPECT_EQ(fh->hdr->tiny_nobjs, 0U);
    EXPECT_EQ(fh->hdr->tiny_size, 0U);
  }
}

TEST(H5HFtinyTest, ExtendedFormatMultiByteLengthAndFileImageRoundTrip) {
  constexpr uint16_t kIdLen = 300;
  haddr_t heap_addr = HADDR_UNDEF;
  std::vector<uint8_t> image;

  const std::vector<uint8_t> payload_min_ext = MakePattern(17, 0x11U);
  const std::vector<uint8_t> payload_100 = MakePattern(100, 0x22U);
  // 256 -> enc_obj_size = 255 (0x00FF), max extended size unaffected by
  // upstream H5HFtiny.c:199 decoding mask H5HF_TINY_MASK_EXT_1 (0x0F00 vs 0x0F).
  const std::vector<uint8_t> payload_256 = MakePattern(256, 0x33U);
  const std::vector<uint8_t> payload_257 = MakePattern(257, 0x44U);

  std::array<uint8_t, kIdLen> id_min_ext{};
  std::array<uint8_t, kIdLen> id_100{};
  std::array<uint8_t, kIdLen> id_256{};
  std::array<uint8_t, kIdLen> id_257{};

  // 1. Create an extended-tiny heap, verify both 2-byte length encoding
  //    (including upper nibble bits on 257 bytes) and round-trip persistence.
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = GetFilePtr(file.get());
    ASSERT_NE(f, nullptr);

    const H5HF_create_t cparam = MakeHeapCreateParams(kIdLen);
    H5HF_t* fh = H5HF_create(f, &cparam);
    ASSERT_NE(fh, nullptr);
    auto close_fh = absl::MakeCleanup([&]() {
      if (fh != nullptr) {
        H5HF_close(fh);
      }
    });
    ASSERT_GE(H5HF_get_heap_addr(fh, &heap_addr), 0);

    EXPECT_EQ(fh->hdr->tiny_max_len, 298U);
    EXPECT_TRUE(fh->hdr->tiny_len_extended);

    id_min_ext.fill(0xbbU);
    ASSERT_GE(H5HF_insert(fh, payload_min_ext.size(), payload_min_ext.data(),
                          id_min_ext.data()),
              0);
    ASSERT_GE(
        H5HF_insert(fh, payload_100.size(), payload_100.data(), id_100.data()),
        0);
    ASSERT_GE(
        H5HF_insert(fh, payload_256.size(), payload_256.data(), id_256.data()),
        0);
    ASSERT_GE(
        H5HF_insert(fh, payload_257.size(), payload_257.data(), id_257.data()),
        0);

    // Verify 12-bit big-endian split on encode (enc_obj_size = 256 = 0x0100).
    EXPECT_EQ(id_257[0], static_cast<uint8_t>(H5HF_ID_VERS_CURR |
                                              H5HF_ID_TYPE_TINY | 0x01));
    EXPECT_EQ(id_257[1], 0x00U);

    // Check tail zero-padding for shorter extended object.
    for (size_t tail = 2 + payload_min_ext.size(); tail < kIdLen; ++tail) {
      EXPECT_EQ(id_min_ext[tail], 0U);
    }

    EXPECT_EQ(fh->hdr->tiny_nobjs, 4U);
    EXPECT_EQ(fh->hdr->tiny_size, payload_min_ext.size() + payload_100.size() +
                                      payload_256.size() + payload_257.size());

    EXPECT_GE(H5HF_close(fh), 0);
    fh = nullptr;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
    ASSERT_GT(image_size, 0);
    image.resize(static_cast<size_t>(image_size));
    ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
              image_size);
  }

  // 2. Reopen image, read & operate on extended tiny objects (<= 256 bytes),
  //    then remove them.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("reopen_tiny.h5", H5F_ACC_RDWR, fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedApiContext ctx2;
  ASSERT_TRUE(ctx2.valid());
  H5F_t* f2 = GetFilePtr(reopened.get());
  ASSERT_NE(f2, nullptr);

  H5HF_t* fh2 = H5HF_open(f2, heap_addr);
  ASSERT_NE(fh2, nullptr);
  auto close_fh2 = absl::MakeCleanup([&]() { H5HF_close(fh2); });

  EXPECT_EQ(fh2->hdr->tiny_max_len, 298U);
  EXPECT_TRUE(fh2->hdr->tiny_len_extended);

  const std::array<const std::vector<uint8_t>*, 3> payloads = {
      &payload_min_ext, &payload_100, &payload_256};
  const std::array<const uint8_t*, 3> ids = {id_min_ext.data(), id_100.data(),
                                             id_256.data()};

  std::vector<uint8_t> op_observed;
  auto capture_op = [](const void* obj, size_t len, void* op_data) -> herr_t {
    const auto* p = static_cast<const uint8_t*>(obj);
    static_cast<std::vector<uint8_t>*>(op_data)->assign(p, p + len);
    return 0;
  };

  for (size_t i = 0; i < payloads.size(); ++i) {
    size_t got_len = 0;
    EXPECT_GE(H5HF_get_obj_len(fh2, ids[i], &got_len), 0);
    EXPECT_EQ(got_len, payloads[i]->size());

    std::vector<uint8_t> out(payloads[i]->size(), 0x5cU);
    EXPECT_GE(H5HF_read(fh2, ids[i], out.data()), 0);
    EXPECT_THAT(out, ElementsAreArray(*payloads[i]));

    op_observed.clear();
    EXPECT_GE(H5HF_op(fh2, ids[i], capture_op, &op_observed), 0);
    EXPECT_THAT(op_observed, ElementsAreArray(*payloads[i]));

    EXPECT_GE(H5HF_remove(fh2, ids[i]), 0);
  }

  H5HF_stat_t stats{};
  EXPECT_GE(H5HF_stat_info(fh2, &stats), 0);
  EXPECT_EQ(stats.tiny_nobjs, 1U);
  EXPECT_EQ(stats.tiny_size, payload_257.size());
}

}  // namespace
}  // namespace hdf5
