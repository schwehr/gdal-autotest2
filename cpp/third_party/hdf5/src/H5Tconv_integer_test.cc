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
// Unit tests for HDF5 soft and hard integer datatype conversion paths:
//   - Soft int-to-int conversions (H5T__conv_i_i) across sizes, signs,
//     offsets, paddings, byte orders, overlaps, strides, and callbacks.
//   - Soft int-to-float and int-to-complex conversions (H5T__conv_i_f,
//     H5T__conv_i_complex, H5T__conv_i_f_loop) across rounding, normalization,
//     VAX/BE/LE byte orders, overflow/precision callbacks, and complex zeroing.
//   - All 160 compiled-in hard (hardware-cast) integer conversions covering
//     LP64 shadowed long/llong paths, _Float16, float, double, long double, and
//     complex targets, including alignment, striding, and exception handling.
//
// Google3 build notes:
//   - Compiled on LP64 Linux with -DNDEBUG, H5_HAVE__FLOAT16=1,
//     H5_HAVE_COMPLEX_NUMBERS=1, and H5T_CONV_INTERNAL_{LLONG,ULLONG}_LDOUBLE.
//   - Note on `sizeof(long double)`: 16 bytes with 6 padding bytes on x86_64;
//     all long double / complex buffers are zeroed via std::memset first.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_integer.c

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match its struct layout for H5T_t and
// H5T_cdata_t/H5T_path_t across the internal prelude and H5Tpkg.h.
#ifndef NDEBUG
#define H5TCONV_INT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
// H5private.h suppresses H5__Float16 under __cplusplus, but H5Tpkg.h declares
// H5T_NATIVE_FLOAT16_{POS,NEG}_INF_g using it when H5_HAVE__FLOAT16 is set.
#ifdef H5_HAVE__FLOAT16
typedef uint16_t H5__Float16;
#endif
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tconv_integer.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TCONV_INT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_INT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

struct ExceptionLog {
  H5T_conv_ret_t action = H5T_CONV_UNHANDLED;
  int calls = 0;
  H5T_conv_except_t last_event = H5T_CONV_EXCEPT_RANGE_HI;
  uint64_t fill_value = 0;
  size_t fill_size = 0;
};

H5T_conv_ret_t TestExceptionCallback(H5T_conv_except_t event_type,
                                     hid_t /*src_id*/, hid_t /*dst_id*/,
                                     void* /*src_buf*/, void* dst_buf,
                                     void* user_data) {
  auto* log = static_cast<ExceptionLog*>(user_data);
  if (log == nullptr) {
    return H5T_CONV_ABORT;
  }
  log->calls++;
  log->last_event = event_type;
  if (log->action == H5T_CONV_HANDLED && dst_buf != nullptr &&
      log->fill_size > 0 && log->fill_size <= sizeof(log->fill_value)) {
    std::memcpy(dst_buf, &log->fill_value, log->fill_size);
  }
  return log->action;
}

ScopedPlist MakeExceptionDxpl(ExceptionLog* log) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  if (dxpl.valid()) {
    if (H5Pset_type_conv_cb(dxpl.get(), TestExceptionCallback, log) < 0) {
      dxpl.reset();
    }
  }
  return dxpl;
}

ScopedType MakeCustomInt(size_t size, size_t precision, size_t offset,
                         H5T_sign_t sign, H5T_order_t order,
                         H5T_pad_t lsb_pad = H5T_PAD_ZERO,
                         H5T_pad_t msb_pad = H5T_PAD_ZERO) {
  ScopedType tid(
      H5Tcopy(sign == H5T_SGN_NONE ? H5T_STD_U32LE : H5T_STD_I32LE));
  if (!tid.valid()) {
    return ScopedType();
  }
  if (H5Tset_size(tid.get(), size) < 0 ||
      H5Tset_order(tid.get(), order) < 0 ||
      H5Tset_precision(tid.get(), precision) < 0 ||
      H5Tset_offset(tid.get(), offset) < 0 ||
      H5Tset_pad(tid.get(), lsb_pad, msb_pad) < 0) {
    tid.reset();
  }
  return tid;
}

ScopedType MakeCustomFloatF32(H5T_order_t order, H5T_norm_t norm,
                              size_t size = 4, size_t offset = 0,
                              H5T_pad_t lsb_pad = H5T_PAD_ZERO,
                              H5T_pad_t msb_pad = H5T_PAD_ZERO) {
  ScopedType tid(H5Tcopy(H5T_IEEE_F32LE));
  if (!tid.valid()) {
    return ScopedType();
  }
  if (H5Tset_size(tid.get(), size) < 0 ||
      H5Tset_order(tid.get(), order) < 0 ||
      H5Tset_norm(tid.get(), norm) < 0 ||
      H5Tset_pad(tid.get(), lsb_pad, msb_pad) < 0) {
    tid.reset();
    return ScopedType();
  }
  if (offset > 0) {
    // IEEE 32-bit fields shifted by `offset`: set offset before fields so
    // H5Tset_fields validates (epos + esize - offset <= prec).
    if (H5Tset_offset(tid.get(), offset) < 0 ||
        H5Tset_fields(tid.get(), 31 + offset, 23 + offset, 8, offset, 23) < 0 ||
        H5Tset_precision(tid.get(), 32) < 0) {
      tid.reset();
    }
  }
  return tid;
}

H5T_t* LookupTypeObj(hid_t id) {
  return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_conv_ctx_t MakeConvCtx(hid_t src_id, hid_t dst_id, ExceptionLog* log) {
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  ctx.u.conv.src_type_id = src_id;
  ctx.u.conv.dst_type_id = dst_id;
  if (log != nullptr) {
    ctx.u.conv.cb_struct.func = TestExceptionCallback;
    ctx.u.conv.cb_struct.user_data = log;
  }
  return ctx;
}

uint32_t ReadUint32LE(const void* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

float ReadFloatLE(const void* p) {
  float v = 0.0f;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

float ReadFloatBE(const uint8_t* p) {
  const std::array<uint8_t, 4> le = {p[3], p[2], p[1], p[0]};
  return ReadFloatLE(le.data());
}

// ============================================================================
// Soft integer-to-integer conversion (H5T__conv_i_i) tests
// ============================================================================

TEST(H5TconvIntegerSoftIntTest, UnsignedSignedSignCombinationsAndZero) {
  // Use custom 12-bit precision inside 2-byte types to guarantee soft path i_i.
  // U -> U widening (12-bit -> 16-bit) & zero
  {
    ScopedType src = MakeCustomInt(2, 12, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ScopedType dst = MakeCustomInt(4, 24, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ASSERT_TRUE(src.valid() && dst.valid());
    EXPECT_EQ(H5Tcompiler_conv(src.get(), dst.get()), 0);

    std::vector<uint8_t> buf(4 * 3, 0);
    const std::array<uint16_t, 3> inputs = {0u, 0x0abcu, 0x0fffu};
    std::memcpy(buf.data(), inputs.data(), sizeof(inputs));
    ASSERT_GE(H5Tconvert(src.get(), dst.get(), 3, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    EXPECT_EQ(ReadUint32LE(buf.data() + 0), 0u);
    EXPECT_EQ(ReadUint32LE(buf.data() + 4), 0x0abcu);
    EXPECT_EQ(ReadUint32LE(buf.data() + 8), 0x0fffu);
  }

  // U -> U narrowing fit and overflow clamping
  {
    ScopedType src = MakeCustomInt(4, 24, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ScopedType dst = MakeCustomInt(2, 10, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ASSERT_TRUE(src.valid() && dst.valid());

    std::vector<uint8_t> buf(4 * 3, 0);
    const std::array<uint32_t, 3> inputs = {0u, 500u, 5000u};
    std::memcpy(buf.data(), inputs.data(), sizeof(inputs));
    ASSERT_GE(H5Tconvert(src.get(), dst.get(), 3, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    uint16_t v0 = 0, v1 = 0, v2 = 0;
    std::memcpy(&v0, buf.data() + 0, 2);
    std::memcpy(&v1, buf.data() + 2, 2);
    std::memcpy(&v2, buf.data() + 4, 2);
    EXPECT_EQ(v0, 0u);
    EXPECT_EQ(v1, 500u);
    EXPECT_EQ(v2, 0x03ffu);
  }

  // S -> U widening (positive fits) and negative clamping
  {
    ScopedType src = MakeCustomInt(2, 12, 0, H5T_SGN_2, H5T_ORDER_LE);
    ScopedType dst = MakeCustomInt(4, 20, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ASSERT_TRUE(src.valid() && dst.valid());

    std::vector<uint8_t> buf(4 * 2, 0);
    // 12-bit signed: -5 (0x0ffb), +123 (0x007b)
    const std::array<uint16_t, 2> inputs = {0x0ffbu, 123u};
    std::memcpy(buf.data(), inputs.data(), sizeof(inputs));
    ASSERT_GE(H5Tconvert(src.get(), dst.get(), 2, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    EXPECT_EQ(ReadUint32LE(buf.data() + 0), 0u);
    EXPECT_EQ(ReadUint32LE(buf.data() + 4), 123u);
  }

  // S -> U narrowing: negative, positive overflow, positive fit
  {
    ScopedType src = MakeCustomInt(4, 20, 0, H5T_SGN_2, H5T_ORDER_LE);
    ScopedType dst = MakeCustomInt(2, 10, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ASSERT_TRUE(src.valid() && dst.valid());

    std::vector<uint8_t> buf(4 * 3, 0);
    const std::array<int32_t, 3> inputs = {-17, 5000, 700};
    std::memcpy(buf.data(), inputs.data(), sizeof(inputs));
    ASSERT_GE(H5Tconvert(src.get(), dst.get(), 3, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    uint16_t v0 = 0, v1 = 0, v2 = 0;
    std::memcpy(&v0, buf.data() + 0, 2);
    std::memcpy(&v1, buf.data() + 2, 2);
    std::memcpy(&v2, buf.data() + 4, 2);
    EXPECT_EQ(v0, 0u);
    EXPECT_EQ(v1, 0x03ffu);
    EXPECT_EQ(v2, 700u);
  }

  // U -> S widening fit & narrowing fit + overflow
  {
    ScopedType src = MakeCustomInt(2, 10, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ScopedType dst = MakeCustomInt(4, 20, 0, H5T_SGN_2, H5T_ORDER_LE);
    ASSERT_TRUE(src.valid() && dst.valid());
    std::vector<uint8_t> buf(4 * 2, 0);
    const std::array<uint16_t, 2> inputs = {10u, 900u};
    std::memcpy(buf.data(), inputs.data(), sizeof(inputs));
    ASSERT_GE(H5Tconvert(src.get(), dst.get(), 2, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    EXPECT_EQ(ReadUint32LE(buf.data() + 0), 10u);
    EXPECT_EQ(ReadUint32LE(buf.data() + 4), 900u);

    ScopedType src_wide = MakeCustomInt(4, 20, 0, H5T_SGN_NONE, H5T_ORDER_LE);
    ScopedType dst_narrow = MakeCustomInt(2, 10, 0, H5T_SGN_2, H5T_ORDER_LE);
    ASSERT_TRUE(src_wide.valid() && dst_narrow.valid());
    std::vector<uint8_t> buf2(4 * 2, 0);
    const std::array<uint32_t, 2> in2 = {500u, 2000u};
    std::memcpy(buf2.data(), in2.data(), sizeof(in2));
    ASSERT_GE(H5Tconvert(src_wide.get(), dst_narrow.get(), 2, buf2.data(),
                         nullptr, H5P_DEFAULT),
              0);
    uint16_t r0 = 0, r1 = 0;
    std::memcpy(&r0, buf2.data() + 0, 2);
    std::memcpy(&r1, buf2.data() + 2, 2);
    EXPECT_EQ(r0, 500u);
    EXPECT_EQ(r1, 0x01ffu);
  }

  // S -> S: negative sign-extend, negative fit, negative underflow,
  // positive widen, positive fit, positive overflow.
  {
    ScopedType src_narrow = MakeCustomInt(2, 12, 0, H5T_SGN_2, H5T_ORDER_LE);
    ScopedType dst_wide = MakeCustomInt(4, 24, 0, H5T_SGN_2, H5T_ORDER_LE);
    ASSERT_TRUE(src_narrow.valid() && dst_wide.valid());

    std::vector<uint8_t> buf(4 * 2, 0);
    // 12-bit -5 (0x0ffb), +123
    const std::array<uint16_t, 2> in_widen = {0x0ffbu, 123u};
    std::memcpy(buf.data(), in_widen.data(), sizeof(in_widen));
    ASSERT_GE(H5Tconvert(src_narrow.get(), dst_wide.get(), 2, buf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    EXPECT_EQ(ReadUint32LE(buf.data() + 0), 0x00fffffbu);
    EXPECT_EQ(ReadUint32LE(buf.data() + 4), 123u);

    std::vector<uint8_t> buf2(4 * 4, 0);
    const std::array<int32_t, 4> in_narrow = {-100000, -100, 100000, 100};
    std::memcpy(buf2.data(), in_narrow.data(), sizeof(in_narrow));
    ASSERT_GE(H5Tconvert(dst_wide.get(), src_narrow.get(), 4, buf2.data(),
                         nullptr, H5P_DEFAULT),
              0);
    std::array<uint16_t, 4> out{};
    std::memcpy(out.data(), buf2.data(), sizeof(out));
    EXPECT_EQ(out[0], 0x0800u);  // clamped min (-2048 in 12 bits)
    EXPECT_EQ(out[1], static_cast<uint16_t>(-100) & 0x0fffu);
    EXPECT_EQ(out[2], 0x07ffu);  // clamped max (+2047 in 12 bits)
    EXPECT_EQ(out[3], 100u);
  }
}

TEST(H5TconvIntegerSoftIntTest, BigEndianPaddingOverlapsAndStride) {
  // Test src_size > dst_size (5 bytes -> 2 bytes, olap=1, nelmts=6),
  // src_size < dst_size (2 bytes -> 5 bytes, olap=1, nelmts=6),
  // src_size == dst_size (3 bytes -> 3 bytes),
  // Big-Endian source & destination, LSB offset > 0, MSB pad != 8*size,
  // and H5T_PAD_ZERO / H5T_PAD_ONE.
  ScopedType src_be5 =
      MakeCustomInt(5, 32, 0, H5T_SGN_2, H5T_ORDER_BE);
  ScopedType dst_be2_pad1 =
      MakeCustomInt(2, 10, 3, H5T_SGN_2, H5T_ORDER_BE, H5T_PAD_ONE,
                    H5T_PAD_ONE);
  ASSERT_TRUE(src_be5.valid() && dst_be2_pad1.valid());

  std::vector<uint8_t> buf(5 * 6, 0);
  for (size_t i = 0; i < 6; ++i) {
    // Store 32-bit value at BE offset 1..4 inside 5-byte element (value = i*10
    // for even, -5000 for 5).
    const int32_t v = (i == 5) ? -5000 : static_cast<int32_t>(i * 10);
    const uint32_t u = static_cast<uint32_t>(v);
    buf[i * 5 + 1] = static_cast<uint8_t>((u >> 24) & 0xffu);
    buf[i * 5 + 2] = static_cast<uint8_t>((u >> 16) & 0xffu);
    buf[i * 5 + 3] = static_cast<uint8_t>((u >> 8) & 0xffu);
    buf[i * 5 + 4] = static_cast<uint8_t>(u & 0xffu);
  }
  ASSERT_GE(H5Tconvert(src_be5.get(), dst_be2_pad1.get(), 6, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // Convert back from 2-byte padded BE -> 5-byte BE with PAD_ZERO (src < dst).
  ScopedType dst_be5_pad0 = MakeCustomInt(5, 24, 4, H5T_SGN_2, H5T_ORDER_BE,
                                          H5T_PAD_ZERO, H5T_PAD_ZERO);
  ASSERT_TRUE(dst_be5_pad0.valid());
  ASSERT_GE(H5Tconvert(dst_be2_pad1.get(), dst_be5_pad0.get(), 6, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // Equal sizes (3 -> 3) BE -> LE with non-zero offset and MSB/LSB padding.
  ScopedType src_eq = MakeCustomInt(3, 18, 2, H5T_SGN_2, H5T_ORDER_BE);
  ScopedType dst_eq = MakeCustomInt(3, 16, 4, H5T_SGN_NONE, H5T_ORDER_BE,
                                    H5T_PAD_ZERO, H5T_PAD_ONE);
  ASSERT_TRUE(src_eq.valid() && dst_eq.valid());
  std::vector<uint8_t> eq_buf = {0x00, 0x00, 0x10, 0x00, 0x01, 0x00};
  ASSERT_GE(H5Tconvert(src_eq.get(), dst_eq.get(), 2, eq_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);

  // Explicit buf_stride != 0 via H5T__conv_i_i directly.
  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  H5T_t* s_obj = LookupTypeObj(src_eq.get());
  H5T_t* d_obj = LookupTypeObj(dst_eq.get());
  ASSERT_NE(s_obj, nullptr);
  ASSERT_NE(d_obj, nullptr);
  EXPECT_GE(H5T__conv_i_i(s_obj, d_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
  std::vector<uint8_t> strided_buf(16 * 2, 0);
  strided_buf[2] = 0x20;
  strided_buf[18] = 0x40;
  H5T_conv_ctx_t ctx = MakeConvCtx(src_eq.get(), dst_eq.get(), nullptr);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_i_i(s_obj, d_obj, &cdata, &ctx, 2, 16, 0,
                          strided_buf.data(), nullptr),
            0);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_i_i(s_obj, d_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
}

TEST(H5TconvIntegerSoftIntTest, ExceptionsUnhandledHandledAndAbort) {
  // Test every exception callback branch inside H5T__conv_i_i:
  // 1) U -> U RANGE_HI (UNHANDLED, HANDLED with BE dst, ABORT)
  // 2) S -> U RANGE_LOW (UNHANDLED, HANDLED, ABORT)
  // 3) S -> U RANGE_HI (UNHANDLED, HANDLED, ABORT)
  // 4) U -> S RANGE_HI (UNHANDLED, HANDLED, ABORT)
  // 5) S -> S negative RANGE_LOW (UNHANDLED, HANDLED, ABORT)
  // 6) S -> S positive RANGE_HI (UNHANDLED, HANDLED, ABORT)
  struct Case {
    H5T_sign_t src_sgn;
    size_t src_prec;
    H5T_sign_t dst_sgn;
    size_t dst_prec;
    uint32_t input;
    H5T_conv_except_t expected_event;
    uint32_t unhandled_expected;
  };
  const std::array<Case, 6> kCases = {{
      {H5T_SGN_NONE, 20, H5T_SGN_NONE, 10, 2000u, H5T_CONV_EXCEPT_RANGE_HI,
       0x03ffu},
      {H5T_SGN_2, 20, H5T_SGN_NONE, 10, static_cast<uint32_t>(-5),
       H5T_CONV_EXCEPT_RANGE_LOW, 0u},
      {H5T_SGN_2, 20, H5T_SGN_NONE, 10, 5000u, H5T_CONV_EXCEPT_RANGE_HI,
       0x03ffu},
      {H5T_SGN_NONE, 20, H5T_SGN_2, 10, 5000u, H5T_CONV_EXCEPT_RANGE_HI,
       0x01ffu},
      {H5T_SGN_2, 20, H5T_SGN_2, 10, static_cast<uint32_t>(-5000),
       H5T_CONV_EXCEPT_RANGE_LOW, 0x0200u},
      {H5T_SGN_2, 20, H5T_SGN_2, 10, 5000u, H5T_CONV_EXCEPT_RANGE_HI,
       0x01ffu},
  }};

  for (const auto& tc : kCases) {
    ScopedType src = MakeCustomInt(4, tc.src_prec, 0, tc.src_sgn, H5T_ORDER_LE);
    ScopedType dst_be =
        MakeCustomInt(2, tc.dst_prec, 0, tc.dst_sgn, H5T_ORDER_BE);
    ASSERT_TRUE(src.valid() && dst_be.valid());

    for (H5T_conv_ret_t action :
         {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
      ExceptionLog log;
      log.action = action;
      log.fill_value = 0x0234u;  // Fits in 10-bit dst_prec so MSB pad keeps it
      log.fill_size = 2;
      ScopedPlist dxpl = MakeExceptionDxpl(&log);
      ASSERT_TRUE(dxpl.valid());

      std::array<uint8_t, 4> buf{};
      std::memcpy(buf.data(), &tc.input, sizeof(tc.input));

      if (action == H5T_CONV_ABORT) {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Tconvert(src.get(), dst_be.get(), 1, buf.data(), nullptr,
                             dxpl.get()),
                  0);
        EXPECT_EQ(log.last_event, tc.expected_event);
      } else {
        ASSERT_GE(H5Tconvert(src.get(), dst_be.get(), 1, buf.data(), nullptr,
                             dxpl.get()),
                  0);
        EXPECT_EQ(log.calls, 1);
        EXPECT_EQ(log.last_event, tc.expected_event);
        uint16_t raw = 0;
        std::memcpy(&raw, buf.data(), 2);
        if (action == H5T_CONV_HANDLED) {
          // HANDLED leaves reverse=false on dst_be, keeping LE user value
          // while MSB pad clears bits [offset+prec, 8*size).
          EXPECT_EQ(raw, 0x0234u);
        } else {
          // BE decoded 2-byte integer matches unhandled_expected.
          const uint16_t decoded =
              static_cast<uint16_t>((buf[0] << 8) | buf[1]);
          EXPECT_EQ(decoded, tc.unhandled_expected);
        }
      }
    }
  }
}

TEST(H5TconvIntegerSoftIntTest, InternalInitConvAndUnknownCommandErrors) {
  ScopedType src = MakeCustomInt(4, 24, 0, H5T_SGN_2, H5T_ORDER_LE);
  ScopedType dst = MakeCustomInt(2, 12, 0, H5T_SGN_2, H5T_ORDER_LE);
  ScopedType too_large = MakeCustomInt(65, 24, 0, H5T_SGN_2, H5T_ORDER_LE);
  ASSERT_TRUE(src.valid() && dst.valid() && too_large.valid());

  H5T_t* s_obj = LookupTypeObj(src.get());
  H5T_t* d_obj = LookupTypeObj(dst.get());
  H5T_t* big_obj = LookupTypeObj(too_large.get());
  ASSERT_NE(s_obj, nullptr);
  ASSERT_NE(d_obj, nullptr);
  ASSERT_NE(big_obj, nullptr);

  // H5Tset_order rejects H5T_ORDER_NONE on integer types; wrap stack copies to
  // exercise H5T__conv_i_i's H5T_ORDER_NONE validation guards safely.
  H5T_shared_t bad_int_sh = *s_obj->shared;
  bad_int_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_int_t = *s_obj;
  bad_int_t.shared = &bad_int_sh;
  H5T_t* bad_obj = &bad_int_t;

  ScopedErrorSilence silence;
  H5T_cdata_t cdata{};

  // H5T_CONV_INIT errors
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_i_i(nullptr, d_obj, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_i_i(s_obj, nullptr, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(H5T__conv_i_i(bad_obj, d_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_i_i(s_obj, bad_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_i_i(s_obj, big_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);

  // H5T_CONV_CONV errors
  cdata.command = H5T_CONV_CONV;
  H5T_conv_ctx_t ctx = MakeConvCtx(src.get(), dst.get(), nullptr);
  uint32_t val = 1;
  EXPECT_LT(
      H5T__conv_i_i(nullptr, d_obj, &cdata, &ctx, 1, 0, 0, &val, nullptr), 0);
  EXPECT_LT(
      H5T__conv_i_i(s_obj, nullptr, &cdata, &ctx, 1, 0, 0, &val, nullptr), 0);
  EXPECT_LT(
      H5T__conv_i_i(s_obj, d_obj, &cdata, nullptr, 1, 0, 0, &val, nullptr), 0);

  // Unknown command
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_i_i(s_obj, d_obj, &cdata, &ctx, 1, 0, 0, &val, nullptr), 0);
}

// ============================================================================
// Soft integer-to-float and integer-to-complex (H5T__conv_i_f / _i_complex)
// ============================================================================

TEST(H5TconvIntegerSoftFloatTest, RoundingEdgeCasesAndMaxNegAndNormNone) {
  // Custom 32-bit unsigned LE with prec=31 guarantees H5T__conv_i_f (not hard).
  ScopedType u31 = MakeCustomInt(4, 31, 0, H5T_SGN_NONE, H5T_ORDER_LE);
  ScopedType f32 = MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_IMPLIED);
  ScopedType f32_norm_none = MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_NONE);
  ASSERT_TRUE(u31.valid() && f32.valid() && f32_norm_none.valid());
  EXPECT_EQ(H5Tcompiler_conv(u31.get(), f32.get()), 0);

  // 1) Special values in H5T__conv_i_f_loop with NORM_IMPLIED:
  //    - 0 (zero jump to padding)
  //    - 42 (fits mantissa, first <= msize)
  //    - 0x2000001 (dropoff bit = 0 -> round down)
  //    - 0x2000003 (dropoff bit = 1 + trailing bits > 0 -> round up)
  //    - 0x1000001 (.5 tie, even LSB -> round down)
  //    - 0x1000003 (.5 tie, odd LSB -> round up)
  //    - 0x1ffffff (all 1s -> rounding carry overflows mantissa, expo++)
  const std::array<uint32_t, 7> inputs = {
      0u,          42u,         0x2000001u, 0x2000003u,
      0x1000001u,  0x1000003u,  0x1ffffffu,
  };
  std::array<uint32_t, 7> buf = inputs;
  ASSERT_GE(H5Tconvert(u31.get(), f32.get(), buf.size(), buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[0]), 0.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[1]), 42.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[2]), 33554432.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[3]), 33554436.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[4]), 16777216.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[5]), 16777220.0f);
  EXPECT_FLOAT_EQ(ReadFloatLE(&buf[6]), 33554432.0f);

  // 2) Signed negative including max negative 0x80000000 and normal negative.
  ScopedType i32_be = MakeCustomInt(4, 32, 0, H5T_SGN_2, H5T_ORDER_BE);
  ScopedType f32_be = MakeCustomFloatF32(H5T_ORDER_BE, H5T_NORM_IMPLIED);
  ASSERT_TRUE(i32_be.valid() && f32_be.valid());
  std::vector<uint8_t> be_buf(4 * 4, 0);
  const std::array<uint32_t, 4> signed_vals = {
      0x80000000u,                  // INT32_MIN (is_max_neg)
      static_cast<uint32_t>(-42),   // normal negative
      0u,                           // zero
      0x1ffffffu,                   // positive carry
  };
  for (size_t i = 0; i < 4; ++i) {
    be_buf[i * 4 + 0] = static_cast<uint8_t>((signed_vals[i] >> 24) & 0xffu);
    be_buf[i * 4 + 1] = static_cast<uint8_t>((signed_vals[i] >> 16) & 0xffu);
    be_buf[i * 4 + 2] = static_cast<uint8_t>((signed_vals[i] >> 8) & 0xffu);
    be_buf[i * 4 + 3] = static_cast<uint8_t>(signed_vals[i] & 0xffu);
  }
  ASSERT_GE(H5Tconvert(i32_be.get(), f32_be.get(), 4, be_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(ReadFloatBE(be_buf.data() + 0), -2147483648.0f);
  EXPECT_FLOAT_EQ(ReadFloatBE(be_buf.data() + 4), -42.0f);
  EXPECT_FLOAT_EQ(ReadFloatBE(be_buf.data() + 8), 0.0f);
  EXPECT_FLOAT_EQ(ReadFloatBE(be_buf.data() + 12), 33554432.0f);

  // 3) H5T_NORM_NONE rounding carry (0x0ffffff -> expo++ and right-shift 1).
  std::array<uint32_t, 2> norm_none_buf = {0x0ffffffu, 5u};
  ASSERT_GE(H5Tconvert(u31.get(), f32_norm_none.get(), norm_none_buf.size(),
                       norm_none_buf.data(), nullptr, H5P_DEFAULT),
            0);
  // Explicit leading bit in 23-bit mantissa => bit 22 set (0x400000), expo=151.
  const uint32_t bits = norm_none_buf[0];
  EXPECT_EQ((bits >> 23) & 0xffu, 151u);
  EXPECT_EQ(bits & 0x7fffffu, 0x400000u);

  // 4) Exact byte-multiple mantissa width (msize=16) with src prec <= 16
  //    exercises `(int_buf_size * 8) <= msize` in H5T__conv_i_f_loop.
  ScopedType u12 = MakeCustomInt(2, 12, 0, H5T_SGN_NONE, H5T_ORDER_LE);
  ScopedType flt_m16(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(u12.valid() && flt_m16.valid());
  ASSERT_GE(H5Tset_fields(flt_m16.get(), 20, 16, 4, 0, 16), 0);
  ASSERT_GE(H5Tset_precision(flt_m16.get(), 21), 0);
  ASSERT_GE(H5Tset_ebias(flt_m16.get(), 7), 0);
  std::array<uint8_t, 4> m16_buf{};
  const uint16_t u12_in = 42u;
  std::memcpy(m16_buf.data(), &u12_in, sizeof(u12_in));
  ASSERT_GE(H5Tconvert(u12.get(), flt_m16.get(), 1, m16_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_NE(ReadUint32LE(m16_buf.data()), 0u);
}

TEST(H5TconvIntegerSoftFloatTest, OverlapsVaxPaddingOverflowAndCallbacks) {
  // 1) VAX F32 & F64 byte ordering: both src > dst and src < dst
  ScopedType i16_cust = MakeCustomInt(2, 16, 0, H5T_SGN_2, H5T_ORDER_LE);
  ScopedType i64_cust = MakeCustomInt(8, 64, 0, H5T_SGN_2, H5T_ORDER_LE);
  ASSERT_TRUE(i16_cust.valid() && i64_cust.valid());

  // i16 (2 bytes) -> H5T_VAX_F32 (4 bytes): src < dst, nelmts=5 (dp & dbuf)
  std::vector<uint8_t> vax32_buf(5 * 4, 0);
  const std::array<int16_t, 5> s16 = {0, 1, -2, 100, -32768};
  std::memcpy(vax32_buf.data(), s16.data(), sizeof(s16));
  ASSERT_GE(H5Tconvert(i16_cust.get(), H5T_VAX_F32, 5, vax32_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // i64 (8 bytes) -> H5T_VAX_F32 (4 bytes): src > dst, nelmts=5 (dbuf & dp)
  std::vector<uint8_t> shrink_buf(5 * 8, 0);
  const std::array<int64_t, 5> s64 = {0, 7, -9, 1024, 99999};
  std::memcpy(shrink_buf.data(), s64.data(), sizeof(s64));
  ASSERT_GE(H5Tconvert(i64_cust.get(), H5T_VAX_F32, 5, shrink_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // Also test H5T_VAX_F64
  std::vector<uint8_t> vax64_buf(8, 0);
  const int16_t v16 = 42;
  std::memcpy(vax64_buf.data(), &v16, sizeof(v16));
  ASSERT_GE(H5Tconvert(i16_cust.get(), H5T_VAX_F64, 1, vax64_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // 2) Destination float LSB & MSB padding (offset=4 in 8-byte container)
  ScopedType padded_f_one =
      MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_IMPLIED, 8, 4, H5T_PAD_ONE,
                         H5T_PAD_ONE);
  ScopedType padded_f_zero =
      MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_IMPLIED, 8, 4, H5T_PAD_ZERO,
                         H5T_PAD_ZERO);
  ASSERT_TRUE(padded_f_one.valid() && padded_f_zero.valid());
  std::array<uint8_t, 16> pad_buf{};
  std::memset(pad_buf.data(), 0, pad_buf.size());
  const int64_t val = 10;
  std::memcpy(pad_buf.data(), &val, sizeof(val));
  ASSERT_GE(H5Tconvert(i64_cust.get(), padded_f_one.get(), 1, pad_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_EQ(pad_buf[0] & 0x0fu, 0x0fu);  // LSB pad = ONE
  EXPECT_EQ(pad_buf[7], 0xffu);         // MSB pad = ONE

  std::memset(pad_buf.data(), 0xff, pad_buf.size());
  std::memcpy(pad_buf.data(), &val, sizeof(val));
  ASSERT_GE(H5Tconvert(i64_cust.get(), padded_f_zero.get(), 1, pad_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_EQ(pad_buf[0] & 0x0fu, 0x00u);  // LSB pad = ZERO
  EXPECT_EQ(pad_buf[7], 0x00u);          // MSB pad = ZERO

  // 3) Exponent overflow to F4E2M1 (esize=2, expo_max=3, max finite = 6.0)
  //    plus PRECISION & RANGE_HI callbacks: UNHANDLED, HANDLED, ABORT.
  ScopedType i32_le = MakeCustomInt(4, 31, 0, H5T_SGN_2, H5T_ORDER_LE);
  ASSERT_TRUE(i32_le.valid());
  {
    // No callback -> infinity pattern in F4E2M1 (sign=0, exp=0b11, mant=0 -> 6)
    std::array<uint8_t, 4> ovf{};
    const int32_t big = 1000;
    std::memcpy(ovf.data(), &big, sizeof(big));
    ASSERT_GE(H5Tconvert(i32_le.get(), H5T_FLOAT_F4E2M1, 1, ovf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    EXPECT_EQ(ovf[0] & 0x0fu, 0x06u);
  }
  for (H5T_conv_ret_t prec_action :
       {H5T_CONV_HANDLED, H5T_CONV_ABORT, H5T_CONV_UNHANDLED}) {
    ExceptionLog log;
    log.action = prec_action;
    log.fill_value = 0x03u;
    log.fill_size = 1;
    ScopedPlist dxpl = MakeExceptionDxpl(&log);
    ASSERT_TRUE(dxpl.valid());
    std::array<uint8_t, 4> buf{};
    const int32_t in_val = 0x1000003;  // >23 bits for F4E2M1 msize=1 & >expo
    std::memcpy(buf.data(), &in_val, sizeof(in_val));
    if (prec_action == H5T_CONV_ABORT) {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tconvert(i32_le.get(), H5T_FLOAT_F4E2M1, 1, buf.data(),
                           nullptr, dxpl.get()),
                0);
      EXPECT_EQ(log.last_event, H5T_CONV_EXCEPT_PRECISION);
    } else {
      ASSERT_GE(H5Tconvert(i32_le.get(), H5T_FLOAT_F4E2M1, 1, buf.data(),
                           nullptr, dxpl.get()),
                0);
      EXPECT_GE(log.calls, 1);
    }
  }
  // Trigger RANGE_HI after UNHANDLED precision: input=16 -> first=4 > msize=1,
  // hitting precision loss and RANGE_HI (expo=5 > 3). Check ABORT & HANDLED.
  struct RangeHiCtx {
    H5T_conv_ret_t range_action;
    int range_calls = 0;
  };
  auto range_cb = [](H5T_conv_except_t ev, hid_t, hid_t, void*, void* dst_buf,
                     void* ud) -> H5T_conv_ret_t {
    auto* c = static_cast<RangeHiCtx*>(ud);
    if (ev == H5T_CONV_EXCEPT_PRECISION) {
      return H5T_CONV_UNHANDLED;
    }
    if (ev == H5T_CONV_EXCEPT_RANGE_HI) {
      c->range_calls++;
      if (c->range_action == H5T_CONV_HANDLED) {
        static_cast<uint8_t*>(dst_buf)[0] = 0x05u;
      }
      return c->range_action;
    }
    return H5T_CONV_UNHANDLED;
  };
  for (H5T_conv_ret_t act :
       {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
    RangeHiCtx rctx{act, 0};
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), range_cb, &rctx), 0);
    std::array<uint8_t, 4> buf{};
    const int32_t v = 16;  // first=4 > msize=1 AND expo=4+1=5 > 3
    std::memcpy(buf.data(), &v, sizeof(v));
    if (act == H5T_CONV_ABORT) {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tconvert(i32_le.get(), H5T_FLOAT_F4E2M1, 1, buf.data(),
                           nullptr, dxpl.get()),
                0);
    } else {
      ASSERT_GE(H5Tconvert(i32_le.get(), H5T_FLOAT_F4E2M1, 1, buf.data(),
                           nullptr, dxpl.get()),
                0);
    }
    EXPECT_EQ(rctx.range_calls, 1);
  }
}

TEST(H5TconvIntegerSoftFloatTest, CustomComplexConversionsAndErrors) {
  // Custom integer -> LE and BE complex with nelmts=5 so both d==dbuf and
  // non-overlapping d!=dbuf (memset imaginary zeroing) execute cleanly.
  ScopedType i16_be = MakeCustomInt(2, 16, 0, H5T_SGN_2, H5T_ORDER_BE);
  ScopedType f32_be = MakeCustomFloatF32(H5T_ORDER_BE, H5T_NORM_IMPLIED);
  ScopedType f64_le(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(i16_be.valid() && f32_be.valid() && f64_le.valid());

  ScopedType cplx_be(H5Tcomplex_create(f32_be.get()));
  ScopedType cplx_le(H5Tcomplex_create(f64_le.get()));
  ASSERT_TRUE(cplx_be.valid() && cplx_le.valid());
  EXPECT_EQ(H5Tcompiler_conv(i16_be.get(), cplx_be.get()), 0);
  EXPECT_EQ(H5Tcompiler_conv(i16_be.get(), cplx_le.get()), 0);

  constexpr size_t kElmts = 5;
  std::vector<uint8_t> buf(kElmts * 16, 0xaa);
  const std::array<int16_t, kElmts> vals = {0, -7, 15, -32768, 256};
  for (size_t i = 0; i < kElmts; ++i) {
    const uint16_t u = static_cast<uint16_t>(vals[i]);
    buf[i * 2 + 0] = static_cast<uint8_t>((u >> 8) & 0xffu);
    buf[i * 2 + 1] = static_cast<uint8_t>(u & 0xffu);
  }
  ASSERT_GE(H5Tconvert(i16_be.get(), cplx_be.get(), kElmts, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kElmts; ++i) {
    EXPECT_FLOAT_EQ(ReadFloatBE(buf.data() + i * 8),
                    static_cast<float>(vals[i]));
    EXPECT_FLOAT_EQ(ReadFloatBE(buf.data() + i * 8 + 4), 0.0f);
  }

  // Test F64_LE complex too (src size 2 < dst size 16 -> olap=1, direction=-1).
  for (size_t i = 0; i < kElmts; ++i) {
    const uint16_t u = static_cast<uint16_t>(vals[i]);
    buf[i * 2 + 0] = static_cast<uint8_t>((u >> 8) & 0xffu);
    buf[i * 2 + 1] = static_cast<uint8_t>(u & 0xffu);
  }
  ASSERT_GE(H5Tconvert(i16_be.get(), cplx_le.get(), kElmts, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kElmts; ++i) {
    double re = 0.0, im = 1.0;
    std::memcpy(&re, buf.data() + i * 16, sizeof(double));
    std::memcpy(&im, buf.data() + i * 16 + 8, sizeof(double));
    EXPECT_DOUBLE_EQ(re, static_cast<double>(vals[i]));
    EXPECT_DOUBLE_EQ(im, 0.0);
  }

  // Test buf_stride != 0 directly in H5T__conv_i_f and H5T__conv_i_complex.
  H5T_t* s_obj = LookupTypeObj(i16_be.get());
  H5T_t* d_flt_obj = LookupTypeObj(f32_be.get());
  H5T_t* d_cplx_obj = LookupTypeObj(cplx_be.get());
  ASSERT_NE(s_obj, nullptr);
  ASSERT_NE(d_flt_obj, nullptr);
  ASSERT_NE(d_cplx_obj, nullptr);

  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  EXPECT_GE(H5T__conv_i_f(s_obj, d_flt_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
  EXPECT_GE(H5T__conv_i_complex(s_obj, d_cplx_obj, &cdata, nullptr, 0, 0, 0,
                                nullptr, nullptr),
            0);

  std::vector<uint8_t> strided(16 * 2, 0);
  strided[0] = 0x00;
  strided[1] = 0x09;
  strided[16] = 0xff;
  strided[17] = 0xfb;  // -5 BE
  H5T_conv_ctx_t ctx = MakeConvCtx(i16_be.get(), cplx_be.get(), nullptr);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_i_complex(s_obj, d_cplx_obj, &cdata, &ctx, 2, 16, 0,
                                strided.data(), nullptr),
            0);
  EXPECT_FLOAT_EQ(ReadFloatBE(strided.data() + 0), 9.0f);
  EXPECT_FLOAT_EQ(ReadFloatBE(strided.data() + 16), -5.0f);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_i_f(s_obj, d_flt_obj, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);
  EXPECT_GE(H5T__conv_i_complex(s_obj, d_cplx_obj, &cdata, nullptr, 0, 0, 0,
                                nullptr, nullptr),
            0);
}

TEST(H5TconvIntegerSoftFloatTest, InternalErrorAndUnsupportedPaths) {
  ScopedType int_le = MakeCustomInt(4, 31, 0, H5T_SGN_2, H5T_ORDER_LE);
  ScopedType flt_le = MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_IMPLIED);
  ScopedType flt_msbset = MakeCustomFloatF32(H5T_ORDER_LE, H5T_NORM_MSBSET);
  ScopedType flt_big(H5Tcopy(H5T_IEEE_F64LE));
  ScopedType flt_33(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(int_le.valid() && flt_le.valid() && flt_msbset.valid() &&
              flt_big.valid() && flt_33.valid());
  ASSERT_GE(H5Tset_size(flt_big.get(), 65), 0);
  ASSERT_GE(H5Tset_size(flt_33.get(), 33), 0);

  ScopedType cplx_ok(H5Tcomplex_create(flt_le.get()));
  ScopedType cplx_big(H5Tcomplex_create(flt_33.get()));
  ScopedType cplx_msbset(H5Tcomplex_create(flt_msbset.get()));
  ASSERT_TRUE(cplx_ok.valid() && cplx_big.valid() && cplx_msbset.valid());

  H5T_t* i_ok = LookupTypeObj(int_le.get());
  H5T_t* f_ok = LookupTypeObj(flt_le.get());
  H5T_t* f_msb = LookupTypeObj(flt_msbset.get());
  H5T_t* f_big = LookupTypeObj(flt_big.get());
  H5T_t* c_ok = LookupTypeObj(cplx_ok.get());
  H5T_t* c_big = LookupTypeObj(cplx_big.get());
  H5T_t* c_msb = LookupTypeObj(cplx_msbset.get());
  H5T_t* vax_f32 = LookupTypeObj(H5T_VAX_F32);
  ASSERT_TRUE(i_ok && f_ok && f_msb && f_big && c_ok && c_big && c_msb &&
              vax_f32);

  // Stack-local synthetic H5T_t wrappers to hit internal defensive guards
  // (ORDER_NONE, esize > 63, non-atomic complex parent, VAX complex in loop)
  // without mutating any registered HDF5 datatype object.
  H5T_shared_t i_bad_sh = *i_ok->shared;
  i_bad_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t i_bad_t = *i_ok;
  i_bad_t.shared = &i_bad_sh;
  H5T_t* i_bad = &i_bad_t;

  H5T_shared_t f_bad_sh = *f_ok->shared;
  f_bad_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t f_bad_t = *f_ok;
  f_bad_t.shared = &f_bad_sh;
  H5T_t* f_bad = &f_bad_t;

  H5T_shared_t c_bad_sh = *c_ok->shared;
  c_bad_sh.parent = f_bad;
  H5T_t c_bad_t = *c_ok;
  c_bad_t.shared = &c_bad_sh;
  H5T_t* c_bad = &c_bad_t;
  H5T_shared_t esize_flt_sh = *f_ok->shared;
  esize_flt_sh.u.atomic.u.f.esize = 64;
  H5T_t esize_flt_t = *f_ok;
  esize_flt_t.shared = &esize_flt_sh;

  H5T_shared_t esize_cplx_sh = *c_ok->shared;
  esize_cplx_sh.parent = &esize_flt_t;
  H5T_t esize_cplx_t = *c_ok;
  esize_cplx_t.shared = &esize_cplx_sh;

  ScopedType comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(comp.valid());
  H5T_t* comp_obj = LookupTypeObj(comp.get());
  ASSERT_NE(comp_obj, nullptr);
  H5T_shared_t non_atomic_cplx_sh = *c_ok->shared;
  non_atomic_cplx_sh.parent = comp_obj;
  H5T_t non_atomic_cplx_t = *c_ok;
  non_atomic_cplx_t.shared = &non_atomic_cplx_sh;

  H5T_shared_t vax_cplx_sh = *c_ok->shared;
  vax_cplx_sh.parent = vax_f32;
  H5T_t vax_cplx_t = *c_ok;
  vax_cplx_t.shared = &vax_cplx_sh;

  ScopedErrorSilence silence;
  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx = MakeConvCtx(int_le.get(), flt_le.get(), nullptr);
  std::array<uint8_t, 16> scratch{};
  const uint32_t sample = 5;
  std::memcpy(scratch.data(), &sample, sizeof(sample));

  // H5T__conv_i_f error paths
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_i_f(nullptr, f_ok, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_i_f(i_ok, nullptr, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_i_f(i_bad, f_ok, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_i_f(i_ok, f_bad, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_i_f(i_ok, f_big, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(H5T__conv_i_f(i_ok, &esize_flt_t, &cdata, nullptr, 0, 0, 0, nullptr,
                          nullptr),
            0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_i_f(nullptr, f_ok, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_i_f(i_ok, nullptr, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_i_f(i_ok, f_ok, &cdata, nullptr, 1, 0, 0, scratch.data(),
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_i_f(i_ok, f_msb, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_i_f(i_ok, f_ok, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);

  // H5T__conv_i_complex error paths
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_i_complex(nullptr, c_ok, &cdata, nullptr, 0, 0, 0,
                                nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, nullptr, &cdata, nullptr, 0, 0, 0,
                                nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, &non_atomic_cplx_t, &cdata, nullptr, 0, 0,
                                0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_bad, c_ok, &cdata, nullptr, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, c_bad, &cdata, nullptr, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, c_big, &cdata, nullptr, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, &esize_cplx_t, &cdata, nullptr, 0, 0, 0,
                                nullptr, nullptr),
            0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_i_complex(nullptr, c_ok, &cdata, &ctx, 1, 0, 0,
                                scratch.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, nullptr, &cdata, &ctx, 1, 0, 0,
                                scratch.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, c_ok, &cdata, nullptr, 1, 0, 0,
                                scratch.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_i_complex(i_ok, c_msb, &cdata, &ctx, 1, 0, 0,
                                scratch.data(), nullptr),
            0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_i_complex(i_ok, c_ok, &cdata, &ctx, 1, 0, 0,
                                scratch.data(), nullptr),
            0);

  // H5T__conv_i_f_loop directly with VAX byte order complex destination
  std::memcpy(scratch.data(), &sample, sizeof(sample));
  EXPECT_LT(H5T__conv_i_f_loop(i_ok, &vax_cplx_t, &ctx, 1, 0, scratch.data()),
            0);
}

// ============================================================================
// Hard (compiler cast) conversion functions (all 160 in H5Tconv_integer.c)
// ============================================================================

TEST(H5TconvIntegerHardTest, PublicNativeDispatchAndExceptions) {
  // Public H5Tconvert paths across non-shadowed integer, float, float16, and
  // complex pairs, including overlap (nelmts = 10), overflow/underflow, and
  // precision exceptions.
  ASSERT_GE(H5open(), 0);
  EXPECT_GT(H5Tcompiler_conv(H5T_NATIVE_INT, H5T_NATIVE_UCHAR), 0);
  EXPECT_GT(H5Tcompiler_conv(H5T_NATIVE_UINT, H5T_NATIVE_FLOAT), 0);
  EXPECT_GT(H5Tcompiler_conv(H5T_NATIVE_SCHAR, H5T_NATIVE_FLOAT_COMPLEX), 0);

  // 1) Shrinking integer with nelmts=10 (forward pass) + exception callback
  {
    std::vector<uint8_t> buf(10 * sizeof(int), 0);
    const std::array<int, 10> vals = {-200, -1, 0, 10, 127, 255, 256, 300,
                                      -5, 42};
    std::memcpy(buf.data(), vals.data(), sizeof(vals));
    ASSERT_GE(H5Tconvert(H5T_NATIVE_INT, H5T_NATIVE_UCHAR, 10, buf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    EXPECT_EQ(buf[0], 0u);
    EXPECT_EQ(buf[2], 0u);
    EXPECT_EQ(buf[4], 127u);
    EXPECT_EQ(buf[5], 255u);
    EXPECT_EQ(buf[6], 255u);
    EXPECT_EQ(buf[9], 42u);

    // Repeat with exception callbacks: UNHANDLED, HANDLED, ABORT
    for (H5T_conv_ret_t act :
         {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
      ExceptionLog log;
      log.action = act;
      log.fill_value = 77u;
      log.fill_size = 1;
      ScopedPlist dxpl = MakeExceptionDxpl(&log);
      ASSERT_TRUE(dxpl.valid());
      std::memcpy(buf.data(), vals.data(), sizeof(vals));
      if (act == H5T_CONV_ABORT) {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Tconvert(H5T_NATIVE_INT, H5T_NATIVE_UCHAR, 10, buf.data(),
                             nullptr, dxpl.get()),
                  0);
      } else {
        ASSERT_GE(H5Tconvert(H5T_NATIVE_INT, H5T_NATIVE_UCHAR, 10, buf.data(),
                             nullptr, dxpl.get()),
                  0);
        EXPECT_GT(log.calls, 0);
        if (act == H5T_CONV_HANDLED) {
          EXPECT_EQ(buf[0], 77u);
        }
      }
    }
  }

  // 2) Expanding conversion with nelmts=10 (hits safe>=2 and safe<2 overlap)
  {
    std::vector<uint8_t> buf(10 * sizeof(long double) + 8, 0);
    for (size_t i = 0; i < 10; ++i) {
      const signed char v = static_cast<signed char>(static_cast<int>(i) - 5);
      std::memcpy(buf.data() + i, &v, 1);
    }
    ASSERT_GE(H5Tconvert(H5T_NATIVE_SCHAR, H5T_NATIVE_LDOUBLE, 10, buf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    for (size_t i = 0; i < 10; ++i) {
      long double out = 0.0L;
      std::memcpy(&out, buf.data() + i * sizeof(long double), sizeof(out));
      EXPECT_EQ(out, static_cast<long double>(static_cast<int>(i) - 5));
    }
  }

  // 3) Unaligned buffer (buf + 1) to exercise s_mv / d_mv alignment branches
  {
    std::vector<uint8_t> raw(1 + 4 * sizeof(double) + 16, 0);
    uint8_t* unaligned = raw.data() + 1;
    const std::array<uint32_t, 4> in = {0u, 1u, 100000001u, 0xffffffffu};
    std::memcpy(unaligned, in.data(), sizeof(in));
    ASSERT_GE(H5Tconvert(H5T_NATIVE_UINT, H5T_NATIVE_FLOAT, 4, unaligned,
                         nullptr, H5P_DEFAULT),
              0);
  }

#ifdef H5_HAVE__FLOAT16
  // 4) Public _Float16 overflow & precision checks
  {
    std::vector<uint8_t> buf(8 * 4, 0);
    const std::array<int32_t, 5> in = {-100000, -1, 0, 100000, 12345};
    std::memcpy(buf.data(), in.data(), sizeof(in));
    ASSERT_GE(H5Tconvert(H5T_NATIVE_INT, H5T_NATIVE_FLOAT16, 5, buf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    uint16_t neg_inf_bits = 0, zero_bits = 0, pos_inf_bits = 0;
    std::memcpy(&neg_inf_bits, buf.data() + 0, 2);
    std::memcpy(&zero_bits, buf.data() + 4, 2);
    std::memcpy(&pos_inf_bits, buf.data() + 6, 2);
    EXPECT_EQ(neg_inf_bits, 0xfc00u);  // -Inf in IEEE binary16
    EXPECT_EQ(zero_bits, 0x0000u);     // 0.0
    EXPECT_EQ(pos_inf_bits, 0x7c00u);  // +Inf in IEEE binary16
  }
#endif
}

TEST(H5TconvIntegerHardTest, All160HardFunctionsInitConvertAndFree) {
  // Explicitly exercise every compiled H5T__conv_<src>_<dst> function in
  // H5Tconv_integer.c, including LP64-shadowed long/llong/ulong/ullong pairs
  // that the path table deduplicates.
  ASSERT_GE(H5open(), 0);

  enum class SrcKind {
    kSchar,
    kUchar,
    kShort,
    kUshort,
    kInt,
    kUint,
    kLong,
    kUlong,
    kLlong,
    kUllong,
  };

  struct HardEntry {
    const char* name;
    H5T_lib_conv_t fn;
    SrcKind src_kind;
    hid_t src_id;
    hid_t dst_id;
    size_t src_size;
    size_t dst_size;
  };

  const std::vector<HardEntry> kHardEntries = {
      // schar
      {"schar_uchar", H5T__conv_schar_uchar, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_UCHAR, sizeof(signed char), sizeof(unsigned char)},
      {"schar_short", H5T__conv_schar_short, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_SHORT, sizeof(signed char), sizeof(short)},
      {"schar_ushort", H5T__conv_schar_ushort, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_USHORT, sizeof(signed char),
       sizeof(unsigned short)},
      {"schar_int", H5T__conv_schar_int, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_INT, sizeof(signed char), sizeof(int)},
      {"schar_uint", H5T__conv_schar_uint, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_UINT, sizeof(signed char), sizeof(unsigned int)},
      {"schar_long", H5T__conv_schar_long, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_LONG, sizeof(signed char), sizeof(long)},
      {"schar_ulong", H5T__conv_schar_ulong, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_ULONG, sizeof(signed char), sizeof(unsigned long)},
      {"schar_llong", H5T__conv_schar_llong, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_LLONG, sizeof(signed char), sizeof(long long)},
      {"schar_ullong", H5T__conv_schar_ullong, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_ULLONG, sizeof(signed char),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"schar__Float16", H5T__conv_schar__Float16, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_FLOAT16, sizeof(signed char), 2},
#endif
      {"schar_float", H5T__conv_schar_float, SrcKind::kSchar, H5T_NATIVE_SCHAR,
       H5T_NATIVE_FLOAT, sizeof(signed char), sizeof(float)},
      {"schar_double", H5T__conv_schar_double, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_DOUBLE, sizeof(signed char),
       sizeof(double)},
      {"schar_ldouble", H5T__conv_schar_ldouble, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_LDOUBLE, sizeof(signed char),
       sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"schar_fcomplex", H5T__conv_schar_fcomplex, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_FLOAT_COMPLEX, sizeof(signed char),
       2 * sizeof(float)},
      {"schar_dcomplex", H5T__conv_schar_dcomplex, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(signed char),
       2 * sizeof(double)},
      {"schar_lcomplex", H5T__conv_schar_lcomplex, SrcKind::kSchar,
       H5T_NATIVE_SCHAR, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(signed char),
       2 * sizeof(long double)},
#endif

      // uchar
      {"uchar_schar", H5T__conv_uchar_schar, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_SCHAR, sizeof(unsigned char), sizeof(signed char)},
      {"uchar_short", H5T__conv_uchar_short, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_SHORT, sizeof(unsigned char), sizeof(short)},
      {"uchar_ushort", H5T__conv_uchar_ushort, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_USHORT, sizeof(unsigned char),
       sizeof(unsigned short)},
      {"uchar_int", H5T__conv_uchar_int, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_INT, sizeof(unsigned char), sizeof(int)},
      {"uchar_uint", H5T__conv_uchar_uint, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_UINT, sizeof(unsigned char), sizeof(unsigned int)},
      {"uchar_long", H5T__conv_uchar_long, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_LONG, sizeof(unsigned char), sizeof(long)},
      {"uchar_ulong", H5T__conv_uchar_ulong, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_ULONG, sizeof(unsigned char), sizeof(unsigned long)},
      {"uchar_llong", H5T__conv_uchar_llong, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_LLONG, sizeof(unsigned char), sizeof(long long)},
      {"uchar_ullong", H5T__conv_uchar_ullong, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_ULLONG, sizeof(unsigned char),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"uchar__Float16", H5T__conv_uchar__Float16, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_FLOAT16, sizeof(unsigned char), 2},
#endif
      {"uchar_float", H5T__conv_uchar_float, SrcKind::kUchar, H5T_NATIVE_UCHAR,
       H5T_NATIVE_FLOAT, sizeof(unsigned char), sizeof(float)},
      {"uchar_double", H5T__conv_uchar_double, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_DOUBLE, sizeof(unsigned char),
       sizeof(double)},
      {"uchar_ldouble", H5T__conv_uchar_ldouble, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_LDOUBLE, sizeof(unsigned char),
       sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"uchar_fcomplex", H5T__conv_uchar_fcomplex, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_FLOAT_COMPLEX, sizeof(unsigned char),
       2 * sizeof(float)},
      {"uchar_dcomplex", H5T__conv_uchar_dcomplex, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(unsigned char),
       2 * sizeof(double)},
      {"uchar_lcomplex", H5T__conv_uchar_lcomplex, SrcKind::kUchar,
       H5T_NATIVE_UCHAR, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(unsigned char),
       2 * sizeof(long double)},
#endif

      // short
      {"short_schar", H5T__conv_short_schar, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_SCHAR, sizeof(short), sizeof(signed char)},
      {"short_uchar", H5T__conv_short_uchar, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_UCHAR, sizeof(short), sizeof(unsigned char)},
      {"short_ushort", H5T__conv_short_ushort, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_USHORT, sizeof(short),
       sizeof(unsigned short)},
      {"short_int", H5T__conv_short_int, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_INT, sizeof(short), sizeof(int)},
      {"short_uint", H5T__conv_short_uint, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_UINT, sizeof(short), sizeof(unsigned int)},
      {"short_long", H5T__conv_short_long, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_LONG, sizeof(short), sizeof(long)},
      {"short_ulong", H5T__conv_short_ulong, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_ULONG, sizeof(short), sizeof(unsigned long)},
      {"short_llong", H5T__conv_short_llong, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_LLONG, sizeof(short), sizeof(long long)},
      {"short_ullong", H5T__conv_short_ullong, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_ULLONG, sizeof(short),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"short__Float16", H5T__conv_short__Float16, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_FLOAT16, sizeof(short), 2},
#endif
      {"short_float", H5T__conv_short_float, SrcKind::kShort, H5T_NATIVE_SHORT,
       H5T_NATIVE_FLOAT, sizeof(short), sizeof(float)},
      {"short_double", H5T__conv_short_double, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_DOUBLE, sizeof(short), sizeof(double)},
      {"short_ldouble", H5T__conv_short_ldouble, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_LDOUBLE, sizeof(short),
       sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"short_fcomplex", H5T__conv_short_fcomplex, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_FLOAT_COMPLEX, sizeof(short),
       2 * sizeof(float)},
      {"short_dcomplex", H5T__conv_short_dcomplex, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(short),
       2 * sizeof(double)},
      {"short_lcomplex", H5T__conv_short_lcomplex, SrcKind::kShort,
       H5T_NATIVE_SHORT, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(short),
       2 * sizeof(long double)},
#endif

      // ushort
      {"ushort_schar", H5T__conv_ushort_schar, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_SCHAR, sizeof(unsigned short),
       sizeof(signed char)},
      {"ushort_uchar", H5T__conv_ushort_uchar, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_UCHAR, sizeof(unsigned short),
       sizeof(unsigned char)},
      {"ushort_short", H5T__conv_ushort_short, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_SHORT, sizeof(unsigned short),
       sizeof(short)},
      {"ushort_int", H5T__conv_ushort_int, SrcKind::kUshort, H5T_NATIVE_USHORT,
       H5T_NATIVE_INT, sizeof(unsigned short), sizeof(int)},
      {"ushort_uint", H5T__conv_ushort_uint, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_UINT, sizeof(unsigned short),
       sizeof(unsigned int)},
      {"ushort_long", H5T__conv_ushort_long, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_LONG, sizeof(unsigned short),
       sizeof(long)},
      {"ushort_ulong", H5T__conv_ushort_ulong, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_ULONG, sizeof(unsigned short),
       sizeof(unsigned long)},
      {"ushort_llong", H5T__conv_ushort_llong, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_LLONG, sizeof(unsigned short),
       sizeof(long long)},
      {"ushort_ullong", H5T__conv_ushort_ullong, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_ULLONG, sizeof(unsigned short),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"ushort__Float16", H5T__conv_ushort__Float16, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_FLOAT16, sizeof(unsigned short), 2},
#endif
      {"ushort_float", H5T__conv_ushort_float, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_FLOAT, sizeof(unsigned short),
       sizeof(float)},
      {"ushort_double", H5T__conv_ushort_double, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_DOUBLE, sizeof(unsigned short),
       sizeof(double)},
      {"ushort_ldouble", H5T__conv_ushort_ldouble, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_LDOUBLE, sizeof(unsigned short),
       sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"ushort_fcomplex", H5T__conv_ushort_fcomplex, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_FLOAT_COMPLEX, sizeof(unsigned short),
       2 * sizeof(float)},
      {"ushort_dcomplex", H5T__conv_ushort_dcomplex, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(unsigned short),
       2 * sizeof(double)},
      {"ushort_lcomplex", H5T__conv_ushort_lcomplex, SrcKind::kUshort,
       H5T_NATIVE_USHORT, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(unsigned short),
       2 * sizeof(long double)},
#endif

      // int
      {"int_schar", H5T__conv_int_schar, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_SCHAR, sizeof(int), sizeof(signed char)},
      {"int_uchar", H5T__conv_int_uchar, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_UCHAR, sizeof(int), sizeof(unsigned char)},
      {"int_short", H5T__conv_int_short, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_SHORT, sizeof(int), sizeof(short)},
      {"int_ushort", H5T__conv_int_ushort, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_USHORT, sizeof(int), sizeof(unsigned short)},
      {"int_uint", H5T__conv_int_uint, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_UINT, sizeof(int), sizeof(unsigned int)},
      {"int_long", H5T__conv_int_long, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_LONG, sizeof(int), sizeof(long)},
      {"int_ulong", H5T__conv_int_ulong, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_ULONG, sizeof(int), sizeof(unsigned long)},
      {"int_llong", H5T__conv_int_llong, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_LLONG, sizeof(int), sizeof(long long)},
      {"int_ullong", H5T__conv_int_ullong, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_ULLONG, sizeof(int), sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"int__Float16", H5T__conv_int__Float16, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_FLOAT16, sizeof(int), 2},
#endif
      {"int_float", H5T__conv_int_float, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_FLOAT, sizeof(int), sizeof(float)},
      {"int_double", H5T__conv_int_double, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_DOUBLE, sizeof(int), sizeof(double)},
      {"int_ldouble", H5T__conv_int_ldouble, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_LDOUBLE, sizeof(int), sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"int_fcomplex", H5T__conv_int_fcomplex, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_FLOAT_COMPLEX, sizeof(int), 2 * sizeof(float)},
      {"int_dcomplex", H5T__conv_int_dcomplex, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_DOUBLE_COMPLEX, sizeof(int), 2 * sizeof(double)},
      {"int_lcomplex", H5T__conv_int_lcomplex, SrcKind::kInt, H5T_NATIVE_INT,
       H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(int), 2 * sizeof(long double)},
#endif

      // uint
      {"uint_schar", H5T__conv_uint_schar, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_SCHAR, sizeof(unsigned int), sizeof(signed char)},
      {"uint_uchar", H5T__conv_uint_uchar, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_UCHAR, sizeof(unsigned int), sizeof(unsigned char)},
      {"uint_short", H5T__conv_uint_short, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_SHORT, sizeof(unsigned int), sizeof(short)},
      {"uint_ushort", H5T__conv_uint_ushort, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_USHORT, sizeof(unsigned int), sizeof(unsigned short)},
      {"uint_int", H5T__conv_uint_int, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_INT, sizeof(unsigned int), sizeof(int)},
      {"uint_long", H5T__conv_uint_long, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_LONG, sizeof(unsigned int), sizeof(long)},
      {"uint_ulong", H5T__conv_uint_ulong, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_ULONG, sizeof(unsigned int), sizeof(unsigned long)},
      {"uint_llong", H5T__conv_uint_llong, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_LLONG, sizeof(unsigned int), sizeof(long long)},
      {"uint_ullong", H5T__conv_uint_ullong, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_ULLONG, sizeof(unsigned int), sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"uint__Float16", H5T__conv_uint__Float16, SrcKind::kUint,
       H5T_NATIVE_UINT, H5T_NATIVE_FLOAT16, sizeof(unsigned int), 2},
#endif
      {"uint_float", H5T__conv_uint_float, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_FLOAT, sizeof(unsigned int), sizeof(float)},
      {"uint_double", H5T__conv_uint_double, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_DOUBLE, sizeof(unsigned int), sizeof(double)},
      {"uint_ldouble", H5T__conv_uint_ldouble, SrcKind::kUint, H5T_NATIVE_UINT,
       H5T_NATIVE_LDOUBLE, sizeof(unsigned int), sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"uint_fcomplex", H5T__conv_uint_fcomplex, SrcKind::kUint,
       H5T_NATIVE_UINT, H5T_NATIVE_FLOAT_COMPLEX, sizeof(unsigned int),
       2 * sizeof(float)},
      {"uint_dcomplex", H5T__conv_uint_dcomplex, SrcKind::kUint,
       H5T_NATIVE_UINT, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(unsigned int),
       2 * sizeof(double)},
      {"uint_lcomplex", H5T__conv_uint_lcomplex, SrcKind::kUint,
       H5T_NATIVE_UINT, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(unsigned int),
       2 * sizeof(long double)},
#endif

      // long
      {"long_schar", H5T__conv_long_schar, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_SCHAR, sizeof(long), sizeof(signed char)},
      {"long_uchar", H5T__conv_long_uchar, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_UCHAR, sizeof(long), sizeof(unsigned char)},
      {"long_short", H5T__conv_long_short, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_SHORT, sizeof(long), sizeof(short)},
      {"long_ushort", H5T__conv_long_ushort, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_USHORT, sizeof(long), sizeof(unsigned short)},
      {"long_int", H5T__conv_long_int, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_INT, sizeof(long), sizeof(int)},
      {"long_uint", H5T__conv_long_uint, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_UINT, sizeof(long), sizeof(unsigned int)},
      {"long_ulong", H5T__conv_long_ulong, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_ULONG, sizeof(long), sizeof(unsigned long)},
      {"long_llong", H5T__conv_long_llong, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_LLONG, sizeof(long), sizeof(long long)},
      {"long_ullong", H5T__conv_long_ullong, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_ULLONG, sizeof(long), sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"long__Float16", H5T__conv_long__Float16, SrcKind::kLong,
       H5T_NATIVE_LONG, H5T_NATIVE_FLOAT16, sizeof(long), 2},
#endif
      {"long_float", H5T__conv_long_float, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_FLOAT, sizeof(long), sizeof(float)},
      {"long_double", H5T__conv_long_double, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_DOUBLE, sizeof(long), sizeof(double)},
      {"long_ldouble", H5T__conv_long_ldouble, SrcKind::kLong, H5T_NATIVE_LONG,
       H5T_NATIVE_LDOUBLE, sizeof(long), sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"long_fcomplex", H5T__conv_long_fcomplex, SrcKind::kLong,
       H5T_NATIVE_LONG, H5T_NATIVE_FLOAT_COMPLEX, sizeof(long),
       2 * sizeof(float)},
      {"long_dcomplex", H5T__conv_long_dcomplex, SrcKind::kLong,
       H5T_NATIVE_LONG, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(long),
       2 * sizeof(double)},
      {"long_lcomplex", H5T__conv_long_lcomplex, SrcKind::kLong,
       H5T_NATIVE_LONG, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(long),
       2 * sizeof(long double)},
#endif

      // ulong
      {"ulong_schar", H5T__conv_ulong_schar, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_SCHAR, sizeof(unsigned long), sizeof(signed char)},
      {"ulong_uchar", H5T__conv_ulong_uchar, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_UCHAR, sizeof(unsigned long), sizeof(unsigned char)},
      {"ulong_short", H5T__conv_ulong_short, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_SHORT, sizeof(unsigned long), sizeof(short)},
      {"ulong_ushort", H5T__conv_ulong_ushort, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_USHORT, sizeof(unsigned long),
       sizeof(unsigned short)},
      {"ulong_int", H5T__conv_ulong_int, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_INT, sizeof(unsigned long), sizeof(int)},
      {"ulong_uint", H5T__conv_ulong_uint, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_UINT, sizeof(unsigned long), sizeof(unsigned int)},
      {"ulong_long", H5T__conv_ulong_long, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_LONG, sizeof(unsigned long), sizeof(long)},
      {"ulong_llong", H5T__conv_ulong_llong, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_LLONG, sizeof(unsigned long), sizeof(long long)},
      {"ulong_ullong", H5T__conv_ulong_ullong, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_ULLONG, sizeof(unsigned long),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"ulong__Float16", H5T__conv_ulong__Float16, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_FLOAT16, sizeof(unsigned long), 2},
#endif
      {"ulong_float", H5T__conv_ulong_float, SrcKind::kUlong, H5T_NATIVE_ULONG,
       H5T_NATIVE_FLOAT, sizeof(unsigned long), sizeof(float)},
      {"ulong_double", H5T__conv_ulong_double, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_DOUBLE, sizeof(unsigned long),
       sizeof(double)},
      {"ulong_ldouble", H5T__conv_ulong_ldouble, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_LDOUBLE, sizeof(unsigned long),
       sizeof(long double)},
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"ulong_fcomplex", H5T__conv_ulong_fcomplex, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_FLOAT_COMPLEX, sizeof(unsigned long),
       2 * sizeof(float)},
      {"ulong_dcomplex", H5T__conv_ulong_dcomplex, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(unsigned long),
       2 * sizeof(double)},
      {"ulong_lcomplex", H5T__conv_ulong_lcomplex, SrcKind::kUlong,
       H5T_NATIVE_ULONG, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(unsigned long),
       2 * sizeof(long double)},
#endif

      // llong
      {"llong_schar", H5T__conv_llong_schar, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_SCHAR, sizeof(long long), sizeof(signed char)},
      {"llong_uchar", H5T__conv_llong_uchar, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_UCHAR, sizeof(long long), sizeof(unsigned char)},
      {"llong_short", H5T__conv_llong_short, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_SHORT, sizeof(long long), sizeof(short)},
      {"llong_ushort", H5T__conv_llong_ushort, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_USHORT, sizeof(long long),
       sizeof(unsigned short)},
      {"llong_int", H5T__conv_llong_int, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_INT, sizeof(long long), sizeof(int)},
      {"llong_uint", H5T__conv_llong_uint, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_UINT, sizeof(long long), sizeof(unsigned int)},
      {"llong_long", H5T__conv_llong_long, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_LONG, sizeof(long long), sizeof(long)},
      {"llong_ulong", H5T__conv_llong_ulong, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_ULONG, sizeof(long long), sizeof(unsigned long)},
      {"llong_ullong", H5T__conv_llong_ullong, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_ULLONG, sizeof(long long),
       sizeof(unsigned long long)},
#ifdef H5_HAVE__FLOAT16
      {"llong__Float16", H5T__conv_llong__Float16, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_FLOAT16, sizeof(long long), 2},
#endif
      {"llong_float", H5T__conv_llong_float, SrcKind::kLlong, H5T_NATIVE_LLONG,
       H5T_NATIVE_FLOAT, sizeof(long long), sizeof(float)},
      {"llong_double", H5T__conv_llong_double, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_DOUBLE, sizeof(long long), sizeof(double)},
#ifdef H5T_CONV_INTERNAL_LLONG_LDOUBLE
      {"llong_ldouble", H5T__conv_llong_ldouble, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_LDOUBLE, sizeof(long long),
       sizeof(long double)},
#endif
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"llong_fcomplex", H5T__conv_llong_fcomplex, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_FLOAT_COMPLEX, sizeof(long long),
       2 * sizeof(float)},
      {"llong_dcomplex", H5T__conv_llong_dcomplex, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(long long),
       2 * sizeof(double)},
#ifdef H5T_CONV_INTERNAL_LLONG_LDOUBLE
      {"llong_lcomplex", H5T__conv_llong_lcomplex, SrcKind::kLlong,
       H5T_NATIVE_LLONG, H5T_NATIVE_LDOUBLE_COMPLEX, sizeof(long long),
       2 * sizeof(long double)},
#endif
#endif

      // ullong
      {"ullong_schar", H5T__conv_ullong_schar, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_SCHAR, sizeof(unsigned long long),
       sizeof(signed char)},
      {"ullong_uchar", H5T__conv_ullong_uchar, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_UCHAR, sizeof(unsigned long long),
       sizeof(unsigned char)},
      {"ullong_short", H5T__conv_ullong_short, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_SHORT, sizeof(unsigned long long),
       sizeof(short)},
      {"ullong_ushort", H5T__conv_ullong_ushort, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_USHORT, sizeof(unsigned long long),
       sizeof(unsigned short)},
      {"ullong_int", H5T__conv_ullong_int, SrcKind::kUllong, H5T_NATIVE_ULLONG,
       H5T_NATIVE_INT, sizeof(unsigned long long), sizeof(int)},
      {"ullong_uint", H5T__conv_ullong_uint, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_UINT, sizeof(unsigned long long),
       sizeof(unsigned int)},
      {"ullong_long", H5T__conv_ullong_long, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_LONG, sizeof(unsigned long long),
       sizeof(long)},
      {"ullong_ulong", H5T__conv_ullong_ulong, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_ULONG, sizeof(unsigned long long),
       sizeof(unsigned long)},
      {"ullong_llong", H5T__conv_ullong_llong, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_LLONG, sizeof(unsigned long long),
       sizeof(long long)},
#ifdef H5_HAVE__FLOAT16
      {"ullong__Float16", H5T__conv_ullong__Float16, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_FLOAT16, sizeof(unsigned long long), 2},
#endif
      {"ullong_float", H5T__conv_ullong_float, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_FLOAT, sizeof(unsigned long long),
       sizeof(float)},
      {"ullong_double", H5T__conv_ullong_double, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_DOUBLE, sizeof(unsigned long long),
       sizeof(double)},
#ifdef H5T_CONV_INTERNAL_ULLONG_LDOUBLE
      {"ullong_ldouble", H5T__conv_ullong_ldouble, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_LDOUBLE, sizeof(unsigned long long),
       sizeof(long double)},
#endif
#ifdef H5_HAVE_COMPLEX_NUMBERS
      {"ullong_fcomplex", H5T__conv_ullong_fcomplex, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_FLOAT_COMPLEX, sizeof(unsigned long long),
       2 * sizeof(float)},
      {"ullong_dcomplex", H5T__conv_ullong_dcomplex, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_DOUBLE_COMPLEX, sizeof(unsigned long long),
       2 * sizeof(double)},
#ifdef H5T_CONV_INTERNAL_ULLONG_LDOUBLE
      {"ullong_lcomplex", H5T__conv_ullong_lcomplex, SrcKind::kUllong,
       H5T_NATIVE_ULLONG, H5T_NATIVE_LDOUBLE_COMPLEX,
       sizeof(unsigned long long), 2 * sizeof(long double)},
#endif
#endif
  };

  auto write_sample = [](SrcKind kind, uint8_t* dst, int idx) {
    switch (kind) {
      case SrcKind::kSchar: {
        const std::array<signed char, 6> v = {-100, -1, 0, 5, 120, 127};
        std::memcpy(dst, &v[idx], sizeof(signed char));
        break;
      }
      case SrcKind::kUchar: {
        const std::array<unsigned char, 6> v = {0, 1, 42, 127, 200, 255};
        std::memcpy(dst, &v[idx], sizeof(unsigned char));
        break;
      }
      case SrcKind::kShort: {
        const std::array<short, 6> v = {-30000, -1, 0, 42, 200, 32000};
        std::memcpy(dst, &v[idx], sizeof(short));
        break;
      }
      case SrcKind::kUshort: {
        const std::array<unsigned short, 6> v = {0, 1, 200, 32767, 65000,
                                                 65535};
        std::memcpy(dst, &v[idx], sizeof(unsigned short));
        break;
      }
      case SrcKind::kInt: {
        const std::array<int, 6> v = {-1000000, -1, 0, 42, 123456, 100000001};
        std::memcpy(dst, &v[idx], sizeof(int));
        break;
      }
      case SrcKind::kUint: {
        const std::array<unsigned int, 6> v = {0u, 1u, 300u, 100000u,
                                               100000001u, 3000000000u};
        std::memcpy(dst, &v[idx], sizeof(unsigned int));
        break;
      }
      case SrcKind::kLong: {
        const std::array<long, 6> v = {-1000000L, -1L, 0L, 42L, 100000L,
                                       1000000000000L};
        std::memcpy(dst, &v[idx], sizeof(long));
        break;
      }
      case SrcKind::kUlong: {
        const std::array<unsigned long, 6> v = {0UL, 1UL, 300UL, 100000UL,
                                                1000000000UL,
                                                1000000000000000UL};
        std::memcpy(dst, &v[idx], sizeof(unsigned long));
        break;
      }
      case SrcKind::kLlong: {
        const std::array<long long, 6> v = {-1000000LL, -1LL, 0LL, 42LL,
                                            100000LL, 1000000000000000LL};
        std::memcpy(dst, &v[idx], sizeof(long long));
        break;
      }
      case SrcKind::kUllong: {
        const std::array<unsigned long long, 6> v = {
            0ULL, 1ULL, 300ULL, 100000ULL, 1000000000ULL,
            10000000000000000ULL};
        std::memcpy(dst, &v[idx], sizeof(unsigned long long));
        break;
      }
    }
  };

  constexpr size_t kNelmts = 6;
  for (const auto& entry : kHardEntries) {
    SCOPED_TRACE(entry.name);
    H5T_t* s_obj = LookupTypeObj(entry.src_id);
    H5T_t* d_obj = LookupTypeObj(entry.dst_id);
    ASSERT_NE(s_obj, nullptr);
    ASSERT_NE(d_obj, nullptr);

    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    ASSERT_GE(
        entry.fn(s_obj, d_obj, &cdata, nullptr, 0, 0, 0, nullptr, nullptr), 0);
    EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);

    const size_t elem_span = std::max(entry.src_size, entry.dst_size);
    const size_t stride = elem_span + 4;
    std::vector<uint8_t> buf(kNelmts * stride + 32, 0);

    // Run without callback, strided + unaligned, and unstrided packed.
    H5T_conv_ctx_t ctx_noex = MakeConvCtx(entry.src_id, entry.dst_id, nullptr);
    cdata.command = H5T_CONV_CONV;
    for (size_t i = 0; i < kNelmts; ++i) {
      write_sample(entry.src_kind, buf.data() + i * entry.src_size,
                   static_cast<int>(i));
    }
    ASSERT_GE(entry.fn(s_obj, d_obj, &cdata, &ctx_noex, kNelmts, 0, 0,
                       buf.data(), nullptr),
              0);

    // Run with exception callback (UNHANDLED) and stride on offset +1.
    // Note: H5T_CONV_Xf_CORE evaluates `*(S) < (ST)(D_MIN)` when `*(S) <=
    // D_MAX` (`FLT16_MAX = 65504`). For unsigned sources -> _Float16, casting
    // `-FLT16_MAX` to an unsigned integer triggers UBSan float-cast-overflow;
    // exercise callback path on those 4 functions with overflow inputs > 65504
    // (while NOEX above exercises <= 65504 safely via intmax_t).
    ExceptionLog log;
    log.action = H5T_CONV_UNHANDLED;
    H5T_conv_ctx_t ctx_ex = MakeConvCtx(entry.src_id, entry.dst_id, &log);
    std::memset(buf.data(), 0, buf.size());
#ifdef H5_HAVE__FLOAT16
    const bool is_unsigned_float16_cb =
        (entry.dst_id == H5T_NATIVE_FLOAT16) &&
        (entry.src_kind == SrcKind::kUshort ||
         entry.src_kind == SrcKind::kUint ||
         entry.src_kind == SrcKind::kUlong ||
         entry.src_kind == SrcKind::kUllong);
#else
    const bool is_unsigned_float16_cb = false;
#endif
    for (size_t i = 0; i < kNelmts; ++i) {
      if (is_unsigned_float16_cb) {
        const uint64_t ovf = 65510ULL + static_cast<uint64_t>(i);
        std::memcpy(buf.data() + 1 + i * stride, &ovf, entry.src_size);
      } else {
        write_sample(entry.src_kind, buf.data() + 1 + i * stride,
                     static_cast<int>(i));
      }
    }
    ASSERT_GE(entry.fn(s_obj, d_obj, &cdata, &ctx_ex, kNelmts, stride, 0,
                       buf.data() + 1, nullptr),
              0);

    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(
        entry.fn(s_obj, d_obj, &cdata, nullptr, 0, 0, 0, nullptr, nullptr), 0);
  }
}

TEST(H5TconvIntegerHardTest, HardErrorPathsAndMacroExceptionCoverage) {
  ASSERT_GE(H5open(), 0);
  H5T_t* schar_obj = LookupTypeObj(H5T_NATIVE_SCHAR);
  H5T_t* uchar_obj = LookupTypeObj(H5T_NATIVE_UCHAR);
  H5T_t* int_obj = LookupTypeObj(H5T_NATIVE_INT);
  H5T_t* f32_obj = LookupTypeObj(H5T_NATIVE_FLOAT);
  ASSERT_TRUE(schar_obj && uchar_obj && int_obj && f32_obj);

  {
    ScopedErrorSilence silence;
    H5T_cdata_t cdata{};
    H5T_conv_ctx_t ctx = MakeConvCtx(H5T_NATIVE_INT, H5T_NATIVE_UCHAR, nullptr);
    std::array<uint8_t, 8> buf{};

    // INIT error paths (null src, null dst, size disagreement)
    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_int_uchar(nullptr, uchar_obj, &cdata, nullptr, 0, 0, 0,
                                  nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_int_uchar(int_obj, nullptr, &cdata, nullptr, 0, 0, 0,
                                  nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_int_uchar(schar_obj, uchar_obj, &cdata, nullptr, 0, 0,
                                  0, nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_int_uchar(int_obj, f32_obj, &cdata, nullptr, 0, 0, 0,
                                  nullptr, nullptr),
              0);

    // CONV error paths (null src, null dst, null conv_ctx)
    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(H5T__conv_int_uchar(nullptr, uchar_obj, &cdata, &ctx, 1, 0, 0,
                                  buf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_int_uchar(int_obj, nullptr, &cdata, &ctx, 1, 0, 0,
                                  buf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_int_uchar(int_obj, uchar_obj, &cdata, nullptr, 1, 0, 0,
                                  buf.data(), nullptr),
              0);

    // Unknown command
    cdata.command = static_cast<H5T_cmd_t>(99);
    EXPECT_LT(H5T__conv_int_uchar(int_obj, uchar_obj, &cdata, &ctx, 1, 0, 0,
                                  buf.data(), nullptr),
              0);
  }

  // Test shadowed long / llong / ulong / ullong ABORT + HANDLED callbacks
  {
    ScopedErrorSilence silence;
    H5T_t* llong_obj = LookupTypeObj(H5T_NATIVE_LLONG);
    H5T_t* long_obj = LookupTypeObj(H5T_NATIVE_LONG);
    ASSERT_TRUE(llong_obj && long_obj);

    ExceptionLog abort_log;
    abort_log.action = H5T_CONV_ABORT;
    H5T_conv_ctx_t ctx =
        MakeConvCtx(H5T_NATIVE_LLONG, H5T_NATIVE_UCHAR, &abort_log);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    ASSERT_GE(H5T__conv_llong_uchar(llong_obj, uchar_obj, &cdata, nullptr, 0, 0,
                                    0, nullptr, nullptr),
              0);
    cdata.command = H5T_CONV_CONV;
    long long neg = -10LL;
    EXPECT_LT(H5T__conv_llong_uchar(llong_obj, uchar_obj, &cdata, &ctx, 1, 0, 0,
                                    &neg, nullptr),
              0);

    ExceptionLog handled_log;
    handled_log.action = H5T_CONV_HANDLED;
    handled_log.fill_value = 99u;
    handled_log.fill_size = 1;
    ctx = MakeConvCtx(H5T_NATIVE_LLONG, H5T_NATIVE_UCHAR, &handled_log);
    std::array<uint8_t, 8> buf{};
    std::memcpy(buf.data(), &neg, sizeof(neg));
    ASSERT_GE(H5T__conv_llong_uchar(llong_obj, uchar_obj, &cdata, &ctx, 1, 0, 0,
                                    buf.data(), nullptr),
              0);
    EXPECT_EQ(buf[0], 99u);
  }
}

}  // namespace
}  // namespace hdf5
