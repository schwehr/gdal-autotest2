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
// Tests floating-point datatype conversion paths: soft float-to-float
// (`H5T__conv_f_f`), soft float-to-integer (`H5T__conv_f_i`), soft
// float-to-complex (`H5T__conv_f_complex`), special-value inspection
// (`H5T__conv_float_find_special`), and all native hardware float conversion
// routines (third_party/hdf5/src/H5Tconv_float.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_float.c

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which clears H5T_DEBUG and omits debug
// statistics fields from `H5T_path_t`. Match that layout when reading internal
// H5T structures.
#ifndef NDEBUG
#define H5TCONV_FLOAT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h suppresses `H5__Float16` under __cplusplus, while H5Tpkg.h
// declares extern variables using `H5__Float16`.
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tconv_float.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TCONV_FLOAT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_FLOAT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// Zero-initializes padding bytes of `long double` elements (e.g. bytes 10..15
// on x86_64) before storing a converted value so MSan never observes
// uninitialized padding when HDF5 inspects raw bytes.
inline void StoreZeroPaddedLongDouble(void* dst, long double val) {
  alignas(long double) std::array<uint8_t, sizeof(long double)> tmp{};
  *reinterpret_cast<long double*>(tmp.data()) = val;
  std::memcpy(dst, tmp.data(), tmp.size());
}

inline long double LoadLongDouble(const void* src) {
  long double out;
  std::memcpy(&out, src, sizeof(out));
  return out;
}

inline void StoreZeroPaddedLdoubleComplex(void* dst, long double re,
                                          long double im) {
  auto* out = static_cast<uint8_t*>(dst);
  StoreZeroPaddedLongDouble(out, re);
  StoreZeroPaddedLongDouble(out + sizeof(long double), im);
}

// IEEE 754 binary16 constants represented as raw uint16_t bit patterns to avoid
// compiler-specific literal suffixes.
constexpr uint16_t kF16PosZero = 0x0000;
constexpr uint16_t kF16NegZero = 0x8000;
constexpr uint16_t kF16SubnormalMin = 0x0001;
constexpr uint16_t kF16One = 0x3C00;
constexpr uint16_t kF16OnePointFive = 0x3E00;
constexpr uint16_t kF16Two = 0x4000;
constexpr uint16_t kF16NegTwo = 0xC000;
constexpr uint16_t kF16FortyTwo = 0x5140;      // 42.0
constexpr uint16_t kF16FortyTwoHalf = 0x5150;  // 42.5
constexpr uint16_t kF16NegFortyTwoHalf = 0xD150;
constexpr uint16_t kF16MaxFinite = 0x7BFF;     // 65504.0
constexpr uint16_t kF16NegMaxFinite = 0xFBFF;  // -65504.0
constexpr uint16_t kF16PosInf = 0x7C00;
constexpr uint16_t kF16NegInf = 0xFC00;
constexpr uint16_t kF16Nan = 0x7E00;

float DecodeF16(uint16_t bits) {
  const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
  const uint32_t exp = (bits >> 10) & 0x1F;
  const uint32_t mant = bits & 0x3FF;
  uint32_t f32_bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      f32_bits = sign;
    } else {
      uint32_t m = mant;
      int shift = 0;
      while ((m & 0x400) == 0) {
        m <<= 1;
        shift++;
      }
      m &= 0x3FF;
      const uint32_t e32 = static_cast<uint32_t>(127 - 15 + 1 - shift);
      f32_bits = sign | (e32 << 23) | (m << 13);
    }
  } else if (exp == 0x1F) {
    f32_bits = sign | 0x7F800000U | (mant << 13);
  } else {
    const uint32_t e32 = exp - 15 + 127;
    f32_bits = sign | (e32 << 23) | (mant << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &f32_bits, sizeof(out));
  return out;
}

template <typename T>
std::array<uint8_t, sizeof(T)> ToBigEndianBytes(T value) {
  std::array<uint8_t, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(T));
  std::reverse(bytes.begin(), bytes.end());
  return bytes;
}

template <typename T>
T FromBigEndianBytes(const uint8_t* ptr) {
  std::array<uint8_t, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), ptr, sizeof(T));
  std::reverse(bytes.begin(), bytes.end());
  T out{};
  std::memcpy(&out, bytes.data(), sizeof(T));
  return out;
}

uint32_t FloatToBits(float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

float BitsToFloat(uint32_t bits) {
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

uint64_t DoubleToBits(double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

struct ConvCbState {
  H5T_conv_ret_t action = H5T_CONV_UNHANDLED;
  int call_count = 0;
  H5T_conv_except_t last_except = H5T_CONV_EXCEPT_RANGE_HI;
  std::array<uint8_t, 64> fill_bytes{};
  size_t fill_size = 0;
};

H5T_conv_ret_t TestConvExceptCb(H5T_conv_except_t except_type, hid_t /*src_id*/,
                                hid_t /*dst_id*/, void* /*src_buf*/,
                                void* dst_buf, void* user_data) {
  auto* state = static_cast<ConvCbState*>(user_data);
  state->call_count++;
  state->last_except = except_type;
  if (state->action == H5T_CONV_HANDLED && state->fill_size > 0 &&
      dst_buf != nullptr) {
    std::memcpy(dst_buf, state->fill_bytes.data(), state->fill_size);
  }
  return state->action;
}

ScopedPlist MakeDxplWithCb(ConvCbState* state) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  if (dxpl.valid()) {
    H5Pset_type_conv_cb(dxpl.get(), TestConvExceptCb, state);
  }
  return dxpl;
}

const H5T_t* AsInternalType(hid_t type_id) {
  return static_cast<const H5T_t*>(H5I_object_verify(type_id, H5I_DATATYPE));
}

H5T_conv_ctx_t MakeDefaultConvCtx(hid_t src_id = H5I_INVALID_HID,
                                  hid_t dst_id = H5I_INVALID_HID,
                                  ConvCbState* state = nullptr) {
  H5T_conv_ctx_t ctx{};
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  ctx.u.conv.src_type_id = src_id;
  ctx.u.conv.dst_type_id = dst_id;
  if (state != nullptr) {
    ctx.u.conv.cb_struct.func = TestConvExceptCb;
    ctx.u.conv.cb_struct.user_data = state;
  }
  return ctx;
}

// Encodes an IEEE binary32 value into HDF5's H5T_VAX_F32 byte order
// (PDP-11 / VAX 16-bit word-swapped F_floating layout, noting exponent bias is
// VAX=129 while IEEE=127, so VAX exponent is ieee_exp + 2 for normal values).
std::array<uint8_t, 4> EncodeVaxF32FromFinite(float val) {
  uint32_t bits = FloatToBits(val);
  if ((bits & 0x7FFFFFFFU) != 0) {
    uint32_t exp = (bits >> 23) & 0xFF;
    exp += 2;  // VAX F_floating bias is 129 (significand in [0.5, 1)).
    bits = (bits & 0x807FFFFFU) | (exp << 23);
  }
  std::array<uint8_t, 4> le{};
  std::memcpy(le.data(), &bits, 4);
  // Swap halves as VAX F32 reverses 16-bit words relative to little endian.
  return {le[2], le[3], le[0], le[1]};
}

float DecodeVaxF32ToHost(const std::array<uint8_t, 4>& vax) {
  std::array<uint8_t, 4> le = {vax[2], vax[3], vax[0], vax[1]};
  uint32_t bits = 0;
  std::memcpy(&bits, le.data(), 4);
  if ((bits & 0x7FFFFFFFU) != 0) {
    uint32_t exp = (bits >> 23) & 0xFF;
    exp -= 2;
    bits = (bits & 0x807FFFFFU) | (exp << 23);
  }
  return BitsToFloat(bits);
}

// -----------------------------------------------------------------------------
// Special-value classifier tests (H5T__conv_float_find_special)
// -----------------------------------------------------------------------------

TEST(H5TconvFloatSpecialTest, ClassifiesFloatAndLongDoubleSpecials) {
  ASSERT_GE(H5open(), 0);
  const H5T_t* f32 = AsInternalType(H5T_IEEE_F32LE);
  ASSERT_NE(f32, nullptr);
  const H5T_atomic_t& f32_atomic = f32->shared->u.atomic;

  uint64_t sign = 99;
  uint32_t bits = 0x00000000U;  // +0.0f
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_POSZERO);
  EXPECT_EQ(sign, 0U);

  bits = 0x80000000U;  // -0.0f
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, nullptr),
            H5T_CONV_FLOAT_SPECVAL_NEGZERO);

  bits = 0x7F800000U;  // +Inf
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_POSINF);
  EXPECT_EQ(sign, 0U);

  bits = 0xFF800000U;  // -Inf
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_NEGINF);
  EXPECT_EQ(sign, 1U);

  bits = 0x7FC00001U;  // NaN
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_NAN);

  bits = 0x3F800000U;  // 1.0f regular
  EXPECT_EQ(H5T__conv_float_find_special(reinterpret_cast<uint8_t*>(&bits),
                                         &f32_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_REGULAR);

  // H5T_NORM_NONE explicit integer bit Inf / -Inf / NaN on x86_64 long double.
  const H5T_t* ld = AsInternalType(H5T_NATIVE_LDOUBLE);
  ASSERT_NE(ld, nullptr);
  const H5T_atomic_t& ld_atomic = ld->shared->u.atomic;

  alignas(16) std::array<uint8_t, sizeof(long double)> ldbuf{};
  StoreZeroPaddedLongDouble(ldbuf.data(),
                            std::numeric_limits<double>::infinity());
  EXPECT_EQ(H5T__conv_float_find_special(ldbuf.data(), &ld_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_POSINF);
  EXPECT_EQ(sign, 0U);

  StoreZeroPaddedLongDouble(ldbuf.data(),
                            -std::numeric_limits<double>::infinity());
  EXPECT_EQ(H5T__conv_float_find_special(ldbuf.data(), &ld_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_NEGINF);
  EXPECT_EQ(sign, 1U);

  StoreZeroPaddedLongDouble(ldbuf.data(),
                            std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(H5T__conv_float_find_special(ldbuf.data(), &ld_atomic, &sign),
            H5T_CONV_FLOAT_SPECVAL_NAN);
}

// -----------------------------------------------------------------------------
// Soft float -> float conversion tests (H5T__conv_f_f / H5T__conv_f_f_loop)
// -----------------------------------------------------------------------------

TEST(H5TconvFloatSoftFFTest, InitConvFreeAndDefensiveErrorBranches) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  const H5T_t* f32le = AsInternalType(H5T_IEEE_F32LE);
  const H5T_t* f64le = AsInternalType(H5T_IEEE_F64LE);
  ASSERT_NE(f32le, nullptr);
  ASSERT_NE(f64le, nullptr);

  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  H5T_conv_ctx_t ctx = MakeDefaultConvCtx(H5T_IEEE_F32LE, H5T_IEEE_F64LE);

  // Null datatype args on INIT and CONV.
  EXPECT_LT(
      H5T__conv_f_f(nullptr, f64le, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_f(f32le, nullptr, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  // Unsupported source and destination byte orders (public API forbids
  // H5T_ORDER_NONE on floats, so construct transient internal copies).
  H5T_shared_t bad_src_sh = *(f32le->shared);
  bad_src_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_src_t = *f32le;
  bad_src_t.shared = &bad_src_sh;
  EXPECT_LT(
      H5T__conv_f_f(&bad_src_t, f64le, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  H5T_shared_t bad_dst_sh = *(f64le->shared);
  bad_dst_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_dst_t = *f64le;
  bad_dst_t.shared = &bad_dst_sh;
  EXPECT_LT(
      H5T__conv_f_f(f32le, &bad_dst_t, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  // Oversized destination (> TEMP_FLOAT_CONV_BUFFER_SIZE = 64).
  ScopedType big_dst(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(big_dst.valid());
  ASSERT_GE(H5Tset_size(big_dst.get(), 65), 0);
  EXPECT_LT(H5T__conv_f_f(f32le, AsInternalType(big_dst.get()), &cdata, &ctx, 1,
                          0, 0, nullptr, nullptr),
            0);

  // Oversized exponent field (> 63 bits).
  ScopedType huge_exp(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(huge_exp.valid());
  ASSERT_GE(H5Tset_size(huge_exp.get(), 16), 0);
  ASSERT_GE(H5Tset_precision(huge_exp.get(), 128), 0);
  ASSERT_GE(H5Tset_fields(huge_exp.get(), 127, 63, 64, 0, 63), 0);
  EXPECT_LT(H5T__conv_f_f(AsInternalType(huge_exp.get()), f64le, &cdata, &ctx,
                          1, 0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_f_f(f32le, AsInternalType(huge_exp.get()), &cdata, &ctx,
                          1, 0, 0, nullptr, nullptr),
            0);

  // Valid INIT + FREE.
  EXPECT_GE(
      H5T__conv_f_f(f32le, f64le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_f_f(f32le, f64le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  // CONV errors: null types, null conv_ctx, unsupported normalization.
  cdata.command = H5T_CONV_CONV;
  float buf_val = 1.0f;
  EXPECT_LT(
      H5T__conv_f_f(nullptr, f64le, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_f(f32le, nullptr, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_f(f32le, f64le, &cdata, nullptr, 1, 0, 0, &buf_val, nullptr),
      0);

  // Unsupported normalization method (H5T_NORM_MSBSET) with expo > 0.
  ScopedType msbset_src(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(msbset_src.valid());
  ASSERT_GE(H5Tset_norm(msbset_src.get(), H5T_NORM_MSBSET), 0);
  double msb_buf[1] = {1.5};
  std::memcpy(msb_buf, &buf_val, sizeof(buf_val));
  EXPECT_LT(H5T__conv_f_f(AsInternalType(msbset_src.get()), f64le, &cdata, &ctx,
                          1, 0, 0, msb_buf, nullptr),
            0);

  // Unknown command.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_f_f(f32le, f64le, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr), 0);

  // Direct H5T__conv_f_f_loop with VAX-ordered complex src and dst -> error.
  ScopedType cplx_vax(H5Tcomplex_create(H5T_VAX_F32));
  ASSERT_TRUE(cplx_vax.valid());
  std::array<uint8_t, 16> cbuf{};
  EXPECT_LT(H5T__conv_f_f_loop(AsInternalType(cplx_vax.get()), f64le, &ctx, 1,
                               0, cbuf.data()),
            0);
  float two_f32 = 2.0f;
  std::memcpy(cbuf.data(), &two_f32, sizeof(two_f32));
  EXPECT_LT(H5T__conv_f_f_loop(f32le, AsInternalType(cplx_vax.get()), &ctx, 1,
                               0, cbuf.data()),
            0);

  // Strided loop path (buf_stride != 0).
  struct StridedElem {
    uint64_t val_bits;
    uint64_t pad;
  };
  std::array<StridedElem, 3> strided{};
  strided[0].val_bits = FloatToBits(2.5f);
  strided[1].val_bits = FloatToBits(-3.25f);
  strided[2].val_bits = FloatToBits(0.0f);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_f_f(f32le, f64le, &cdata, &ctx, 3, sizeof(StridedElem), 0,
                          strided.data(), nullptr),
            0);
  for (const auto& [idx, expected] : std::array<std::pair<size_t, double>, 3>{
           {{0, 2.5}, {1, -3.25}, {2, 0.0}}}) {
    double out = 0.0;
    std::memcpy(&out, &strided[idx].val_bits, sizeof(out));
    EXPECT_DOUBLE_EQ(out, expected);
  }
}

TEST(H5TconvFloatSoftFFTest, OverlapForwardBackwardAndBigEndianAndVax) {
  ASSERT_GE(H5open(), 0);

  // Forward overlap + non-overlap: 5x F64BE (8B) -> F16LE (2B), olap = 1.
  std::vector<uint8_t> buf(5 * sizeof(double), 0);
  const std::array<double, 5> src_vals = {1.5, -2.0, 0.0, -0.0, 42.5};
  for (size_t i = 0; i < 5; ++i) {
    auto be = ToBigEndianBytes(src_vals[i]);
    std::memcpy(buf.data() + i * 8, be.data(), 8);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F64BE, H5T_IEEE_F16LE, 5, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 5; ++i) {
    uint16_t bits = 0;
    std::memcpy(&bits, buf.data() + i * 2, 2);
    EXPECT_FLOAT_EQ(DecodeF16(bits), static_cast<float>(src_vals[i]));
  }
  uint16_t neg_zero_bits = 0;
  std::memcpy(&neg_zero_bits, buf.data() + 3 * 2, 2);
  EXPECT_EQ(neg_zero_bits, kF16NegZero);

  // Backward overlap + non-overlap: 5x F16LE (2B) -> F64BE (8B), olap = 1.
  std::fill(buf.begin(), buf.end(), 0);
  const std::array<uint16_t, 5> f16_vals = {kF16One, kF16NegTwo, kF16FortyTwo,
                                            kF16FortyTwoHalf, kF16OnePointFive};
  for (size_t i = 0; i < 5; ++i) {
    std::memcpy(buf.data() + i * 2, &f16_vals[i], 2);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F16LE, H5T_IEEE_F64BE, 5, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + 0 * 8), 1.0);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + 1 * 8), -2.0);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + 2 * 8), 42.0);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + 3 * 8), 42.5);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + 4 * 8), 1.5);

  // Equal-size custom soft float conversion: BFLOAT16LE -> IEEE_F16BE.
  std::array<uint8_t, 4> bf_buf{};
  uint16_t bf_one = 0x3F80;      // 1.0 in bfloat16
  uint16_t bf_neg_two = 0xC000;  // -2.0 in bfloat16
  std::memcpy(bf_buf.data(), &bf_one, 2);
  std::memcpy(bf_buf.data() + 2, &bf_neg_two, 2);
  ASSERT_GE(H5Tconvert(H5T_FLOAT_BFLOAT16LE, H5T_IEEE_F16BE, 2, bf_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(DecodeF16(FromBigEndianBytes<uint16_t>(bf_buf.data())), 1.0f);
  EXPECT_FLOAT_EQ(DecodeF16(FromBigEndianBytes<uint16_t>(bf_buf.data() + 2)),
                  -2.0f);

  // VAX float source & destination conversions.
  std::array<uint8_t, 16> vax_buf{};
  std::memcpy(vax_buf.data(), EncodeVaxF32FromFinite(3.5f).data(), 4);
  std::memcpy(vax_buf.data() + 4, EncodeVaxF32FromFinite(-6.25f).data(), 4);
  ASSERT_GE(H5Tconvert(H5T_VAX_F32, H5T_IEEE_F64LE, 2, vax_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  double v0 = 0.0, v1 = 0.0;
  std::memcpy(&v0, vax_buf.data(), 8);
  std::memcpy(&v1, vax_buf.data() + 8, 8);
  EXPECT_DOUBLE_EQ(v0, 3.5);
  EXPECT_DOUBLE_EQ(v1, -6.25);

  // F64LE -> VAX_F32 (also exercises H5T_ORDER_VAX destination byte swap).
  // Note: HDF5's H5T__conv_f_f_loop sets `tsize = dst_size / 2` on VAX dst,
  // leaving 4-byte H5T_VAX_F32 bytes in LE byte order with VAX (+2) exp bias.
  ASSERT_GE(H5Tconvert(H5T_IEEE_F64LE, H5T_VAX_F32, 2, vax_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  uint32_t vax32_le_bits = 0;
  std::memcpy(&vax32_le_bits, vax_buf.data(), 4);
  uint32_t ieee32_bits = (vax32_le_bits & 0x807FFFFFU) |
                         ((((vax32_le_bits >> 23) & 0xFF) - 2) << 23);
  EXPECT_FLOAT_EQ(BitsToFloat(ieee32_bits), 3.5f);

  // Also test 8-byte H5T_VAX_F64 dst and src (tsize = 4 on dst swap).
  std::array<uint8_t, 8> vax64_buf{};
  double v64_in = 3.5;
  std::memcpy(vax64_buf.data(), &v64_in, 8);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F64LE, H5T_VAX_F64, 1, vax64_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Tconvert(H5T_VAX_F64, H5T_IEEE_F64LE, 1, vax64_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // Big-endian complex -> unmatched BE float exercises H5T_ORDER_BE complex
  // byte swapping in H5T__conv_f_f_loop.
  std::array<uint8_t, 8> cbe_buf{};
  auto re_be = ToBigEndianBytes(1.5f);
  auto im_be = ToBigEndianBytes(9.0f);
  std::memcpy(cbe_buf.data(), re_be.data(), 4);
  std::memcpy(cbe_buf.data() + 4, im_be.data(), 4);
  ASSERT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_IEEE_F64BE, 1,
                       cbe_buf.data(), nullptr, H5P_DEFAULT),
            0);
  EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(cbe_buf.data()), 1.5);
}

TEST(H5TconvFloatSoftFFTest, SpecialValuesExceptionCallbacksAndNormNone) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // Special values without callback -> F32BE -> long double (H5T_NORM_NONE
  // dst).
  alignas(16) std::vector<uint8_t> buf(4 * sizeof(long double), 0);
  const std::array<uint32_t, 4> spec_f32 = {0x7F800000U, 0xFF800000U,
                                            0x7FC00001U, 0x80000000U};
  for (size_t i = 0; i < 4; ++i) {
    auto be = ToBigEndianBytes(spec_f32[i]);
    std::memcpy(buf.data() + i * 4, be.data(), 4);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_NATIVE_LDOUBLE, 4, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_TRUE(std::isinf(LoadLongDouble(buf.data() + 0 * sizeof(long double))));
  EXPECT_GT(LoadLongDouble(buf.data() + 0 * sizeof(long double)), 0.0L);
  EXPECT_TRUE(std::isinf(LoadLongDouble(buf.data() + 1 * sizeof(long double))));
  EXPECT_LT(LoadLongDouble(buf.data() + 1 * sizeof(long double)), 0.0L);
  EXPECT_TRUE(std::isnan(LoadLongDouble(buf.data() + 2 * sizeof(long double))));
  EXPECT_EQ(LoadLongDouble(buf.data() + 3 * sizeof(long double)), 0.0L);

  // Convert back from H5T_NATIVE_LDOUBLE (BE conversion via soft loop) ->
  // F32BE. First convert long double -> F16BE (no hard path exists for ldbl ->
  // F16BE).
  std::fill(buf.begin(), buf.end(), 0);
  StoreZeroPaddedLongDouble(buf.data() + 0 * sizeof(long double), 1.5);
  StoreZeroPaddedLongDouble(buf.data() + 1 * sizeof(long double), -2.5);
  // Also include a subnormal long double bit pattern (expo=0, msb bit set in
  // m).
  uint8_t ld_subnormal[16] = {};
  ld_subnormal[0] = 0x01;  // bitno == 0 case for NORM_NONE/expo==0
  std::memcpy(buf.data() + 2 * sizeof(long double), ld_subnormal, 16);
  ASSERT_GE(H5Tconvert(H5T_NATIVE_LDOUBLE, H5T_IEEE_F16BE, 3, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(DecodeF16(FromBigEndianBytes<uint16_t>(buf.data())), 1.5f);
  EXPECT_FLOAT_EQ(DecodeF16(FromBigEndianBytes<uint16_t>(buf.data() + 2)),
                  -2.5f);
  EXPECT_EQ(FromBigEndianBytes<uint16_t>(buf.data() + 4), kF16PosZero);

  // Exception callback for PINF / NINF / NAN: UNHANDLED, HANDLED, ABORT.
  for (H5T_conv_except_t kind :
       {H5T_CONV_EXCEPT_PINF, H5T_CONV_EXCEPT_NINF, H5T_CONV_EXCEPT_NAN}) {
    uint32_t f32_bits = kind == H5T_CONV_EXCEPT_PINF   ? 0x7F800000U
                        : kind == H5T_CONV_EXCEPT_NINF ? 0xFF800000U
                                                       : 0x7FC00001U;

    for (H5T_conv_ret_t action :
         {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
      ConvCbState state;
      state.action = action;
      uint16_t sentinel = kF16FortyTwo;
      std::memcpy(state.fill_bytes.data(), &sentinel, 2);
      state.fill_size = 2;
      ScopedPlist dxpl = MakeDxplWithCb(&state);
      ASSERT_TRUE(dxpl.valid());

      auto be = ToBigEndianBytes(f32_bits);
      std::array<uint8_t, 4> cbuf{};
      std::memcpy(cbuf.data(), be.data(), 4);
      herr_t rc = H5Tconvert(H5T_IEEE_F32BE, H5T_IEEE_F16LE, 1, cbuf.data(),
                             nullptr, dxpl.get());
      EXPECT_EQ(state.call_count, 1);
      EXPECT_EQ(state.last_except, kind);
      if (action == H5T_CONV_ABORT) {
        EXPECT_LT(rc, 0);
      } else {
        EXPECT_GE(rc, 0);
        uint16_t out_bits = 0;
        std::memcpy(&out_bits, cbuf.data(), 2);
        if (action == H5T_CONV_HANDLED) {
          EXPECT_EQ(out_bits, kF16FortyTwo);
        }
      }
    }
  }
}

TEST(H5TconvFloatSoftFFTest, SubnormalsRoundingOverflowMrshAndPadding) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // Test F32BE -> F16LE across subnormals, rounding, carry, underflow,
  // overflow: 1) tiny underflow: 2^-30 -> +0 in F16 2) bitno==0 F32 subnormal:
  // 0x00000001 -> +0 in F16 3) F32 subnormal with bitno>0 converted to custom
  // F32 with smaller bias 4) mrsh == dst.msize (10): 2^-24 = 0x33800000 -> F16
  // 0x0001 5) mrsh == dst.msize + 1 (11): 2^-25 = 0x33000000 -> F16 0x0001 6)
  // denormalized rounding (line 492): 2^-24 + 2^-25 = 0x33C00000 ->
  //    carry out of bit 22 ignored for denormalized -> 0x0001;
  //    and 1.25 * 2^-23 = 0x34200000 (mrsh=9) -> rounds bit 22 -> 0x0003
  // 7) 0 < mrsh < dst.msize: 2^-16 * 1.5 = 0x37C00000 -> F16 subnormal 0x0180
  // 8) rounding with carry (implied=2): 1.9999f (0x3FFFFFFF) -> 2.0f (0x4000)
  // 9) rounding cutoff at expo == expo_max - 1 with trailing all-1 vs 0 bit
  const std::vector<uint32_t> inputs = {
      0x30800000U,  // 2^-30 -> underflow to 0
      0x00000001U,  // bitno == 0 F32 subnormal
      0x33800000U,  // 2^-24 -> mrsh == 10
      0x33000000U,  // 2^-25 -> mrsh == 11
      0x33C00000U,  // 2^-24 + 2^-25 -> denormalized rounding (mrsh == 10) -> 1
      0x34200000U,  // 1.25 * 2^-23 -> denormalized rounding (mrsh == 9) -> 3
      0x37C00000U,  // 2^-16 * 1.5 -> 0 < mrsh < msize -> 0x0180
      0x3FFFFFFFU,  // ~2.0 -> rounding carry (implied = 2) -> 0x4000
      0x477FF000U,  // 65520.0 -> rounding skipped at expo_max - 1 (all 1s)
      0x477EF000U,  // expo_max - 1 with false bit & LSB cut bit 1 -> rounds
      0x46400400U,  // normal rounding without carry
  };
  std::vector<uint8_t> conv_buf(inputs.size() * 4);
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto be = ToBigEndianBytes(inputs[i]);
    std::memcpy(conv_buf.data() + i * 4, be.data(), 4);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_IEEE_F16LE, inputs.size(),
                       conv_buf.data(), nullptr, H5P_DEFAULT),
            0);
  auto out16 = [&](size_t idx) {
    uint16_t v = 0;
    std::memcpy(&v, conv_buf.data() + idx * 2, 2);
    return v;
  };
  EXPECT_EQ(out16(0), kF16PosZero);
  EXPECT_EQ(out16(1), kF16PosZero);
  EXPECT_EQ(out16(2), 0x0001);
  EXPECT_EQ(out16(3), 0x0001);
  EXPECT_EQ(out16(4), 0x0001);
  EXPECT_EQ(out16(5), 0x0003);
  EXPECT_EQ(out16(6), 0x0180);
  EXPECT_EQ(out16(7), kF16Two);

  // Convert from F32 subnormal with bitno > 0 (0x00400000 = 2^-127) to a custom
  // IEEE-like float with large exponent bias so bitno > 0 shifts into range.
  ScopedType wide_bias(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(wide_bias.valid());
  std::array<uint8_t, 8> sub_buf{};
  uint32_t f32_sub = 0x00400000U;
  std::memcpy(sub_buf.data(), &f32_sub, 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32LE, wide_bias.get(), 1, sub_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  double sub_out = 0.0;
  std::memcpy(&sub_out, sub_buf.data(), 8);
  EXPECT_NEAR(sub_out, std::ldexp(1.0, -127), 1e-45);

  // Fewer src mantissa bits than dst (mrsh + msize < dst.msize): F8E4M3 ->
  // F16LE.
  std::array<uint8_t, 2> f8_buf{0x3C, 0x00};  // 1.5 in FP8 E4M3
  ASSERT_GE(H5Tconvert(H5T_FLOAT_F8E4M3, H5T_IEEE_F16LE, 1, f8_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  uint16_t f16_from_f8 = 0;
  std::memcpy(&f16_from_f8, f8_buf.data(), 2);
  EXPECT_FLOAT_EQ(DecodeF16(f16_from_f8), 1.5f);

  // mrsh > dst.msize + 1: destination with H5T_NORM_NONE and msize=10 where
  // expo == -dst.msize (-10) gives mrsh = 1 + 11 = 12.
  ScopedType norm_none_16(H5Tcopy(H5T_IEEE_F16LE));
  ASSERT_TRUE(norm_none_16.valid());
  ASSERT_GE(H5Tset_norm(norm_none_16.get(), H5T_NORM_NONE), 0);
  std::array<uint8_t, 4> nn_buf{};
  uint32_t f32_2m25 = 0x33000000U;
  auto nn_be = ToBigEndianBytes(f32_2m25);
  std::memcpy(nn_buf.data(), nn_be.data(), 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, norm_none_16.get(), 1, nn_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  uint16_t nn_out = 0;
  std::memcpy(&nn_out, nn_buf.data(), 2);
  EXPECT_EQ(nn_out, kF16PosZero);

  // Overflow (expo >= expo_max) with callback UNHANDLED, HANDLED, ABORT.
  for (H5T_conv_ret_t action :
       {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
    ConvCbState state;
    state.action = action;
    uint16_t max_sentinel = kF16MaxFinite;
    std::memcpy(state.fill_bytes.data(), &max_sentinel, 2);
    state.fill_size = 2;
    ScopedPlist dxpl = MakeDxplWithCb(&state);
    ASSERT_TRUE(dxpl.valid());

    std::array<uint8_t, 8> ov_buf{};
    auto be = ToBigEndianBytes(1.0e20);
    std::memcpy(ov_buf.data(), be.data(), 8);
    herr_t rc = H5Tconvert(H5T_IEEE_F64BE, H5T_IEEE_F16LE, 1, ov_buf.data(),
                           nullptr, dxpl.get());
    EXPECT_EQ(state.call_count, 1);
    EXPECT_EQ(state.last_except, H5T_CONV_EXCEPT_RANGE_HI);
    if (action == H5T_CONV_ABORT) {
      EXPECT_LT(rc, 0);
    } else {
      EXPECT_GE(rc, 0);
      uint16_t out_bits = 0;
      std::memcpy(&out_bits, ov_buf.data(), 2);
      EXPECT_EQ(out_bits,
                action == H5T_CONV_UNHANDLED ? kF16PosInf : kF16MaxFinite);
    }
  }

  // External padding on destination float (offset > 0, msb_pad,
  // H5T_PAD_ZERO/ONE).
  ScopedType padded_zero(H5Tcopy(H5T_IEEE_F16LE));
  ASSERT_TRUE(padded_zero.valid());
  ASSERT_GE(H5Tset_size(padded_zero.get(), 4), 0);
  ASSERT_GE(H5Tset_offset(padded_zero.get(), 4), 0);
  ASSERT_GE(H5Tset_pad(padded_zero.get(), H5T_PAD_ZERO, H5T_PAD_ZERO), 0);
  ASSERT_GE(H5Tset_fields(padded_zero.get(), 19, 14, 5, 4, 10), 0);
  std::array<uint8_t, 4> pad_buf{};
  float one = 1.0f;
  std::memcpy(pad_buf.data(), &one, 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32LE, padded_zero.get(), 1, pad_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  uint32_t zbits = 0;
  std::memcpy(&zbits, pad_buf.data(), 4);
  EXPECT_EQ(zbits & 0xFU, 0U);
  EXPECT_EQ(zbits >> 20, 0U);

  ScopedType padded_one(H5Tcopy(padded_zero.get()));
  ASSERT_TRUE(padded_one.valid());
  ASSERT_GE(H5Tset_pad(padded_one.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
  std::memcpy(pad_buf.data(), &one, 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32LE, padded_one.get(), 1, pad_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  uint32_t obits = 0;
  std::memcpy(&obits, pad_buf.data(), 4);
  EXPECT_EQ(obits & 0xFU, 0xFU);
  EXPECT_EQ(obits >> 20, 0xFFFU);
}

// -----------------------------------------------------------------------------
// Soft float -> integer conversion tests (H5T__conv_f_i / H5T__conv_f_i_loop)
// -----------------------------------------------------------------------------

TEST(H5TconvFloatSoftFITest, InitConvFreeAndDefensiveErrorBranches) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  const H5T_t* f32le = AsInternalType(H5T_IEEE_F32LE);
  const H5T_t* i32le = AsInternalType(H5T_STD_I32LE);
  ASSERT_NE(f32le, nullptr);
  ASSERT_NE(i32le, nullptr);

  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  H5T_conv_ctx_t ctx = MakeDefaultConvCtx(H5T_IEEE_F32LE, H5T_STD_I32LE);

  // Null datatypes.
  EXPECT_LT(
      H5T__conv_f_i(nullptr, i32le, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_i(f32le, nullptr, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  // Bad byte orders (transient internal copies with H5T_ORDER_NONE).
  H5T_shared_t bad_src_sh = *(f32le->shared);
  bad_src_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_src_t = *f32le;
  bad_src_t.shared = &bad_src_sh;
  EXPECT_LT(
      H5T__conv_f_i(&bad_src_t, i32le, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  H5T_shared_t bad_dst_sh = *(i32le->shared);
  bad_dst_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_dst_t = *i32le;
  bad_dst_t.shared = &bad_dst_sh;
  EXPECT_LT(
      H5T__conv_f_i(f32le, &bad_dst_t, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);

  // Dst size > 64 bytes.
  ScopedType big_int(H5Tcopy(H5T_STD_I64LE));
  ASSERT_TRUE(big_int.valid());
  ASSERT_GE(H5Tset_size(big_int.get(), 65), 0);
  EXPECT_LT(H5T__conv_f_i(f32le, AsInternalType(big_int.get()), &cdata, &ctx, 1,
                          0, 0, nullptr, nullptr),
            0);

  // Src esize > 63.
  ScopedType huge_exp(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(huge_exp.valid());
  ASSERT_GE(H5Tset_size(huge_exp.get(), 16), 0);
  ASSERT_GE(H5Tset_precision(huge_exp.get(), 128), 0);
  ASSERT_GE(H5Tset_fields(huge_exp.get(), 127, 63, 64, 0, 63), 0);
  EXPECT_LT(H5T__conv_f_i(AsInternalType(huge_exp.get()), i32le, &cdata, &ctx,
                          1, 0, 0, nullptr, nullptr),
            0);

  // Valid INIT + FREE.
  EXPECT_GE(
      H5T__conv_f_i(f32le, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_f_i(f32le, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  // CONV errors.
  cdata.command = H5T_CONV_CONV;
  float buf_val = 4.0f;
  EXPECT_LT(
      H5T__conv_f_i(nullptr, i32le, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_i(f32le, nullptr, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_f_i(f32le, i32le, &cdata, nullptr, 1, 0, 0, &buf_val, nullptr),
      0);

  // Unsupported norm -> error propagated from H5T__conv_f_i_loop.
  ScopedType msbset_src(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(msbset_src.valid());
  ASSERT_GE(H5Tset_norm(msbset_src.get(), H5T_NORM_MSBSET), 0);
  EXPECT_LT(H5T__conv_f_i(AsInternalType(msbset_src.get()), i32le, &cdata, &ctx,
                          1, 0, 0, &buf_val, nullptr),
            0);

  // Unknown command.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_f_i(f32le, i32le, &cdata, &ctx, 1, 0, 0, &buf_val, nullptr), 0);

  // Complex-of-VAX source rejected inside H5T__conv_f_i_loop.
  ScopedType cplx_vax(H5Tcomplex_create(H5T_VAX_F32));
  ASSERT_TRUE(cplx_vax.valid());
  std::array<uint8_t, 8> cbuf{};
  EXPECT_LT(H5T__conv_f_i_loop(AsInternalType(cplx_vax.get()), i32le, &ctx, 1,
                               0, cbuf.data()),
            0);

  // Strided call (buf_stride != 0).
  struct StridedFI {
    uint64_t data;
    uint64_t pad;
  };
  std::array<StridedFI, 2> sbuf{};
  sbuf[0].data = FloatToBits(7.0f);
  sbuf[1].data = FloatToBits(-9.0f);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_f_i(f32le, i32le, &cdata, &ctx, 2, sizeof(StridedFI), 0,
                          sbuf.data(), nullptr),
            0);
  int32_t out0 = 0, out1 = 0;
  std::memcpy(&out0, &sbuf[0].data, 4);
  std::memcpy(&out1, &sbuf[1].data, 4);
  EXPECT_EQ(out0, 7);
  EXPECT_EQ(out1, -9);
}

TEST(H5TconvFloatSoftFITest, PublicConversionsOverlapOrderPaddingAndSpecials) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // 1) Forward non-equal size (src > dst): 5x F64BE -> I16BE (olap = 1).
  std::vector<uint8_t> buf(5 * 8, 0);
  const std::array<double, 5> src_d = {12.75, -19.25, 0.25, -0.0, 50000.0};
  for (size_t i = 0; i < 5; ++i) {
    auto be = ToBigEndianBytes(src_d[i]);
    std::memcpy(buf.data() + i * 8, be.data(), 8);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F64BE, H5T_STD_I16BE, 5, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(FromBigEndianBytes<int16_t>(buf.data() + 0 * 2), 12);
  EXPECT_EQ(FromBigEndianBytes<int16_t>(buf.data() + 1 * 2), -19);
  EXPECT_EQ(FromBigEndianBytes<int16_t>(buf.data() + 2 * 2), 0);  // new_msb=-2
  EXPECT_EQ(FromBigEndianBytes<int16_t>(buf.data() + 3 * 2), 0);
  EXPECT_EQ(FromBigEndianBytes<int16_t>(buf.data() + 4 * 2), 32767);

  // 2) Backward non-equal size (src < dst, dst.prec/8 > src_base_size):
  // 5x F16BE (2B) -> U64BE (8B).
  std::fill(buf.begin(), buf.end(), 0);
  const std::array<uint16_t, 5> f16s = {kF16FortyTwo, kF16OnePointFive, 0x3800,
                                        kF16PosInf, kF16Nan};  // 0x3800 = 0.5
  for (size_t i = 0; i < 5; ++i) {
    auto be = ToBigEndianBytes(f16s[i]);
    std::memcpy(buf.data() + i * 2, be.data(), 2);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F16BE, H5T_STD_U64BE, 5, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(FromBigEndianBytes<uint64_t>(buf.data() + 0 * 8), 42ULL);
  EXPECT_EQ(FromBigEndianBytes<uint64_t>(buf.data() + 1 * 8), 1ULL);
  EXPECT_EQ(FromBigEndianBytes<uint64_t>(buf.data() + 2 * 8),
            0ULL);  // 0.5 -> new_msb_pos == -1
  EXPECT_EQ(FromBigEndianBytes<uint64_t>(buf.data() + 3 * 8),
            std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(FromBigEndianBytes<uint64_t>(buf.data() + 4 * 8), 0ULL);

  // 3) VAX F32 -> STD_I32BE and H5T_COMPLEX_IEEE_F32BE -> STD_I32LE.
  std::array<uint8_t, 8> vax = {EncodeVaxF32FromFinite(17.5f)[0],
                                EncodeVaxF32FromFinite(17.5f)[1],
                                EncodeVaxF32FromFinite(17.5f)[2],
                                EncodeVaxF32FromFinite(17.5f)[3],
                                0,
                                0,
                                0,
                                0};
  ASSERT_GE(H5Tconvert(H5T_VAX_F32, H5T_STD_I32BE, 1, vax.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(FromBigEndianBytes<int32_t>(vax.data()), 17);

  std::array<uint8_t, 8> cbe{};
  auto rbe = ToBigEndianBytes(23.75f);
  auto ibe = ToBigEndianBytes(99.0f);
  std::memcpy(cbe.data(), rbe.data(), 4);
  std::memcpy(cbe.data() + 4, ibe.data(), 4);
  ASSERT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_STD_I32LE, 1, cbe.data(),
                       nullptr, H5P_DEFAULT),
            0);
  int32_t c_out = 0;
  std::memcpy(&c_out, cbe.data(), 4);
  EXPECT_EQ(c_out, 23);

  // 4) H5T_NORM_NONE pseudo-zero float (expo > 0, mantissa == 0 -> msb_pos <
  // 0).
  alignas(16) std::array<uint8_t, 16> pzero{};
  pzero[8] = 0x01;
  pzero[9] = 0x40;  // biased exp = 0x4001 (finite), mantissa == 0
  ASSERT_GE(H5Tconvert(H5T_NATIVE_LDOUBLE, H5T_STD_I32BE, 1, pzero.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_EQ(FromBigEndianBytes<int32_t>(pzero.data()), 0);

  // 5) Special values to signed BE int without callback (+Inf, -Inf, NaN).
  std::vector<uint8_t> sp_buf(3 * 4);
  const std::array<uint32_t, 3> sp_in = {0x7F800000U, 0xFF800000U, 0x7FC00001U};
  for (size_t i = 0; i < 3; ++i) {
    auto be = ToBigEndianBytes(sp_in[i]);
    std::memcpy(sp_buf.data() + i * 4, be.data(), 4);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_STD_I32BE, 3, sp_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(FromBigEndianBytes<int32_t>(sp_buf.data() + 0),
            std::numeric_limits<int32_t>::max());
  EXPECT_EQ(FromBigEndianBytes<int32_t>(sp_buf.data() + 4),
            std::numeric_limits<int32_t>::min());
  EXPECT_EQ(FromBigEndianBytes<int32_t>(sp_buf.data() + 8), 0);

  // 6) Destination padding (offset > 0, lsb_pad / msb_pad H5T_PAD_ONE).
  ScopedType pad_int(H5Tcopy(H5T_STD_U16LE));
  ASSERT_TRUE(pad_int.valid());
  ASSERT_GE(H5Tset_size(pad_int.get(), 4), 0);
  ASSERT_GE(H5Tset_precision(pad_int.get(), 16), 0);
  ASSERT_GE(H5Tset_offset(pad_int.get(), 4), 0);
  ASSERT_GE(H5Tset_pad(pad_int.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
  std::array<uint8_t, 4> pbuf{};
  float fval = 5.0f;
  std::memcpy(pbuf.data(), &fval, 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32LE, pad_int.get(), 1, pbuf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  uint32_t raw_pi = 0;
  std::memcpy(&raw_pi, pbuf.data(), 4);
  EXPECT_EQ(raw_pi & 0xFU, 0xFU);
  EXPECT_EQ((raw_pi >> 4) & 0xFFFFU, 5U);
  EXPECT_EQ(raw_pi >> 20, 0xFFFU);
}

TEST(H5TconvFloatSoftFITest, AllExceptionCallbacksUnsignedAndSigned) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  struct Case {
    double val;
    hid_t dst_type;
    H5T_conv_except_t expected_except;
  };
  const std::vector<Case> cases = {
      {std::numeric_limits<double>::infinity(), H5T_STD_I32BE,
       H5T_CONV_EXCEPT_PINF},
      {-std::numeric_limits<double>::infinity(), H5T_STD_I32BE,
       H5T_CONV_EXCEPT_NINF},
      {std::numeric_limits<double>::quiet_NaN(), H5T_STD_I32BE,
       H5T_CONV_EXCEPT_NAN},
      {-5.0, H5T_STD_U32BE, H5T_CONV_EXCEPT_RANGE_LOW},
      {1.0e20, H5T_STD_U32BE, H5T_CONV_EXCEPT_RANGE_HI},
      {42.5, H5T_STD_U32BE, H5T_CONV_EXCEPT_TRUNCATE},
      {-42.5, H5T_STD_I32BE, H5T_CONV_EXCEPT_TRUNCATE},
      {-1.0e20, H5T_STD_I32BE, H5T_CONV_EXCEPT_RANGE_LOW},
      {1.0e20, H5T_STD_I32BE, H5T_CONV_EXCEPT_RANGE_HI},
      {42.5, H5T_STD_I32BE, H5T_CONV_EXCEPT_TRUNCATE},
  };

  for (const Case& c : cases) {
    for (H5T_conv_ret_t action :
         {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
      ConvCbState state;
      state.action = action;
      uint32_t sentinel = 0x11223344U;
      std::memcpy(state.fill_bytes.data(), &sentinel, 4);
      state.fill_size = 4;
      ScopedPlist dxpl = MakeDxplWithCb(&state);
      ASSERT_TRUE(dxpl.valid());

      std::array<uint8_t, 8> buf{};
      auto be = ToBigEndianBytes(c.val);
      std::memcpy(buf.data(), be.data(), 8);
      herr_t rc = H5Tconvert(H5T_IEEE_F64BE, c.dst_type, 1, buf.data(), nullptr,
                             dxpl.get());
      EXPECT_EQ(state.call_count, 1);
      EXPECT_EQ(state.last_except, c.expected_except);
      if (action == H5T_CONV_ABORT) {
        EXPECT_LT(rc, 0);
      } else {
        EXPECT_GE(rc, 0);
      }
    }
  }

  // Exact integers with callback installed (truncated == false requires
  // (size_t)expo >= src_atomic.u.f.msize; use F16BE with msize=10 and +/-2048).
  {
    ConvCbState state;
    ScopedPlist dxpl = MakeDxplWithCb(&state);
    ASSERT_TRUE(dxpl.valid());
    std::array<uint8_t, 4> buf{};
    constexpr uint16_t kF16Pos2048 = 0x6800;  // expo=11 >= msize=10
    constexpr uint16_t kF16Neg2048 = 0xE800;
    auto be = ToBigEndianBytes(kF16Pos2048);
    std::memcpy(buf.data(), be.data(), 2);
    ASSERT_GE(H5Tconvert(H5T_IEEE_F16BE, H5T_STD_U32BE, 1, buf.data(), nullptr,
                         dxpl.get()),
              0);
    EXPECT_EQ(state.call_count, 0);
    EXPECT_EQ(FromBigEndianBytes<uint32_t>(buf.data()), 2048U);

    be = ToBigEndianBytes(kF16Neg2048);
    std::memcpy(buf.data(), be.data(), 2);
    ASSERT_GE(H5Tconvert(H5T_IEEE_F16BE, H5T_STD_I32BE, 1, buf.data(), nullptr,
                         dxpl.get()),
              0);
    EXPECT_EQ(state.call_count, 0);
    EXPECT_EQ(FromBigEndianBytes<int32_t>(buf.data()), -2048);
  }
}

// -----------------------------------------------------------------------------
// Soft float -> complex tests (H5T__conv_f_complex)
// -----------------------------------------------------------------------------

TEST(H5TconvFloatSoftFComplexTest, MatchedUnmatchedAndErrorBranches) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // Unmatched BE float -> complex BE (uses H5T__conv_f_f_loop).
  std::vector<uint8_t> buf(4 * 16, 0);
  for (size_t i = 0; i < 4; ++i) {
    auto be = ToBigEndianBytes(static_cast<float>(i) + 1.5f);
    std::memcpy(buf.data() + i * 4, be.data(), 4);
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F64BE, 4, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + i * 16),
                     static_cast<double>(i) + 1.5);
    EXPECT_DOUBLE_EQ(FromBigEndianBytes<double>(buf.data() + i * 16 + 8), 0.0);
  }

  // Matched soft float -> complex (F32BE -> COMPLEX_IEEE_F32BE).
  std::fill(buf.begin(), buf.end(), 0);
  auto fbe = ToBigEndianBytes(7.25f);
  std::memcpy(buf.data(), fbe.data(), 4);
  ASSERT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F32BE, 1, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(FromBigEndianBytes<float>(buf.data()), 7.25f);
  EXPECT_FLOAT_EQ(FromBigEndianBytes<float>(buf.data() + 4), 0.0f);

  // VAX float -> COMPLEX_IEEE_F64LE.
  std::fill(buf.begin(), buf.end(), 0);
  std::memcpy(buf.data(), EncodeVaxF32FromFinite(3.25f).data(), 4);
  ASSERT_GE(H5Tconvert(H5T_VAX_F32, H5T_COMPLEX_IEEE_F64LE, 1, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  double re_out = 0.0, im_out = 0.0;
  std::memcpy(&re_out, buf.data(), 8);
  std::memcpy(&im_out, buf.data() + 8, 8);
  EXPECT_DOUBLE_EQ(re_out, 3.25);
  EXPECT_DOUBLE_EQ(im_out, 0.0);

  // Direct H5T__conv_f_complex defensive checks.
  const H5T_t* f32le = AsInternalType(H5T_IEEE_F32LE);
  const H5T_t* cf64le = AsInternalType(H5T_COMPLEX_IEEE_F64LE);
  ASSERT_NE(f32le, nullptr);
  ASSERT_NE(cf64le, nullptr);

  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  H5T_conv_ctx_t ctx =
      MakeDefaultConvCtx(H5T_IEEE_F32LE, H5T_COMPLEX_IEEE_F64LE);

  EXPECT_LT(H5T__conv_f_complex(nullptr, cf64le, &cdata, &ctx, 1, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_f_complex(f32le, nullptr, &cdata, &ctx, 1, 0, 0, nullptr,
                                nullptr),
            0);

  // Unsupported source byte order (transient internal copy with ORDER_NONE).
  H5T_shared_t bad_src_sh = *(f32le->shared);
  bad_src_sh.u.atomic.order = H5T_ORDER_NONE;
  H5T_t bad_src_t = *f32le;
  bad_src_t.shared = &bad_src_sh;
  EXPECT_LT(H5T__conv_f_complex(&bad_src_t, cf64le, &cdata, &ctx, 1, 0, 0,
                                nullptr, nullptr),
            0);

  ScopedType cplx_vax(H5Tcomplex_create(H5T_VAX_F32));
  ASSERT_TRUE(cplx_vax.valid());
  EXPECT_LT(H5T__conv_f_complex(f32le, AsInternalType(cplx_vax.get()), &cdata,
                                &ctx, 1, 0, 0, nullptr, nullptr),
            0);

  // Complex destination > 64 bytes.
  ScopedType wide_f(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(wide_f.valid());
  ASSERT_GE(H5Tset_size(wide_f.get(), 33), 0);
  ScopedType big_cplx(H5Tcomplex_create(wide_f.get()));
  ASSERT_TRUE(big_cplx.valid());
  EXPECT_LT(H5T__conv_f_complex(f32le, AsInternalType(big_cplx.get()), &cdata,
                                &ctx, 1, 0, 0, nullptr, nullptr),
            0);

  // Complex destination exponent > 63 bits.
  ScopedType huge_exp_f(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(huge_exp_f.valid());
  ASSERT_GE(H5Tset_size(huge_exp_f.get(), 16), 0);
  ASSERT_GE(H5Tset_precision(huge_exp_f.get(), 128), 0);
  ASSERT_GE(H5Tset_fields(huge_exp_f.get(), 127, 63, 64, 0, 63), 0);
  ScopedType huge_exp_cplx(H5Tcomplex_create(huge_exp_f.get()));
  ASSERT_TRUE(huge_exp_cplx.valid());
  EXPECT_LT(H5T__conv_f_complex(f32le, AsInternalType(huge_exp_cplx.get()),
                                &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_f_complex(AsInternalType(huge_exp_f.get()), cf64le,
                                &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
            0);

  // Synthetic non-atomic parent check.
  H5T_t fake_cplx = *cf64le;
  H5T_shared_t fake_cplx_sh = *(cf64le->shared);
  H5T_t fake_parent = *(cf64le->shared->parent);
  H5T_shared_t fake_parent_sh = *(cf64le->shared->parent->shared);
  fake_parent_sh.type = H5T_OPAQUE;
  fake_parent.shared = &fake_parent_sh;
  fake_cplx_sh.parent = &fake_parent;
  fake_cplx.shared = &fake_cplx_sh;
  EXPECT_LT(H5T__conv_f_complex(f32le, &fake_cplx, &cdata, &ctx, 1, 0, 0,
                                nullptr, nullptr),
            0);

  // Valid INIT + FREE.
  EXPECT_GE(H5T__conv_f_complex(f32le, cf64le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_f_complex(f32le, cf64le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);

  // CONV errors.
  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_f_complex(nullptr, cf64le, &cdata, &ctx, 1, 0, 0,
                                buf.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_f_complex(f32le, nullptr, &cdata, &ctx, 1, 0, 0,
                                buf.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_f_complex(f32le, cf64le, &cdata, nullptr, 1, 0, 0,
                                buf.data(), nullptr),
            0);

  // Bad src norm in unmatched CONV path -> error, plus matched CONV path
  // returning failure via H5T_CONV_ABORT callback.
  ConvCbState abort_state;
  abort_state.action = H5T_CONV_ABORT;
  ScopedPlist abort_dxpl = MakeDxplWithCb(&abort_state);
  ASSERT_TRUE(abort_dxpl.valid());
  std::array<uint8_t, 8> abort_buf{};
  auto inf_be = ToBigEndianBytes(0x7F800000U);
  std::memcpy(abort_buf.data(), inf_be.data(), 4);
  EXPECT_LT(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F32BE, 1,
                       abort_buf.data(), nullptr, abort_dxpl.get()),
            0);
  EXPECT_EQ(abort_state.call_count, 1);

  ScopedType msbset_f(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(msbset_f.valid());
  ASSERT_GE(H5Tset_norm(msbset_f.get(), H5T_NORM_MSBSET), 0);
  std::array<uint8_t, 16> err_buf{};
  float one = 1.0f;
  std::memcpy(err_buf.data(), &one, 4);
  EXPECT_LT(H5T__conv_f_complex(AsInternalType(msbset_f.get()), cf64le, &cdata,
                                &ctx, 1, 0, 0, err_buf.data(), nullptr),
            0);

  // Unknown command.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_f_complex(f32le, cf64le, &cdata, &ctx, 1, 0, 0,
                                err_buf.data(), nullptr),
            0);
}

// -----------------------------------------------------------------------------
// Hard (hardware) native float conversions (lines 1445-2685)
// -----------------------------------------------------------------------------

template <typename DstInt>
void VerifyFloat16ToInt(hid_t dst_id, bool bounds_checked_or_signed,
                        int64_t expected_pos, int64_t expected_neg_or_zero) {
  alignas(16) std::array<uint8_t, 64> buf{};
  // Unchecked H5T_CONV_fX -> unsigned paths (ushort, uint, ulong, ullong) do a
  // raw C cast without clamping negatives, so avoid negative floats there.
  const std::array<uint16_t, 4> in =
      bounds_checked_or_signed
          ? std::array<uint16_t, 4>{kF16FortyTwoHalf, kF16NegTwo, kF16MaxFinite,
                                    kF16NegMaxFinite}
          : std::array<uint16_t, 4>{kF16FortyTwoHalf, kF16PosZero,
                                    kF16OnePointFive, kF16MaxFinite};
  std::memcpy(buf.data(), in.data(), in.size() * 2);
  ASSERT_GE(H5Tconvert(H5T_NATIVE_FLOAT16, dst_id, 4, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  DstInt d0 = 0, d1 = 0;
  std::memcpy(&d0, buf.data() + 0 * sizeof(DstInt), sizeof(DstInt));
  std::memcpy(&d1, buf.data() + 1 * sizeof(DstInt), sizeof(DstInt));
  EXPECT_EQ(d0, static_cast<DstInt>(expected_pos));
  EXPECT_EQ(d1, static_cast<DstInt>(expected_neg_or_zero));
}

template <typename SrcFloat, typename DstInt>
void VerifyNativeFloatToInt(hid_t src_id, hid_t dst_id, SrcFloat big_pos,
                            SrcFloat big_neg, bool dst_is_signed,
                            bool long_double_src = false) {
  // 4 elements: 42.5, -2.5, overflow, underflow (clamped safely without
  // float-cast UB).
  alignas(16) std::vector<uint8_t> buf(
      4 * std::max(sizeof(SrcFloat), sizeof(DstInt)), 0);
  const std::array<double, 4> vals = {42.5, -2.5, static_cast<double>(big_pos),
                                      static_cast<double>(big_neg)};
  for (size_t i = 0; i < 4; ++i) {
    if (long_double_src) {
      StoreZeroPaddedLongDouble(buf.data() + i * sizeof(SrcFloat), vals[i]);
    } else {
      SrcFloat v = static_cast<SrcFloat>(vals[i]);
      std::memcpy(buf.data() + i * sizeof(SrcFloat), &v, sizeof(SrcFloat));
    }
  }
  ASSERT_GE(H5Tconvert(src_id, dst_id, 4, buf.data(), nullptr, H5P_DEFAULT), 0);
  DstInt d0 = 0, d1 = 0, d2 = 0, d3 = 0;
  std::memcpy(&d0, buf.data() + 0 * sizeof(DstInt), sizeof(DstInt));
  std::memcpy(&d1, buf.data() + 1 * sizeof(DstInt), sizeof(DstInt));
  std::memcpy(&d2, buf.data() + 2 * sizeof(DstInt), sizeof(DstInt));
  std::memcpy(&d3, buf.data() + 3 * sizeof(DstInt), sizeof(DstInt));
  EXPECT_EQ(d0, static_cast<DstInt>(42));
  EXPECT_EQ(d1,
            dst_is_signed ? static_cast<DstInt>(-2) : static_cast<DstInt>(0));
  EXPECT_EQ(d2, std::numeric_limits<DstInt>::max());
  EXPECT_EQ(d3, std::numeric_limits<DstInt>::min());
}

TEST(H5TconvFloatHardTest, AllHardConversionsViaPublicApi) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  // _Float16 -> integers (schar/uchar/short use bounds-checked H5T_CONV_Fx;
  // larger integer targets use H5T_CONV_fX raw casts).
  VerifyFloat16ToInt<signed char>(H5T_NATIVE_SCHAR, true, 42, -2);
  VerifyFloat16ToInt<unsigned char>(H5T_NATIVE_UCHAR, true, 42, 0);
  VerifyFloat16ToInt<short>(H5T_NATIVE_SHORT, true, 42, -2);
  VerifyFloat16ToInt<unsigned short>(H5T_NATIVE_USHORT, false, 42, 0);
  VerifyFloat16ToInt<int>(H5T_NATIVE_INT, true, 42, -2);
  VerifyFloat16ToInt<unsigned int>(H5T_NATIVE_UINT, false, 42, 0);
  VerifyFloat16ToInt<long>(H5T_NATIVE_LONG, true, 42, -2);
  VerifyFloat16ToInt<unsigned long>(H5T_NATIVE_ULONG, false, 42, 0);
  VerifyFloat16ToInt<long long>(H5T_NATIVE_LLONG, true, 42, -2);
  VerifyFloat16ToInt<unsigned long long>(H5T_NATIVE_ULLONG, false, 42, 0);

  // _Float16 -> floats & complex.
  for (hid_t dst_id :
       {H5T_NATIVE_FLOAT, H5T_NATIVE_DOUBLE, H5T_NATIVE_LDOUBLE}) {
    alignas(16) std::array<uint8_t, 64> buf{};
    const std::array<uint16_t, 3> in = {kF16OnePointFive, kF16NegTwo,
                                        kF16FortyTwo};
    std::memcpy(buf.data(), in.data(), 6);
    ASSERT_GE(H5Tconvert(H5T_NATIVE_FLOAT16, dst_id, 3, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
    if (dst_id == H5T_NATIVE_FLOAT) {
      float f0 = 0.0f;
      std::memcpy(&f0, buf.data(), 4);
      EXPECT_FLOAT_EQ(f0, 1.5f);
    } else if (dst_id == H5T_NATIVE_DOUBLE) {
      double d0 = 0.0;
      std::memcpy(&d0, buf.data(), 8);
      EXPECT_DOUBLE_EQ(d0, 1.5);
    } else {
      EXPECT_EQ(LoadLongDouble(buf.data()), 1.5L);
    }
  }
  for (hid_t dst_id : {H5T_NATIVE_FLOAT_COMPLEX, H5T_NATIVE_DOUBLE_COMPLEX,
                       H5T_NATIVE_LDOUBLE_COMPLEX}) {
    alignas(16) std::array<uint8_t, 128> buf{};
    const std::array<uint16_t, 2> in = {kF16OnePointFive, kF16NegTwo};
    std::memcpy(buf.data(), in.data(), 4);
    ASSERT_GE(H5Tconvert(H5T_NATIVE_FLOAT16, dst_id, 2, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }

  // float -> integers.
  VerifyNativeFloatToInt<float, signed char>(H5T_NATIVE_FLOAT, H5T_NATIVE_SCHAR,
                                             1e10f, -1e10f, true);
  VerifyNativeFloatToInt<float, unsigned char>(
      H5T_NATIVE_FLOAT, H5T_NATIVE_UCHAR, 1e10f, -10.0f, false);
  VerifyNativeFloatToInt<float, short>(H5T_NATIVE_FLOAT, H5T_NATIVE_SHORT,
                                       1e10f, -1e10f, true);
  VerifyNativeFloatToInt<float, unsigned short>(
      H5T_NATIVE_FLOAT, H5T_NATIVE_USHORT, 1e10f, -10.0f, false);
  VerifyNativeFloatToInt<float, int>(H5T_NATIVE_FLOAT, H5T_NATIVE_INT, 1e20f,
                                     -1e20f, true);
  VerifyNativeFloatToInt<float, unsigned int>(H5T_NATIVE_FLOAT, H5T_NATIVE_UINT,
                                              1e20f, -10.0f, false);
  VerifyNativeFloatToInt<float, long>(H5T_NATIVE_FLOAT, H5T_NATIVE_LONG, 1e25f,
                                      -1e25f, true);
  VerifyNativeFloatToInt<float, unsigned long>(
      H5T_NATIVE_FLOAT, H5T_NATIVE_ULONG, 1e25f, -10.0f, false);
  VerifyNativeFloatToInt<float, long long>(H5T_NATIVE_FLOAT, H5T_NATIVE_LLONG,
                                           1e25f, -1e25f, true);
  VerifyNativeFloatToInt<float, unsigned long long>(
      H5T_NATIVE_FLOAT, H5T_NATIVE_ULLONG, 1e25f, -10.0f, false);

  // float -> float16, double, ldouble, complexes.
  {
    std::array<uint8_t, 64> buf{};
    std::array<float, 3> in = {1.5f, 1e10f, -1e10f};
    std::memcpy(buf.data(), in.data(), 12);
    ASSERT_GE(H5Tconvert(H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT16, 3, buf.data(),
                         nullptr, H5P_DEFAULT),
              0);
    uint16_t h0 = 0, h1 = 0, h2 = 0;
    std::memcpy(&h0, buf.data() + 0, 2);
    std::memcpy(&h1, buf.data() + 2, 2);
    std::memcpy(&h2, buf.data() + 4, 2);
    EXPECT_FLOAT_EQ(DecodeF16(h0), 1.5f);
    EXPECT_EQ(h1, kF16PosInf);
    EXPECT_EQ(h2, kF16NegInf);
  }
  for (hid_t dst_id :
       {H5T_NATIVE_DOUBLE, H5T_NATIVE_LDOUBLE, H5T_NATIVE_FLOAT_COMPLEX,
        H5T_NATIVE_DOUBLE_COMPLEX, H5T_NATIVE_LDOUBLE_COMPLEX}) {
    alignas(16) std::array<uint8_t, 128> buf{};
    std::array<float, 3> in = {1.5f, -3.25f, 0.0f};
    std::memcpy(buf.data(), in.data(), 12);
    ASSERT_GE(H5Tconvert(H5T_NATIVE_FLOAT, dst_id, 3, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }

  // double -> integers.
  VerifyNativeFloatToInt<double, signed char>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_SCHAR, 1e10, -1e10, true);
  VerifyNativeFloatToInt<double, unsigned char>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_UCHAR, 1e10, -10.0, false);
  VerifyNativeFloatToInt<double, short>(H5T_NATIVE_DOUBLE, H5T_NATIVE_SHORT,
                                        1e10, -1e10, true);
  VerifyNativeFloatToInt<double, unsigned short>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_USHORT, 1e10, -10.0, false);
  VerifyNativeFloatToInt<double, int>(H5T_NATIVE_DOUBLE, H5T_NATIVE_INT, 1e20,
                                      -1e20, true);
  VerifyNativeFloatToInt<double, unsigned int>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_UINT, 1e20, -10.0, false);
  VerifyNativeFloatToInt<double, long>(H5T_NATIVE_DOUBLE, H5T_NATIVE_LONG, 1e25,
                                       -1e25, true);
  VerifyNativeFloatToInt<double, unsigned long>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_ULONG, 1e25, -10.0, false);
  VerifyNativeFloatToInt<double, long long>(H5T_NATIVE_DOUBLE, H5T_NATIVE_LLONG,
                                            1e25, -1e25, true);
  VerifyNativeFloatToInt<double, unsigned long long>(
      H5T_NATIVE_DOUBLE, H5T_NATIVE_ULLONG, 1e25, -10.0, false);

  // double -> float16, float, ldouble, complexes (with overflow/underflow).
  for (hid_t dst_id : {H5T_NATIVE_FLOAT16, H5T_NATIVE_FLOAT, H5T_NATIVE_LDOUBLE,
                       H5T_NATIVE_FLOAT_COMPLEX, H5T_NATIVE_DOUBLE_COMPLEX,
                       H5T_NATIVE_LDOUBLE_COMPLEX}) {
    alignas(16) std::array<uint8_t, 160> buf{};
    std::array<double, 4> in = {1.5, -2.25, 1.0e100, -1.0e100};
    std::memcpy(buf.data(), in.data(), 32);
    ASSERT_GE(H5Tconvert(H5T_NATIVE_DOUBLE, dst_id, 4, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }

  // long double -> integers.
  VerifyNativeFloatToInt<long double, signed char>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_SCHAR, 1e10L, -1e10L, true, true);
  VerifyNativeFloatToInt<long double, unsigned char>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_UCHAR, 1e10L, -10.0L, false, true);
  VerifyNativeFloatToInt<long double, short>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_SHORT, 1e10L, -1e10L, true, true);
  VerifyNativeFloatToInt<long double, unsigned short>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_USHORT, 1e10L, -10.0L, false, true);
  VerifyNativeFloatToInt<long double, int>(H5T_NATIVE_LDOUBLE, H5T_NATIVE_INT,
                                           1e20L, -1e20L, true, true);
  VerifyNativeFloatToInt<long double, unsigned int>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_UINT, 1e20L, -10.0L, false, true);
  VerifyNativeFloatToInt<long double, long>(H5T_NATIVE_LDOUBLE, H5T_NATIVE_LONG,
                                            1e25L, -1e25L, true, true);
  VerifyNativeFloatToInt<long double, unsigned long>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_ULONG, 1e25L, -10.0L, false, true);
  VerifyNativeFloatToInt<long double, long long>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_LLONG, 1e25L, -1e25L, true, true);
  VerifyNativeFloatToInt<long double, unsigned long long>(
      H5T_NATIVE_LDOUBLE, H5T_NATIVE_ULLONG, 1e25L, -10.0L, false, true);

  // long double -> float16, float, double, complexes.
  for (hid_t dst_id : {H5T_NATIVE_FLOAT16, H5T_NATIVE_FLOAT, H5T_NATIVE_DOUBLE,
                       H5T_NATIVE_FLOAT_COMPLEX, H5T_NATIVE_DOUBLE_COMPLEX,
                       H5T_NATIVE_LDOUBLE_COMPLEX}) {
    alignas(16) std::array<uint8_t, 160> buf{};
    const std::array<double, 4> in = {1.5, -2.25, 1.0e100, -1.0e100};
    for (size_t i = 0; i < 4; ++i) {
      StoreZeroPaddedLongDouble(buf.data() + i * sizeof(long double), in[i]);
    }
    ASSERT_GE(H5Tconvert(H5T_NATIVE_LDOUBLE, dst_id, 4, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }
}

TEST(H5TconvFloatHardTest, DirectDispatchAndExceptionCallbacksOnAllHardFuncs) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  struct HardEntry {
    H5T_lib_conv_t fn;
    hid_t src_id;
    hid_t dst_id;
    bool test_exceptions;
    bool src_is_ldouble;
    bool src_is_f16;
  };
  const std::vector<HardEntry> entries = {
      {H5T__conv__Float16_schar, H5T_NATIVE_FLOAT16, H5T_NATIVE_SCHAR, true,
       false, true},
      {H5T__conv__Float16_uchar, H5T_NATIVE_FLOAT16, H5T_NATIVE_UCHAR, true,
       false, true},
      {H5T__conv__Float16_short, H5T_NATIVE_FLOAT16, H5T_NATIVE_SHORT, true,
       false, true},
      {H5T__conv__Float16_ushort, H5T_NATIVE_FLOAT16, H5T_NATIVE_USHORT, false,
       false, true},
      {H5T__conv__Float16_int, H5T_NATIVE_FLOAT16, H5T_NATIVE_INT, false, false,
       true},
      {H5T__conv__Float16_uint, H5T_NATIVE_FLOAT16, H5T_NATIVE_UINT, false,
       false, true},
      {H5T__conv__Float16_long, H5T_NATIVE_FLOAT16, H5T_NATIVE_LONG, false,
       false, true},
      {H5T__conv__Float16_ulong, H5T_NATIVE_FLOAT16, H5T_NATIVE_ULONG, false,
       false, true},
      {H5T__conv__Float16_llong, H5T_NATIVE_FLOAT16, H5T_NATIVE_LLONG, false,
       false, true},
      {H5T__conv__Float16_ullong, H5T_NATIVE_FLOAT16, H5T_NATIVE_ULLONG, false,
       false, true},
      {H5T__conv__Float16_float, H5T_NATIVE_FLOAT16, H5T_NATIVE_FLOAT, false,
       false, true},
      {H5T__conv__Float16_double, H5T_NATIVE_FLOAT16, H5T_NATIVE_DOUBLE, false,
       false, true},
      {H5T__conv__Float16_ldouble, H5T_NATIVE_FLOAT16, H5T_NATIVE_LDOUBLE,
       false, false, true},
      {H5T__conv__Float16_fcomplex, H5T_NATIVE_FLOAT16,
       H5T_NATIVE_FLOAT_COMPLEX, false, false, true},
      {H5T__conv__Float16_dcomplex, H5T_NATIVE_FLOAT16,
       H5T_NATIVE_DOUBLE_COMPLEX, false, false, true},
      {H5T__conv__Float16_lcomplex, H5T_NATIVE_FLOAT16,
       H5T_NATIVE_LDOUBLE_COMPLEX, false, false, true},

      {H5T__conv_float_schar, H5T_NATIVE_FLOAT, H5T_NATIVE_SCHAR, true, false,
       false},
      {H5T__conv_float_uchar, H5T_NATIVE_FLOAT, H5T_NATIVE_UCHAR, true, false,
       false},
      {H5T__conv_float_short, H5T_NATIVE_FLOAT, H5T_NATIVE_SHORT, true, false,
       false},
      {H5T__conv_float_ushort, H5T_NATIVE_FLOAT, H5T_NATIVE_USHORT, true, false,
       false},
      {H5T__conv_float_int, H5T_NATIVE_FLOAT, H5T_NATIVE_INT, true, false,
       false},
      {H5T__conv_float_uint, H5T_NATIVE_FLOAT, H5T_NATIVE_UINT, true, false,
       false},
      {H5T__conv_float_long, H5T_NATIVE_FLOAT, H5T_NATIVE_LONG, true, false,
       false},
      {H5T__conv_float_ulong, H5T_NATIVE_FLOAT, H5T_NATIVE_ULONG, true, false,
       false},
      {H5T__conv_float_llong, H5T_NATIVE_FLOAT, H5T_NATIVE_LLONG, true, false,
       false},
      {H5T__conv_float_ullong, H5T_NATIVE_FLOAT, H5T_NATIVE_ULLONG, true, false,
       false},
      {H5T__conv_float__Float16, H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT16, true,
       false, false},
      {H5T__conv_float_double, H5T_NATIVE_FLOAT, H5T_NATIVE_DOUBLE, false,
       false, false},
      {H5T__conv_float_ldouble, H5T_NATIVE_FLOAT, H5T_NATIVE_LDOUBLE, false,
       false, false},
      {H5T__conv_float_fcomplex, H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT_COMPLEX,
       false, false, false},
      {H5T__conv_float_dcomplex, H5T_NATIVE_FLOAT, H5T_NATIVE_DOUBLE_COMPLEX,
       false, false, false},
      {H5T__conv_float_lcomplex, H5T_NATIVE_FLOAT, H5T_NATIVE_LDOUBLE_COMPLEX,
       false, false, false},

      {H5T__conv_double_schar, H5T_NATIVE_DOUBLE, H5T_NATIVE_SCHAR, true, false,
       false},
      {H5T__conv_double_uchar, H5T_NATIVE_DOUBLE, H5T_NATIVE_UCHAR, true, false,
       false},
      {H5T__conv_double_short, H5T_NATIVE_DOUBLE, H5T_NATIVE_SHORT, true, false,
       false},
      {H5T__conv_double_ushort, H5T_NATIVE_DOUBLE, H5T_NATIVE_USHORT, true,
       false, false},
      {H5T__conv_double_int, H5T_NATIVE_DOUBLE, H5T_NATIVE_INT, true, false,
       false},
      {H5T__conv_double_uint, H5T_NATIVE_DOUBLE, H5T_NATIVE_UINT, true, false,
       false},
      {H5T__conv_double_long, H5T_NATIVE_DOUBLE, H5T_NATIVE_LONG, true, false,
       false},
      {H5T__conv_double_ulong, H5T_NATIVE_DOUBLE, H5T_NATIVE_ULONG, true, false,
       false},
      {H5T__conv_double_llong, H5T_NATIVE_DOUBLE, H5T_NATIVE_LLONG, true, false,
       false},
      {H5T__conv_double_ullong, H5T_NATIVE_DOUBLE, H5T_NATIVE_ULLONG, true,
       false, false},
      {H5T__conv_double__Float16, H5T_NATIVE_DOUBLE, H5T_NATIVE_FLOAT16, true,
       false, false},
      {H5T__conv_double_float, H5T_NATIVE_DOUBLE, H5T_NATIVE_FLOAT, true, false,
       false},
      {H5T__conv_double_ldouble, H5T_NATIVE_DOUBLE, H5T_NATIVE_LDOUBLE, false,
       false, false},
      {H5T__conv_double_fcomplex, H5T_NATIVE_DOUBLE, H5T_NATIVE_FLOAT_COMPLEX,
       true, false, false},
      {H5T__conv_double_dcomplex, H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE_COMPLEX,
       false, false, false},
      {H5T__conv_double_lcomplex, H5T_NATIVE_DOUBLE, H5T_NATIVE_LDOUBLE_COMPLEX,
       false, false, false},

      {H5T__conv_ldouble_schar, H5T_NATIVE_LDOUBLE, H5T_NATIVE_SCHAR, true,
       true, false},
      {H5T__conv_ldouble_uchar, H5T_NATIVE_LDOUBLE, H5T_NATIVE_UCHAR, true,
       true, false},
      {H5T__conv_ldouble_short, H5T_NATIVE_LDOUBLE, H5T_NATIVE_SHORT, true,
       true, false},
      {H5T__conv_ldouble_ushort, H5T_NATIVE_LDOUBLE, H5T_NATIVE_USHORT, true,
       true, false},
      {H5T__conv_ldouble_int, H5T_NATIVE_LDOUBLE, H5T_NATIVE_INT, true, true,
       false},
      {H5T__conv_ldouble_uint, H5T_NATIVE_LDOUBLE, H5T_NATIVE_UINT, true, true,
       false},
      {H5T__conv_ldouble_long, H5T_NATIVE_LDOUBLE, H5T_NATIVE_LONG, true, true,
       false},
      {H5T__conv_ldouble_ulong, H5T_NATIVE_LDOUBLE, H5T_NATIVE_ULONG, true,
       true, false},
      {H5T__conv_ldouble_llong, H5T_NATIVE_LDOUBLE, H5T_NATIVE_LLONG, true,
       true, false},
      {H5T__conv_ldouble_ullong, H5T_NATIVE_LDOUBLE, H5T_NATIVE_ULLONG, true,
       true, false},
      {H5T__conv_ldouble__Float16, H5T_NATIVE_LDOUBLE, H5T_NATIVE_FLOAT16, true,
       true, false},
      {H5T__conv_ldouble_float, H5T_NATIVE_LDOUBLE, H5T_NATIVE_FLOAT, true,
       true, false},
      {H5T__conv_ldouble_double, H5T_NATIVE_LDOUBLE, H5T_NATIVE_DOUBLE, true,
       true, false},
      {H5T__conv_ldouble_fcomplex, H5T_NATIVE_LDOUBLE, H5T_NATIVE_FLOAT_COMPLEX,
       true, true, false},
      {H5T__conv_ldouble_dcomplex, H5T_NATIVE_LDOUBLE,
       H5T_NATIVE_DOUBLE_COMPLEX, true, true, false},
      {H5T__conv_ldouble_lcomplex, H5T_NATIVE_LDOUBLE,
       H5T_NATIVE_LDOUBLE_COMPLEX, false, true, false},
  };

  for (const auto& entry : entries) {
    const H5T_t* st = AsInternalType(entry.src_id);
    const H5T_t* dt = AsInternalType(entry.dst_id);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(dt, nullptr);

    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    H5T_conv_ctx_t ctx = MakeDefaultConvCtx(entry.src_id, entry.dst_id);
    ASSERT_GE(entry.fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

    // Run CONV with an unaligned pointer (buf + 1) to exercise alignment
    // branches inside H5T_CONV macros.
    alignas(16) std::array<uint8_t, 256> raw{};
    uint8_t* unaligned = raw.data() + 1;
    auto write_val = [&](uint8_t* dst_buf, size_t idx, long double v) {
      if (entry.src_is_f16) {
        uint16_t h = (v > 100.0L)   ? kF16MaxFinite
                     : (v < -10.0L) ? kF16NegMaxFinite
                     : (v > 0.0L)   ? kF16FortyTwoHalf
                                    : kF16NegTwo;
        std::memcpy(dst_buf + idx * 2, &h, 2);
      } else if (entry.src_is_ldouble) {
        StoreZeroPaddedLongDouble(dst_buf + idx * sizeof(long double), v);
      } else if (entry.src_id == H5T_NATIVE_FLOAT) {
        float fv = static_cast<float>(v);
        std::memcpy(dst_buf + idx * 4, &fv, 4);
      } else {
        double dv = static_cast<double>(v);
        std::memcpy(dst_buf + idx * 8, &dv, 8);
      }
    };
    write_val(unaligned, 0, 4.0L);
    write_val(unaligned, 1, 7.0L);
    cdata.command = H5T_CONV_CONV;
    EXPECT_GE(entry.fn(st, dt, &cdata, &ctx, 2, 0, 0, unaligned, nullptr), 0);

    // With exception callback (UNHANDLED on overflow, underflow, truncation).
    if (entry.test_exceptions) {
      ConvCbState cb_state;
      H5T_conv_ctx_t cb_ctx =
          MakeDefaultConvCtx(entry.src_id, entry.dst_id, &cb_state);
      raw.fill(0);
      long double hi_val = entry.src_is_ldouble                  ? 1.0e400L
                           : (entry.src_id == H5T_NATIVE_DOUBLE) ? 1.0e100L
                                                                 : 1.0e38L;
      write_val(raw.data(), 0, hi_val);
      write_val(raw.data(), 1, -hi_val);
      write_val(raw.data(), 2, 42.5L);
      EXPECT_GE(entry.fn(st, dt, &cdata, &cb_ctx, 3, 0, 0, raw.data(), nullptr),
                0);
      EXPECT_GT(cb_state.call_count, 0);
    }

    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(entry.fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  }
}

}  // namespace
}  // namespace hdf5
