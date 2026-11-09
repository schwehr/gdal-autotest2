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
// Tests array datatype conversions, background-buffer propagation, application
// conversion functions, exception callbacks, and error paths
// (third_party/hdf5/src/H5Tconv_array.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_array.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5T_CONV_ARRAY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#ifdef H5_HAVE__FLOAT16
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv_array.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5T_CONV_ARRAY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5T_CONV_ARRAY_TEST_RESTORE_UNDEF_NDEBUG
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

struct ConvCbState {
  int calls = 0;
  H5T_conv_ret_t ret = H5T_CONV_UNHANDLED;
  int32_t replacement = 777;
  bool lock_temp_types = false;
  hid_t locked_src_id = H5I_INVALID_HID;
  hid_t locked_dst_id = H5I_INVALID_HID;
};

H5T_conv_ret_t TestArrayExceptCb(H5T_conv_except_t, hid_t src_id, hid_t dst_id,
                                 void*, void* dst_buf, void* user_data) {
  auto* state = static_cast<ConvCbState*>(user_data);
  ++state->calls;
  if (state->lock_temp_types && state->calls == 1) {
    state->locked_src_id = src_id;
    state->locked_dst_id = dst_id;
    H5Tlock(src_id);
    H5Tlock(dst_id);
  }
  if (state->ret == H5T_CONV_HANDLED) {
    std::memcpy(dst_buf, &state->replacement, sizeof(state->replacement));
  }
  return state->ret;
}

herr_t TestOpaqueAppConv(hid_t, hid_t, H5T_cdata_t* cdata, size_t nelmts,
                         size_t buf_stride, size_t, void* buf, void*, hid_t) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_NO;
      return 0;
    case H5T_CONV_CONV: {
      auto* bytes = static_cast<uint8_t*>(buf);
      const size_t stride = buf_stride ? buf_stride : 4;
      for (size_t i = 0; i < nelmts; ++i) {
        for (size_t k = 0; k < 4; ++k) {
          bytes[i * stride + k] ^= 0x5A;
        }
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

TEST(H5TconvArrayTest, Convert1DAnd2DArraysNarrowingWideningAndSameSize) {
  // 1. Narrowing 2D double[2][2] -> int32_t[2][2] (direction = 1).
  constexpr std::array<hsize_t, 2> kDims2D = {2, 2};
  ScopedType src_2d(H5Tarray_create2(H5T_NATIVE_DOUBLE, 2, kDims2D.data()));
  ScopedType dst_2d(H5Tarray_create2(H5T_NATIVE_INT32, 2, kDims2D.data()));
  ASSERT_TRUE(src_2d.valid());
  ASSERT_TRUE(dst_2d.valid());

  std::vector<uint8_t> narrow_buf(2 * 4 * sizeof(double));
  const std::array<double, 8> src_doubles = {1.0, -2.0, 3.0, 4.0,
                                             5.0, 6.0,  -7.0, 8.0};
  std::memcpy(narrow_buf.data(), src_doubles.data(), narrow_buf.size());
  ASSERT_GE(H5Tconvert(src_2d.get(), dst_2d.get(), 2, narrow_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  std::array<int32_t, 8> got_ints{};
  std::memcpy(got_ints.data(), narrow_buf.data(),
              got_ints.size() * sizeof(int32_t));
  EXPECT_THAT(got_ints, ElementsAre(1, -2, 3, 4, 5, 6, -7, 8));

  // 2. Widening 1D int16_t[3] -> double[3] (nelmts = 4, direction = -1).
  constexpr hsize_t kDim3 = 3;
  ScopedType src_1d(H5Tarray_create2(H5T_NATIVE_INT16, 1, &kDim3));
  ScopedType dst_1d(H5Tarray_create2(H5T_NATIVE_DOUBLE, 1, &kDim3));
  ASSERT_TRUE(src_1d.valid());
  ASSERT_TRUE(dst_1d.valid());

  std::vector<uint8_t> wide_buf(4 * 3 * sizeof(double));
  const std::array<int16_t, 12> src_shorts = {10, -20, 30,  40,  50,  -60,
                                              70, 80,  -90, 100, 110, 120};
  std::memcpy(wide_buf.data(), src_shorts.data(),
              src_shorts.size() * sizeof(int16_t));
  ASSERT_GE(H5Tconvert(src_1d.get(), dst_1d.get(), 4, wide_buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  std::array<double, 12> got_doubles{};
  std::memcpy(got_doubles.data(), wide_buf.data(),
              got_doubles.size() * sizeof(double));
  EXPECT_THAT(got_doubles,
              ElementsAre(10.0, -20.0, 30.0, 40.0, 50.0, -60.0, 70.0, 80.0,
                          -90.0, 100.0, 110.0, 120.0));
}

TEST(H5TconvArrayTest, ConvertArrayWithBackgroundBufferForwardAndBackward) {
  struct SmallComp {
    int32_t a;
  };
  struct WideComp {
    int32_t a;
    int32_t b;
  };

  ScopedType small_comp(H5Tcreate(H5T_COMPOUND, sizeof(SmallComp)));
  ScopedType wide_comp(H5Tcreate(H5T_COMPOUND, sizeof(WideComp)));
  ASSERT_TRUE(small_comp.valid());
  ASSERT_TRUE(wide_comp.valid());
  ASSERT_GE(
      H5Tinsert(small_comp.get(), "a", offsetof(SmallComp, a), H5T_NATIVE_INT32),
      0);
  ASSERT_GE(
      H5Tinsert(wide_comp.get(), "a", offsetof(WideComp, a), H5T_NATIVE_INT32),
      0);
  ASSERT_GE(
      H5Tinsert(wide_comp.get(), "b", offsetof(WideComp, b), H5T_NATIVE_INT32),
      0);

  constexpr hsize_t kDim2 = 2;
  ScopedType src_arr(H5Tarray_create2(small_comp.get(), 1, &kDim2));
  ScopedType dst_arr(H5Tarray_create2(wide_comp.get(), 1, &kDim2));
  ASSERT_TRUE(src_arr.valid());
  ASSERT_TRUE(dst_arr.valid());

  // Widening direction = -1 with initialized background buffer.
  std::vector<uint8_t> buf(3 * 2 * sizeof(WideComp));
  const std::array<SmallComp, 6> src_vals = {{{1}, {2}, {3}, {4}, {5}, {6}}};
  std::memcpy(buf.data(), src_vals.data(), src_vals.size() * sizeof(SmallComp));

  std::array<WideComp, 6> bkg = {
      {{-1, 100}, {-1, 200}, {-1, 300}, {-1, 400}, {-1, 500}, {-1, 600}}};

  ASSERT_GE(H5Tconvert(src_arr.get(), dst_arr.get(), 3, buf.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
  std::array<WideComp, 6> out{};
  std::memcpy(out.data(), buf.data(), sizeof(out));
  EXPECT_EQ(out[0].a, 1);
  EXPECT_EQ(out[0].b, 100);
  EXPECT_EQ(out[5].a, 6);
  EXPECT_EQ(out[5].b, 600);

  // Direct call with explicit buf_stride and bkg_stride (direction = 1).
  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  ctx.u.conv.src_type_id = H5I_INVALID_HID;
  ctx.u.conv.dst_type_id = H5I_INVALID_HID;

  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_array(LookupType(src_arr.get()),
                            LookupType(dst_arr.get()), &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
            0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_YES);

  // Re-running INIT with existing cdata.priv hits the reuse branch cleanly.
  ASSERT_GE(H5T__conv_array(LookupType(src_arr.get()),
                            LookupType(dst_arr.get()), &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
            0);

  constexpr size_t kStride = 32;
  std::vector<uint8_t> sbuf(2 * kStride, 0);
  std::vector<uint8_t> sbkg(2 * kStride, 0);
  const std::array<SmallComp, 2> e0 = {{{11}, {12}}};
  const std::array<SmallComp, 2> e1 = {{{21}, {22}}};
  std::memcpy(sbuf.data(), e0.data(), sizeof(e0));
  std::memcpy(sbuf.data() + kStride, e1.data(), sizeof(e1));
  const std::array<WideComp, 2> be0 = {{{0, 71}, {0, 72}}};
  const std::array<WideComp, 2> be1 = {{{0, 81}, {0, 82}}};
  std::memcpy(sbkg.data(), be0.data(), sizeof(be0));
  std::memcpy(sbkg.data() + kStride, be1.data(), sizeof(be1));

  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_array(LookupType(src_arr.get()),
                            LookupType(dst_arr.get()), &cdata, &ctx, 2, kStride,
                            kStride, sbuf.data(), sbkg.data()),
            0);

  std::array<WideComp, 2> r0{};
  std::array<WideComp, 2> r1{};
  std::memcpy(r0.data(), sbuf.data(), sizeof(r0));
  std::memcpy(r1.data(), sbuf.data() + kStride, sizeof(r1));
  EXPECT_EQ(r0[0].a, 11);
  EXPECT_EQ(r0[0].b, 71);
  EXPECT_EQ(r1[1].a, 22);
  EXPECT_EQ(r1[1].b, 82);

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_array(LookupType(src_arr.get()),
                            LookupType(dst_arr.get()), &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
            0);
}

TEST(H5TconvArrayTest, ConvertArrayWithNoopBaseTypePreservesValues) {
  constexpr hsize_t kDim2 = 2;
  ScopedType arr_i32(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim2));
  ASSERT_TRUE(arr_i32.valid());

  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  const H5T_t* dt = LookupType(arr_i32.get());

  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_array(dt, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  std::array<int32_t, 4> buf = {5, 6, 7, 8};
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(
      H5T__conv_array(dt, dt, &cdata, &ctx, 2, 0, 0, buf.data(), nullptr), 0);
  EXPECT_THAT(buf, ElementsAre(5, 6, 7, 8));

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_array(dt, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);
}

TEST(H5TconvArrayTest, ConvertArrayUsesRegisteredAppConversion) {
  ScopedType src_opq(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType dst_opq(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(src_opq.valid());
  ASSERT_TRUE(dst_opq.valid());
  ASSERT_GE(H5Tset_tag(src_opq.get(), "test_arr_src"), 0);
  ASSERT_GE(H5Tset_tag(dst_opq.get(), "test_arr_dst"), 0);

  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_arr_app_opq", src_opq.get(),
                        dst_opq.get(), TestOpaqueAppConv),
            0);
  absl::Cleanup unregister([&]() {
    H5Tunregister(H5T_PERS_HARD, "test_arr_app_opq", src_opq.get(),
                  dst_opq.get(), TestOpaqueAppConv);
  });

  constexpr hsize_t kDim2 = 2;
  ScopedType src_arr(H5Tarray_create2(src_opq.get(), 1, &kDim2));
  ScopedType dst_arr(H5Tarray_create2(dst_opq.get(), 1, &kDim2));
  ASSERT_TRUE(src_arr.valid());
  ASSERT_TRUE(dst_arr.valid());

  std::array<uint8_t, 8> buf = {0x00, 0x01, 0x02, 0x03, 0x5A, 0x5B, 0xFF, 0x10};
  ASSERT_GE(H5Tconvert(src_arr.get(), dst_arr.get(), 1, buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(buf,
              ElementsAre(0x5A, 0x5B, 0x58, 0x59, 0x00, 0x01, 0xA5, 0x4A));
}

TEST(H5TconvArrayTest, ConvertArrayExceptionCallbackHandlesAndAborts) {
  constexpr hsize_t kDim2 = 2;
  ScopedType src_arr(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, &kDim2));
  ScopedType dst_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim2));
  ASSERT_TRUE(src_arr.valid());
  ASSERT_TRUE(dst_arr.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  ConvCbState state;
  state.ret = H5T_CONV_UNHANDLED;
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestArrayExceptCb, &state), 0);

  std::array<uint8_t, 8> buf{};
  std::array<float, 2> ovf = {1e20f, 3.0f};
  std::memcpy(buf.data(), ovf.data(), sizeof(ovf));
  ASSERT_GE(H5Tconvert(src_arr.get(), dst_arr.get(), 1, buf.data(), nullptr,
                       dxpl.get()),
            0);
  EXPECT_EQ(state.calls, 1);

  state.ret = H5T_CONV_HANDLED;
  std::memcpy(buf.data(), ovf.data(), sizeof(ovf));
  ASSERT_GE(H5Tconvert(src_arr.get(), dst_arr.get(), 1, buf.data(), nullptr,
                       dxpl.get()),
            0);
  int32_t out0 = 0;
  std::memcpy(&out0, buf.data(), sizeof(out0));
  EXPECT_EQ(out0, 777);

  ScopedErrorSilence silence;
  state.ret = H5T_CONV_ABORT;
  std::memcpy(buf.data(), ovf.data(), sizeof(ovf));
  EXPECT_LT(H5Tconvert(src_arr.get(), dst_arr.get(), 1, buf.data(), nullptr,
                       dxpl.get()),
            0);
}

TEST(H5TconvArrayTest, ConvArrayCleanupDecRefFailureReturnsError) {
  constexpr hsize_t kDim2 = 2;
  ScopedType src_arr(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, &kDim2));
  ScopedType dst_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim2));
  ASSERT_TRUE(src_arr.valid());
  ASSERT_TRUE(dst_arr.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  ConvCbState state;
  state.ret = H5T_CONV_HANDLED;
  state.lock_temp_types = true;
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), TestArrayExceptCb, &state), 0);

  absl::Cleanup unlock_and_free([&]() {
    for (hid_t* id_ptr : {&state.locked_src_id, &state.locked_dst_id}) {
      if (*id_ptr != H5I_INVALID_HID) {
        if (H5T_t* dt = LookupMutableType(*id_ptr)) {
          dt->shared->state = H5T_STATE_TRANSIENT;
        }
        H5I_dec_ref(*id_ptr);
        *id_ptr = H5I_INVALID_HID;
      }
    }
  });

  std::array<uint8_t, 8> buf{};
  std::array<float, 2> ovf = {1e20f, 3.0f};
  std::memcpy(buf.data(), ovf.data(), sizeof(ovf));

  ScopedErrorSilence silence;
  EXPECT_LT(H5Tconvert(src_arr.get(), dst_arr.get(), 1, buf.data(), nullptr,
                       dxpl.get()),
            0);
  EXPECT_EQ(state.calls, 1);
  EXPECT_NE(state.locked_src_id, H5I_INVALID_HID);
  EXPECT_NE(state.locked_dst_id, H5I_INVALID_HID);
}

TEST(H5TconvArrayTest, ConvArrayErrorPathsRejectInvalidInputs) {
  ScopedErrorSilence silence;
  constexpr hsize_t kDim2 = 2;
  constexpr std::array<hsize_t, 2> kDims2x3 = {2, 3};
  constexpr std::array<hsize_t, 2> kDims2x4 = {2, 4};

  ScopedType a_1d(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim2));
  ScopedType a_2x3(H5Tarray_create2(H5T_NATIVE_INT32, 2, kDims2x3.data()));
  ScopedType a_2x4(H5Tarray_create2(H5T_NATIVE_INT32, 2, kDims2x4.data()));
  ScopedType opq(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(a_1d.valid());
  ASSERT_TRUE(a_2x3.valid());
  ASSERT_TRUE(a_2x4.valid());
  ASSERT_TRUE(opq.valid());
  ScopedType a_opq(H5Tarray_create2(opq.get(), 1, &kDim2));
  ASSERT_TRUE(a_opq.valid());

  H5T_cdata_t cdata{};
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;

  const H5T_t* dt_1d = LookupType(a_1d.get());
  const H5T_t* dt_2x3 = LookupType(a_2x3.get());
  const H5T_t* dt_2x4 = LookupType(a_2x4.get());
  const H5T_t* dt_opq = LookupType(a_opq.get());

  // INIT failures.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_array(nullptr, dt_1d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_1d, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_1d, dt_2x3, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_2x3, dt_2x4, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_1d, dt_opq, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  // CONV failures.
  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(
      H5T__conv_array(nullptr, dt_1d, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_1d, nullptr, &cdata, &ctx, 1, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_array(dt_1d, dt_1d, &cdata, nullptr, 1, 0, 0, nullptr, nullptr),
      0);

  // Unknown cmd.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_array(dt_1d, dt_1d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
}

}  // namespace
}  // namespace hdf5
