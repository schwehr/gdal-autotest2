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
// Tests bitfield datatype conversions across endianness, overlap, stride,
// padding modes, precision truncation callbacks, and error paths
// (third_party/hdf5/src/H5Tconv_bitfield.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_bitfield.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5T_CONV_BITFIELD_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#ifdef H5_HAVE__FLOAT16
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv_bitfield.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5T_CONV_BITFIELD_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5T_CONV_BITFIELD_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

const H5T_t* LookupType(hid_t id) {
  return static_cast<const H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_t* LookupMutableType(hid_t id) {
  return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

struct BfCbState {
  int calls = 0;
  H5T_conv_ret_t ret = H5T_CONV_UNHANDLED;
  uint16_t override_be = 0xCAFE;
};

H5T_conv_ret_t TestBitfieldExceptCb(H5T_conv_except_t, hid_t, hid_t, void*,
                                    void* dst_buf, void* user_data) {
  auto* state = static_cast<BfCbState*>(user_data);
  ++state->calls;
  if (state->ret == H5T_CONV_HANDLED) {
    auto* bytes = static_cast<uint8_t*>(dst_buf);
    bytes[0] = static_cast<uint8_t>((state->override_be >> 8) & 0xff);
    bytes[1] = static_cast<uint8_t>(state->override_be & 0xff);
  }
  return state->ret;
}

TEST(H5TconvBitfieldTest, ConvertSameSizeBetweenLeAndBe) {
  // Custom precision difference ensures H5T__conv_b_b is used rather than
  // pure byte-order swap.
  ScopedType src(H5Tcopy(H5T_STD_B32LE));
  ScopedType dst(H5Tcopy(H5T_STD_B32BE));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());
  ASSERT_GE(H5Tset_precision(src.get(), 24), 0);
  ASSERT_GE(H5Tset_precision(dst.get(), 28), 0);

  std::array<uint8_t, 8> buf = {0x34, 0x12, 0xab, 0xff, 0x0f, 0xfe, 0xdc, 0xff};
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), 2, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  // Zero-extended to 28 bits in BE order.
  EXPECT_THAT(buf,
              ElementsAre(0x00, 0xab, 0x12, 0x34, 0x00, 0xdc, 0xfe, 0x0f));
}

TEST(H5TconvBitfieldTest, ConvertNarrowingAndWideningAcrossOverlapBoundary) {
  // 1. Narrowing B32LE -> B8LE with nelmts = 6 (olap = 1 < nelmts).
  std::vector<uint8_t> narrow = {
      0x11, 0xff, 0xff, 0xff, 0x22, 0xff, 0xff, 0xff,
      0x33, 0xff, 0xff, 0xff, 0x44, 0xff, 0xff, 0xff,
      0x55, 0xff, 0xff, 0xff, 0x66, 0xff, 0xff, 0xff};
  ASSERT_GE(H5Tconvert(H5T_STD_B32LE, H5T_STD_B8LE, 6, narrow.data(), nullptr,
                       H5P_DEFAULT),
            0);
  std::array<uint8_t, 6> narrow_head{};
  std::memcpy(narrow_head.data(), narrow.data(), narrow_head.size());
  EXPECT_THAT(narrow_head, ElementsAre(0x11, 0x22, 0x33, 0x44, 0x55, 0x66));

  // 2. Widening B8BE -> B32BE with nelmts = 6 (direction = -1, olap = 1).
  std::vector<uint8_t> wide(6 * 4, 0xff);
  const std::array<uint8_t, 6> src_bytes = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
  std::memcpy(wide.data(), src_bytes.data(), src_bytes.size());
  ASSERT_GE(H5Tconvert(H5T_STD_B8BE, H5T_STD_B32BE, 6, wide.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(wide[i * 4 + 0], 0x00);
    EXPECT_EQ(wide[i * 4 + 1], 0x00);
    EXPECT_EQ(wide[i * 4 + 2], 0x00);
    EXPECT_EQ(wide[i * 4 + 3], src_bytes[i]);
  }
}

TEST(H5TconvBitfieldTest, ConvertBitfieldWithNonZeroStride) {
  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;

  const H5T_t* src = LookupType(H5T_STD_B16BE);
  const H5T_t* dst = LookupType(H5T_STD_B16LE);

  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(
      H5T__conv_b_b(src, dst, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  // Two 16-bit elements separated by stride 6.
  std::array<uint8_t, 12> strided = {0x12, 0x34, 0x99, 0x99, 0x99, 0x99,
                                     0xab, 0xcd, 0x88, 0x88, 0x88, 0x88};
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(
      H5T__conv_b_b(src, dst, &cdata, &ctx, 2, 6, 0, strided.data(), nullptr),
      0);
  EXPECT_THAT(strided, ElementsAre(0x34, 0x12, 0x99, 0x99, 0x99, 0x99,
                                   0xcd, 0xab, 0x88, 0x88, 0x88, 0x88));

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_b_b(src, dst, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
}

TEST(H5TconvBitfieldTest, ConvertBitfieldPrecisionTruncationCallbacks) {
  ScopedType src(H5Tcopy(H5T_STD_B32BE));
  ScopedType dst(H5Tcopy(H5T_STD_B16BE));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());
  ASSERT_GE(H5Tset_precision(src.get(), 24), 0);
  ASSERT_GE(H5Tset_precision(dst.get(), 12), 0);
  ASSERT_GE(H5Tset_pad(dst.get(), H5T_PAD_ZERO, H5T_PAD_ONE), 0);

  // 1. Without callback: keeps low 12 bits and sets top 4 MSB pad bits to 1.
  std::array<uint8_t, 4> no_cb = {0x00, 0x12, 0x34, 0x56};
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), 1, no_cb.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(no_cb[0], 0xf4);
  EXPECT_EQ(no_cb[1], 0x56);

  // 2. With callback: UNHANDLED, HANDLED (leaves BE reversal off), and ABORT.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  BfCbState state;
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestBitfieldExceptCb, &state), 0);

  std::array<uint8_t, 4> cb_unhandled = {0x00, 0x12, 0x34, 0x56};
  state.ret = H5T_CONV_UNHANDLED;
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), 1, cb_unhandled.data(), nullptr,
                       dxpl.get()),
            0);
  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(cb_unhandled[0], 0xf4);
  EXPECT_EQ(cb_unhandled[1], 0x56);

  std::array<uint8_t, 4> cb_handled = {0x00, 0x12, 0x34, 0x56};
  state.ret = H5T_CONV_HANDLED;
  state.override_be = 0x05a5;
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), 1, cb_handled.data(), nullptr,
                       dxpl.get()),
            0);
  // Handled keeps reverse=false, then applies LSB/MSB padding (msb_pad_offset=12
  // fills bits [12..15] in LE order -> upper nibble of byte 1 becomes 0xf).
  EXPECT_EQ(cb_handled[0], 0x05);
  EXPECT_EQ(cb_handled[1], 0xf5);

  ScopedErrorSilence silence;
  std::array<uint8_t, 4> cb_abort = {0x00, 0x12, 0x34, 0x56};
  state.ret = H5T_CONV_ABORT;
  EXPECT_LT(H5Tconvert(src.get(), dst.get(), 1, cb_abort.data(), nullptr,
                       dxpl.get()),
            0);
}

TEST(H5TconvBitfieldTest, ConvertBitfieldAppliesLsbAndMsbPaddingModes) {
  ScopedType src(H5Tcopy(H5T_STD_B8LE));
  ScopedType dst_one_zero(H5Tcopy(H5T_STD_B16LE));
  ScopedType dst_zero_one(H5Tcopy(H5T_STD_B16LE));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst_one_zero.valid());
  ASSERT_TRUE(dst_zero_one.valid());

  // Set precision before offset so expanding offset does not grow size to 3.
  ASSERT_GE(H5Tset_precision(dst_one_zero.get(), 8), 0);
  ASSERT_GE(H5Tset_offset(dst_one_zero.get(), 4), 0);
  ASSERT_GE(H5Tset_pad(dst_one_zero.get(), H5T_PAD_ONE, H5T_PAD_ZERO), 0);
  ASSERT_EQ(H5Tget_size(dst_one_zero.get()), 2);

  ASSERT_GE(H5Tset_precision(dst_zero_one.get(), 8), 0);
  ASSERT_GE(H5Tset_offset(dst_zero_one.get(), 4), 0);
  ASSERT_GE(H5Tset_pad(dst_zero_one.get(), H5T_PAD_ZERO, H5T_PAD_ONE), 0);
  ASSERT_EQ(H5Tget_size(dst_zero_one.get()), 2);

  std::array<uint8_t, 2> b_one_zero = {0x2a, 0x00};
  ASSERT_GE(H5Tconvert(src.get(), dst_one_zero.get(), 1, b_one_zero.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(b_one_zero, ElementsAre(0xaf, 0x02));

  std::array<uint8_t, 2> b_zero_one = {0x2a, 0x00};
  ASSERT_GE(H5Tconvert(src.get(), dst_zero_one.get(), 1, b_zero_one.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(b_zero_one, ElementsAre(0xa0, 0xf2));

  // Direct test for unsupported LSB and MSB padding error branches.
  ScopedErrorSilence silence;
  ScopedType bad_pad(H5Tcopy(H5T_STD_B16LE));
  ASSERT_TRUE(bad_pad.valid());
  H5T_t* bad_dt = LookupMutableType(bad_pad.get());
  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  cdata.command = H5T_CONV_CONV;
  uint8_t buf[2] = {0x12, 0x34};

  bad_dt->shared->u.atomic.lsb_pad = H5T_PAD_BACKGROUND;
  EXPECT_LT(
      H5T__conv_b_b(LookupType(src.get()), bad_dt, &cdata, &ctx, 1, 0, 0, buf,
                    nullptr),
      0);

  bad_dt->shared->u.atomic.lsb_pad = H5T_PAD_ZERO;
  bad_dt->shared->u.atomic.msb_pad = H5T_PAD_BACKGROUND;
  EXPECT_LT(
      H5T__conv_b_b(LookupType(src.get()), bad_dt, &cdata, &ctx, 1, 0, 0, buf,
                    nullptr),
      0);
}

TEST(H5TconvBitfieldTest, ConvBitfieldErrorPathsRejectInvalidInputs) {
  ScopedErrorSilence silence;
  const H5T_t* b16le = LookupType(H5T_STD_B16LE);

  ScopedType bad_order(H5Tcopy(H5T_STD_B16LE));
  ASSERT_TRUE(bad_order.valid());
  LookupMutableType(bad_order.get())->shared->u.atomic.order = H5T_ORDER_VAX;
  const H5T_t* b16vax = LookupType(bad_order.get());

  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;

  // INIT failures.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_b_b(nullptr, b16le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_b_b(b16le, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_b_b(b16vax, b16le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(
      H5T__conv_b_b(b16le, b16vax, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  // CONV failures.
  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(
      H5T__conv_b_b(nullptr, b16le, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_b_b(b16le, nullptr, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_b_b(b16le, b16le, &cdata, nullptr, 1, 0, 0, nullptr, nullptr),
      0);

  // Unknown cmd.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_b_b(b16le, b16le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
