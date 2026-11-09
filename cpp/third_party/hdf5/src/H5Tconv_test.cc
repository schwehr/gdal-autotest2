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
// Tests general datatype conversion, byte-order swapping (generic and
// size-optimized), no-op conversion, force-conversion state, and VLEN/ref
// buffer reclamation (third_party/hdf5/src/H5Tconv.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv.c

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, so internal structs must be parsed under
// NDEBUG to match binary layout. Also define H5__Float16 for C++20 before
// H5Tpkg.h.
#ifndef NDEBUG
#define H5T_CONV_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#ifdef H5_HAVE__FLOAT16
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5R_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Rpkg.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5T_CONV_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5T_CONV_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct VlenCounters {
  int allocs = 0;
  int frees = 0;
};

void* TestVlenAlloc(size_t size, void* alloc_info) {
  auto* counters = static_cast<VlenCounters*>(alloc_info);
  ++counters->allocs;
  return std::malloc(size);
}

void TestVlenFree(void* mem, void* free_info) {
  auto* counters = static_cast<VlenCounters*>(free_info);
  ++counters->frees;
  std::free(mem);
}

const H5T_t* LookupType(hid_t id) {
  return static_cast<const H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_t* LookupMutableType(hid_t id) {
  return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
}

H5T_conv_ctx_t MakeConvCtx(hid_t src_id = H5I_INVALID_HID,
                           hid_t dst_id = H5I_INVALID_HID) {
  H5T_conv_ctx_t ctx{};
  ctx.u.conv.dxpl_id = H5P_DEFAULT;
  ctx.u.conv.src_type_id = src_id;
  ctx.u.conv.dst_type_id = dst_id;
  return ctx;
}

ScopedType CreateIntTypeWithSize(hid_t base_id, size_t size_bytes, size_t prec,
                                 size_t offset = 0) {
  ScopedType t(H5Tcopy(base_id));
  if (!t.valid()) return ScopedType();
  if (H5Tset_size(t.get(), size_bytes) < 0) return ScopedType();
  if (H5Tset_precision(t.get(), prec) < 0) return ScopedType();
  if (offset > 0 && H5Tset_offset(t.get(), offset) < 0) return ScopedType();
  return t;
}

TEST(H5TconvTest, ReclaimVlenSequenceAndStringFreesCustomAllocations) {
  VlenCounters counters;
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), TestVlenAlloc, &counters,
                                    TestVlenFree, &counters),
            0);

  constexpr hsize_t kDim = 2;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Reclaim a variable-length integer sequence buffer.
  ScopedType vlen_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_int.valid());

  std::array<hvl_t, 2> seq_buf{};
  for (size_t i = 0; i < seq_buf.size(); ++i) {
    seq_buf[i].len = 3;
    seq_buf[i].p = TestVlenAlloc(3 * sizeof(int32_t), &counters);
    ASSERT_NE(seq_buf[i].p, nullptr);
    auto* vals = static_cast<int32_t*>(seq_buf[i].p);
    vals[0] = static_cast<int32_t>(i * 10 + 1);
    vals[1] = static_cast<int32_t>(i * 10 + 2);
    vals[2] = static_cast<int32_t>(i * 10 + 3);
  }

  ASSERT_GE(H5Treclaim(vlen_int.get(), space.get(), dxpl.get(), seq_buf.data()),
            0);
  EXPECT_EQ(counters.frees, 2);

  // 2. Reclaim a variable-length string buffer.
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlen_str.valid());
  ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);

  std::array<char*, 2> str_buf{};
  for (size_t i = 0; i < str_buf.size(); ++i) {
    str_buf[i] = static_cast<char*>(TestVlenAlloc(8, &counters));
    ASSERT_NE(str_buf[i], nullptr);
    std::memcpy(str_buf[i], "hello", 6);
  }

  ASSERT_GE(H5Treclaim(vlen_str.get(), space.get(), dxpl.get(), str_buf.data()),
            0);
  EXPECT_EQ(counters.frees, 4);
}

TEST(H5TconvTest, ReclaimReferenceBufferDestroysObjectRefs) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  std::array<H5R_ref_t, 2> refs{};
  ASSERT_GE(
      H5Rcreate_object(file.get(), "/grp", H5P_DEFAULT, &refs[0]), 0);
  ASSERT_GE(
      H5Rcreate_object(file.get(), "/grp", H5P_DEFAULT, &refs[1]), 0);

  constexpr hsize_t kDim = 2;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  EXPECT_GE(H5Treclaim(H5T_STD_REF, space.get(), H5P_DEFAULT, refs.data()), 0);
}

TEST(H5TconvTest, ReclaimCbErrorPathsFailCleanly) {
  ScopedErrorSilence silence;

  // Trigger H5T__vlen_reclaim failure via invalid type class H5T_NO_CLASS.
  H5T_shared_t bad_vlen_shared{};
  bad_vlen_shared.type = H5T_NO_CLASS;
  H5T_t bad_vlen_dt{};
  bad_vlen_dt.shared = &bad_vlen_shared;
  uint8_t dummy_elem = 0;
  H5T_vlen_alloc_info_t alloc_info{};
  EXPECT_LT(
      H5T_reclaim_cb(&dummy_elem, &bad_vlen_dt, 0, nullptr, &alloc_info), 0);

  // Trigger H5T__ref_reclaim failure via opaque reference with bogus loc_id.
  H5T_shared_t bad_ref_shared{};
  bad_ref_shared.type = H5T_REFERENCE;
  bad_ref_shared.u.atomic.u.r.opaque = true;
  H5T_t bad_ref_dt{};
  bad_ref_dt.shared = &bad_ref_shared;

  H5R_ref_priv_t bad_ref{};
  bad_ref.type = H5R_OBJECT2;
  bad_ref.loc_id = 99999999;
  EXPECT_LT(H5T_reclaim_cb(&bad_ref, &bad_ref_dt, 0, nullptr, nullptr), 0);
}

TEST(H5TconvTest, GetForceConvReflectsSharedFlag) {
  const H5T_t* native_dt = LookupType(H5T_NATIVE_INT32);
  ASSERT_NE(native_dt, nullptr);
  EXPECT_FALSE(H5T_get_force_conv(native_dt));

  ScopedType transient(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient.valid());
  H5T_t* mutable_dt = LookupMutableType(transient.get());
  ASSERT_NE(mutable_dt, nullptr);
  EXPECT_FALSE(H5T_get_force_conv(mutable_dt));

  mutable_dt->shared->force_conv = true;
  EXPECT_TRUE(H5T_get_force_conv(mutable_dt));
}

TEST(H5TconvTest, ReverseOrderHandlesLeBeComplexAndVax) {
  std::array<uint8_t, 4> src4 = {0x11, 0x22, 0x33, 0x44};
  std::array<uint8_t, 4> rev4{};

  // 1. Little-endian atomic type copies unchanged.
  ASSERT_GE(
      H5T__reverse_order(rev4.data(), src4.data(), LookupType(H5T_STD_I32LE)),
      0);
  EXPECT_THAT(rev4, ElementsAre(0x11, 0x22, 0x33, 0x44));

  // 2. Big-endian atomic type reverses all bytes.
  ASSERT_GE(
      H5T__reverse_order(rev4.data(), src4.data(), LookupType(H5T_STD_I32BE)),
      0);
  EXPECT_THAT(rev4, ElementsAre(0x44, 0x33, 0x22, 0x11));

  // 3. Big-endian complex type reverses real and imaginary parts separately.
  std::array<uint8_t, 8> csrc = {0x01, 0x02, 0x03, 0x04,
                                 0x05, 0x06, 0x07, 0x08};
  std::array<uint8_t, 8> crev{};
  ASSERT_GE(H5T__reverse_order(crev.data(), csrc.data(),
                               LookupType(H5T_COMPLEX_IEEE_F32BE)),
            0);
  EXPECT_THAT(crev,
              ElementsAre(0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05));

  // 4. VAX order swaps 2-byte chunks from the end.
  ScopedType vax_type(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(vax_type.valid());
  H5T_t* vax_dt = LookupMutableType(vax_type.get());
  ASSERT_NE(vax_dt, nullptr);
  vax_dt->shared->u.atomic.order = H5T_ORDER_VAX;

  ASSERT_GE(H5T__reverse_order(rev4.data(), src4.data(), vax_dt), 0);
  EXPECT_THAT(rev4, ElementsAre(0x33, 0x44, 0x11, 0x22));
}

TEST(H5TconvTest, ConvNoopHandlesInitConvFreeAndUnknownCmd) {
  std::array<int32_t, 3> vals = {10, -20, 30};
  ASSERT_GE(
      H5Tconvert(H5T_NATIVE_INT32, H5T_NATIVE_INT32, vals.size(), vals.data(),
                 nullptr, H5P_DEFAULT),
      0);
  EXPECT_THAT(vals, ElementsAre(10, -20, 30));

  H5T_cdata_t cdata{};
  const H5T_t* dt = LookupType(H5T_NATIVE_INT32);
  const H5T_conv_ctx_t ctx = MakeConvCtx();

  cdata.command = H5T_CONV_INIT;
  cdata.need_bkg = H5T_BKG_YES;
  ASSERT_GE(H5T__conv_noop(dt, dt, &cdata, &ctx, 1, 0, 0, vals.data(), nullptr),
            0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);

  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_noop(dt, dt, &cdata, &ctx, 1, 0, 0, vals.data(), nullptr),
            0);

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_noop(dt, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  ScopedErrorSilence silence;
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_noop(dt, dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
}

TEST(H5TconvTest, ConvOrderPublicCoversFallback3ByteAndComplexTypes) {
  // 3-byte integer BE <-> LE falls back from H5T__conv_order_opt to
  // H5T__conv_order.
  ScopedType i24be = CreateIntTypeWithSize(H5T_STD_I32BE, 3, 24);
  ScopedType i24le = CreateIntTypeWithSize(H5T_STD_I32LE, 3, 24);
  ASSERT_TRUE(i24be.valid());
  ASSERT_TRUE(i24le.valid());

  std::array<uint8_t, 6> ibuf = {0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC};
  ASSERT_GE(H5Tconvert(i24be.get(), i24le.get(), 2, ibuf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(ibuf, ElementsAre(0x33, 0x22, 0x11, 0xCC, 0xBB, 0xAA));
  ASSERT_GE(H5Tconvert(i24le.get(), i24be.get(), 2, ibuf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(ibuf, ElementsAre(0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC));

  // 3-byte bitfield BE -> LE exercises H5T_BITFIELD branch in H5T__conv_order.
  ScopedType b24be = CreateIntTypeWithSize(H5T_STD_B32BE, 3, 24);
  ScopedType b24le = CreateIntTypeWithSize(H5T_STD_B32LE, 3, 24);
  ASSERT_TRUE(b24be.valid());
  ASSERT_TRUE(b24le.valid());

  std::array<uint8_t, 6> bbuf = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  ASSERT_GE(H5Tconvert(b24be.get(), b24le.get(), 2, bbuf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(bbuf, ElementsAre(0x03, 0x02, 0x01, 0x06, 0x05, 0x04));

  // Complex F32 BE <-> LE exercises H5T_COMPLEX in H5T__conv_order (rejected by
  // H5T__conv_order_opt).
  std::array<uint8_t, 16> cf32 = {0x3f, 0x80, 0x00, 0x00,  // BE 1.0f
                                  0x40, 0x00, 0x00, 0x00,  // BE 2.0f
                                  0xc0, 0x40, 0x00, 0x00,  // BE -3.0f
                                  0x40, 0x80, 0x00, 0x00}; // BE 4.0f
  ASSERT_GE(H5Tconvert(H5T_COMPLEX_IEEE_F32BE, H5T_COMPLEX_IEEE_F32LE, 2,
                       cf32.data(), nullptr, H5P_DEFAULT),
            0);
  std::array<float, 4> expected_floats{};
  std::memcpy(expected_floats.data(), cf32.data(), cf32.size());
  EXPECT_FLOAT_EQ(expected_floats[0], 1.0f);
  EXPECT_FLOAT_EQ(expected_floats[1], 2.0f);
  EXPECT_FLOAT_EQ(expected_floats[2], -3.0f);
  EXPECT_FLOAT_EQ(expected_floats[3], 4.0f);

  // Direct H5T__conv_order on matching float and strided complex buffers.
  H5T_cdata_t cdata{};
  const H5T_conv_ctx_t ctx = MakeConvCtx();
  const H5T_t* f32be = LookupType(H5T_IEEE_F32BE);
  const H5T_t* f32le = LookupType(H5T_IEEE_F32LE);
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_order(f32be, f32le, &cdata, &ctx, 0, 0, 0, nullptr,
                            nullptr),
            0);
  std::array<uint8_t, 8> fstrided = {0x3f, 0x80, 0x00, 0x00,
                                     0xaa, 0xbb, 0xcc, 0xdd};
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_order(f32be, f32le, &cdata, &ctx, 1, 8, 0,
                            fstrided.data(), nullptr),
            0);
  EXPECT_THAT(fstrided,
              ElementsAre(0x00, 0x00, 0x80, 0x3f, 0xaa, 0xbb, 0xcc, 0xdd));
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_order(f32be, f32le, &cdata, &ctx, 0, 0, 0, nullptr,
                            nullptr),
            0);
}

TEST(H5TconvTest, ConvOrderRejectsInvalidInputsAndMismatchedFloatFields) {
  ScopedErrorSilence silence;
  const H5T_conv_ctx_t ctx = MakeConvCtx();
  H5T_cdata_t cdata{};

  const H5T_t* i32be = LookupType(H5T_STD_I32BE);
  const H5T_t* i32le = LookupType(H5T_STD_I32LE);
  const H5T_t* i16le = LookupType(H5T_STD_I16LE);

  // NULL types in INIT and CONV.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_order(nullptr, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_order(i32be, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(
      H5T__conv_order(nullptr, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  // Size mismatch and order mismatch.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_order(i32be, i16le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_order(i32be, i32be, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_order(i32le, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  // Non-atomic parents on src and dst.
  constexpr hsize_t kOne = 1;
  ScopedType arr_base(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kOne));
  ASSERT_TRUE(arr_base.valid());
  ScopedType bad_src(H5Tcomplex_create(H5T_IEEE_F32BE));
  ScopedType good_dst(H5Tcomplex_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(bad_src.valid());
  ASSERT_TRUE(good_dst.valid());
  H5T_t* bad_src_dt = LookupMutableType(bad_src.get());
  H5T_t* real_parent = bad_src_dt->shared->parent;
  bad_src_dt->shared->parent = LookupMutableType(arr_base.get());
  auto restore_parent =
      absl::MakeCleanup([&]() { bad_src_dt->shared->parent = real_parent; });
  EXPECT_LT(H5T__conv_order(bad_src_dt, LookupType(good_dst.get()), &cdata,
                            &ctx, 0, 0, 0, nullptr, nullptr),
            0);
  std::move(restore_parent).Invoke();

  H5T_t* bad_dst_dt = LookupMutableType(good_dst.get());
  H5T_t* real_dst_parent = bad_dst_dt->shared->parent;
  bad_dst_dt->shared->parent = LookupMutableType(arr_base.get());
  auto restore_dst_parent = absl::MakeCleanup(
      [&]() { bad_dst_dt->shared->parent = real_dst_parent; });
  EXPECT_LT(H5T__conv_order(bad_src_dt, bad_dst_dt, &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
            0);
  std::move(restore_dst_parent).Invoke();

  // Non-zero offsets.
  ScopedType off_src = CreateIntTypeWithSize(H5T_STD_I32BE, 4, 24, 4);
  ScopedType off_dst = CreateIntTypeWithSize(H5T_STD_I32LE, 4, 24, 4);
  ASSERT_TRUE(off_src.valid());
  ASSERT_TRUE(off_dst.valid());
  EXPECT_LT(H5T__conv_order(LookupType(off_src.get()), i32le, &cdata, &ctx, 0,
                            0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_order(i32be, LookupType(off_dst.get()), &cdata, &ctx, 0,
                            0, 0, nullptr, nullptr),
            0);

  // Float field mismatches on scalar float and complex base float.
  for (int field = 0; field < 8; ++field) {
    ScopedType f_src(H5Tcopy(H5T_IEEE_F32BE));
    ScopedType f_dst(H5Tcopy(H5T_IEEE_F32LE));
    ASSERT_TRUE(f_src.valid());
    ASSERT_TRUE(f_dst.valid());
    H5T_t* f_dt = LookupMutableType(f_src.get());
    auto& flt = f_dt->shared->u.atomic.u.f;
    switch (field) {
      case 0: ++flt.sign; break;
      case 1: ++flt.epos; break;
      case 2: ++flt.esize; break;
      case 3: ++flt.ebias; break;
      case 4: ++flt.mpos; break;
      case 5: ++flt.msize; break;
      case 6: flt.norm = H5T_NORM_NONE; break;
      case 7: flt.pad = H5T_PAD_ONE; break;
    }
    EXPECT_LT(H5T__conv_order(f_dt, LookupType(f_dst.get()), &cdata, &ctx, 0, 0,
                              0, nullptr, nullptr),
              0);

    ScopedType c_src(H5Tcomplex_create(f_src.get()));
    ScopedType c_dst(H5Tcomplex_create(f_dst.get()));
    ASSERT_TRUE(c_src.valid());
    ASSERT_TRUE(c_dst.valid());
    EXPECT_LT(H5T__conv_order(LookupType(c_src.get()), LookupType(c_dst.get()),
                              &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
              0);
  }

  // Unsupported datatype classes (array has atomic parent; time has atomic
  // representation).
  ScopedType arr_be(H5Tarray_create2(H5T_STD_I32BE, 1, &kOne));
  ScopedType arr_le(H5Tarray_create2(H5T_STD_I32LE, 1, &kOne));
  ASSERT_TRUE(arr_be.valid());
  ASSERT_TRUE(arr_le.valid());
  EXPECT_LT(H5T__conv_order(LookupType(arr_be.get()), LookupType(arr_le.get()),
                            &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_order(LookupType(H5T_UNIX_D32BE),
                            LookupType(H5T_UNIX_D32LE), &cdata, &ctx, 0, 0, 0,
                            nullptr, nullptr),
            0);

  // Unknown command.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_order(i32be, i32le, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
}

TEST(H5TconvTest, ConvOrderOptExercisesAllUnrolledSizesAndEdgePaths) {
  // Size 1 (I8 BE <-> LE: no-op swap).
  std::array<uint8_t, 5> b1 = {1, 2, 3, 4, 5};
  ASSERT_GE(
      H5Tconvert(H5T_STD_I8BE, H5T_STD_I8LE, b1.size(), b1.data(), nullptr,
                 H5P_DEFAULT),
      0);
  EXPECT_THAT(b1, ElementsAre(1, 2, 3, 4, 5));

  // Size 2 with nelmts = 23 (hits >= 20 unrolled block AND remainder loop).
  std::vector<uint8_t> b2(23 * 2);
  for (size_t i = 0; i < 23; ++i) {
    b2[2 * i] = static_cast<uint8_t>(i);
    b2[2 * i + 1] = static_cast<uint8_t>(0x80 + i);
  }
  ASSERT_GE(H5Tconvert(H5T_STD_I16BE, H5T_STD_I16LE, 23, b2.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 23; ++i) {
    EXPECT_EQ(b2[2 * i], static_cast<uint8_t>(0x80 + i));
    EXPECT_EQ(b2[2 * i + 1], static_cast<uint8_t>(i));
  }

  // Size 4 with nelmts = 23 (hits >= 20 unrolled block AND remainder loop).
  std::vector<uint8_t> b4(23 * 4);
  for (size_t i = 0; i < 23; ++i) {
    b4[4 * i + 0] = 0x10;
    b4[4 * i + 1] = 0x20;
    b4[4 * i + 2] = 0x30;
    b4[4 * i + 3] = static_cast<uint8_t>(i);
  }
  ASSERT_GE(H5Tconvert(H5T_STD_I32BE, H5T_STD_I32LE, 23, b4.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 23; ++i) {
    EXPECT_EQ(b4[4 * i + 0], static_cast<uint8_t>(i));
    EXPECT_EQ(b4[4 * i + 1], 0x30);
    EXPECT_EQ(b4[4 * i + 2], 0x20);
    EXPECT_EQ(b4[4 * i + 3], 0x10);
  }

  // Size 8 with nelmts = 13 (hits >= 10 unrolled block AND remainder loop).
  std::vector<uint8_t> b8(13 * 8);
  for (size_t i = 0; i < 13; ++i) {
    for (size_t k = 0; k < 8; ++k) {
      b8[8 * i + k] = static_cast<uint8_t>(i * 8 + k);
    }
  }
  ASSERT_GE(H5Tconvert(H5T_IEEE_F64BE, H5T_IEEE_F64LE, 13, b8.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 13; ++i) {
    for (size_t k = 0; k < 8; ++k) {
      EXPECT_EQ(b8[8 * i + k], static_cast<uint8_t>(i * 8 + (7 - k)));
    }
  }

  // Size 16 with nelmts = 13 (hits >= 10 unrolled block AND remainder loop).
  ScopedType i128be = CreateIntTypeWithSize(H5T_STD_I64BE, 16, 128);
  ScopedType i128le = CreateIntTypeWithSize(H5T_STD_I64LE, 16, 128);
  ASSERT_TRUE(i128be.valid());
  ASSERT_TRUE(i128le.valid());
  std::vector<uint8_t> b16(13 * 16);
  for (size_t i = 0; i < 13; ++i) {
    for (size_t k = 0; k < 16; ++k) {
      b16[16 * i + k] = static_cast<uint8_t>((i + k) & 0xff);
    }
  }
  ASSERT_GE(H5Tconvert(i128be.get(), i128le.get(), 13, b16.data(), nullptr,
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < 13; ++i) {
    for (size_t k = 0; k < 16; ++k) {
      EXPECT_EQ(b16[16 * i + k], static_cast<uint8_t>((i + (15 - k)) & 0xff));
    }
  }

  // H5T_REFERENCE no-op path in H5T__conv_order_opt (size must be <= 16,
  // e.g. 8-byte H5T_STD_REF_OBJ).
  H5T_cdata_t cdata{};
  const H5T_conv_ctx_t ctx = MakeConvCtx();
  const H5T_t* ref_dt = LookupType(H5T_STD_REF_OBJ);
  std::array<uint8_t, sizeof(hobj_ref_t)> ref_buf{};
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_order_opt(ref_dt, ref_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  cdata.command = H5T_CONV_CONV;
  EXPECT_GE(H5T__conv_order_opt(ref_dt, ref_dt, &cdata, &ctx, 1, 0, 0,
                                ref_buf.data(), nullptr),
            0);
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_order_opt(ref_dt, ref_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
}

TEST(H5TconvTest, ConvOrderOptRejectsInvalidInputsAndClasses) {
  ScopedErrorSilence silence;
  const H5T_conv_ctx_t ctx = MakeConvCtx();
  H5T_cdata_t cdata{};

  const H5T_t* i32be = LookupType(H5T_STD_I32BE);
  const H5T_t* i32le = LookupType(H5T_STD_I32LE);
  const H5T_t* i16le = LookupType(H5T_STD_I16LE);
  const H5T_t* i64le = LookupType(H5T_STD_I64LE);
  const H5T_t* ref_dt = LookupType(H5T_STD_REF_OBJ);

  // NULL args in INIT and CONV.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_order_opt(nullptr, i32le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(i32be, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_order_opt(nullptr, i32le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(i32be, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);

  // Reference dst mismatch in CONV.
  uint8_t dummy[64]{};
  EXPECT_LT(
      H5T__conv_order_opt(ref_dt, i64le, &cdata, &ctx, 1, 0, 0, dummy, nullptr),
      0);

  // Invalid conversion size in CONV (size == 3).
  ScopedType i24be = CreateIntTypeWithSize(H5T_STD_I32BE, 3, 24);
  ScopedType i24le = CreateIntTypeWithSize(H5T_STD_I32LE, 3, 24);
  ASSERT_TRUE(i24be.valid());
  ASSERT_TRUE(i24le.valid());
  EXPECT_LT(H5T__conv_order_opt(LookupType(i24be.get()),
                                LookupType(i24le.get()), &cdata, &ctx, 1, 0, 0,
                                dummy, nullptr),
            0);

  // INIT checks.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_order_opt(i32be, i16le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(LookupType(i24be.get()),
                                LookupType(i24le.get()), &cdata, &ctx, 0, 0, 0,
                                nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(i32be, i32be, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(ref_dt, i64le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
  EXPECT_LT(H5T__conv_order_opt(i64le, ref_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);

  // Non-atomic parents on src and dst.
  constexpr hsize_t kOne = 1;
  ScopedType arr_base(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kOne));
  ScopedType c_src(H5Tcomplex_create(H5T_IEEE_F32BE));
  ScopedType c_dst(H5Tcomplex_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(arr_base.valid());
  ASSERT_TRUE(c_src.valid());
  ASSERT_TRUE(c_dst.valid());
  EXPECT_LT(H5T__conv_order_opt(LookupType(c_src.get()),
                                LookupType(c_dst.get()), &cdata, &ctx, 0, 0, 0,
                                nullptr, nullptr),
            0);

  H5T_t* c_src_dt = LookupMutableType(c_src.get());
  H5T_t* orig_p = c_src_dt->shared->parent;
  c_src_dt->shared->parent = LookupMutableType(arr_base.get());
  auto restore_p = absl::MakeCleanup([&]() { c_src_dt->shared->parent = orig_p; });
  EXPECT_LT(H5T__conv_order_opt(c_src_dt, LookupType(c_dst.get()), &cdata, &ctx,
                                0, 0, 0, nullptr, nullptr),
            0);
  std::move(restore_p).Invoke();

  H5T_t* c_dst_dt = LookupMutableType(c_dst.get());
  H5T_t* orig_dp = c_dst_dt->shared->parent;
  c_dst_dt->shared->parent = LookupMutableType(arr_base.get());
  auto restore_dp =
      absl::MakeCleanup([&]() { c_dst_dt->shared->parent = orig_dp; });
  EXPECT_LT(H5T__conv_order_opt(c_src_dt, c_dst_dt, &cdata, &ctx, 0, 0, 0,
                                nullptr, nullptr),
            0);
  std::move(restore_dp).Invoke();

  // Offset mismatch.
  ScopedType off_src = CreateIntTypeWithSize(H5T_STD_I32BE, 4, 24, 4);
  ASSERT_TRUE(off_src.valid());
  EXPECT_LT(H5T__conv_order_opt(LookupType(off_src.get()), i32le, &cdata, &ctx,
                                0, 0, 0, nullptr, nullptr),
            0);

  // Float field mismatch.
  ScopedType flt_bad(H5Tcopy(H5T_IEEE_F32BE));
  ASSERT_TRUE(flt_bad.valid());
  LookupMutableType(flt_bad.get())->shared->u.atomic.u.f.ebias += 1;
  EXPECT_LT(H5T__conv_order_opt(LookupType(flt_bad.get()),
                                LookupType(H5T_IEEE_F32LE), &cdata, &ctx, 0, 0,
                                0, nullptr, nullptr),
            0);

  // Unknown cmd.
  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_order_opt(i32be, i32le, &cdata, &ctx, 0, 0, 0, nullptr,
                                nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
