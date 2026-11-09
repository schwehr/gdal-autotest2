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
// Tests Object Header Fill Value messages (old & new encode/decode/size,
// copy with VL & reference types, dynamic reset, conversion, version bounds,
// pre-copy file checks, shared messages, debug formatting, and corrupt buffer
// rejection) in third_party/hdf5/src/H5Ofill.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ofill.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OFILL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5Fpkg.h's H5F__efc_open uses `try` as a parameter name, which is both a C++
// keyword and macro-defined under -fno-exceptions.
#pragma push_macro("try")
#undef try
#define try h5f_try_param
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Tprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OFILL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OFILL_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__IGNORE_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (tag != H5AC__IGNORE_TAG) {
        H5AC_tag(tag, &prev_tag_);
        tagged_ = true;
      }
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(false);
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

// Custom converter used to test background-buffer cleanup & conversion failure
// handling inside H5O__fill_copy.
herr_t FailingBkgConverter(hid_t /*src_id*/, hid_t /*dst_id*/,
                           H5T_cdata_t* cdata, size_t /*nelmts*/,
                           size_t /*buf_stride*/, size_t /*bkg_stride*/,
                           void* /*buf*/, void* /*bkg*/, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV:
      return -1;
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

TEST(H5OfillTest, EncodeDecodeNewAndOldMessagesAcrossVersions) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Test new fill value message (H5O_FILL_NEW_ID) for v1, v2, and v3:
  //    undefined (-1), default (0), and explicit user-defined (4-byte int).
  const int32_t kFillInt = 0x12345678;

  // v1 & v2: undefined (fill_defined = false).
  for (const unsigned vers : {H5O_FILL_VERSION_1, H5O_FILL_VERSION_2}) {
    H5O_fill_t src{};
    src.version = vers;
    src.alloc_time = H5D_ALLOC_TIME_EARLY;
    src.fill_time = H5D_FILL_TIME_ALLOC;
    src.fill_defined = false;
    src.size = -1;

    EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_NEW_ID, false, &src), 4u);
    std::array<uint8_t, 4> buf{};
    ASSERT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, buf.data(), &src), 0);

    auto* decoded = static_cast<H5O_fill_t*>(
        H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, buf.size(), buf.data()));
    ASSERT_NE(decoded, nullptr);
    const auto free_dec = absl::MakeCleanup([decoded] {
      H5O_msg_reset(H5O_FILL_NEW_ID, decoded);
      H5O_msg_free(H5O_FILL_NEW_ID, decoded);
    });
    EXPECT_EQ(decoded->version, vers);
    EXPECT_EQ(decoded->alloc_time, H5D_ALLOC_TIME_EARLY);
    EXPECT_EQ(decoded->fill_time, H5D_FILL_TIME_ALLOC);
    EXPECT_FALSE(decoded->fill_defined);
    EXPECT_EQ(decoded->size, -1);
    EXPECT_EQ(decoded->buf, nullptr);

    // v2 with fill_defined=true and explicit bytes (4 bytes).
    src.fill_defined = true;
    src.size = sizeof(kFillInt);
    src.buf = H5MM_malloc(sizeof(kFillInt));
    ASSERT_NE(src.buf, nullptr);
    std::memcpy(src.buf, &kFillInt, sizeof(kFillInt));
    const auto free_src = absl::MakeCleanup([&src] { H5O_fill_reset_dyn(&src); });

    EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_NEW_ID, false, &src), 12u);
    std::vector<uint8_t> val_buf(12, 0);
    ASSERT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, val_buf.data(), &src),
              0);

    auto* dec_val = static_cast<H5O_fill_t*>(H5O_msg_decode(
        f, nullptr, H5O_FILL_NEW_ID, val_buf.size(), val_buf.data()));
    ASSERT_NE(dec_val, nullptr);
    const auto free_dec_val = absl::MakeCleanup([dec_val] {
      H5O_msg_reset(H5O_FILL_NEW_ID, dec_val);
      H5O_msg_free(H5O_FILL_NEW_ID, dec_val);
    });
    EXPECT_TRUE(dec_val->fill_defined);
    ASSERT_EQ(dec_val->size, static_cast<ssize_t>(sizeof(kFillInt)));
    ASSERT_NE(dec_val->buf, nullptr);
    int32_t out_val = 0;
    std::memcpy(&out_val, dec_val->buf, sizeof(out_val));
    EXPECT_EQ(out_val, kFillInt);

    // Exercise v1/v2 encode when fill_defined=true, size > 0, and buf == NULL.
    H5O_fill_t empty_ptr_src{};
    empty_ptr_src.version = vers;
    empty_ptr_src.alloc_time = H5D_ALLOC_TIME_INCR;
    empty_ptr_src.fill_time = H5D_FILL_TIME_NEVER;
    empty_ptr_src.fill_defined = true;
    empty_ptr_src.size = 4;
    std::vector<uint8_t> dummy_enc(12, 0);
    EXPECT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, dummy_enc.data(),
                             &empty_ptr_src),
              0);
  }

  // v3: undefined (-1), default (0), and explicit value (> 0).
  {
    H5O_fill_t v3{};
    v3.version = H5O_FILL_VERSION_3;
    v3.alloc_time = H5D_ALLOC_TIME_INCR;
    v3.fill_time = H5D_FILL_TIME_IFSET;

    // Undefined.
    v3.size = -1;
    EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_NEW_ID, false, &v3), 2u);
    std::array<uint8_t, 2> buf_undef{};
    ASSERT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, buf_undef.data(), &v3),
              0);
    auto* dec_undef = static_cast<H5O_fill_t*>(H5O_msg_decode(
        f, nullptr, H5O_FILL_NEW_ID, buf_undef.size(), buf_undef.data()));
    ASSERT_NE(dec_undef, nullptr);
    EXPECT_EQ(dec_undef->size, -1);
    EXPECT_FALSE(dec_undef->fill_defined);
    H5O_msg_free(H5O_FILL_NEW_ID, dec_undef);

    // Default (size == 0).
    v3.size = 0;
    EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_NEW_ID, false, &v3), 2u);
    std::array<uint8_t, 2> buf_def{};
    ASSERT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, buf_def.data(), &v3),
              0);
    auto* dec_def = static_cast<H5O_fill_t*>(H5O_msg_decode(
        f, nullptr, H5O_FILL_NEW_ID, buf_def.size(), buf_def.data()));
    ASSERT_NE(dec_def, nullptr);
    EXPECT_EQ(dec_def->size, 0);
    EXPECT_TRUE(dec_def->fill_defined);
    H5O_msg_free(H5O_FILL_NEW_ID, dec_def);

    // Explicit value.
    v3.size = sizeof(kFillInt);
    v3.buf = H5MM_malloc(sizeof(kFillInt));
    ASSERT_NE(v3.buf, nullptr);
    std::memcpy(v3.buf, &kFillInt, sizeof(kFillInt));
    const auto free_v3 = absl::MakeCleanup([&v3] { H5O_fill_reset_dyn(&v3); });
    EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_NEW_ID, false, &v3), 10u);
    std::array<uint8_t, 10> buf_val{};
    ASSERT_GE(H5O_msg_encode(f, H5O_FILL_NEW_ID, false, buf_val.data(), &v3),
              0);
    auto* dec_val = static_cast<H5O_fill_t*>(H5O_msg_decode(
        f, nullptr, H5O_FILL_NEW_ID, buf_val.size(), buf_val.data()));
    ASSERT_NE(dec_val, nullptr);
    const auto free_dec_val = absl::MakeCleanup([dec_val] {
      H5O_msg_reset(H5O_FILL_NEW_ID, dec_val);
      H5O_msg_free(H5O_FILL_NEW_ID, dec_val);
    });
    EXPECT_TRUE(dec_val->fill_defined);
    EXPECT_EQ(dec_val->size, 4);
    int32_t out_v3 = 0;
    std::memcpy(&out_v3, dec_val->buf, sizeof(out_v3));
    EXPECT_EQ(out_v3, kFillInt);
  }

  // 2. Old fill value message (H5O_FILL_ID): encode, size, and decode using an
  //    actual dataset object header (has DTYPE) and group object header (no
  //    DTYPE).
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5O_loc_t* dset_oloc = H5O_get_loc(dset.get());
  H5O_loc_t* grp_oloc = H5O_get_loc(grp.get());
  ASSERT_NE(dset_oloc, nullptr);
  ASSERT_NE(grp_oloc, nullptr);

  H5O_t* dset_oh = H5O_protect(dset_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(dset_oh, nullptr);
  const auto unprot_dset = absl::MakeCleanup(
      [&] { H5O_unprotect(dset_oloc, dset_oh, H5AC__NO_FLAGS_SET); });

  H5O_t* grp_oh = H5O_protect(grp_oloc, H5AC__READ_ONLY_FLAG, false);
  ASSERT_NE(grp_oh, nullptr);
  const auto unprot_grp = absl::MakeCleanup(
      [&] { H5O_unprotect(grp_oloc, grp_oh, H5AC__NO_FLAGS_SET); });

  // size == 0 -> decoded size becomes -1.
  H5O_fill_t old_zero{};
  old_zero.size = 0;
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_ID, false, &old_zero), 4u);
  std::array<uint8_t, 4> old_zero_buf{};
  ASSERT_GE(
      H5O_msg_encode(f, H5O_FILL_ID, false, old_zero_buf.data(), &old_zero), 0);
  auto* dec_old_zero = static_cast<H5O_fill_t*>(H5O_msg_decode(
      f, dset_oh, H5O_FILL_ID, old_zero_buf.size(), old_zero_buf.data()));
  ASSERT_NE(dec_old_zero, nullptr);
  EXPECT_EQ(dec_old_zero->size, -1);
  H5O_msg_free(H5O_FILL_ID, dec_old_zero);

  // size == 4 matches dataset's H5T_NATIVE_INT32 DTYPE message.
  H5O_fill_t old_val{};
  old_val.size = sizeof(kFillInt);
  old_val.buf = H5MM_malloc(sizeof(kFillInt));
  ASSERT_NE(old_val.buf, nullptr);
  std::memcpy(old_val.buf, &kFillInt, sizeof(kFillInt));
  const auto free_old =
      absl::MakeCleanup([&old_val] { H5O_fill_reset_dyn(&old_val); });
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_FILL_ID, false, &old_val), 8u);
  std::array<uint8_t, 8> old_val_buf{};
  ASSERT_GE(H5O_msg_encode(f, H5O_FILL_ID, false, old_val_buf.data(), &old_val),
            0);

  auto* dec_old_val = static_cast<H5O_fill_t*>(H5O_msg_decode(
      f, dset_oh, H5O_FILL_ID, old_val_buf.size(), old_val_buf.data()));
  ASSERT_NE(dec_old_val, nullptr);
  EXPECT_EQ(dec_old_val->size, 4);
  EXPECT_TRUE(dec_old_val->fill_defined);
  H5O_msg_reset(H5O_FILL_ID, dec_old_val);
  H5O_msg_free(H5O_FILL_ID, dec_old_val);

  // Decode old fill message against a group ohdr (no DTYPE message present).
  auto* dec_old_nodt = static_cast<H5O_fill_t*>(H5O_msg_decode(
      f, grp_oh, H5O_FILL_ID, old_val_buf.size(), old_val_buf.data()));
  ASSERT_NE(dec_old_nodt, nullptr);
  EXPECT_EQ(dec_old_nodt->size, 4);
  H5O_msg_reset(H5O_FILL_ID, dec_old_nodt);
  H5O_msg_free(H5O_FILL_ID, dec_old_nodt);

  // Inconsistent fill size (8 bytes vs 4-byte int32 dtype) fails old decode.
  ScopedErrorSilence silence;
  const int64_t kFillInt64 = 99;
  H5O_fill_t old_bad_size{};
  old_bad_size.size = sizeof(kFillInt64);
  old_bad_size.buf = H5MM_malloc(sizeof(kFillInt64));
  ASSERT_NE(old_bad_size.buf, nullptr);
  std::memcpy(old_bad_size.buf, &kFillInt64, sizeof(kFillInt64));
  const auto free_old_bad =
      absl::MakeCleanup([&old_bad_size] { H5O_fill_reset_dyn(&old_bad_size); });
  std::array<uint8_t, 12> old_bad_buf{};
  ASSERT_GE(
      H5O_msg_encode(f, H5O_FILL_ID, false, old_bad_buf.data(), &old_bad_size),
      0);
  EXPECT_EQ(H5O_msg_decode(f, dset_oh, H5O_FILL_ID, old_bad_buf.size(),
                           old_bad_buf.data()),
            nullptr);
}

TEST(H5OfillTest, DecodeRejectsMalformedOldAndNewBuffers) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  constexpr std::array<uint8_t, 2> kDummy = {0, 0};

  // New fill value decode errors.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, 0, kDummy.data() + 1),
            nullptr);
  // Version < 1 and > latest.
  for (const uint8_t bad_v : {uint8_t{0}, uint8_t{H5O_FILL_VERSION_LATEST + 1}}) {
    const std::array<uint8_t, 2> buf = {bad_v, 0};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, buf.size(), buf.data()),
              nullptr);
  }
  // v2 truncated before alloc/fill/defined flags.
  constexpr auto kV2ShortHdr = std::to_array<uint8_t>({2, 0, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV2ShortHdr.size(),
                           kV2ShortHdr.data()),
            nullptr);
  // v2 fill_defined=1 truncated before size.
  constexpr auto kV2ShortSize = std::to_array<uint8_t>({2, 0, 0, 1, 4, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV2ShortSize.size(),
                           kV2ShortSize.data()),
            nullptr);
  // v2 fill_defined=1 size=8 truncated before value bytes.
  constexpr auto kV2ShortVal =
      std::to_array<uint8_t>({2, 0, 0, 1, 8, 0, 0, 0, 1, 2});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV2ShortVal.size(),
                           kV2ShortVal.data()),
            nullptr);
  // v3 truncated before flags.
  constexpr auto kV3NoFlags = std::to_array<uint8_t>({3});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV3NoFlags.size(),
                           kV3NoFlags.data()),
            nullptr);
  // v3 unknown flag bit.
  constexpr auto kV3UnknownFlag = std::to_array<uint8_t>({3, 0x40});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV3UnknownFlag.size(),
                           kV3UnknownFlag.data()),
            nullptr);
  // v3 both UNDEFINED_VALUE (0x10) and HAVE_VALUE (0x20) set.
  constexpr auto kV3BothFlags = std::to_array<uint8_t>({3, 0x30});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV3BothFlags.size(),
                           kV3BothFlags.data()),
            nullptr);
  // v3 HAVE_VALUE truncated before size.
  constexpr auto kV3ShortSize = std::to_array<uint8_t>({3, 0x20, 4, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV3ShortSize.size(),
                           kV3ShortSize.data()),
            nullptr);
  // v3 HAVE_VALUE truncated before value bytes.
  constexpr auto kV3ShortVal =
      std::to_array<uint8_t>({3, 0x20, 4, 0, 0, 0, 1, 2});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_NEW_ID, kV3ShortVal.size(),
                           kV3ShortVal.data()),
            nullptr);

  // Old fill value decode errors.
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_ID, 0, kDummy.data() + 1),
            nullptr);
  constexpr auto kOldShortSize = std::to_array<uint8_t>({4, 0, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_ID, kOldShortSize.size(),
                           kOldShortSize.data()),
            nullptr);
  constexpr auto kOldShortVal = std::to_array<uint8_t>({8, 0, 0, 0, 1, 2, 3, 4});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FILL_ID, kOldShortVal.size(),
                           kOldShortVal.data()),
            nullptr);
}

TEST(H5OfillTest, CopyResetVlenReferenceAndBkgErrorPaths) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Copy fill message with type == NULL and buf == NULL.
  {
    H5O_fill_t src{};
    src.version = H5O_FILL_VERSION_3;
    src.size = -1;
    auto* copy = static_cast<H5O_fill_t*>(
        H5O_msg_copy(H5O_FILL_NEW_ID, &src, nullptr));
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->buf, nullptr);
    EXPECT_EQ(copy->type, nullptr);
    H5O_msg_free(H5O_FILL_NEW_ID, copy);
  }

  // 2. Copy fill message with atomic type (noop conversion path).
  {
    H5O_fill_t src{};
    auto* int_dt = static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
    ASSERT_NE(int_dt, nullptr);
    src.type = H5T_copy(int_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(src.type, nullptr);
    src.size = sizeof(int32_t);
    src.buf = H5MM_malloc(sizeof(int32_t));
    ASSERT_NE(src.buf, nullptr);
    const int32_t val = 777;
    std::memcpy(src.buf, &val, sizeof(val));
    const auto free_src = absl::MakeCleanup([&src] { H5O_fill_reset_dyn(&src); });

    H5O_fill_t dst{};
    ASSERT_EQ(H5O_msg_copy(H5O_FILL_NEW_ID, &src, &dst), &dst);
    const auto free_dst = absl::MakeCleanup([&dst] { H5O_fill_reset_dyn(&dst); });
    ASSERT_NE(dst.type, nullptr);
    ASSERT_NE(dst.buf, nullptr);
    int32_t copied_val = 0;
    std::memcpy(&copied_val, dst.buf, sizeof(copied_val));
    EXPECT_EQ(copied_val, 777);
  }

  // 3. Copy & reset fill message with variable-length sequence datatype
  //    (exercises H5T_VLEN branch in H5O__fill_copy and H5O_fill_reset_dyn).
  {
    ScopedType vl_id(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_id.valid());
    auto* vl_dt = static_cast<H5T_t*>(H5I_object_verify(vl_id.get(), H5I_DATATYPE));
    ASSERT_NE(vl_dt, nullptr);

    H5O_fill_t src{};
    src.type = H5T_copy(vl_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(src.type, nullptr);
    src.size = static_cast<ssize_t>(sizeof(hvl_t));
    src.buf = H5MM_calloc(sizeof(hvl_t));
    ASSERT_NE(src.buf, nullptr);

    hvl_t vl{};
    vl.len = 2;
    vl.p = malloc(2 * sizeof(int32_t));
    ASSERT_NE(vl.p, nullptr);
    static_cast<int32_t*>(vl.p)[0] = 11;
    static_cast<int32_t*>(vl.p)[1] = 22;
    std::memcpy(src.buf, &vl, sizeof(vl));
    const auto free_src = absl::MakeCleanup([&src] { H5O_fill_reset_dyn(&src); });

    auto* copy = static_cast<H5O_fill_t*>(
        H5O_msg_copy(H5O_FILL_NEW_ID, &src, nullptr));
    ASSERT_NE(copy, nullptr);
    const auto free_copy = absl::MakeCleanup([copy] {
      H5O_msg_reset(H5O_FILL_NEW_ID, copy);
      H5O_msg_free(H5O_FILL_NEW_ID, copy);
    });
    hvl_t copied_vl{};
    std::memcpy(&copied_vl, copy->buf, sizeof(copied_vl));
    EXPECT_EQ(copied_vl.len, 2u);
    ASSERT_NE(copied_vl.p, nullptr);
    EXPECT_NE(copied_vl.p, vl.p);
    EXPECT_EQ(static_cast<int32_t*>(copied_vl.p)[0], 11);
    EXPECT_EQ(static_cast<int32_t*>(copied_vl.p)[1], 22);
  }

  // 4. Copy fill message with reference datatype (H5T_REFERENCE branch).
  {
    ScopedFile file = CreateInMemoryFile();
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/target", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5R_ref_t ref{};
    ASSERT_GE(H5Rcreate_object(file.get(), "/target", H5P_DEFAULT, &ref), 0);
    auto* ref_dt = static_cast<H5T_t*>(H5I_object_verify(H5T_STD_REF, H5I_DATATYPE));
    ASSERT_NE(ref_dt, nullptr);

    H5O_fill_t src{};
    src.type = H5T_copy(ref_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(src.type, nullptr);
    src.size = static_cast<ssize_t>(sizeof(H5R_ref_t));
    src.buf = H5MM_calloc(sizeof(H5R_ref_t));
    ASSERT_NE(src.buf, nullptr);
    std::memcpy(src.buf, &ref, sizeof(ref));
    const auto free_src = absl::MakeCleanup([&src] {
      H5Rdestroy(static_cast<H5R_ref_t*>(src.buf));
      H5MM_xfree(src.buf);
      src.buf = nullptr;
      H5T_close_real(src.type);
      src.type = nullptr;
    });

    auto* copy = static_cast<H5O_fill_t*>(
        H5O_msg_copy(H5O_FILL_NEW_ID, &src, nullptr));
    ASSERT_NE(copy, nullptr);
    const auto free_copy = absl::MakeCleanup([copy] {
      H5Rdestroy(static_cast<H5R_ref_t*>(copy->buf));
      H5MM_xfree(copy->buf);
      copy->buf = nullptr;
      H5O_msg_reset(H5O_FILL_NEW_ID, copy);
      H5O_msg_free(H5O_FILL_NEW_ID, copy);
    });
    EXPECT_EQ(copy->size, static_cast<ssize_t>(sizeof(H5R_ref_t)));
  }

  // 5. Custom failing soft converter on a VL type (which has force_conv=true
  //    so H5T_path_find does not short-circuit to noop) requesting
  //    H5T_BKG_TEMP triggers bkg allocation, conversion failure cleanup, and
  //    NULL return.
  {
    ScopedType vl_id(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_id.valid());
    ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "failing_fill_bkg", vl_id.get(),
                          vl_id.get(), FailingBkgConverter),
              0);
    const auto unregister = absl::MakeCleanup([&] {
      H5Tunregister(H5T_PERS_SOFT, "failing_fill_bkg", vl_id.get(),
                    vl_id.get(), FailingBkgConverter);
    });

    auto* vl_dt =
        static_cast<H5T_t*>(H5I_object_verify(vl_id.get(), H5I_DATATYPE));
    ASSERT_NE(vl_dt, nullptr);

    H5O_fill_t src{};
    src.type = H5T_copy(vl_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(src.type, nullptr);
    src.size = static_cast<ssize_t>(sizeof(hvl_t));
    src.buf = H5MM_calloc(sizeof(hvl_t));
    ASSERT_NE(src.buf, nullptr);
    const auto free_src = absl::MakeCleanup([&src] {
      H5MM_xfree(src.buf);
      src.buf = nullptr;
      H5T_close_real(src.type);
      src.type = nullptr;
    });

    ScopedErrorSilence silence;
    void* unexpected_copy = H5O_msg_copy(H5O_FILL_NEW_ID, &src, nullptr);
    EXPECT_EQ(unexpected_copy, nullptr);
    if (unexpected_copy != nullptr) {
      H5O_msg_reset(H5O_FILL_NEW_ID, unexpected_copy);
      H5O_msg_free(H5O_FILL_NEW_ID, unexpected_copy);
    }
  }
}

TEST(H5OfillTest, FillConvertAndSetVersionPaths) {
  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_early.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile f_early_obj(H5Fcreate("early.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                   fapl_early.get()));
  ASSERT_TRUE(f_early_obj.valid());

  ScopedFile f_latest_obj = CreateInMemoryFile();
  ASSERT_TRUE(f_latest_obj.valid());

  auto* f_early = static_cast<H5F_t*>(H5VL_object(f_early_obj.get()));
  auto* f_latest = static_cast<H5F_t*>(H5VL_object(f_latest_obj.get()));
  ASSERT_NE(f_early, nullptr);
  ASSERT_NE(f_latest, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. H5O_fill_set_version across EARLIEST and LATEST bounds.
  H5O_fill_t fill_ver{};
  fill_ver.version = H5O_FILL_VERSION_1;
  EXPECT_GE(H5O_fill_set_version(f_latest, &fill_ver), 0);
  EXPECT_EQ(fill_ver.version, static_cast<unsigned>(H5O_FILL_VERSION_LATEST));

  // Low bound = EARLIEST preserves v1; high_bound = EARLIEST rejects v3.
  {
    const H5F_libver_t orig_high = f_early->shared->high_bound;
    f_early->shared->high_bound = H5F_LIBVER_EARLIEST;
    const auto restore_high = absl::MakeCleanup(
        [&] { f_early->shared->high_bound = orig_high; });
    ScopedErrorSilence silence;
    EXPECT_LT(H5O_fill_set_version(f_early, &fill_ver), 0);
  }

  // 2. H5O_fill_convert no-op when buf == NULL or type == NULL or identical.
  auto* int32_dt =
      static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  auto* double_dt =
      static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_DOUBLE, H5I_DATATYPE));
  ASSERT_NE(int32_dt, nullptr);
  ASSERT_NE(double_dt, nullptr);

  {
    H5O_fill_t f_noop{};
    f_noop.type = H5T_copy(int32_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(f_noop.type, nullptr);
    bool changed = false;
    EXPECT_GE(H5O_fill_convert(&f_noop, int32_dt, &changed), 0);
    EXPECT_TRUE(changed);
    EXPECT_EQ(f_noop.type, nullptr);
  }

  // 3. H5O_fill_convert incompatible types (INT32 -> OPAQUE) fails before
  //    allocating conversion buffers.
  {
    ScopedType opq_id(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_id.valid());
    auto* opq_dt =
        static_cast<H5T_t*>(H5I_object_verify(opq_id.get(), H5I_DATATYPE));
    ASSERT_NE(opq_dt, nullptr);

    H5O_fill_t f_bad{};
    f_bad.type = H5T_copy(int32_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(f_bad.type, nullptr);
    f_bad.size = sizeof(int32_t);
    f_bad.buf = H5MM_malloc(sizeof(int32_t));
    ASSERT_NE(f_bad.buf, nullptr);
    const int32_t v = 9;
    std::memcpy(f_bad.buf, &v, sizeof(v));
    const auto free_bad = absl::MakeCleanup([&f_bad] { H5O_fill_reset_dyn(&f_bad); });

    ScopedErrorSilence silence;
    bool changed = false;
    EXPECT_LT(H5O_fill_convert(&f_bad, opq_dt, &changed), 0);
  }

  // 4. H5O_fill_convert compound conversion requiring a background buffer.
  {
    struct SrcComp {
      int32_t a;
      double b;
    };
    struct DstComp {
      double b;
      int32_t a;
    };
    ScopedType src_cid(H5Tcreate(H5T_COMPOUND, sizeof(SrcComp)));
    ScopedType dst_cid(H5Tcreate(H5T_COMPOUND, sizeof(DstComp)));
    ASSERT_TRUE(src_cid.valid());
    ASSERT_TRUE(dst_cid.valid());
    ASSERT_GE(H5Tinsert(src_cid.get(), "a", offsetof(SrcComp, a), H5T_NATIVE_INT32),
              0);
    ASSERT_GE(
        H5Tinsert(src_cid.get(), "b", offsetof(SrcComp, b), H5T_NATIVE_DOUBLE), 0);
    ASSERT_GE(
        H5Tinsert(dst_cid.get(), "b", offsetof(DstComp, b), H5T_NATIVE_DOUBLE), 0);
    ASSERT_GE(H5Tinsert(dst_cid.get(), "a", offsetof(DstComp, a), H5T_NATIVE_INT32),
              0);

    auto* src_dt =
        static_cast<H5T_t*>(H5I_object_verify(src_cid.get(), H5I_DATATYPE));
    auto* dst_dt =
        static_cast<H5T_t*>(H5I_object_verify(dst_cid.get(), H5I_DATATYPE));
    ASSERT_NE(src_dt, nullptr);
    ASSERT_NE(dst_dt, nullptr);

    H5O_fill_t f_conv{};
    f_conv.type = H5T_copy(src_dt, H5T_COPY_TRANSIENT);
    ASSERT_NE(f_conv.type, nullptr);
    f_conv.size = sizeof(SrcComp);
    f_conv.buf = H5MM_calloc(sizeof(SrcComp));
    ASSERT_NE(f_conv.buf, nullptr);
    const SrcComp src_val{.a = 42, .b = 3.5};
    std::memcpy(f_conv.buf, &src_val, sizeof(src_val));
    const auto free_conv =
        absl::MakeCleanup([&f_conv] { H5O_fill_reset_dyn(&f_conv); });

    bool changed = false;
    ASSERT_GE(H5O_fill_convert(&f_conv, dst_dt, &changed), 0);
    EXPECT_TRUE(changed);
    EXPECT_EQ(f_conv.type, nullptr);
    EXPECT_EQ(f_conv.size, static_cast<ssize_t>(sizeof(DstComp)));
    DstComp out_val{};
    std::memcpy(&out_val, f_conv.buf, sizeof(out_val));
    EXPECT_EQ(out_val.a, 42);
    EXPECT_DOUBLE_EQ(out_val.b, 3.5);
  }
}

TEST(H5OfillTest, DebugFormatsAllAllocFillAndStatusCombinations) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  struct DebugCase {
    H5D_alloc_time_t alloc_time;
    H5D_fill_time_t fill_time;
    ssize_t size;
    bool fill_defined;
    bool with_buf;
    bool with_type;
    const char* expect_alloc;
    const char* expect_fill;
    const char* expect_status;
  };
  constexpr auto kDebugCases = std::to_array<DebugCase>({
      {.alloc_time = H5D_ALLOC_TIME_EARLY,
       .fill_time = H5D_FILL_TIME_ALLOC,
       .size = -1,
       .fill_defined = false,
       .with_buf = false,
       .with_type = false,
       .expect_alloc = "Early",
       .expect_fill = "On Allocation",
       .expect_status = "Undefined"},
      {.alloc_time = H5D_ALLOC_TIME_LATE,
       .fill_time = H5D_FILL_TIME_NEVER,
       .size = 0,
       .fill_defined = true,
       .with_buf = false,
       .with_type = false,
       .expect_alloc = "Late",
       .expect_fill = "Never",
       .expect_status = "Default"},
      {.alloc_time = H5D_ALLOC_TIME_INCR,
       .fill_time = H5D_FILL_TIME_IFSET,
       .size = 4,
       .fill_defined = true,
       .with_buf = true,
       .with_type = true,
       .expect_alloc = "Incremental",
       .expect_fill = "If Set",
       .expect_status = "User Defined"},
      {.alloc_time = H5D_ALLOC_TIME_ERROR,
       .fill_time = H5D_FILL_TIME_ERROR,
       .size = -2,
       .fill_defined = true,
       .with_buf = false,
       .with_type = false,
       .expect_alloc = "Unknown!",
       .expect_fill = "Unknown!",
       .expect_status = "Unknown!"},
  });

  auto* int_dt =
      static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(int_dt, nullptr);

  for (const auto& tc : kDebugCases) {
    H5O_fill_t fill{};
    fill.version = H5O_FILL_VERSION_3;
    fill.alloc_time = tc.alloc_time;
    fill.fill_time = tc.fill_time;
    fill.size = tc.size;
    fill.fill_defined = tc.fill_defined;
    int32_t raw_val = 123;
    if (tc.with_buf) {
      fill.buf = &raw_val;
    }
    if (tc.with_type) {
      fill.type = int_dt;
    }

    std::array<char, 1024> out_buf{};
    FILE* stream = fmemopen(out_buf.data(), out_buf.size(), "w");
    ASSERT_NE(stream, nullptr);
    const auto close_stream = absl::MakeCleanup([stream] { fclose(stream); });

    ASSERT_GE(
        H5O_debug_id(H5O_FILL_NEW_ID, f, &fill, stream, /*indent=*/2, 30), 0);
    ASSERT_EQ(fflush(stream), 0);
    const std::string out(out_buf.data());
    EXPECT_THAT(out, HasSubstr(tc.expect_alloc));
    EXPECT_THAT(out, HasSubstr(tc.expect_fill));
    EXPECT_THAT(out, HasSubstr(tc.expect_status));
    if (tc.with_type) {
      EXPECT_THAT(out, HasSubstr("int[predefined]"));
    } else {
      EXPECT_THAT(out, HasSubstr("<dataset type>"));
    }
  }
}

TEST(H5OfillTest, SharedFillMessagesAndObjectCopyVersionBounds) {
  // 1. Create source file with shared object header message (SOHM) index for
  //    fill values so shared message decode/encode/size/link/delete/debug run.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_FILL_FLAG, 4), 0);

  ScopedPlist fapl_src = MakeCoreFapl();
  ASSERT_TRUE(fapl_src.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_src.get(), H5F_LIBVER_V18,
                                 H5F_LIBVER_LATEST),
            0);

  ScopedFile src_file(
      H5Fcreate("fill_sohm.h5", H5F_ACC_TRUNC, fcpl.get(), fapl_src.get()));
  ASSERT_TRUE(src_file.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  const std::array<int32_t, 4> fill_arr = {0x11223344, 0x55667788, 0x1234, 42};
  ScopedType arr_dt(
      H5Tarray_create2(H5T_NATIVE_INT32, 1, std::array<hsize_t, 1>{4}.data()));
  ASSERT_TRUE(arr_dt.valid());
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), arr_dt.get(), fill_arr.data()), 0);

  const std::array<hsize_t, 1> dims = {1};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  {
    ScopedDataset d1(H5Dcreate2(src_file.get(), "/d1", arr_dt.get(),
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ScopedDataset d2(H5Dcreate2(src_file.get(), "/d2", arr_dt.get(),
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    // Debug object header of /d2 (exercises shared fill debug & decode).
    auto* f = static_cast<H5F_t*>(H5VL_object(src_file.get()));
    H5O_loc_t* oloc = H5O_get_loc(d2.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(oloc, nullptr);
    std::array<char, 4096> dbg_buf{};
    FILE* stream = fmemopen(dbg_buf.data(), dbg_buf.size(), "w");
    ASSERT_NE(stream, nullptr);
    const auto close_stream = absl::MakeCleanup([stream] { fclose(stream); });
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_GE(H5O_debug(f, oloc->addr, stream, 0, 30), 0);
    EXPECT_EQ(fflush(stream), 0);
  }

  // Delete /d1 to exercise shared delete reference decrement, then flush &
  // reopen via file image and copy /d2 to both V18+ and EARLIEST destinations.
  ASSERT_GE(H5Ldelete(src_file.get(), "/d1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(src_file.get(), H5F_SCOPE_GLOBAL), 0);

  // Copy to another V18+ file succeeds (pre_copy_file succeeds).
  ScopedPlist fapl_v18 = MakeCoreFapl();
  ASSERT_TRUE(fapl_v18.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v18.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
      0);
  ScopedFile dst_v18(
      H5Fcreate("fill_dst_v18.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v18.get()));
  ASSERT_TRUE(dst_v18.valid());
  EXPECT_GE(H5Ocopy(src_file.get(), "/d2", dst_v18.get(), "/d2_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Exercise H5O__fill_pre_copy_file version bounds failure directly when the
  // destination file's high bound only allows H5O_FILL_VERSION_1.
  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_early.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile dst_early(H5Fcreate("fill_dst_early.h5", H5F_ACC_TRUNC,
                                 H5P_DEFAULT, fapl_early.get()));
  ASSERT_TRUE(dst_early.valid());

  auto* src_f = static_cast<H5F_t*>(H5VL_object(src_file.get()));
  auto* early_f = static_cast<H5F_t*>(H5VL_object(dst_early.get()));
  ASSERT_NE(src_f, nullptr);
  ASSERT_NE(early_f, nullptr);
  const H5F_libver_t orig_high = early_f->shared->high_bound;
  early_f->shared->high_bound = H5F_LIBVER_EARLIEST;
  const auto restore_high = absl::MakeCleanup(
      [&] { early_f->shared->high_bound = orig_high; });

  H5O_fill_t v3_fill{};
  v3_fill.version = H5O_FILL_VERSION_3;
  H5O_copy_t cpy_info{};
  cpy_info.file_dst = early_f;
  bool deleted = false;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;
  EXPECT_LT((H5O_MSG_FILL_NEW[0].pre_copy_file)(src_f, &v3_fill, &deleted,
                                                &cpy_info, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
