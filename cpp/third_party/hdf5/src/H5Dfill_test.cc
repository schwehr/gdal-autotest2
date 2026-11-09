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
// Tests dataset fill-value memory operations (H5Dfill / H5D__fill) and
// internal fill buffer initialization, VL refill, release, and termination
// routines (third_party/hdf5/src/H5Dfill.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dfill.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t / H5T_path_t inside H5Dpkg.h and H5Tpkg.h. Define NDEBUG
// around both `hdf5_internal_prelude.h` and internal headers so struct layouts
// match.
#ifndef NDEBUG
#define H5DFILL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5T_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DFILL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DFILL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;

// RAII wrapper for pushing/popping an HDF5 internal API context.
class ScopedApiContext {
 public:
  explicit ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct CustomAllocState {
  int alloc_calls = 0;
  int free_calls = 0;
  int fail_after = -1;  // Negative means never fail.
  std::vector<void*> live_ptrs;
};

void* CustomAlloc(size_t size, void* alloc_info) {
  auto* state = static_cast<CustomAllocState*>(alloc_info);
  const int idx = state->alloc_calls++;
  if (state->fail_after >= 0 && idx >= state->fail_after) {
    return nullptr;
  }
  void* ptr = std::malloc(size > 0 ? size : 1);
  if (ptr != nullptr) {
    state->live_ptrs.push_back(ptr);
  }
  return ptr;
}

void CustomFree(void* mem, void* free_info) {
  auto* state = static_cast<CustomAllocState*>(free_info);
  state->free_calls++;
  std::erase(state->live_ptrs, mem);
  std::free(mem);
}

// Custom converter whose H5T_CONV_INIT requests a background buffer and whose
// H5T_CONV_CONV intentionally fails so error branches inside H5D__fill /
// H5D__fill_refill_vl can be tested deterministically.
herr_t FailOnConvWithBkg(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                          size_t /*nelmts*/, size_t /*buf_stride*/,
                          size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                          hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_TEMP;
    return 0;
  }
  if (cdata->command == H5T_CONV_CONV) {
    return -1;
  }
  return 0;
}

// Custom converter succeeding in INIT without needing a background buffer.
herr_t NoopInitNoBkg(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                     size_t /*nelmts*/, size_t /*buf_stride*/,
                     size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                     hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_NO;
  }
  return 0;
}

TEST(H5DfillTest, PublicH5DfillZeroAndNonVlenConversions) {
  constexpr std::array<hsize_t, 2> kDims = {4, 5};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Invalid buffer and unset dataspace extent error paths.
  {
    ScopedErrorSilence silence;
    std::array<int32_t, 4> buf{};
    EXPECT_LT(H5Dfill(nullptr, H5T_NATIVE_INT32, nullptr, H5T_NATIVE_INT32,
                      space.get()),
              0);

    ScopedSpace no_extent(H5Screate(H5S_SIMPLE));
    ASSERT_TRUE(no_extent.valid());
    EXPECT_LT(H5Dfill(nullptr, H5T_NATIVE_INT32, buf.data(), H5T_NATIVE_INT32,
                      no_extent.get()),
              0);
  }

  // 2. Default zero-fill across H5S_ALL, hyperslab, points, NONE, SCALAR, NULL.
  {
    std::array<int32_t, 20> buf{};
    buf.fill(0x11223344);
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, buf.data(), H5T_NATIVE_INT32,
                      space.get()),
              0);
    EXPECT_THAT(buf, Each(0));

    buf.fill(-7);
    constexpr std::array<hsize_t, 2> kStart = {1, 2};
    constexpr std::array<hsize_t, 2> kCount = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kCount.data(), nullptr),
              0);
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, buf.data(), H5T_NATIVE_INT32,
                      space.get()),
              0);
    EXPECT_EQ(buf[0], -7);
    EXPECT_EQ(buf[1 * 5 + 2], 0);
    EXPECT_EQ(buf[2 * 5 + 3], 0);
    EXPECT_EQ(buf[3 * 5 + 4], -7);

    constexpr std::array<hsize_t, 2> kPt = {0, 4};
    ASSERT_GE(H5Sselect_elements(space.get(), H5S_SELECT_SET, 1, kPt.data()),
              0);
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, buf.data(), H5T_NATIVE_INT32,
                      space.get()),
              0);
    EXPECT_EQ(buf[4], 0);

    ASSERT_GE(H5Sselect_none(space.get()), 0);
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, buf.data(), H5T_NATIVE_INT32,
                      space.get()),
              0);

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    int32_t scalar_val = 99;
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, &scalar_val, H5T_NATIVE_INT32,
                      scalar.get()),
              0);
    EXPECT_EQ(scalar_val, 0);

    ScopedSpace null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    int32_t sentinel = 12345;
    ASSERT_GE(H5Dfill(nullptr, H5T_NATIVE_INT32, &sentinel, H5T_NATIVE_INT32,
                      null_space.get()),
              0);
    EXPECT_EQ(sentinel, 12345);
  }

  // 3. Large zero-fill element (> H5T_ELEM_BUF_SIZE=256) exercises H5WB heap.
  {
    constexpr size_t kLargeBytes = 320;
    ScopedType large_opq(H5Tcreate(H5T_OPAQUE, kLargeBytes));
    ASSERT_TRUE(large_opq.valid());
    ASSERT_GE(H5Tset_tag(large_opq.get(), "large_zero"), 0);

    constexpr std::array<hsize_t, 1> kDims3 = {3};
    ScopedSpace sp3(H5Screate_simple(1, kDims3.data(), nullptr));
    ASSERT_TRUE(sp3.valid());
    std::vector<uint8_t> large_buf(3 * kLargeBytes, 0xAB);
    ASSERT_GE(H5Dfill(nullptr, large_opq.get(), large_buf.data(),
                      large_opq.get(), sp3.get()),
              0);
    EXPECT_THAT(large_buf, Each(0));
  }

  // 4. Non-NULL fill: NOOP path & scalar numeric conversion (including long
  //    double zero-initialized to avoid MSan padding reads).
  {
    ASSERT_GE(H5Sselect_all(space.get()), 0);
    const int32_t fill_i32 = 42;
    std::array<int32_t, 20> i32_buf{};
    ASSERT_GE(H5Dfill(&fill_i32, H5T_NATIVE_INT32, i32_buf.data(),
                      H5T_NATIVE_INT32, space.get()),
              0);
    EXPECT_THAT(i32_buf, Each(42));

    std::array<double, 20> f64_buf{};
    ASSERT_GE(H5Dfill(&fill_i32, H5T_NATIVE_INT32, f64_buf.data(),
                      H5T_NATIVE_DOUBLE, space.get()),
              0);
    EXPECT_THAT(f64_buf, Each(42.0));

    long double fill_ld;
    std::memset(&fill_ld, 0, sizeof(fill_ld));
    fill_ld = 17.5L;
    ASSERT_GE(H5Dfill(&fill_ld, H5T_NATIVE_LDOUBLE, f64_buf.data(),
                      H5T_NATIVE_DOUBLE, space.get()),
              0);
    EXPECT_THAT(f64_buf, Each(17.5));
  }

  // 5. Large converted type (> 256 bytes) exercises H5WB_actual heap fallback.
  {
    constexpr hsize_t kArrLen = 80;  // 80 * 8 = 640 bytes > 256.
    ScopedType src_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kArrLen));
    ASSERT_TRUE(src_arr.valid());
    ScopedType dst_arr(H5Tarray_create2(H5T_NATIVE_DOUBLE, 1, &kArrLen));
    ASSERT_TRUE(dst_arr.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    ScopedSpace sp2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(sp2.valid());

    std::array<int32_t, kArrLen> fill_arr{};
    for (size_t i = 0; i < fill_arr.size(); ++i) {
      fill_arr[i] = static_cast<int32_t>(i + 1);
    }
    std::vector<double> out(2 * kArrLen, -1.0);
    ASSERT_GE(H5Dfill(fill_arr.data(), src_arr.get(), out.data(), dst_arr.get(),
                      sp2.get()),
              0);
    EXPECT_EQ(out[0], 1.0);
    EXPECT_EQ(out[kArrLen - 1], static_cast<double>(kArrLen));
    EXPECT_EQ(out[kArrLen], 1.0);
    EXPECT_EQ(out[2 * kArrLen - 1], static_cast<double>(kArrLen));
  }

  // 6. Non-VLEN compound requiring a background buffer (both small and > 256B
  //    to exercise bkg_elem_wb), plus unconvertible and failing conversions.
  {
    struct SrcCompound {
      int32_t a;
      std::array<uint8_t, 280> pad;
    };
    struct DstCompound {
      int32_t a;
      std::array<uint8_t, 280> pad;
      int32_t extra;
    };
    static_assert(sizeof(DstCompound) > H5T_ELEM_BUF_SIZE);

    ScopedType pad_arr(H5Tarray_create2(H5T_NATIVE_UINT8, 1,
                                        std::array<hsize_t, 1>{280}.data()));
    ASSERT_TRUE(pad_arr.valid());

    ScopedType src_cmp(H5Tcreate(H5T_COMPOUND, sizeof(SrcCompound)));
    ASSERT_TRUE(src_cmp.valid());
    ASSERT_GE(H5Tinsert(src_cmp.get(), "a", HOFFSET(SrcCompound, a),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(src_cmp.get(), "pad", HOFFSET(SrcCompound, pad),
                        pad_arr.get()),
              0);

    ScopedType dst_cmp(H5Tcreate(H5T_COMPOUND, sizeof(DstCompound)));
    ASSERT_TRUE(dst_cmp.valid());
    ASSERT_GE(H5Tinsert(dst_cmp.get(), "a", HOFFSET(DstCompound, a),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(dst_cmp.get(), "pad", HOFFSET(DstCompound, pad),
                        pad_arr.get()),
              0);
    ASSERT_GE(H5Tinsert(dst_cmp.get(), "extra", HOFFSET(DstCompound, extra),
                        H5T_NATIVE_INT32),
              0);

    SrcCompound fill_cmp{};
    fill_cmp.a = 77;
    fill_cmp.pad.fill(0x3C);

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    ScopedSpace sp2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(sp2.valid());

    std::vector<DstCompound> dst_vals(2);
    for (auto& v : dst_vals) {
      v.extra = 999;  // Cleared in bkg_elem_wb during fill conversion.
    }
    ASSERT_GE(H5Dfill(&fill_cmp, src_cmp.get(), dst_vals.data(), dst_cmp.get(),
                      sp2.get()),
              0);
    EXPECT_EQ(dst_vals[0].a, 77);
    EXPECT_EQ(dst_vals[1].a, 77);
    EXPECT_EQ(dst_vals[0].pad[0], 0x3C);
    EXPECT_EQ(dst_vals[1].pad[279], 0x3C);

    // Incompatible types (tpath == NULL) and failing custom non-VLEN converter.
    ScopedType opq_src(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_src.valid());
    ASSERT_GE(H5Tset_tag(opq_src.get(), "h5dfill_nv_src"), 0);
    ScopedType opq_dst(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_dst.valid());
    ASSERT_GE(H5Tset_tag(opq_dst.get(), "h5dfill_nv_dst"), 0);

    std::array<uint8_t, 8> opq_buf{};
    constexpr int32_t kIncompatibleFill = 5;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dfill(&kIncompatibleFill, H5T_NATIVE_INT32, opq_buf.data(),
                        opq_dst.get(), sp2.get()),
                0);
    }

    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dfill_nv_fail", opq_src.get(),
                          opq_dst.get(), FailOnConvWithBkg),
              0);
    absl::Cleanup unreg_nv = [&] {
      H5Tunregister(H5T_PERS_HARD, "h5dfill_nv_fail", opq_src.get(),
                    opq_dst.get(), FailOnConvWithBkg);
    };
    const std::array<uint8_t, 4> opq_fill = {1, 2, 3, 4};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dfill(opq_fill.data(), opq_src.get(), opq_buf.data(),
                        opq_dst.get(), sp2.get()),
                0);
    }
  }
}

TEST(H5DfillTest, PublicH5DfillVlenBackgroundAndFailurePaths) {
  constexpr std::array<hsize_t, 2> kDims = {3, 4};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. VLEN int32 fill with H5S_ALL and conversion to VLEN double.
  {
    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vl_i32.valid());
    ScopedType vl_f64(H5Tvlen_create(H5T_NATIVE_DOUBLE));
    ASSERT_TRUE(vl_f64.valid());

    std::array<int32_t, 3> seq = {10, 20, 30};
    const hvl_t fill_vl = {seq.size(), seq.data()};

    std::array<hvl_t, 12> out_i32{};
    ASSERT_GE(
        H5Dfill(&fill_vl, vl_i32.get(), out_i32.data(), vl_i32.get(), space.get()),
        0);
    absl::Cleanup reclaim_i32 = [&] {
      H5Treclaim(vl_i32.get(), space.get(), H5P_DEFAULT, out_i32.data());
    };
    for (const hvl_t& elem : out_i32) {
      ASSERT_EQ(elem.len, 3U);
      const auto* ptr = static_cast<const int32_t*>(elem.p);
      EXPECT_EQ(ptr[0], 10);
      EXPECT_EQ(ptr[2], 30);
    }

    // Hyperslab fill with conversion from int32 VLEN -> double VLEN.
    ScopedSpace sub(H5Screate_simple(2, kDims.data(), nullptr));
    ASSERT_TRUE(sub.valid());
    constexpr std::array<hsize_t, 2> kStart = {1, 1};
    constexpr std::array<hsize_t, 2> kCount = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(sub.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kCount.data(), nullptr),
              0);
    std::array<hvl_t, 12> out_f64{};
    ASSERT_GE(H5Dfill(&fill_vl, vl_i32.get(), out_f64.data(), vl_f64.get(),
                      sub.get()),
              0);
    absl::Cleanup reclaim_f64 = [&] {
      H5Treclaim(vl_f64.get(), sub.get(), H5P_DEFAULT, out_f64.data());
    };
    EXPECT_EQ(out_f64[0].p, nullptr);
    ASSERT_EQ(out_f64[1 * 4 + 1].len, 3U);
    EXPECT_EQ(static_cast<const double*>(out_f64[1 * 4 + 1].p)[1], 20.0);
  }

  // 2. Custom converter on VLEN-containing opaque-tagged VLEN types requiring
  //    background buffer and failing in H5T_CONV_CONV (covers lines 185-186
  //    and 195).
  {
    ScopedType opq_a(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_a.valid());
    ASSERT_GE(H5Tset_tag(opq_a.get(), "h5dfill_vl_a"), 0);
    ScopedType opq_b(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq_b.valid());
    ASSERT_GE(H5Tset_tag(opq_b.get(), "h5dfill_vl_b"), 0);

    ScopedType vl_a(H5Tvlen_create(opq_a.get()));
    ASSERT_TRUE(vl_a.valid());
    ScopedType vl_b(H5Tvlen_create(opq_b.get()));
    ASSERT_TRUE(vl_b.valid());

    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dfill_vl_fail_bkg", vl_a.get(),
                          vl_b.get(), FailOnConvWithBkg),
              0);
    absl::Cleanup unreg_vl = [&] {
      H5Tunregister(H5T_PERS_HARD, "h5dfill_vl_fail_bkg", vl_a.get(),
                    vl_b.get(), FailOnConvWithBkg);
    };

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    ScopedSpace sp2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(sp2.valid());

    std::array<uint8_t, 4> item = {9, 8, 7, 6};
    const hvl_t fill_vl = {1, item.data()};
    std::array<hvl_t, 2> dst_vl{};
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Dfill(&fill_vl, vl_a.get(), dst_vl.data(), vl_b.get(), sp2.get()), 0);
  }
}

TEST(H5DfillTest, InternalFillInitAndTermZeroAndNonVlenPaths) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Default zero-fill (fill->buf == nullptr):
  //    a) total_nelmts > 0 with caller buffer
  //    b) total_nelmts == 0 with custom allocator (success + failure)
  //    c) repeated internal zero_fill free-list allocations (!buf_avail then
  //       buf_avail)
  {
    const auto* i32_dt =
        static_cast<const H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
    ASSERT_NE(i32_dt, nullptr);

    H5O_fill_t zero_fill{};
    std::array<int32_t, 4> caller_buf{};
    caller_buf.fill(-1);

    H5D_fill_buf_info_t fb{};
    ASSERT_GE(H5D__fill_init(&fb, caller_buf.data(), nullptr, nullptr, nullptr,
                             nullptr, &zero_fill, i32_dt, 4, 64),
              0);
    EXPECT_TRUE(fb.use_caller_fill_buf);
    EXPECT_THAT(caller_buf, Each(0));
    EXPECT_GE(H5D__fill_term(&fb), 0);

    // total_nelmts == 0 + custom allocator succeeding, then freeing in term().
    CustomAllocState alloc_state;
    ASSERT_GE(H5D__fill_init(&fb, nullptr, CustomAlloc, &alloc_state,
                             CustomFree, &alloc_state, &zero_fill, i32_dt, 0,
                             64),
              0);
    EXPECT_FALSE(fb.use_caller_fill_buf);
    EXPECT_EQ(fb.fill_buf_size, 64U);
    EXPECT_EQ(alloc_state.alloc_calls, 1);
    EXPECT_GE(H5D__fill_term(&fb), 0);
    EXPECT_EQ(alloc_state.free_calls, 1);
    EXPECT_TRUE(alloc_state.live_ptrs.empty());

    // Custom allocator returning nullptr fails cleanly (line 458).
    CustomAllocState fail_state{.fail_after = 0};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__fill_init(&fb, nullptr, CustomAlloc, &fail_state,
                               CustomFree, &fail_state, &zero_fill, i32_dt, 2,
                               64),
                0);
    }

    // Prime byte size (73) ensures the first allocation sees !buf_avail
    // (CALLOC path) and after H5D__fill_term releases it to the zero_fill
    // free list, the second allocation sees buf_avail (MALLOC path).
    ScopedType opq73(H5Tcreate(H5T_OPAQUE, 73));
    ASSERT_TRUE(opq73.valid());
    const auto* opq73_dt =
        static_cast<const H5T_t*>(H5I_object_verify(opq73.get(), H5I_DATATYPE));
    ASSERT_NE(opq73_dt, nullptr);

    ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &zero_fill, opq73_dt, 1, 73),
              0);
    const std::vector<uint8_t> first_bytes(
        static_cast<const uint8_t*>(fb.fill_buf),
        static_cast<const uint8_t*>(fb.fill_buf) + 73);
    EXPECT_THAT(first_bytes, Each(0));
    ASSERT_GE(H5D__fill_term(&fb), 0);

    ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &zero_fill, opq73_dt, 1, 73),
              0);
    const std::vector<uint8_t> second_bytes(
        static_cast<const uint8_t*>(fb.fill_buf),
        static_cast<const uint8_t*>(fb.fill_buf) + 73);
    EXPECT_THAT(second_bytes, Each(0));
    ASSERT_GE(H5D__fill_term(&fb), 0);
  }

  // 2. Explicit non-VLEN fill (fill->buf != nullptr):
  //    caller_fill_buf, internal non_zero_fill, custom alloc success/fail, and
  //    total_nelmts == 0 vs > 0.
  {
    const auto* i32_dt =
        static_cast<const H5T_t*>(H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
    ASSERT_NE(i32_dt, nullptr);

    int32_t fill_val = 0x13572468;
    H5O_fill_t non_zero_fill{};
    non_zero_fill.size = sizeof(fill_val);
    non_zero_fill.buf = &fill_val;
    non_zero_fill.fill_defined = true;

    std::array<int32_t, 4> caller_buf{};
    H5D_fill_buf_info_t fb{};
    ASSERT_GE(H5D__fill_init(&fb, caller_buf.data(), nullptr, nullptr, nullptr,
                             nullptr, &non_zero_fill, i32_dt, 4, 64),
              0);
    EXPECT_THAT(caller_buf, Each(fill_val));
    ASSERT_GE(H5D__fill_term(&fb), 0);

    ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &non_zero_fill, i32_dt, 0, 16),
              0);
    EXPECT_EQ(fb.elmts_per_buf, 4U);
    EXPECT_EQ(static_cast<const int32_t*>(fb.fill_buf)[3], fill_val);
    ASSERT_GE(H5D__fill_term(&fb), 0);

    CustomAllocState ok_state;
    ASSERT_GE(H5D__fill_init(&fb, nullptr, CustomAlloc, &ok_state, CustomFree,
                             &ok_state, &non_zero_fill, i32_dt, 2, 16),
              0);
    EXPECT_EQ(static_cast<const int32_t*>(fb.fill_buf)[1], fill_val);
    ASSERT_GE(H5D__fill_term(&fb), 0);
    EXPECT_TRUE(ok_state.live_ptrs.empty());

    CustomAllocState fail_state{.fail_after = 0};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__fill_init(&fb, nullptr, CustomAlloc, &fail_state,
                               CustomFree, &fail_state, &non_zero_fill, i32_dt,
                               2, 16),
                0);
    }
  }
}

TEST(H5DfillTest, InternalFillInitRefillVlSuccessAndErrorPaths) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedFile file = CreateInMemoryFile("vl_fill_internal.h5");
  ASSERT_TRUE(file.valid());

  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_i32.valid());

  std::array<int32_t, 2> seq = {101, 202};
  const hvl_t fill_vl = {seq.size(), seq.data()};

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), vl_i32.get(), &fill_vl), 0);

  constexpr std::array<hsize_t, 1> kDims1 = {2};
  ScopedSpace sp1(H5Screate_simple(1, kDims1.data(), nullptr));
  ASSERT_TRUE(sp1.valid());

  ScopedDataset vl_dset_id(H5Dcreate2(file.get(), "vl_dset", vl_i32.get(),
                                      sp1.get(), H5P_DEFAULT, dcpl.get(),
                                      H5P_DEFAULT));
  ASSERT_TRUE(vl_dset_id.valid());
  auto* vl_dset =
      static_cast<H5D_t*>(H5VL_object_verify(vl_dset_id.get(), H5I_DATASET));
  ASSERT_NE(vl_dset, nullptr);

  const H5O_fill_t* dset_fill = &vl_dset->shared->dcpl_cache.fill;
  const H5T_t* dset_type = vl_dset->shared->type;
  ASSERT_NE(dset_fill->buf, nullptr);

  // 1. VLEN init + refill (nelmts == 1 and nelmts > 1) with default free-list
  //    alloc, caller_fill_buf, custom alloc, total_nelmts == 0, and
  //    alloc failure in both H5D__fill_init and H5D__fill_refill_vl.
  {
    H5D_fill_buf_info_t fb{};
    ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                             dset_fill, dset_type, 3, 256),
              0);
    EXPECT_TRUE(fb.has_vlen_fill_type);
    EXPECT_EQ(fb.elmts_per_buf, 3U);

    // nelmts == 1
    ASSERT_GE(H5D__fill_refill_vl(&fb, 1), 0);
    // nelmts > 1
    ASSERT_GE(H5D__fill_refill_vl(&fb, 3), 0);
    ASSERT_GE(H5D__fill_term(&fb), 0);

    // Caller buffer + total_nelmts == 0.
    std::vector<uint8_t> caller_buf(256, 0);
    ASSERT_GE(H5D__fill_init(&fb, caller_buf.data(), nullptr, nullptr, nullptr,
                             nullptr, dset_fill, dset_type, 0, 128),
              0);
    EXPECT_TRUE(fb.use_caller_fill_buf);
    ASSERT_GE(H5D__fill_refill_vl(&fb, fb.elmts_per_buf), 0);
    ASSERT_GE(H5D__fill_term(&fb), 0);

    // Custom allocator succeeding for both init and refill_vl, with synthetic
    // fill.type != NULL (exercises the `if (fb_info->fill->type)` reclaim
    // branch in H5D__fill_refill_vl).
    ScopedType vl_mem_type(H5Tcopy(vl_i32.get()));
    ASSERT_TRUE(vl_mem_type.valid());
    H5O_fill_t typed_fill = *dset_fill;
    typed_fill.type =
        static_cast<H5T_t*>(H5I_object_verify(vl_mem_type.get(), H5I_DATATYPE));
    ASSERT_NE(typed_fill.type, nullptr);

    CustomAllocState ok_alloc;
    ASSERT_GE(H5D__fill_init(&fb, nullptr, CustomAlloc, &ok_alloc, CustomFree,
                             &ok_alloc, &typed_fill, dset_type, 2, 128),
              0);
    ASSERT_GE(H5D__fill_refill_vl(&fb, 2), 0);
    ASSERT_GE(H5D__fill_term(&fb), 0);
    EXPECT_EQ(ok_alloc.alloc_calls, 2);
    EXPECT_EQ(ok_alloc.free_calls, 2);
    EXPECT_TRUE(ok_alloc.live_ptrs.empty());

    // Custom allocator failing immediately inside H5D__fill_init (line 361).
    CustomAllocState fail_init{.fail_after = 0};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__fill_init(&fb, nullptr, CustomAlloc, &fail_init,
                               CustomFree, &fail_init, dset_fill, dset_type, 2,
                               128),
                0);
    }

    // Custom allocator succeeding in init then failing in refill_vl (line 543).
    // H5D__fill_refill_vl fails after converting the element in fb.fill_buf into
    // memory VLEN form and before allocating `buf`, so reclaim element 0 before
    // terminating `fb`.
    CustomAllocState fail_refill{.fail_after = 1};
    ASSERT_GE(H5D__fill_init(&fb, nullptr, CustomAlloc, &fail_refill,
                             CustomFree, &fail_refill, dset_fill, dset_type, 2,
                             128),
              0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__fill_refill_vl(&fb, 2), 0);
    }
    EXPECT_GE(H5T_vlen_reclaim_elmt(fb.fill_buf, fb.mem_type), 0);
    ASSERT_GE(H5D__fill_term(&fb), 0);
    EXPECT_TRUE(fail_refill.live_ptrs.empty());
  }

  // 2. Corrupt VLEN global-heap reference inside fill.buf (keep 4-byte seq_len
  //    small) so H5D__fill_refill_vl hits fill_to_mem conversion error (line
  //    520), restoring bytes via absl::Cleanup before file close.
  {
    const auto fill_len =
        static_cast<size_t>(vl_dset->shared->dcpl_cache.fill.size);
    ASSERT_GT(fill_len, 4U);
    std::vector<uint8_t> saved(fill_len);
    std::memcpy(saved.data(), vl_dset->shared->dcpl_cache.fill.buf, fill_len);
    absl::Cleanup restore_fill = [&] {
      std::memcpy(vl_dset->shared->dcpl_cache.fill.buf, saved.data(), fill_len);
    };
    std::memset(static_cast<uint8_t*>(vl_dset->shared->dcpl_cache.fill.buf) + 4,
                0xFF, fill_len - 4);

    H5D_fill_buf_info_t fb{};
    ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                             dset_fill, dset_type, 2, 128),
              0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__fill_refill_vl(&fb, 2), 0);
    }
    ASSERT_GE(H5D__fill_term(&fb), 0);
  }

  // 3. Isolated VLEN and Compound {VLEN, OPAQUE} paths covering missing tpath
  //    errors (lines 366 and 370), both bkg buffer sizing branches (lines 379
  //    and 381), bkg zeroing in H5D__fill_refill_vl (lines 514-515 and 528-529),
  //    and mem_to_dset conversion failure (line 550).
  {
    struct VlOpaque {
      hvl_t vl;
      std::array<uint8_t, 4> opq;
    };

    // 3b first, using a dedicated VLEN(INT16) dataset so mem_to_dset uses
    // soft H5T__conv_vlen (need_bkg == H5T_BKG_NO) while fill_to_mem uses our
    // registered hard converter (need_bkg == H5T_BKG_TEMP), hitting line 381
    // (bkg_buf_size = max_elmt_size) and lines 514-515/520 cleanly.
    {
      ScopedType vl_i16(H5Tvlen_create(H5T_NATIVE_INT16));
      ASSERT_TRUE(vl_i16.valid());
      std::array<int16_t, 2> seq16 = {11, 22};
      const hvl_t fill_vl16 = {seq16.size(), seq16.data()};

      ScopedPlist dcpl16(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl16.valid());
      ASSERT_GE(H5Pset_fill_value(dcpl16.get(), vl_i16.get(), &fill_vl16), 0);

      ScopedDataset vl16_dset_id(H5Dcreate2(file.get(), "vl16_3b", vl_i16.get(),
                                            sp1.get(), H5P_DEFAULT,
                                            dcpl16.get(), H5P_DEFAULT));
      ASSERT_TRUE(vl16_dset_id.valid());
      auto* vl16_dset = static_cast<H5D_t*>(
          H5VL_object_verify(vl16_dset_id.get(), H5I_DATASET));
      ASSERT_NE(vl16_dset, nullptr);
      H5T_t* disk_vl16_dt = vl16_dset->shared->type;
      const H5O_fill_t* vl16_fill = &vl16_dset->shared->dcpl_cache.fill;

      ScopedType disk_vl16_tid(
          H5I_register(H5I_DATATYPE, H5T_copy(disk_vl16_dt, H5T_COPY_ALL),
                       /*app_ref=*/true));
      ASSERT_TRUE(disk_vl16_tid.valid());
      ScopedType mem_vl16_tid(H5I_register(
          H5I_DATATYPE, H5T_copy(disk_vl16_dt, H5T_COPY_TRANSIENT),
          /*app_ref=*/true));
      ASSERT_TRUE(mem_vl16_tid.valid());

      ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dfill_vl16_f2m_bkg",
                            disk_vl16_tid.get(), mem_vl16_tid.get(),
                            FailOnConvWithBkg),
                0);
      absl::Cleanup unreg_vl16 = [&] {
        H5Tunregister(H5T_PERS_HARD, "h5dfill_vl16_f2m_bkg",
                      disk_vl16_tid.get(), mem_vl16_tid.get(),
                      FailOnConvWithBkg);
      };

      H5D_fill_buf_info_t fb{};
      ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                               vl16_fill, disk_vl16_dt, 3, 256),
                0);
      EXPECT_EQ(fb.bkg_buf_size, fb.max_elmt_size);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5D__fill_refill_vl(&fb, 3), 0);
      }
      ASSERT_GE(H5D__fill_term(&fb), 0);
    }

    // 3c: dedicated fresh compound dataset ("cmp_3c") whose fill value is set
    // using a 1-member subset type {"vl"} so H5Dcreate2 caches only
    // ({"vl"} -> disk_3c), never (mem_3c {"vl","opq"} -> disk_3c). Registering
    // hard NoopInitNoBkg on (disk_3c -> mem_3c) lets fill_to_mem succeed while
    // mem_to_dset fails lookup when memb[1] has force_conv = true (line 370).
    {
      ScopedType vl_only_dt(H5Tcreate(H5T_COMPOUND, sizeof(VlOpaque)));
      ASSERT_TRUE(vl_only_dt.valid());
      ASSERT_GE(
          H5Tinsert(vl_only_dt.get(), "vl", HOFFSET(VlOpaque, vl), vl_i32.get()),
          0);

      ScopedType opq_3c(H5Tcreate(H5T_OPAQUE, 4));
      ASSERT_TRUE(opq_3c.valid());
      ASSERT_GE(H5Tset_tag(opq_3c.get(), "h5dfill_3c_opq"), 0);

      ScopedType cmp_3c_dt(H5Tcreate(H5T_COMPOUND, sizeof(VlOpaque)));
      ASSERT_TRUE(cmp_3c_dt.valid());
      ASSERT_GE(
          H5Tinsert(cmp_3c_dt.get(), "vl", HOFFSET(VlOpaque, vl), vl_i32.get()),
          0);
      ASSERT_GE(H5Tinsert(cmp_3c_dt.get(), "opq", HOFFSET(VlOpaque, opq),
                          opq_3c.get()),
                0);

      VlOpaque cmp_3c_val{{seq.size(), seq.data()}, {9, 8, 7, 6}};
      ScopedPlist cmp_3c_dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(cmp_3c_dcpl.valid());
      ASSERT_GE(
          H5Pset_fill_value(cmp_3c_dcpl.get(), vl_only_dt.get(), &cmp_3c_val),
          0);

      ScopedDataset cmp_3c_dset_id(H5Dcreate2(file.get(), "cmp_3c",
                                              cmp_3c_dt.get(), sp1.get(),
                                              H5P_DEFAULT, cmp_3c_dcpl.get(),
                                              H5P_DEFAULT));
      ASSERT_TRUE(cmp_3c_dset_id.valid());
      auto* cmp_3c_dset = static_cast<H5D_t*>(
          H5VL_object_verify(cmp_3c_dset_id.get(), H5I_DATASET));
      ASSERT_NE(cmp_3c_dset, nullptr);
      H5T_t* disk_3c_dt = cmp_3c_dset->shared->type;
      const H5O_fill_t* cmp_3c_fill = &cmp_3c_dset->shared->dcpl_cache.fill;

      ScopedType disk_3c_tid(
          H5I_register(H5I_DATATYPE, H5T_copy(disk_3c_dt, H5T_COPY_ALL),
                       /*app_ref=*/true));
      ASSERT_TRUE(disk_3c_tid.valid());
      ScopedType mem_3c_tid(
          H5I_register(H5I_DATATYPE, H5T_copy(disk_3c_dt, H5T_COPY_TRANSIENT),
                       /*app_ref=*/true));
      ASSERT_TRUE(mem_3c_tid.valid());

      ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dfill_3c_f2m_ok",
                            disk_3c_tid.get(), mem_3c_tid.get(), NoopInitNoBkg),
                0);
      absl::Cleanup unreg_3c = [&] {
        H5Tunregister(H5T_PERS_HARD, "h5dfill_3c_f2m_ok", disk_3c_tid.get(),
                      mem_3c_tid.get(), NoopInitNoBkg);
      };

      ASSERT_EQ(disk_3c_dt->shared->u.compnd.nmembs, 2U);
      H5T_t* memb1_3c = disk_3c_dt->shared->u.compnd.memb[1].type;
      ASSERT_NE(memb1_3c, nullptr);
      const bool orig_fc_3c = memb1_3c->shared->force_conv;
      absl::Cleanup restore_fc_3c = [&] {
        memb1_3c->shared->force_conv = orig_fc_3c;
      };
      memb1_3c->shared->force_conv = true;

      H5D_fill_buf_info_t fb{};
      herr_t rc = 0;
      {
        ScopedErrorSilence silence;
        rc = H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                            cmp_3c_fill, disk_3c_dt, 2, 256);
      }
      if (rc >= 0) {
        (void)H5D__fill_term(&fb);
      }
      EXPECT_LT(rc, 0);
    }

    ScopedType opq(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opq.valid());
    ASSERT_GE(H5Tset_tag(opq.get(), "h5dfill_cmp_opq"), 0);

    ScopedType cmp_dt(H5Tcreate(H5T_COMPOUND, sizeof(VlOpaque)));
    ASSERT_TRUE(cmp_dt.valid());
    ASSERT_GE(H5Tinsert(cmp_dt.get(), "vl", HOFFSET(VlOpaque, vl), vl_i32.get()),
              0);
    ASSERT_GE(H5Tinsert(cmp_dt.get(), "opq", HOFFSET(VlOpaque, opq), opq.get()),
              0);

    VlOpaque cmp_fill_val{{seq.size(), seq.data()}, {1, 2, 3, 4}};
    ScopedPlist cmp_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(cmp_dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(cmp_dcpl.get(), cmp_dt.get(), &cmp_fill_val),
              0);

    ScopedDataset cmp_dset_id(H5Dcreate2(file.get(), "cmp_vl", cmp_dt.get(),
                                         sp1.get(), H5P_DEFAULT, cmp_dcpl.get(),
                                         H5P_DEFAULT));
    ASSERT_TRUE(cmp_dset_id.valid());
    auto* cmp_dset =
        static_cast<H5D_t*>(H5VL_object_verify(cmp_dset_id.get(), H5I_DATASET));
    ASSERT_NE(cmp_dset, nullptr);
    H5T_t* disk_cmp_dt = cmp_dset->shared->type;
    const H5O_fill_t* cmp_fill = &cmp_dset->shared->dcpl_cache.fill;

    // 3a. Missing fill_to_mem_tpath (line 366) via force_conv on opaque member.
    ASSERT_EQ(disk_cmp_dt->shared->u.compnd.nmembs, 2U);
    H5T_t* memb1 = disk_cmp_dt->shared->u.compnd.memb[1].type;
    ASSERT_NE(memb1, nullptr);
    const bool orig_fc = memb1->shared->force_conv;
    {
      absl::Cleanup restore_fc = [&] { memb1->shared->force_conv = orig_fc; };
      memb1->shared->force_conv = true;

      H5D_fill_buf_info_t fb{};
      herr_t rc = 0;
      {
        ScopedErrorSilence silence;
        rc = H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                            cmp_fill, disk_cmp_dt, 2, 256);
      }
      if (rc >= 0) {
        (void)H5D__fill_term(&fb);
      }
      EXPECT_LT(rc, 0);
    }

    // Wrap exact copies of disk_cmp_dt and its transient mem_type so we can
    // attach isolated hard converters only to this compound path pair.
    ScopedType disk_tid(H5I_register(
        H5I_DATATYPE, H5T_copy(disk_cmp_dt, H5T_COPY_ALL), /*app_ref=*/true));
    ASSERT_TRUE(disk_tid.valid());
    ScopedType mem_tid(H5I_register(
        H5I_DATATYPE, H5T_copy(disk_cmp_dt, H5T_COPY_TRANSIENT),
        /*app_ref=*/true));
    ASSERT_TRUE(mem_tid.valid());

    // 3d. mem_to_dset needs bkg (line 379) AND fill_to_mem needs bkg (soft
    //     compound path sets H5T_BKG_YES): verify both bkg resets and
    //     mem_to_dset conversion error in H5D__fill_refill_vl (lines 514-515,
    //     528-529, 550) plus clean VLEN reclamation for both fill->type == nullptr
    //     and fill->type != nullptr.
    {
      ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5dfill_m2d_bkg_fail",
                            mem_tid.get(), disk_tid.get(), FailOnConvWithBkg),
                0);
      absl::Cleanup unreg_m2d = [&] {
        H5Tunregister(H5T_PERS_HARD, "h5dfill_m2d_bkg_fail", mem_tid.get(),
                      disk_tid.get(), FailOnConvWithBkg);
      };

      H5D_fill_buf_info_t fb{};
      ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                               cmp_fill, disk_cmp_dt, 2, 256),
                0);
      EXPECT_EQ(fb.bkg_buf_size, fb.elmts_per_buf * fb.max_elmt_size);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5D__fill_refill_vl(&fb, 2), 0);
      }
      ASSERT_GE(H5D__fill_term(&fb), 0);

      // Repeat with fill->type != nullptr so both branches of `done:` reclaim
      // are exercised on failure.
      H5O_fill_t typed_cmp_fill = *cmp_fill;
      typed_cmp_fill.type =
          static_cast<H5T_t*>(H5I_object_verify(mem_tid.get(), H5I_DATATYPE));
      ASSERT_NE(typed_cmp_fill.type, nullptr);
      ASSERT_GE(H5D__fill_init(&fb, nullptr, nullptr, nullptr, nullptr, nullptr,
                               &typed_cmp_fill, disk_cmp_dt, 2, 256),
                0);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5D__fill_refill_vl(&fb, 2), 0);
      }
      ASSERT_GE(H5D__fill_term(&fb), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
