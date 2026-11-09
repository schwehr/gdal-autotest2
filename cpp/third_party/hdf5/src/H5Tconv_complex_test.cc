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
// Tests soft and hard complex number datatype conversions and error handling
// (third_party/hdf5/src/H5Tconv_complex.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_complex.c

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, so internal structs must be parsed under
// NDEBUG to match binary layout. Define H5__Float16 for C++20 before H5Tpkg.h.
#ifndef NDEBUG
#define H5T_CONV_COMPLEX_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#ifdef H5_HAVE__FLOAT16
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv_complex.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5T_CONV_COMPLEX_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5T_CONV_COMPLEX_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

const H5T_t* LookupType(hid_t id) {
  return static_cast<const H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_t* LookupMutableType(hid_t id) {
  return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_conv_ctx_t MakeConvCtx(hid_t src_id = H5I_INVALID_HID,
                           hid_t dst_id = H5I_INVALID_HID,
                           H5T_conv_except_func_t cb = nullptr,
                           void* user_data = nullptr) {
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  ctx.u.conv.src_type_id = src_id;
  ctx.u.conv.dst_type_id = dst_id;
  ctx.u.conv.cb_struct.func = cb;
  ctx.u.conv.cb_struct.user_data = user_data;
  return ctx;
}

template <typename T>
void WriteVal(uint8_t* dst, T val) {
  T zeroed;
  std::memset(&zeroed, 0, sizeof(zeroed));
  zeroed = val;
  std::memcpy(dst, &zeroed, sizeof(zeroed));
}

template <typename T>
void WriteComplex(uint8_t* dst, T re, T im) {
  WriteVal<T>(dst, re);
  WriteVal<T>(dst + sizeof(T), im);
}

template <typename T>
T ReadVal(const uint8_t* src) {
  T val;
  std::memset(&val, 0, sizeof(val));
  std::memcpy(&val, src, sizeof(val));
  return val;
}

struct ExceptLog {
  int count = 0;
  H5T_conv_except_t last_type = H5T_CONV_EXCEPT_RANGE_HI;
  H5T_conv_ret_t action = H5T_CONV_UNHANDLED;
  uint8_t fill_byte = 0x5A;
  size_t fill_bytes = 0;
};

H5T_conv_ret_t TestExceptCb(H5T_conv_except_t except_type, hid_t /*src_id*/,
                            hid_t /*dst_id*/, void* /*src_buf*/, void* dst_buf,
                            void* user_data) {
  auto* log = static_cast<ExceptLog*>(user_data);
  ++log->count;
  log->last_type = except_type;
  if (log->action == H5T_CONV_HANDLED && dst_buf != nullptr &&
      log->fill_bytes > 0) {
    std::memset(dst_buf, log->fill_byte, log->fill_bytes);
  }
  return log->action;
}

ScopedType MakeCustomFloat(H5T_order_t order, size_t size, size_t offset,
                           size_t prec, size_t spos, size_t epos, size_t esize,
                           size_t mpos, size_t msize, size_t ebias,
                           H5T_norm_t norm, H5T_pad_t lsb_pad,
                           H5T_pad_t msb_pad) {
  ScopedType f(H5Tcopy(H5T_IEEE_F32LE));
  if (!f.valid()) return ScopedType();
  if (H5Tset_order(f.get(), order) < 0) return ScopedType();
  if (H5Tset_size(f.get(), size) < 0) return ScopedType();
  if (H5Tset_precision(f.get(), prec) < 0) return ScopedType();
  if (H5Tset_offset(f.get(), offset) < 0) return ScopedType();
  if (H5Tset_fields(f.get(), spos, epos, esize, mpos, msize) < 0) {
    return ScopedType();
  }
  if (H5Tset_ebias(f.get(), ebias) < 0) return ScopedType();
  if (H5Tset_norm(f.get(), norm) < 0) return ScopedType();
  if (H5Tset_pad(f.get(), lsb_pad, msb_pad) < 0) return ScopedType();
  return f;
}

TEST(H5TconvComplexTest, SoftComplexAcrossIeeeFormatsAndNarrowWiden) {
  // F32BE -> F16LE across 5 elements (narrowing: olap < nelmts).
  constexpr size_t kN = 5;
  std::vector<uint8_t> buf(kN * 8, 0);
  auto write_be_f32_complex = [&](size_t idx, float re, float im) {
    uint8_t le[8];
    WriteComplex<float>(le, re, im);
    for (int i = 0; i < 4; ++i) {
      buf[idx * 8 + i] = le[3 - i];
      buf[idx * 8 + 4 + i] = le[7 - i];
    }
  };
  write_be_f32_complex(0, 1.5f, -2.25f);
  write_be_f32_complex(1, +0.0f, -0.0f);
  write_be_f32_complex(2, +0.0f, 3.0f);
  write_be_f32_complex(3, 4.0f, -0.0f);
  write_be_f32_complex(4, 1e-20f, 1.25f);

  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_COMPLEX_IEEE_F16LE, kN,
                       buf.data(), nullptr, H5P_DEFAULT),
            0);

  // Widen back from F16LE -> F64BE across 5 elements (direction = -1).
  buf.resize(kN * 16, 0);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F16LE, H5T_COMPLEX_IEEE_F64BE, kN,
                       buf.data(), nullptr, H5P_DEFAULT),
            0);

  // Narrow F64BE -> F32LE and verify element 0 and element 4 values.
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F64BE, H5T_COMPLEX_IEEE_F32LE, kN,
                       buf.data(), nullptr, H5P_DEFAULT),
            0);
  EXPECT_NEAR(ReadVal<float>(buf.data()), 1.5f, 1e-2f);
  EXPECT_NEAR(ReadVal<float>(buf.data() + 4), -2.25f, 1e-2f);
  EXPECT_FLOAT_EQ(ReadVal<float>(buf.data() + 8), 0.0f);
  EXPECT_FLOAT_EQ(ReadVal<float>(buf.data() + 12), 0.0f);
  EXPECT_FLOAT_EQ(ReadVal<float>(buf.data() + 16), 0.0f);
  EXPECT_NEAR(ReadVal<float>(buf.data() + 20), 3.0f, 1e-2f);
  EXPECT_NEAR(ReadVal<float>(buf.data() + 24), 4.0f, 1e-2f);
  EXPECT_FLOAT_EQ(ReadVal<float>(buf.data() + 32), 0.0f);
  EXPECT_NEAR(ReadVal<float>(buf.data() + 36), 1.25f, 1e-2f);
}

TEST(H5TconvComplexTest, SoftComplexSpecialValuesAndCallbacks) {
  const float pos_inf = std::numeric_limits<float>::infinity();
  const float neg_inf = -std::numeric_limits<float>::infinity();
  const float qnan = std::numeric_limits<float>::quiet_NaN();

  // Destination with H5T_NORM_NONE (long double complex) to exercise
  // H5T_NORM_NONE infinity bit setting for both real and imaginary parts.
  ScopedType ldouble_cplx(H5Tcomplex_create(H5T_NATIVE_LDOUBLE));
  ASSERT_TRUE(ldouble_cplx.valid());

  struct Case {
    float re;
    float im;
  };
  const std::vector<Case> cases = {
      {pos_inf, 1.0f}, {neg_inf, 1.0f}, {1.0f, pos_inf},
      {1.0f, neg_inf}, {qnan, 1.0f},    {1.0f, qnan},
  };

  for (const auto& c : cases) {
    std::vector<uint8_t> buf(32, 0);
    WriteComplex<float>(buf.data(), c.re, c.im);
    H5T_conv_ctx_t soft_ctx = MakeConvCtx(H5T_COMPLEX_IEEE_F32LE, ldouble_cplx.get());
    H5T_cdata_t soft_cd{};
    soft_cd.command = H5T_CONV_CONV;
    EXPECT_GE(H5T__conv_complex(LookupType(H5T_COMPLEX_IEEE_F32LE),
                                LookupType(ldouble_cplx.get()), &soft_cd,
                                &soft_ctx, 1, 0, 0, buf.data(), nullptr),
              0);
    long double out_re = ReadVal<long double>(buf.data());
    long double out_im = ReadVal<long double>(buf.data() + sizeof(long double));
    if (std::isinf(c.re)) EXPECT_TRUE(std::isinf(out_re));
    if (std::isinf(c.im)) EXPECT_TRUE(std::isinf(out_im));
    if (std::isnan(c.re)) EXPECT_TRUE(std::isnan(out_re));
    if (std::isnan(c.im)) EXPECT_TRUE(std::isnan(out_im));
  }

  // Test callbacks returning UNHANDLED, HANDLED, and ABORT across special
  // values (Inf, -Inf, NaN).
  for (H5T_conv_ret_t action :
       {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
    ExceptLog log;
    log.action = action;
    log.fill_bytes = 16;
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestExceptCb, &log), 0);

    for (const auto& c : cases) {
      std::vector<uint8_t> buf(16, 0);
      WriteComplex<float>(buf.data(), c.re, c.im);
      if (action == H5T_CONV_ABORT) {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Tconvert(H5T_COMPLEX_IEEE_F32LE, H5T_COMPLEX_IEEE_F64BE, 1,
                             buf.data(), nullptr, dxpl.get()),
                  0);
      } else {
        EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32LE, H5T_COMPLEX_IEEE_F64BE, 1,
                             buf.data(), nullptr, dxpl.get()),
                  0);
      }
    }
    EXPECT_EQ(log.count, static_cast<int>(cases.size()));
  }
}

TEST(H5TconvComplexTest, SoftComplexSubnormalsRoundingOverflowAndPadding) {
  // 1. Source denormals (mant_msb > 0 and mant_msb == 0) -> F64LE.
  std::vector<uint8_t> buf(3 * 16, 0);
  uint32_t subnorm_hi = 0x00400000U;  // bit 22 set -> mant_msb > 0
  uint32_t subnorm_lo = 0x00000001U;  // bit 0 set -> mant_msb == 0
  uint32_t norm_one = 0x3F800000U;    // 1.0f
  WriteVal<uint32_t>(buf.data(), subnorm_hi);
  WriteVal<uint32_t>(buf.data() + 4, subnorm_lo);
  WriteVal<uint32_t>(buf.data() + 8, subnorm_lo);
  WriteVal<uint32_t>(buf.data() + 12, norm_one);
  WriteComplex<float>(buf.data() + 16, std::ldexp(1.5f, -20),
                      std::ldexp(1.25f, -5));
  H5T_conv_ctx_t soft_ctx1 =
      MakeConvCtx(H5T_COMPLEX_IEEE_F32LE, H5T_COMPLEX_IEEE_F64LE);
  H5T_cdata_t soft_cd{};
  soft_cd.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_complex(LookupType(H5T_COMPLEX_IEEE_F32LE),
                              LookupType(H5T_COMPLEX_IEEE_F64LE), &soft_cd,
                              &soft_ctx1, 3, 0, 0, buf.data(), nullptr),
            0);
  EXPECT_GT(ReadVal<double>(buf.data()), 0.0);
  EXPECT_GT(ReadVal<double>(buf.data() + 8), 0.0);

  // 2. Destination underflow to zero, subnormal mrsh bands, rounding, carry,
  // and expo_max - 1 clamping from F64LE -> F32LE via soft H5T__conv_complex.
  std::vector<uint8_t> nar(16 * 8, 0);
  WriteComplex<double>(nar.data() + 0 * 16, 1e-300, 1.0);  // zero underflow
  WriteComplex<double>(nar.data() + 1 * 16, std::ldexp(1.5, -150),
                       std::ldexp(1.5, -149));  // mrsh == d.msize+1 & == d.msize
  WriteComplex<double>(nar.data() + 2 * 16, std::ldexp(1.5, -135),
                       std::ldexp(1.0 + std::ldexp(1.0, -24), 0));  // round up
  // Carry mantissa (1.111...1 + half ulp) -> exponent increment.
  WriteComplex<double>(nar.data() + 3 * 16, std::ldexp(2.0 - std::ldexp(1.0, -25), 0),
                       1.5 * std::ldexp(1.0, -149));  // denormalized rounding
  // Near FLT_MAX (expo == expo_max - 1, all mantissa 1s + cut bit) -> no Inf.
  WriteComplex<double>(nar.data() + 4 * 16,
                       std::ldexp(2.0 - std::ldexp(1.0, -23), 127) +
                           std::ldexp(1.0, 127 - 24),
                       2.0);
  // Overflow in real part then imag part without cb.
  WriteComplex<double>(nar.data() + 5 * 16, 1e100, 2.0);
  WriteComplex<double>(nar.data() + 6 * 16, 2.0, -1e100);
  // Finite element 7.
  WriteComplex<double>(nar.data() + 7 * 16, 3.0, 4.0);

  H5T_conv_ctx_t soft_ctx2 =
      MakeConvCtx(H5T_COMPLEX_IEEE_F64LE, H5T_COMPLEX_IEEE_F32LE);
  EXPECT_GE(H5T__conv_complex(LookupType(H5T_COMPLEX_IEEE_F64LE),
                              LookupType(H5T_COMPLEX_IEEE_F32LE), &soft_cd,
                              &soft_ctx2, 8, 0, 0, nar.data(), nullptr),
            0);
  EXPECT_FLOAT_EQ(ReadVal<float>(nar.data() + 0 * 8), 0.0f);
  EXPECT_GT(ReadVal<float>(nar.data() + 1 * 8 + 4), 0.0f);
  EXPECT_FLOAT_EQ(ReadVal<float>(nar.data() + 3 * 8), 2.0f);
  EXPECT_TRUE(std::isfinite(ReadVal<float>(nar.data() + 4 * 8)));
  EXPECT_TRUE(std::isinf(ReadVal<float>(nar.data() + 5 * 8)));
  EXPECT_TRUE(std::isinf(ReadVal<float>(nar.data() + 6 * 8 + 4)));

  // 3. Overflow callbacks on real and imaginary parts (UNHANDLED / HANDLED /
  // ABORT).
  for (H5T_conv_ret_t action :
       {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
    ExceptLog log;
    log.action = action;
    log.fill_bytes = 8;
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestExceptCb, &log), 0);

    // Allocate one trailing zeroed element because upstream H5T__conv_complex_part
    // passes the full complex `src_p` to H5T__reverse_order when handling
    // imaginary-part callbacks (`s + src_part_size`).
    std::vector<uint8_t> ovf(3 * 16, 0);
    WriteComplex<double>(ovf.data(), 1e100, 1.0);
    WriteComplex<double>(ovf.data() + 16, 1.0, 1e100);
    if (action == H5T_CONV_ABORT) {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tconvert(H5T_COMPLEX_IEEE_F64LE, H5T_COMPLEX_IEEE_F32BE, 1,
                           ovf.data(), nullptr, dxpl.get()),
                0);
      EXPECT_LT(H5Tconvert(H5T_COMPLEX_IEEE_F64LE, H5T_COMPLEX_IEEE_F32BE, 1,
                           ovf.data() + 16, nullptr, dxpl.get()),
                0);
    } else {
      EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F64LE, H5T_COMPLEX_IEEE_F32BE, 2,
                           ovf.data(), nullptr, dxpl.get()),
                0);
    }
    EXPECT_EQ(log.last_type, H5T_CONV_EXCEPT_RANGE_HI);
  }

  // 4. Custom H5T_NORM_NONE complex destination with offset=4, prec=32, size=5,
  // lsb_pad=ONE, msb_pad=ONE -> covers mrsh > dst.msize+1 and both paddings.
  ScopedType pad_f(MakeCustomFloat(H5T_ORDER_LE, 5, 4, 32, 35, 27, 8, 4, 23,
                                   127, H5T_NORM_NONE, H5T_PAD_ONE,
                                   H5T_PAD_ONE));
  ASSERT_TRUE(pad_f.valid());
  ScopedType pad_cplx(H5Tcomplex_create(pad_f.get()));
  ASSERT_TRUE(pad_cplx.valid());

  std::vector<uint8_t> pbuf(3 * 16, 0);
  WriteComplex<double>(pbuf.data(), std::ldexp(1.5, -150), 2.5);
  WriteComplex<double>(pbuf.data() + 16, +0.0, -0.0);
  WriteComplex<double>(pbuf.data() + 32, 1.5, -2.5);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F64LE, pad_cplx.get(), 3, pbuf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_EQ(pbuf[0] & 0x0F, 0x0F);  // lsb_pad = ONE
  EXPECT_EQ(pbuf[4] & 0xF0, 0xF0);  // msb_pad = ONE
}

TEST(H5TconvComplexTest, SoftComplexDirectCallsStridesAndErrorPaths) {
  H5T_cdata_t cdata{};
  const H5T_t* f32_le = LookupType(H5T_COMPLEX_IEEE_F32LE);
  const H5T_t* f64_be = LookupType(H5T_COMPLEX_IEEE_F64BE);
  ASSERT_NE(f32_le, nullptr);
  ASSERT_NE(f64_be, nullptr);
  H5T_conv_ctx_t ctx =
      MakeConvCtx(H5T_COMPLEX_IEEE_F32LE, H5T_COMPLEX_IEEE_F64BE);

  // Direct INIT + CONV with non-zero buf_stride + FREE.
  cdata.command = H5T_CONV_INIT;
  EXPECT_GE(H5T__conv_complex(f32_le, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                              nullptr),
            0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);

  std::vector<uint8_t> sbuf(2 * 24, 0);
  WriteComplex<float>(sbuf.data(), 1.5f, -2.5f);
  WriteComplex<float>(sbuf.data() + 24, 3.5f, 4.5f);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_complex(f32_le, f64_be, &cdata, &ctx, 2, 24, 0,
                              sbuf.data(), nullptr),
            0);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_complex(f32_le, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                              nullptr),
            0);

  // Null args, bad ctx, unknown command.
  {
    ScopedErrorSilence silence;
    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_complex(nullptr, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(H5T__conv_complex(nullptr, f64_be, &cdata, &ctx, 1, 0, 0,
                                sbuf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, nullptr, &cdata, &ctx, 1, 0, 0,
                                sbuf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, f64_be, &cdata, nullptr, 1, 0, 0,
                                sbuf.data(), nullptr),
              0);
    cdata.command = static_cast<H5T_cmd_t>(99);
    EXPECT_LT(H5T__conv_complex(f32_le, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
  }

  // Invalid parent (non-atomic) synthetic complex type.
  {
    ScopedErrorSilence silence;
    ScopedType cmpd(H5Tcreate(H5T_COMPOUND, 8));
    ASSERT_TRUE(cmpd.valid());
    H5T_t* bad_parent = LookupMutableType(cmpd.get());
    H5T_shared_t fake_shared = *f32_le->shared;
    fake_shared.parent = bad_parent;
    H5T_t fake_cplx = *f32_le;
    fake_cplx.shared = &fake_shared;

    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_complex(&fake_cplx, f64_be, &cdata, &ctx, 0, 0, 0,
                                nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, &fake_cplx, &cdata, &ctx, 0, 0, 0,
                                nullptr, nullptr),
              0);
  }

  // Unsupported orders (NONE, VAX), oversized dst (>128), esize > 63.
  {
    ScopedErrorSilence silence;
    ScopedType f_none(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f_none.valid());
    ScopedType c_none(H5Tcomplex_create(f_none.get()));
    ASSERT_TRUE(c_none.valid());
    H5T_t* cn_p = LookupMutableType(c_none.get());
    cn_p->shared->parent->shared->u.atomic.order = H5T_ORDER_NONE;

    ScopedType c_vax(H5Tcomplex_create(H5T_VAX_F32));
    ASSERT_TRUE(c_vax.valid());
    const H5T_t* cv_p = LookupType(c_vax.get());

    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_complex(cn_p, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, cn_p, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    // LE -> VAX hits dst unsupported order in INIT.
    EXPECT_LT(H5T__conv_complex(f32_le, cv_p, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);

    // VAX -> VAX passes INIT (due to upstream src_atomic check) and fails
    // on source VAX order inside _loop.
    EXPECT_GE(H5T__conv_complex(cv_p, cv_p, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    cdata.command = H5T_CONV_CONV;
    std::array<uint8_t, 8> vbuf{};
    EXPECT_LT(H5T__conv_complex(cv_p, cv_p, &cdata, &ctx, 1, 0, 0, vbuf.data(),
                                nullptr),
              0);

    // Mutate cloned dst order to VAX after INIT to hit dst VAX error in _loop.
    ScopedType dst_mut_f(H5Tcopy(H5T_IEEE_F32LE));
    ScopedType dst_mut_c(H5Tcomplex_create(dst_mut_f.get()));
    H5T_t* dm_p = LookupMutableType(dst_mut_c.get());
    dm_p->shared->parent->shared->u.atomic.order = H5T_ORDER_VAX;
    WriteComplex<float>(vbuf.data(), 1.5f, 2.5f);
    EXPECT_LT(H5T__conv_complex(f32_le, dm_p, &cdata, &ctx, 1, 0, 0,
                                vbuf.data(), nullptr),
              0);
    dm_p->shared->parent->shared->u.atomic.order = H5T_ORDER_LE;

    // Unsupported normalization method (H5T_NORM_MSBSET) in _part.
    ScopedType f_msb(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_GE(H5Tset_norm(f_msb.get(), H5T_NORM_MSBSET), 0);
    ScopedType c_msb(H5Tcomplex_create(f_msb.get()));
    const H5T_t* cmsb_p = LookupType(c_msb.get());
    WriteComplex<float>(vbuf.data(), 1.5f, 2.5f);
    EXPECT_LT(H5T__conv_complex(cmsb_p, f64_be, &cdata, &ctx, 1, 0, 0,
                                vbuf.data(), nullptr),
              0);

    // Oversized dst complex (> 128 bytes).
    ScopedType f_big(H5Tcopy(H5T_IEEE_F64LE));
    ASSERT_GE(H5Tset_size(f_big.get(), 65), 0);
    ScopedType c_big(H5Tcomplex_create(f_big.get()));
    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_complex(f32_le, LookupType(c_big.get()), &cdata, &ctx,
                                0, 0, 0, nullptr, nullptr),
              0);

    // esize > 63 on src and dst.
    ScopedType f_es(H5Tcopy(H5T_IEEE_F64LE));
    ASSERT_GE(H5Tset_size(f_es.get(), 16), 0);
    ASSERT_GE(H5Tset_precision(f_es.get(), 128), 0);
    ASSERT_GE(H5Tset_fields(f_es.get(), 127, 63, 64, 0, 63), 0);
    ScopedType c_es(H5Tcomplex_create(f_es.get()));
    const H5T_t* ces_p = LookupType(c_es.get());
    EXPECT_LT(H5T__conv_complex(ces_p, f64_be, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
    EXPECT_LT(H5T__conv_complex(f32_le, ces_p, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
              0);
  }
}

TEST(H5TconvComplexTest, SoftComplexToIntegerAndErrorPaths) {
  // Public BE complex -> BE signed/unsigned integer via soft H5T__conv_complex_i.
  std::vector<uint8_t> buf(2 * 8, 0);
  auto write_be_f32_c = [&](size_t idx, float re, float im) {
    uint8_t le[8];
    WriteComplex<float>(le, re, im);
    for (int i = 0; i < 4; ++i) {
      buf[idx * 8 + i] = le[3 - i];
      buf[idx * 8 + 4 + i] = le[7 - i];
    }
  };
  write_be_f32_c(0, 42.0f, 9.0f);
  write_be_f32_c(1, -17.0f, 3.0f);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_STD_I32BE, 2, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_EQ(buf[3], 42);

  H5T_cdata_t cdata{};
  const H5T_t* c_f32 = LookupType(H5T_COMPLEX_IEEE_F32BE);
  const H5T_t* i32 = LookupType(H5T_STD_I32BE);
  H5T_conv_ctx_t ctx = MakeConvCtx(H5T_COMPLEX_IEEE_F32BE, H5T_STD_I32BE);

  cdata.command = H5T_CONV_INIT;
  EXPECT_GE(
      H5T__conv_complex_i(c_f32, i32, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_complex_i(c_f32, i32, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  ScopedErrorSilence silence;
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_complex_i(nullptr, i32, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_complex_i(c_f32, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);

  ScopedType cmpd(H5Tcreate(H5T_COMPOUND, 8));
  H5T_shared_t fake_shared = *c_f32->shared;
  fake_shared.parent = LookupMutableType(cmpd.get());
  H5T_t bad_cplx = *c_f32;
  bad_cplx.shared = &fake_shared;
  EXPECT_LT(H5T__conv_complex_i(&bad_cplx, i32, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);

  ScopedType f_none(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(f_none.valid());
  ScopedType c_none(H5Tcomplex_create(f_none.get()));
  LookupMutableType(c_none.get())->shared->parent->shared->u.atomic.order =
      H5T_ORDER_NONE;
  EXPECT_LT(H5T__conv_complex_i(LookupType(c_none.get()), i32, &cdata, &ctx, 0,
                                0, 0, nullptr, nullptr),
            0);

  ScopedType i_none(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i_none.valid());
  LookupMutableType(i_none.get())->shared->u.atomic.order = H5T_ORDER_NONE;
  EXPECT_LT(H5T__conv_complex_i(c_f32, LookupType(i_none.get()), &cdata, &ctx,
                                0, 0, 0, nullptr, nullptr),
            0);

  ScopedType i_big(H5Tcopy(H5T_STD_I32LE));
  ASSERT_GE(H5Tset_size(i_big.get(), 65), 0);
  EXPECT_LT(H5T__conv_complex_i(c_f32, LookupType(i_big.get()), &cdata, &ctx, 0,
                                0, 0, nullptr, nullptr),
            0);

  ScopedType f_es(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_GE(H5Tset_size(f_es.get(), 16), 0);
  ASSERT_GE(H5Tset_precision(f_es.get(), 128), 0);
  ASSERT_GE(H5Tset_fields(f_es.get(), 127, 63, 64, 0, 63), 0);
  ScopedType c_es(H5Tcomplex_create(f_es.get()));
  EXPECT_LT(H5T__conv_complex_i(LookupType(c_es.get()), i32, &cdata, &ctx, 0, 0,
                                0, nullptr, nullptr),
            0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_complex_i(nullptr, i32, &cdata, &ctx, 1, 0, 0, buf.data(),
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_complex_i(c_f32, nullptr, &cdata, &ctx, 1, 0, 0,
                                buf.data(), nullptr),
            0);
  EXPECT_LT(H5T__conv_complex_i(c_f32, i32, &cdata, nullptr, 1, 0, 0,
                                buf.data(), nullptr),
            0);
  ExceptLog abort_log;
  abort_log.action = H5T_CONV_ABORT;
  H5T_conv_ctx_t abort_ctx =
      MakeConvCtx(H5T_COMPLEX_IEEE_F32BE, H5T_STD_I32BE, TestExceptCb, &abort_log);
  write_be_f32_c(0, 1e30f, 0.0f);
  EXPECT_LT(H5T__conv_complex_i(c_f32, i32, &cdata, &abort_ctx, 1, 0, 0,
                                buf.data(), nullptr),
            0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_complex_i(c_f32, i32, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
}

TEST(H5TconvComplexTest, SoftComplexFloatUnmatchedAndMatched) {
  // 1. Unmatched complex -> float (F64BE -> F32BE) via H5Tconvert.
  std::vector<uint8_t> ubuf(16, 0);
  uint8_t le[8];
  WriteVal<double>(le, 12.5);
  for (int i = 0; i < 8; ++i) ubuf[i] = le[7 - i];
  WriteVal<double>(le, -7.25);
  for (int i = 0; i < 8; ++i) ubuf[8 + i] = le[7 - i];
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F64BE, H5T_IEEE_F32BE, 1, ubuf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // 2. Matched complex <-> float (BE and LE, narrowing and widening olap,
  // zero imag clearing on float->complex).
  constexpr size_t kN = 5;
  std::vector<uint8_t> buf(kN * 8, 0);
  for (size_t i = 0; i < kN; ++i) {
    uint8_t flt[4];
    WriteVal<float>(flt, static_cast<float>(i) + 1.5f);
    for (int b = 0; b < 4; ++b) buf[i * 4 + b] = flt[3 - b];
  }
  // Fill trailing area with non-zero sentinel to verify imag zeroing.
  std::memset(buf.data() + kN * 4, 0xCC, kN * 4);
  EXPECT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F32BE, kN, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_IEEE_F32BE, kN, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);

  // 3. Padded custom float <-> matching complex + +/-0, +/-Inf, NaN +
  // callbacks in H5T__conv_complex_f_matched.
  ScopedType pad_f(MakeCustomFloat(H5T_ORDER_LE, 6, 4, 32, 35, 27, 8, 4, 23,
                                   127, H5T_NORM_NONE, H5T_PAD_ONE,
                                   H5T_PAD_ONE));
  ASSERT_TRUE(pad_f.valid());
  ScopedType pad_c(H5Tcomplex_create(pad_f.get()));
  ASSERT_TRUE(pad_c.valid());

  std::vector<uint8_t> pbuf(4 * 12, 0);
  // Finite element at index 0: 2.5 in bits [4..35] -> shift 2.5f left 4 bits.
  uint32_t raw_2_5;
  float v_2_5 = 2.5f;
  std::memcpy(&raw_2_5, &v_2_5, 4);
  uint64_t shifted = static_cast<uint64_t>(raw_2_5) << 4;
  std::memcpy(pbuf.data(), &shifted, 6);
  // +0.0 at index 1 (all 0 in bits 4..35), +Inf at index 2, NaN at index 3.
  uint64_t inf_bits = static_cast<uint64_t>(0x7F800000U) << 4;
  uint64_t nan_bits = static_cast<uint64_t>(0x7FC00000U) << 4;
  std::memcpy(pbuf.data() + 2 * 12, &inf_bits, 6);
  std::memcpy(pbuf.data() + 3 * 12, &nan_bits, 6);

  H5T_conv_ctx_t mctx = MakeConvCtx(pad_c.get(), pad_f.get());
  EXPECT_GE(H5T__conv_complex_f_matched(LookupType(pad_c.get()),
                                        LookupType(pad_f.get()), &mctx, 4, 12,
                                        pbuf.data()),
            0);
  EXPECT_EQ(pbuf[0] & 0x0F, 0x0F);  // lsb_pad = ONE
  EXPECT_EQ(pbuf[5] & 0xF0, 0xF0);  // msb_pad = ONE

  // Exceptions (-Inf, +Inf, NaN) in _matched for both FLOAT->COMPLEX and
  // COMPLEX->FLOAT with UNHANDLED, HANDLED, ABORT.
  const float pinf = std::numeric_limits<float>::infinity();
  const float ninf = -pinf;
  const float qnan = std::numeric_limits<float>::quiet_NaN();
  for (H5T_conv_ret_t action :
       {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED, H5T_CONV_ABORT}) {
    ExceptLog log;
    log.action = action;
    log.fill_bytes = 8;
    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestExceptCb, &log), 0);

    for (float fv : {pinf, ninf, qnan, -0.0f}) {
      std::vector<uint8_t> mbuf(16, 0);
      uint8_t fle[4];
      WriteVal<float>(fle, fv);
      for (int b = 0; b < 4; ++b) mbuf[b] = fle[3 - b];
      if (action == H5T_CONV_ABORT && std::isfinite(fv) == false) {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F32BE, 1,
                             mbuf.data(), nullptr, dxpl.get()),
                  0);
      } else {
        EXPECT_GE(H5Tconvert(H5T_IEEE_F32BE, H5T_COMPLEX_IEEE_F32BE, 1,
                             mbuf.data(), nullptr, dxpl.get()),
                  0);
      }
    }
  }

  // Norm-none H5T_NATIVE_LDOUBLE <-> complex matched for +/-Inf mantissa MSB.
  ScopedType ldc(H5Tcomplex_create(H5T_NATIVE_LDOUBLE));
  std::vector<uint8_t> ldbuf(32, 0);
  WriteVal<long double>(ldbuf.data(), std::numeric_limits<long double>::infinity());
  H5T_conv_ctx_t ldctx = MakeConvCtx(H5T_NATIVE_LDOUBLE, ldc.get());
  EXPECT_GE(H5T__conv_complex_f_matched(LookupType(H5T_NATIVE_LDOUBLE),
                                        LookupType(ldc.get()), &ldctx, 1, 0,
                                        ldbuf.data()),
            0);
  EXPECT_TRUE(std::isinf(ReadVal<long double>(ldbuf.data())));

  // VAX paths inside H5T__conv_complex_f_matched (FLOAT->COMPLEX fails on dst
  // VAX complex, COMPLEX->FLOAT fails on src VAX complex).
  {
    ScopedErrorSilence silence;
    ScopedType vax_c(H5Tcomplex_create(H5T_VAX_F32));
    std::vector<uint8_t> vbuf(8, 0);
    H5T_conv_ctx_t vctx = MakeConvCtx(H5T_VAX_F32, vax_c.get());
    EXPECT_LT(H5T__conv_complex_f_matched(LookupType(H5T_VAX_F32),
                                          LookupType(vax_c.get()), &vctx, 1, 0,
                                          vbuf.data()),
              0);
    EXPECT_LT(H5T__conv_complex_f_matched(LookupType(vax_c.get()),
                                          LookupType(H5T_VAX_F32), &vctx, 1, 0,
                                          vbuf.data()),
              0);
  }

  // H5T__conv_complex_f error paths.
  {
    ScopedErrorSilence silence;
    H5T_cdata_t cdata{};
    const H5T_t* cf32 = LookupType(H5T_COMPLEX_IEEE_F32BE);
    const H5T_t* f32 = LookupType(H5T_IEEE_F32BE);
    H5T_conv_ctx_t fctx = MakeConvCtx(H5T_COMPLEX_IEEE_F32BE, H5T_IEEE_F32BE);

    cdata.command = H5T_CONV_INIT;
    EXPECT_GE(
        H5T__conv_complex_f(cf32, f32, &cdata, &fctx, 0, 0, 0, nullptr, nullptr),
        0);
    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(
        H5T__conv_complex_f(cf32, f32, &cdata, &fctx, 0, 0, 0, nullptr, nullptr),
        0);

    cdata.command = H5T_CONV_INIT;
    EXPECT_LT(H5T__conv_complex_f(nullptr, f32, &cdata, &fctx, 0, 0, 0, nullptr,
                                  nullptr),
              0);
    EXPECT_LT(H5T__conv_complex_f(cf32, nullptr, &cdata, &fctx, 0, 0, 0, nullptr,
                                  nullptr),
              0);

    ScopedType cmpd(H5Tcreate(H5T_COMPOUND, 8));
    H5T_shared_t bad_shared = *cf32->shared;
    bad_shared.parent = LookupMutableType(cmpd.get());
    H5T_t bad_c = *cf32;
    bad_c.shared = &bad_shared;
    EXPECT_LT(H5T__conv_complex_f(&bad_c, f32, &cdata, &fctx, 0, 0, 0, nullptr,
                                  nullptr),
              0);

    ScopedType f_none(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f_none.valid());
    ScopedType c_none(H5Tcomplex_create(f_none.get()));
    LookupMutableType(f_none.get())->shared->u.atomic.order = H5T_ORDER_NONE;
    LookupMutableType(c_none.get())->shared->parent->shared->u.atomic.order =
        H5T_ORDER_NONE;
    EXPECT_LT(H5T__conv_complex_f(LookupType(c_none.get()), f32, &cdata, &fctx,
                                  0, 0, 0, nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_complex_f(cf32, LookupType(f_none.get()), &cdata, &fctx,
                                  0, 0, 0, nullptr, nullptr),
              0);

    ScopedType f_big(H5Tcopy(H5T_IEEE_F64LE));
    ASSERT_GE(H5Tset_size(f_big.get(), 65), 0);
    EXPECT_LT(H5T__conv_complex_f(cf32, LookupType(f_big.get()), &cdata, &fctx,
                                  0, 0, 0, nullptr, nullptr),
              0);

    ScopedType f_es(H5Tcopy(H5T_IEEE_F64LE));
    ASSERT_GE(H5Tset_size(f_es.get(), 16), 0);
    ASSERT_GE(H5Tset_precision(f_es.get(), 128), 0);
    ASSERT_GE(H5Tset_fields(f_es.get(), 127, 63, 64, 0, 63), 0);
    ScopedType c_es(H5Tcomplex_create(f_es.get()));
    EXPECT_LT(H5T__conv_complex_f(LookupType(c_es.get()), f32, &cdata, &fctx, 0,
                                  0, 0, nullptr, nullptr),
              0);
    EXPECT_LT(H5T__conv_complex_f(cf32, LookupType(f_es.get()), &cdata, &fctx,
                                  0, 0, 0, nullptr, nullptr),
              0);

    cdata.command = H5T_CONV_CONV;
    std::vector<uint8_t> errbuf(16, 0);
    EXPECT_LT(H5T__conv_complex_f(nullptr, f32, &cdata, &fctx, 1, 0, 0,
                                  errbuf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_complex_f(cf32, nullptr, &cdata, &fctx, 1, 0, 0,
                                  errbuf.data(), nullptr),
              0);
    EXPECT_LT(H5T__conv_complex_f(cf32, f32, &cdata, nullptr, 1, 0, 0,
                                  errbuf.data(), nullptr),
              0);

    ExceptLog ab;
    ab.action = H5T_CONV_ABORT;
    H5T_conv_ctx_t actx =
        MakeConvCtx(H5T_COMPLEX_IEEE_F64BE, H5T_IEEE_F32BE, TestExceptCb, &ab);
    H5T_conv_ctx_t actx_m =
        MakeConvCtx(H5T_COMPLEX_IEEE_F32BE, H5T_IEEE_F32BE, TestExceptCb, &ab);
    uint8_t inf_f[4];
    WriteVal<float>(inf_f, std::numeric_limits<float>::infinity());
    for (int i = 0; i < 4; ++i) errbuf[i] = inf_f[3 - i];
    EXPECT_LT(H5T__conv_complex_f(cf32, f32, &cdata, &actx_m, 1, 0, 0,
                                  errbuf.data(), nullptr),
              0);
    // Unmatched H5T__conv_f_f_loop fail on abort.
    std::vector<uint8_t> nbuf(16, 0);
    uint8_t infd[8];
    WriteVal<double>(infd, std::numeric_limits<double>::infinity());
    for (int i = 0; i < 8; ++i) nbuf[i] = infd[7 - i];
    EXPECT_LT(H5T__conv_complex_f(LookupType(H5T_COMPLEX_IEEE_F64BE), f32,
                                  &cdata, &actx, 1, 0, 0, nbuf.data(), nullptr),
              0);

    cdata.command = static_cast<H5T_cmd_t>(99);
    EXPECT_LT(
        H5T__conv_complex_f(cf32, f32, &cdata, &fctx, 0, 0, 0, nullptr, nullptr),
        0);
  }
}

TEST(H5TconvComplexTest, ComplexCompatConversionsAndAllRejections) {
  // 1. Valid complex <-> array[2] and complex <-> packed compounds (both
  // unsorted and name-sorted so sorted == H5T_SORT_NAME copies & re-sorts).
  constexpr hsize_t kDim2 = 2;
  ScopedType arr_f32(H5Tarray_create2(H5T_IEEE_F32LE, 1, &kDim2));
  ASSERT_TRUE(arr_f32.valid());

  ScopedType cmpd_std(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_std.get(), "real", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_std.get(), "imaginary", 4, H5T_IEEE_F32LE), 0);

  // Name-sorted compound where "imag" precedes "real" alphabetically so
  // H5T__conv_complex_compat must copy and sort by value.
  ScopedType cmpd_namesort(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_namesort.get(), "r", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_namesort.get(), "im", 4, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5T__sort_name(LookupMutableType(cmpd_namesort.get()), nullptr), 0);

  std::array<float, 4> buf = {3.25f, -7.5f, 1.0f, 2.0f};
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32LE, arr_f32.get(), 2, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Tconvert(arr_f32.get(), H5T_COMPLEX_IEEE_F32LE, 2, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32LE, cmpd_std.get(), 2, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Tconvert(cmpd_std.get(), H5T_COMPLEX_IEEE_F32LE, 2, buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32LE, cmpd_namesort.get(), 2,
                       buf.data(), nullptr, H5P_DEFAULT),
            0);
  EXPECT_FLOAT_EQ(buf[0], 3.25f);
  EXPECT_FLOAT_EQ(buf[1], -7.5f);

  // 2. Direct FREE + every rejection branch in H5T__conv_complex_compat.
  const H5T_t* c_f32 = LookupType(H5T_COMPLEX_IEEE_F32LE);
  H5T_conv_ctx_t ctx = MakeConvCtx(H5T_COMPLEX_IEEE_F32LE, cmpd_std.get());
  H5T_cdata_t cdata{};

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_complex_compat(c_f32, LookupType(cmpd_std.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedErrorSilence silence;
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_std.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  cdata.command = H5T_CONV_INIT;
  // Wrong target class (complex -> int, array -> float).
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(H5T_STD_I32LE), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_complex_compat(LookupType(arr_f32.get()),
                                     LookupType(H5T_IEEE_F32LE), &cdata, &ctx,
                                     0, 0, 0, nullptr, nullptr),
            0);

  // Unsupported complex form (!H5T_COMPLEX_RECTANGULAR).
  {
    ScopedType c_mut(H5Tcopy(H5T_COMPLEX_IEEE_F32LE));
    H5T_t* cm = LookupMutableType(c_mut.get());
    cm->shared->u.cplx.form = static_cast<H5T_complex_form_t>(99);
    EXPECT_LT(H5T__conv_complex_compat(cm, LookupType(arr_f32.get()), &cdata,
                                       &ctx, 0, 0, 0, nullptr, nullptr),
              0);
    cm->shared->u.cplx.form = H5T_COMPLEX_RECTANGULAR;
  }

  // Array nelem != 2 and array base mismatch.
  constexpr hsize_t kDim3 = 3;
  ScopedType arr_3(H5Tarray_create2(H5T_IEEE_F32LE, 1, &kDim3));
  ScopedType arr_i32(H5Tarray_create2(H5T_STD_I32LE, 1, &kDim2));
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(arr_3.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(arr_i32.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // Compound nmembs != 2, !packed, memb_size != complex.size.
  ScopedType cmpd_1(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_1.get(), "real", 0, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_1.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedType cmpd_gap(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_GE(H5Tinsert(cmpd_gap.get(), "real", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_gap.get(), "imaginary", 8, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_gap.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedType cmpd_12(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_GE(H5Tinsert(cmpd_12.get(), "real", 0, H5T_IEEE_F64LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_12.get(), "imaginary", 8, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_12.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // Packed 2-field compound of size 8 where field[0].size == 2 != 4.
  ScopedType cmpd_f0sz(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_f0sz.get(), "real", 0, H5T_STD_I16LE), 0);
  // Second field 6-byte opaque to keep packed=true & memb_size=8.
  ScopedType opq6(H5Tcreate(H5T_OPAQUE, 6));
  ASSERT_GE(H5Tinsert(cmpd_f0sz.get(), "imaginary", 2, opq6.get()), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_f0sz.get()), &cdata,
                                     &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // Wrong field[0] name, wrong field[0] type, wrong field[1] name, wrong
  // field[1] type.
  ScopedType cmpd_bad_n0(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_bad_n0.get(), "foo", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_bad_n0.get(), "imaginary", 4, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_bad_n0.get()),
                                     &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedType cmpd_bad_t0(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_bad_t0.get(), "real", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_bad_t0.get(), "imaginary", 4, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_bad_t0.get()),
                                     &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedType cmpd_bad_n1(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_bad_n1.get(), "real", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_bad_n1.get(), "bar", 4, H5T_IEEE_F32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_bad_n1.get()),
                                     &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  ScopedType cmpd_bad_t1(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_GE(H5Tinsert(cmpd_bad_t1.get(), "real", 0, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tinsert(cmpd_bad_t1.get(), "imaginary", 4, H5T_STD_I32LE), 0);
  EXPECT_LT(H5T__conv_complex_compat(c_f32, LookupType(cmpd_bad_t1.get()),
                                     &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // Mutated field[0].offset != 0, field[1].offset != field[0].size, and
  // field[1].size != base.size.
  {
    ScopedType cmut(H5Tcopy(cmpd_std.get()));
    H5T_t* mp = LookupMutableType(cmut.get());
    mp->shared->u.compnd.memb[0].offset = 1;
    EXPECT_LT(H5T__conv_complex_compat(c_f32, mp, &cdata, &ctx, 0, 0, 0,
                                       nullptr, nullptr),
              0);
    mp->shared->u.compnd.memb[0].offset = 0;

    mp->shared->u.compnd.memb[1].offset = 2;
    EXPECT_LT(H5T__conv_complex_compat(c_f32, mp, &cdata, &ctx, 0, 0, 0,
                                       nullptr, nullptr),
              0);
    mp->shared->u.compnd.memb[1].offset = 4;

    mp->shared->u.compnd.memb[1].size = 2;
    EXPECT_LT(H5T__conv_complex_compat(c_f32, mp, &cdata, &ctx, 0, 0, 0,
                                       nullptr, nullptr),
              0);
    mp->shared->u.compnd.memb[1].size = 4;
  }
}

using ConvFn = herr_t (*)(const H5T_t*, const H5T_t*, H5T_cdata_t*,
                          const H5T_conv_ctx_t*, size_t, size_t, size_t, void*,
                          void*);

template <typename SrcBase, typename DstT>
void ExerciseComplexToInt(ConvFn fn, hid_t src_id, hid_t dst_id) {
  const H5T_t* st = LookupType(src_id);
  const H5T_t* dt = LookupType(dst_id);
  ASSERT_NE(st, nullptr);
  ASSERT_NE(dt, nullptr);

  constexpr size_t kElemSrc = 2 * sizeof(SrcBase);
  constexpr size_t kStride = kElemSrc > sizeof(DstT) ? kElemSrc : sizeof(DstT);
  // Test elements: [normal, fraction/truncate, very large hi, very small low]
  const std::array<SrcBase, 4> reals = {
      static_cast<SrcBase>(5),
      static_cast<SrcBase>(3.25),
      std::ldexp(static_cast<SrcBase>(1.5), 100),
      -std::ldexp(static_cast<SrcBase>(1.5), 100),
  };

  auto run_case = [&](H5T_conv_except_func_t cb, void* udata, size_t offset,
                      size_t stride, bool expect_ok) {
    const size_t step = stride ? stride : kStride;
    std::vector<uint8_t> raw(offset + reals.size() * step + 64, 0);
    uint8_t* ptr = raw.data() + offset;
    for (size_t i = 0; i < reals.size(); ++i) {
      WriteComplex<SrcBase>(ptr + i * step, reals[i],
                            static_cast<SrcBase>(1.5));
    }
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, cb, udata);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    ASSERT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
    cdata.command = H5T_CONV_CONV;
    herr_t rc =
        fn(st, dt, &cdata, &ctx, reals.size(), stride, 0, ptr, nullptr);
    if (expect_ok) {
      EXPECT_GE(rc, 0);
      if (cb == nullptr || static_cast<ExceptLog*>(udata)->action == H5T_CONV_UNHANDLED) {
        EXPECT_EQ(ReadVal<DstT>(ptr), static_cast<DstT>(5));
      }
    } else {
      EXPECT_LT(rc, 0);
    }
    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  };

  // Aligned, no cb
  run_case(nullptr, nullptr, 0, 0, true);
  // Unaligned (+1 byte) + explicit buf_stride
  run_case(nullptr, nullptr, 1, kStride + 8, true);

  // Exception callback UNHANDLED & HANDLED (aligned and unaligned)
  for (H5T_conv_ret_t act : {H5T_CONV_UNHANDLED, H5T_CONV_HANDLED}) {
    ExceptLog log;
    log.action = act;
    log.fill_bytes = sizeof(DstT);
    run_case(TestExceptCb, &log, 0, 0, true);
    EXPECT_GE(log.count, 3);
    run_case(TestExceptCb, &log, 1, 0, true);
  }

  // ABORT on RANGE_HI, RANGE_LOW, TRUNCATE
  for (SrcBase v : {std::ldexp(static_cast<SrcBase>(1.5), 100),
                    -std::ldexp(static_cast<SrcBase>(1.5), 100),
                    static_cast<SrcBase>(3.25)}) {
    ScopedErrorSilence silence;
    ExceptLog log;
    log.action = H5T_CONV_ABORT;
    std::vector<uint8_t> buf(kElemSrc + sizeof(DstT), 0);
    WriteComplex<SrcBase>(buf.data(), v, static_cast<SrcBase>(0));
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, TestExceptCb, &log);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_CONV;
    EXPECT_LT(fn(st, dt, &cdata, &ctx, 1, 0, 0, buf.data(), nullptr), 0);
  }
}

template <typename SrcBase, typename DstT>
void ExerciseComplexToFloat(ConvFn fn, hid_t src_id, hid_t dst_id,
                            bool can_overflow, SrcBase hi_val, SrcBase lo_val) {
  const H5T_t* st = LookupType(src_id);
  const H5T_t* dt = LookupType(dst_id);
  ASSERT_NE(st, nullptr);
  ASSERT_NE(dt, nullptr);

  constexpr size_t kElemSrc = 2 * sizeof(SrcBase);
  constexpr size_t kMaxElem =
      kElemSrc > sizeof(DstT) ? kElemSrc : sizeof(DstT);

  // 5 elements so widening or narrowing exercises multi-element loops, both
  // aligned and unaligned (+1), and with explicit stride.
  for (size_t offset : {size_t{0}, size_t{1}}) {
    std::vector<uint8_t> raw(offset + 5 * (kMaxElem + 8) + 64, 0);
    uint8_t* ptr = raw.data() + offset;
    for (size_t i = 0; i < 5; ++i) {
      WriteComplex<SrcBase>(ptr + i * kElemSrc, static_cast<SrcBase>(i + 1.5),
                            static_cast<SrcBase>(-2.0));
    }
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    ASSERT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
    cdata.command = H5T_CONV_CONV;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 5, 0, 0, ptr, nullptr), 0);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstT>(ptr)), 1.5, 1e-2);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstT>(ptr + 4 * sizeof(DstT))), 5.5,
                1e-2);

    // Explicit buf_stride pass.
    const size_t stride = kMaxElem + 8;
    std::fill(raw.begin(), raw.end(), 0);
    for (size_t i = 0; i < 3; ++i) {
      WriteComplex<SrcBase>(ptr + i * stride, static_cast<SrcBase>(i + 2.5),
                            static_cast<SrcBase>(1.0));
    }
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 3, stride, 0, ptr, nullptr), 0);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstT>(ptr + 2 * stride)), 4.5, 1e-2);
    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  }

  if (can_overflow) {
    const std::array<SrcBase, 3> vals = {static_cast<SrcBase>(2.5), hi_val,
                                         lo_val};
    for (int mode = 0; mode < 3; ++mode) {
      ExceptLog log;
      log.action = (mode == 1) ? H5T_CONV_UNHANDLED : H5T_CONV_HANDLED;
      log.fill_bytes = sizeof(DstT);
      std::vector<uint8_t> raw(3 * kElemSrc + 32, 0);
      for (size_t i = 0; i < 3; ++i) {
        WriteComplex<SrcBase>(raw.data() + i * kElemSrc, vals[i],
                              static_cast<SrcBase>(0));
      }
      H5T_conv_ctx_t ctx = MakeConvCtx(
          src_id, dst_id, mode == 0 ? nullptr : TestExceptCb,
          mode == 0 ? nullptr : &log);
      H5T_cdata_t cdata{};
      cdata.command = H5T_CONV_CONV;
      EXPECT_GE(fn(st, dt, &cdata, &ctx, 3, 0, 0, raw.data(), nullptr), 0);
      if (mode != 2) {
        EXPECT_TRUE(std::isinf(static_cast<double>(
            ReadVal<DstT>(raw.data() + sizeof(DstT)))));
        EXPECT_TRUE(std::isinf(static_cast<double>(
            ReadVal<DstT>(raw.data() + 2 * sizeof(DstT)))));
      }
    }
    for (SrcBase v : {hi_val, lo_val}) {
      ScopedErrorSilence silence;
      ExceptLog log;
      log.action = H5T_CONV_ABORT;
      std::vector<uint8_t> raw(kElemSrc + 16, 0);
      WriteComplex<SrcBase>(raw.data(), v, static_cast<SrcBase>(0));
      H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, TestExceptCb, &log);
      H5T_cdata_t cdata{};
      cdata.command = H5T_CONV_CONV;
      EXPECT_LT(fn(st, dt, &cdata, &ctx, 1, 0, 0, raw.data(), nullptr), 0);
    }
  } else {
    // Pass callback on no-overflow cast types (H5T_CONV_zf / H5T_CONV_zF) to
    // cover cb_struct.func != NULL outer branch too.
    ExceptLog log;
    std::vector<uint8_t> raw(2 * kMaxElem + 16, 0);
    WriteComplex<SrcBase>(raw.data(), static_cast<SrcBase>(3.5),
                          static_cast<SrcBase>(1.25));
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, TestExceptCb, &log);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_CONV;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 1, 0, 0, raw.data(), nullptr), 0);
    EXPECT_EQ(log.count, 0);
  }
}

template <typename SrcBase, typename DstBase>
void ExerciseComplexToComplex(ConvFn fn, hid_t src_id, hid_t dst_id,
                              bool can_overflow, SrcBase hi_val,
                              SrcBase lo_val) {
  const H5T_t* st = LookupType(src_id);
  const H5T_t* dt = LookupType(dst_id);
  ASSERT_NE(st, nullptr);
  ASSERT_NE(dt, nullptr);

  constexpr size_t kElemSrc = 2 * sizeof(SrcBase);
  constexpr size_t kElemDst = 2 * sizeof(DstBase);
  constexpr size_t kMaxElem = kElemSrc > kElemDst ? kElemSrc : kElemDst;

  // 5 elements so widening hits BOTH safe >= 2 and safe < 2 passes, aligned and
  // unaligned (+1).
  for (size_t offset : {size_t{0}, size_t{1}}) {
    std::vector<uint8_t> raw(offset + 5 * kMaxElem + 64, 0);
    uint8_t* ptr = raw.data() + offset;
    for (size_t i = 0; i < 5; ++i) {
      WriteComplex<SrcBase>(ptr + i * kElemSrc, static_cast<SrcBase>(i + 1.25),
                            static_cast<SrcBase>(-i - 0.5));
    }
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_INIT;
    ASSERT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
    cdata.command = H5T_CONV_CONV;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 5, 0, 0, ptr, nullptr), 0);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstBase>(ptr)), 1.25, 1e-3);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstBase>(ptr + sizeof(DstBase))),
                -0.5, 1e-3);
    EXPECT_NEAR(static_cast<double>(ReadVal<DstBase>(ptr + 4 * kElemDst)), 5.25,
                1e-3);
    cdata.command = H5T_CONV_FREE;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  }

  if (can_overflow) {
    struct Pair {
      SrcBase re;
      SrcBase im;
    };
    const std::array<Pair, 5> pairs = {{
        {static_cast<SrcBase>(1.5), static_cast<SrcBase>(2.5)},
        {hi_val, static_cast<SrcBase>(1.0)},
        {lo_val, static_cast<SrcBase>(1.0)},
        {static_cast<SrcBase>(1.0), hi_val},
        {static_cast<SrcBase>(1.0), lo_val},
    }};
    for (int mode = 0; mode < 3; ++mode) {
      ExceptLog log;
      log.action = (mode == 1) ? H5T_CONV_UNHANDLED : H5T_CONV_HANDLED;
      log.fill_bytes = kElemDst;
      std::vector<uint8_t> raw(pairs.size() * kMaxElem + 32, 0);
      for (size_t i = 0; i < pairs.size(); ++i) {
        WriteComplex<SrcBase>(raw.data() + i * kElemSrc, pairs[i].re,
                              pairs[i].im);
      }
      H5T_conv_ctx_t ctx = MakeConvCtx(
          src_id, dst_id, mode == 0 ? nullptr : TestExceptCb,
          mode == 0 ? nullptr : &log);
      H5T_cdata_t cdata{};
      cdata.command = H5T_CONV_CONV;
      EXPECT_GE(
          fn(st, dt, &cdata, &ctx, pairs.size(), 0, 0, raw.data(), nullptr), 0);
    }
    for (size_t i = 1; i < pairs.size(); ++i) {
      ScopedErrorSilence silence;
      ExceptLog log;
      log.action = H5T_CONV_ABORT;
      std::vector<uint8_t> raw(kElemSrc + 16, 0);
      WriteComplex<SrcBase>(raw.data(), pairs[i].re, pairs[i].im);
      H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, TestExceptCb, &log);
      H5T_cdata_t cdata{};
      cdata.command = H5T_CONV_CONV;
      EXPECT_LT(fn(st, dt, &cdata, &ctx, 1, 0, 0, raw.data(), nullptr), 0);
    }
  } else {
    // Exercise cb != NULL branch for widening (zZ).
    ExceptLog log;
    std::vector<uint8_t> raw(kMaxElem + 16, 0);
    WriteComplex<SrcBase>(raw.data(), static_cast<SrcBase>(2.5),
                          static_cast<SrcBase>(-3.5));
    H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id, TestExceptCb, &log);
    H5T_cdata_t cdata{};
    cdata.command = H5T_CONV_CONV;
    EXPECT_GE(fn(st, dt, &cdata, &ctx, 1, 0, 0, raw.data(), nullptr), 0);
    EXPECT_EQ(log.count, 0);
  }
}

void CheckHardConvErrors(ConvFn fn, hid_t src_id, hid_t dst_id) {
  const H5T_t* st = LookupType(src_id);
  const H5T_t* dt = LookupType(dst_id);
  H5T_conv_ctx_t ctx = MakeConvCtx(src_id, dst_id);
  H5T_cdata_t cdata{};

  ScopedType bad_sz_s(H5Tcopy(H5T_STD_I8LE));
  ASSERT_GE(H5Tset_size(bad_sz_s.get(), 63), 0);
  const H5T_t* bsp = LookupType(bad_sz_s.get());

  ScopedErrorSilence silence;
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(fn(nullptr, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(fn(st, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(fn(bsp, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(fn(st, bsp, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  cdata.command = H5T_CONV_CONV;
  std::array<uint8_t, 64> buf{};
  EXPECT_LT(fn(nullptr, dt, &cdata, &ctx, 1, 0, 0, buf.data(), nullptr), 0);
  EXPECT_LT(fn(st, nullptr, &cdata, &ctx, 1, 0, 0, buf.data(), nullptr), 0);
  EXPECT_LT(fn(st, dt, &cdata, nullptr, 1, 0, 0, buf.data(), nullptr), 0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(fn(st, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
}

TEST(H5TconvComplexTest, HardFcomplexConversions) {
  const hid_t fc = H5T_NATIVE_FLOAT_COMPLEX;
  ASSERT_NE(fc, H5I_INVALID_HID);
  ASSERT_NE(H5T_NATIVE_FLOAT16, H5I_INVALID_HID);

  ExerciseComplexToInt<float, signed char>(H5T__conv_fcomplex_schar, fc, H5T_NATIVE_SCHAR);
  ExerciseComplexToInt<float, unsigned char>(H5T__conv_fcomplex_uchar, fc, H5T_NATIVE_UCHAR);
  ExerciseComplexToInt<float, short>(H5T__conv_fcomplex_short, fc, H5T_NATIVE_SHORT);
  ExerciseComplexToInt<float, unsigned short>(H5T__conv_fcomplex_ushort, fc, H5T_NATIVE_USHORT);
  ExerciseComplexToInt<float, int>(H5T__conv_fcomplex_int, fc, H5T_NATIVE_INT);
  ExerciseComplexToInt<float, unsigned int>(H5T__conv_fcomplex_uint, fc, H5T_NATIVE_UINT);
  ExerciseComplexToInt<float, long>(H5T__conv_fcomplex_long, fc, H5T_NATIVE_LONG);
  ExerciseComplexToInt<float, unsigned long>(H5T__conv_fcomplex_ulong, fc, H5T_NATIVE_ULONG);
  ExerciseComplexToInt<float, long long>(H5T__conv_fcomplex_llong, fc, H5T_NATIVE_LLONG);
  ExerciseComplexToInt<float, unsigned long long>(H5T__conv_fcomplex_ullong, fc, H5T_NATIVE_ULLONG);

  ExerciseComplexToFloat<float, H5__Float16>(
      H5T__conv_fcomplex__Float16, fc, H5T_NATIVE_FLOAT16, true, 1e6f, -1e6f);
  ExerciseComplexToFloat<float, float>(
      H5T__conv_fcomplex_float, fc, H5T_NATIVE_FLOAT, false, 0.0f, 0.0f);
  ExerciseComplexToFloat<float, double>(
      H5T__conv_fcomplex_double, fc, H5T_NATIVE_DOUBLE, false, 0.0f, 0.0f);
  ExerciseComplexToFloat<float, long double>(
      H5T__conv_fcomplex_ldouble, fc, H5T_NATIVE_LDOUBLE, false, 0.0f, 0.0f);

  ExerciseComplexToComplex<float, double>(
      H5T__conv_fcomplex_dcomplex, fc, H5T_NATIVE_DOUBLE_COMPLEX, false, 0.0f, 0.0f);
  ExerciseComplexToComplex<float, long double>(
      H5T__conv_fcomplex_lcomplex, fc, H5T_NATIVE_LDOUBLE_COMPLEX, false, 0.0f, 0.0f);

  const std::array<std::pair<ConvFn, hid_t>, 16> errs = {{
      {H5T__conv_fcomplex_schar, H5T_NATIVE_SCHAR},
      {H5T__conv_fcomplex_uchar, H5T_NATIVE_UCHAR},
      {H5T__conv_fcomplex_short, H5T_NATIVE_SHORT},
      {H5T__conv_fcomplex_ushort, H5T_NATIVE_USHORT},
      {H5T__conv_fcomplex_int, H5T_NATIVE_INT},
      {H5T__conv_fcomplex_uint, H5T_NATIVE_UINT},
      {H5T__conv_fcomplex_long, H5T_NATIVE_LONG},
      {H5T__conv_fcomplex_ulong, H5T_NATIVE_ULONG},
      {H5T__conv_fcomplex_llong, H5T_NATIVE_LLONG},
      {H5T__conv_fcomplex_ullong, H5T_NATIVE_ULLONG},
      {H5T__conv_fcomplex__Float16, H5T_NATIVE_FLOAT16},
      {H5T__conv_fcomplex_float, H5T_NATIVE_FLOAT},
      {H5T__conv_fcomplex_double, H5T_NATIVE_DOUBLE},
      {H5T__conv_fcomplex_ldouble, H5T_NATIVE_LDOUBLE},
      {H5T__conv_fcomplex_dcomplex, H5T_NATIVE_DOUBLE_COMPLEX},
      {H5T__conv_fcomplex_lcomplex, H5T_NATIVE_LDOUBLE_COMPLEX},
  }};
  for (const auto& [fn, dst] : errs) CheckHardConvErrors(fn, fc, dst);
}

TEST(H5TconvComplexTest, HardDcomplexConversions) {
  const hid_t dc = H5T_NATIVE_DOUBLE_COMPLEX;
  ASSERT_NE(dc, H5I_INVALID_HID);

  ExerciseComplexToInt<double, signed char>(H5T__conv_dcomplex_schar, dc, H5T_NATIVE_SCHAR);
  ExerciseComplexToInt<double, unsigned char>(H5T__conv_dcomplex_uchar, dc, H5T_NATIVE_UCHAR);
  ExerciseComplexToInt<double, short>(H5T__conv_dcomplex_short, dc, H5T_NATIVE_SHORT);
  ExerciseComplexToInt<double, unsigned short>(H5T__conv_dcomplex_ushort, dc, H5T_NATIVE_USHORT);
  ExerciseComplexToInt<double, int>(H5T__conv_dcomplex_int, dc, H5T_NATIVE_INT);
  ExerciseComplexToInt<double, unsigned int>(H5T__conv_dcomplex_uint, dc, H5T_NATIVE_UINT);
  ExerciseComplexToInt<double, long>(H5T__conv_dcomplex_long, dc, H5T_NATIVE_LONG);
  ExerciseComplexToInt<double, unsigned long>(H5T__conv_dcomplex_ulong, dc, H5T_NATIVE_ULONG);
  ExerciseComplexToInt<double, long long>(H5T__conv_dcomplex_llong, dc, H5T_NATIVE_LLONG);
  ExerciseComplexToInt<double, unsigned long long>(H5T__conv_dcomplex_ullong, dc, H5T_NATIVE_ULLONG);

  ExerciseComplexToFloat<double, H5__Float16>(
      H5T__conv_dcomplex__Float16, dc, H5T_NATIVE_FLOAT16, true, 1e6, -1e6);
  ExerciseComplexToFloat<double, float>(
      H5T__conv_dcomplex_float, dc, H5T_NATIVE_FLOAT, true, 1e50, -1e50);
  ExerciseComplexToFloat<double, double>(
      H5T__conv_dcomplex_double, dc, H5T_NATIVE_DOUBLE, false, 0.0, 0.0);
  ExerciseComplexToFloat<double, long double>(
      H5T__conv_dcomplex_ldouble, dc, H5T_NATIVE_LDOUBLE, false, 0.0, 0.0);

  ExerciseComplexToComplex<double, float>(
      H5T__conv_dcomplex_fcomplex, dc, H5T_NATIVE_FLOAT_COMPLEX, true, 1e50, -1e50);
  ExerciseComplexToComplex<double, long double>(
      H5T__conv_dcomplex_lcomplex, dc, H5T_NATIVE_LDOUBLE_COMPLEX, false, 0.0, 0.0);

  const std::array<std::pair<ConvFn, hid_t>, 16> errs = {{
      {H5T__conv_dcomplex_schar, H5T_NATIVE_SCHAR},
      {H5T__conv_dcomplex_uchar, H5T_NATIVE_UCHAR},
      {H5T__conv_dcomplex_short, H5T_NATIVE_SHORT},
      {H5T__conv_dcomplex_ushort, H5T_NATIVE_USHORT},
      {H5T__conv_dcomplex_int, H5T_NATIVE_INT},
      {H5T__conv_dcomplex_uint, H5T_NATIVE_UINT},
      {H5T__conv_dcomplex_long, H5T_NATIVE_LONG},
      {H5T__conv_dcomplex_ulong, H5T_NATIVE_ULONG},
      {H5T__conv_dcomplex_llong, H5T_NATIVE_LLONG},
      {H5T__conv_dcomplex_ullong, H5T_NATIVE_ULLONG},
      {H5T__conv_dcomplex__Float16, H5T_NATIVE_FLOAT16},
      {H5T__conv_dcomplex_float, H5T_NATIVE_FLOAT},
      {H5T__conv_dcomplex_double, H5T_NATIVE_DOUBLE},
      {H5T__conv_dcomplex_ldouble, H5T_NATIVE_LDOUBLE},
      {H5T__conv_dcomplex_fcomplex, H5T_NATIVE_FLOAT_COMPLEX},
      {H5T__conv_dcomplex_lcomplex, H5T_NATIVE_LDOUBLE_COMPLEX},
  }};
  for (const auto& [fn, dst] : errs) CheckHardConvErrors(fn, dc, dst);
}

TEST(H5TconvComplexTest, HardLcomplexConversions) {
  const hid_t lc = H5T_NATIVE_LDOUBLE_COMPLEX;
  ASSERT_NE(lc, H5I_INVALID_HID);

  ExerciseComplexToInt<long double, signed char>(H5T__conv_lcomplex_schar, lc, H5T_NATIVE_SCHAR);
  ExerciseComplexToInt<long double, unsigned char>(H5T__conv_lcomplex_uchar, lc, H5T_NATIVE_UCHAR);
  ExerciseComplexToInt<long double, short>(H5T__conv_lcomplex_short, lc, H5T_NATIVE_SHORT);
  ExerciseComplexToInt<long double, unsigned short>(H5T__conv_lcomplex_ushort, lc, H5T_NATIVE_USHORT);
  ExerciseComplexToInt<long double, int>(H5T__conv_lcomplex_int, lc, H5T_NATIVE_INT);
  ExerciseComplexToInt<long double, unsigned int>(H5T__conv_lcomplex_uint, lc, H5T_NATIVE_UINT);
  ExerciseComplexToInt<long double, long>(H5T__conv_lcomplex_long, lc, H5T_NATIVE_LONG);
  ExerciseComplexToInt<long double, unsigned long>(H5T__conv_lcomplex_ulong, lc, H5T_NATIVE_ULONG);
  ExerciseComplexToInt<long double, long long>(H5T__conv_lcomplex_llong, lc, H5T_NATIVE_LLONG);
  ExerciseComplexToInt<long double, unsigned long long>(H5T__conv_lcomplex_ullong, lc, H5T_NATIVE_ULLONG);

  ExerciseComplexToFloat<long double, H5__Float16>(
      H5T__conv_lcomplex__Float16, lc, H5T_NATIVE_FLOAT16, true, 1e6L, -1e6L);
  ExerciseComplexToFloat<long double, float>(
      H5T__conv_lcomplex_float, lc, H5T_NATIVE_FLOAT, true, 1e50L, -1e50L);
  ExerciseComplexToFloat<long double, double>(
      H5T__conv_lcomplex_double, lc, H5T_NATIVE_DOUBLE, true, 1e400L, -1e400L);
  ExerciseComplexToFloat<long double, long double>(
      H5T__conv_lcomplex_ldouble, lc, H5T_NATIVE_LDOUBLE, false, 0.0L, 0.0L);

  ExerciseComplexToComplex<long double, float>(
      H5T__conv_lcomplex_fcomplex, lc, H5T_NATIVE_FLOAT_COMPLEX, true, 1e50L, -1e50L);
  ExerciseComplexToComplex<long double, double>(
      H5T__conv_lcomplex_dcomplex, lc, H5T_NATIVE_DOUBLE_COMPLEX, true, 1e400L, -1e400L);

  const std::array<std::pair<ConvFn, hid_t>, 16> errs = {{
      {H5T__conv_lcomplex_schar, H5T_NATIVE_SCHAR},
      {H5T__conv_lcomplex_uchar, H5T_NATIVE_UCHAR},
      {H5T__conv_lcomplex_short, H5T_NATIVE_SHORT},
      {H5T__conv_lcomplex_ushort, H5T_NATIVE_USHORT},
      {H5T__conv_lcomplex_int, H5T_NATIVE_INT},
      {H5T__conv_lcomplex_uint, H5T_NATIVE_UINT},
      {H5T__conv_lcomplex_long, H5T_NATIVE_LONG},
      {H5T__conv_lcomplex_ulong, H5T_NATIVE_ULONG},
      {H5T__conv_lcomplex_llong, H5T_NATIVE_LLONG},
      {H5T__conv_lcomplex_ullong, H5T_NATIVE_ULLONG},
      {H5T__conv_lcomplex__Float16, H5T_NATIVE_FLOAT16},
      {H5T__conv_lcomplex_float, H5T_NATIVE_FLOAT},
      {H5T__conv_lcomplex_double, H5T_NATIVE_DOUBLE},
      {H5T__conv_lcomplex_ldouble, H5T_NATIVE_LDOUBLE},
      {H5T__conv_lcomplex_fcomplex, H5T_NATIVE_FLOAT_COMPLEX},
      {H5T__conv_lcomplex_dcomplex, H5T_NATIVE_DOUBLE_COMPLEX},
  }};
  for (const auto& [fn, dst] : errs) CheckHardConvErrors(fn, lc, dst);
}

}  // namespace
}  // namespace hdf5
