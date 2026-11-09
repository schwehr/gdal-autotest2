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
// Tests compound datatype conversions (subset fast-copy optimization,
// optimized and general struct converters, nested/strided conversion,
// background preservation, member ID tracking, conversion recalculation, and
// error paths) in third_party/hdf5/src/H5Tconv_compound.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_compound.c

#include <algorithm>
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

// `:hdf5` is compiled with -DNDEBUG; match that macro around internal headers
// so struct layouts (e.g. H5T_path_t, H5T_t) match the compiled library.
#ifndef NDEBUG
#define H5TCONV_COMPOUND_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h skips typedef'ing H5__Float16 in C++, yet H5Tpkg.h uses it to
// declare unused internal +/-Inf globals under H5_HAVE__FLOAT16.
typedef uint16_t H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv_compound.h"
}  // extern "C"
#ifdef H5TCONV_COMPOUND_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_COMPOUND_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct FieldSpec {
  const char* name;
  size_t offset;
  hid_t type;
};

ScopedType BuildCompound(size_t total_size,
                         const std::vector<FieldSpec>& fields) {
  ScopedType comp(H5Tcreate(H5T_COMPOUND, total_size));
  if (!comp.valid()) {
    return ScopedType();
  }
  for (const auto& f : fields) {
    if (H5Tinsert(comp.get(), f.name, f.offset, f.type) < 0) {
      return ScopedType();
    }
  }
  return comp;
}

template <typename T>
void WriteField(std::vector<uint8_t>& buf, size_t elem_idx, size_t stride,
                size_t offset, const T& val) {
  std::memcpy(buf.data() + elem_idx * stride + offset, &val, sizeof(T));
}

template <typename T>
T ReadField(const std::vector<uint8_t>& buf, size_t elem_idx, size_t stride,
            size_t offset) {
  T val{};
  std::memcpy(&val, buf.data() + elem_idx * stride + offset, sizeof(T));
  return val;
}

// Custom opaque converter: copies up to min(src_size, dst_size) bytes and fails
// if the leading byte equals kFailSentinel.
constexpr uint8_t kFailSentinel = 0xEE;

herr_t CustomOpaqueConv(hid_t src_id, hid_t dst_id, H5T_cdata_t* cdata,
                        size_t nelmts, size_t buf_stride, size_t /*bkg_stride*/,
                        void* buf, void* /*bkg*/, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_NO;
      return 0;
    case H5T_CONV_FREE:
      return 0;
    case H5T_CONV_CONV: {
      const size_t src_sz = H5Tget_size(src_id);
      const size_t dst_sz = H5Tget_size(dst_id);
      const size_t stride = buf_stride ? buf_stride : std::max(src_sz, dst_sz);
      auto* bytes = static_cast<uint8_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        uint8_t* elem = bytes + i * stride;
        if (elem[0] == kFailSentinel) {
          return -1;
        }
        const uint8_t tag = elem[0];
        if (dst_sz > src_sz) {
          std::memset(elem + src_sz, tag, dst_sz - src_sz);
        }
      }
      return 0;
    }
    default:
      return -1;
  }
}

struct ConvCbState {
  int calls = 0;
  H5T_conv_ret_t action = H5T_CONV_HANDLED;
};

H5T_conv_ret_t CompoundMemberExceptionCb(H5T_conv_except_t /*except_type*/,
                                         hid_t /*src_id*/, hid_t /*dst_id*/,
                                         void* /*src_buf*/, void* dst_buf,
                                         void* user_data) {
  auto* state = static_cast<ConvCbState*>(user_data);
  ++state->calls;
  if (state->action == H5T_CONV_HANDLED && dst_buf != nullptr) {
    int16_t sentinel = -321;
    std::memcpy(dst_buf, &sentinel, sizeof(sentinel));
  }
  return state->action;
}

TEST(H5TconvCompoundTest, OptSubsetSrcAndDstFastCopyPreserveBackground) {
  // 1. Source is a prefix subset of destination (plus trailing padding in src)
  //    so H5T_SUBSET_SRC copies only memb[last].offset + memb[last].size bytes
  //    and leaves extra destination fields in bkg intact.
  struct SrcSub {
    int32_t a;
    double b;
    int64_t trailing_pad;
  };
  struct DstFull {
    int32_t a;
    double b;
    int32_t c;
  };
  ScopedType src_sub = BuildCompound(
      sizeof(SrcSub), {{"a", HOFFSET(SrcSub, a), H5T_NATIVE_INT32},
                       {"b", HOFFSET(SrcSub, b), H5T_NATIVE_DOUBLE}});
  ScopedType dst_full = BuildCompound(
      sizeof(DstFull), {{"a", HOFFSET(DstFull, a), H5T_NATIVE_INT32},
                        {"b", HOFFSET(DstFull, b), H5T_NATIVE_DOUBLE},
                        {"c", HOFFSET(DstFull, c), H5T_NATIVE_INT32}});
  ASSERT_TRUE(src_sub.valid());
  ASSERT_TRUE(dst_full.valid());

  constexpr size_t kCount = 3;
  std::vector<uint8_t> buf(kCount * sizeof(SrcSub), 0);
  std::vector<uint8_t> bkg(kCount * sizeof(DstFull), 0);
  for (size_t i = 0; i < kCount; ++i) {
    WriteField<int32_t>(buf, i, sizeof(SrcSub), HOFFSET(SrcSub, a),
                        static_cast<int32_t>(10 + i));
    WriteField<double>(buf, i, sizeof(SrcSub), HOFFSET(SrcSub, b), 1.5 + i);
    WriteField<int32_t>(bkg, i, sizeof(DstFull), HOFFSET(DstFull, c),
                        static_cast<int32_t>(900 + i));
  }
  static_assert(sizeof(DstFull) <= sizeof(SrcSub));
  ASSERT_GE(H5Tconvert(src_sub.get(), dst_full.get(), kCount, buf.data(),
                       bkg.data(), H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(ReadField<int32_t>(buf, i, sizeof(DstFull), HOFFSET(DstFull, a)),
              static_cast<int32_t>(10 + i));
    EXPECT_DOUBLE_EQ(
        ReadField<double>(buf, i, sizeof(DstFull), HOFFSET(DstFull, b)),
        1.5 + i);
    EXPECT_EQ(ReadField<int32_t>(buf, i, sizeof(DstFull), HOFFSET(DstFull, c)),
              static_cast<int32_t>(900 + i));
  }

  // 2. Destination is a prefix subset of source (H5T_SUBSET_DST).
  ScopedType src_wide = BuildCompound(24, {{"a", 0, H5T_NATIVE_INT32},
                                           {"b", 8, H5T_NATIVE_DOUBLE},
                                           {"c", 16, H5T_NATIVE_INT32}});
  ScopedType dst_narrow = BuildCompound(
      20, {{"a", 0, H5T_NATIVE_INT32}, {"b", 8, H5T_NATIVE_DOUBLE}});
  ASSERT_TRUE(src_wide.valid());
  ASSERT_TRUE(dst_narrow.valid());

  std::vector<uint8_t> buf2(kCount * 24, 0);
  std::vector<uint8_t> bkg2(kCount * 20, 0);
  for (size_t i = 0; i < kCount; ++i) {
    WriteField<int32_t>(buf2, i, 24, 0, static_cast<int32_t>(20 + i));
    WriteField<double>(buf2, i, 24, 8, 2.25 + i);
    WriteField<int32_t>(buf2, i, 24, 16, 999);
  }
  ASSERT_GE(H5Tconvert(src_wide.get(), dst_narrow.get(), kCount, buf2.data(),
                       bkg2.data(), H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(ReadField<int32_t>(buf2, i, 20, 0), static_cast<int32_t>(20 + i));
    EXPECT_DOUBLE_EQ(ReadField<double>(buf2, i, 20, 8), 2.25 + i);
  }

  // 3. Non-subset cases when member counts differ:
  //    (a) src_nmembs < dst_nmembs with member order mismatch, offset mismatch,
  //        and non-noop member conversion.
  //    (b) dst_nmembs < src_nmembs with order mismatch, offset mismatch, and
  //        non-noop member conversion.
  ScopedType s_small = BuildCompound(8, {{"only", 0, H5T_NATIVE_INT32}});
  ScopedType d_reordered = BuildCompound(
      12, {{"first", 0, H5T_NATIVE_INT32}, {"only", 4, H5T_NATIVE_INT32}});
  ScopedType d_shifted = BuildCompound(
      12, {{"only", 4, H5T_NATIVE_INT32}, {"extra", 8, H5T_NATIVE_INT32}});
  ScopedType d_conv = BuildCompound(
      16, {{"only", 0, H5T_NATIVE_DOUBLE}, {"extra", 8, H5T_NATIVE_INT32}});
  ASSERT_TRUE(s_small.valid());
  ASSERT_TRUE(d_reordered.valid());
  ASSERT_TRUE(d_shifted.valid());
  ASSERT_TRUE(d_conv.valid());

  std::vector<uint8_t> b_small(16, 0);
  std::vector<uint8_t> b_dst(16, 0);
  WriteField<int32_t>(b_small, 0, 8, 0, 42);
  WriteField<int32_t>(b_dst, 0, 12, 0, 77);
  ASSERT_GE(H5Tconvert(s_small.get(), d_reordered.get(), 1, b_small.data(),
                       b_dst.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int32_t>(b_small, 0, 12, 0), 77);
  EXPECT_EQ(ReadField<int32_t>(b_small, 0, 12, 4), 42);

  std::fill(b_small.begin(), b_small.end(), 0);
  std::fill(b_dst.begin(), b_dst.end(), 0);
  WriteField<int32_t>(b_small, 0, 8, 0, 55);
  WriteField<int32_t>(b_dst, 0, 12, 8, 88);
  ASSERT_GE(H5Tconvert(s_small.get(), d_shifted.get(), 1, b_small.data(),
                       b_dst.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int32_t>(b_small, 0, 12, 4), 55);
  EXPECT_EQ(ReadField<int32_t>(b_small, 0, 12, 8), 88);

  std::fill(b_small.begin(), b_small.end(), 0);
  std::fill(b_dst.begin(), b_dst.end(), 0);
  WriteField<int32_t>(b_small, 0, 8, 0, 7);
  ASSERT_GE(H5Tconvert(s_small.get(), d_conv.get(), 1, b_small.data(),
                       b_dst.data(), H5P_DEFAULT),
            0);
  EXPECT_DOUBLE_EQ(ReadField<double>(b_small, 0, 16, 0), 7.0);

  // Reverse direction (dst_nmembs < src_nmembs) breaking subset rules.
  std::fill(b_dst.begin(), b_dst.end(), 0);
  std::fill(b_small.begin(), b_small.end(), 0);
  WriteField<int32_t>(b_dst, 0, 12, 4, 61);
  ASSERT_GE(H5Tconvert(d_reordered.get(), s_small.get(), 1, b_dst.data(),
                       b_small.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int32_t>(b_dst, 0, 8, 0), 61);

  std::fill(b_dst.begin(), b_dst.end(), 0);
  std::fill(b_small.begin(), b_small.end(), 0);
  WriteField<int32_t>(b_dst, 0, 12, 4, 62);
  ASSERT_GE(H5Tconvert(d_shifted.get(), s_small.get(), 1, b_dst.data(),
                       b_small.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int32_t>(b_dst, 0, 8, 0), 62);

  std::fill(b_dst.begin(), b_dst.end(), 0);
  std::fill(b_small.begin(), b_small.end(), 0);
  WriteField<double>(b_dst, 0, 16, 0, 9.0);
  ASSERT_GE(H5Tconvert(d_conv.get(), s_small.get(), 1, b_dst.data(),
                       b_small.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int32_t>(b_dst, 0, 8, 0), 9);
}

TEST(H5TconvCompoundTest,
     OptConversionHandlesUnsortedDroppedAndGrowingMembers) {
  // Insert members out of offset order so H5T__sort_value sorts both src & dst.
  // src size = 24, dst size = 28 (> src size, yet growing member `g` 4->8 fits
  // inside src's 24 bytes so H5T__conv_struct_opt accepts the path).
  ScopedType src = BuildCompound(24, {{"g", 16, H5T_NATIVE_INT32},
                                      {"d", 8, H5T_NATIVE_DOUBLE},
                                      {"drop", 0, H5T_NATIVE_INT32}});
  ScopedType dst = BuildCompound(28, {{"pad", 16, H5T_NATIVE_DOUBLE},
                                      {"extra", 12, H5T_NATIVE_INT32},
                                      {"g", 4, H5T_NATIVE_INT64},
                                      {"d", 0, H5T_NATIVE_FLOAT}});
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  constexpr size_t kCount = 3;
  std::vector<uint8_t> buf(kCount * 28, 0);
  std::vector<uint8_t> bkg(kCount * 28, 0);
  for (size_t i = 0; i < kCount; ++i) {
    WriteField<int32_t>(buf, i, 24, 0, static_cast<int32_t>(100 + i));
    WriteField<double>(buf, i, 24, 8, 3.5 + i);
    WriteField<int32_t>(buf, i, 24, 16, static_cast<int32_t>(1000 + i));
    WriteField<int32_t>(bkg, i, 28, 12, static_cast<int32_t>(50 + i));
  }

  ASSERT_GE(H5Tconvert(src.get(), dst.get(), kCount, buf.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_FLOAT_EQ(ReadField<float>(buf, i, 28, 0),
                    static_cast<float>(3.5 + i));
    EXPECT_EQ(ReadField<int64_t>(buf, i, 28, 4),
              static_cast<int64_t>(1000 + i));
    EXPECT_EQ(ReadField<int32_t>(buf, i, 28, 12), static_cast<int32_t>(50 + i));
  }

  // Passing nullptr background to H5T__conv_struct_opt fails cleanly.
  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Tconvert(src.get(), dst.get(), 1, buf.data(), nullptr, H5P_DEFAULT), 0);
}

TEST(H5TconvCompoundTest, NoOptConversionHandlesDroppedShrinkingAndGrowing) {
  // src size = 8 bytes with two growing int16->int64 members (g1, g2).
  // In H5T__conv_struct_opt init: loop1 offset = 2 + 2 = 4; loop2 for g2 sets
  // offset = 2 and checks dst_memb->size (8) > src->size (8) - 2 (6), so opt
  // rejects and HDF5 selects H5T__conv_struct ("struct(no-opt)").
  ScopedType src = BuildCompound(8, {{"g2", 6, H5T_NATIVE_INT16},
                                     {"g1", 4, H5T_NATIVE_INT16},
                                     {"shrink", 2, H5T_NATIVE_INT16},
                                     {"drop", 0, H5T_NATIVE_INT16}});
  ScopedType dst = BuildCompound(28, {{"extra", 24, H5T_NATIVE_INT32},
                                      {"g2", 16, H5T_NATIVE_INT64},
                                      {"g1", 8, H5T_NATIVE_INT64},
                                      {"shrink", 0, H5T_NATIVE_INT8}});
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  constexpr size_t kCount = 3;
  std::vector<uint8_t> buf(kCount * 28, 0);
  std::vector<uint8_t> bkg(kCount * 28, 0);
  for (size_t i = 0; i < kCount; ++i) {
    WriteField<int16_t>(buf, i, 8, 0, static_cast<int16_t>(99 + i));
    WriteField<int16_t>(buf, i, 8, 2, static_cast<int16_t>(10 + i));
    WriteField<int16_t>(buf, i, 8, 4, static_cast<int16_t>(200 + i));
    WriteField<int16_t>(buf, i, 8, 6, static_cast<int16_t>(300 + i));
    WriteField<int32_t>(bkg, i, 28, 24, static_cast<int32_t>(700 + i));
  }

  ASSERT_GE(H5Tconvert(src.get(), dst.get(), kCount, buf.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
  for (size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(ReadField<int8_t>(buf, i, 28, 0), static_cast<int8_t>(10 + i));
    EXPECT_EQ(ReadField<int64_t>(buf, i, 28, 8), static_cast<int64_t>(200 + i));
    EXPECT_EQ(ReadField<int64_t>(buf, i, 28, 16),
              static_cast<int64_t>(300 + i));
    EXPECT_EQ(ReadField<int32_t>(buf, i, 28, 24),
              static_cast<int32_t>(700 + i));
  }
}

TEST(H5TconvCompoundTest, DatasetIoExercisesSubsetInfoAndPreserveBkg) {
  ScopedFile file = CreateInMemoryFile("compound_io.h5");
  ASSERT_TRUE(file.valid());

  // 1. Growing compound member dataset read uses H5T__conv_struct so
  //    H5Dio -> H5T_path_compound_subset -> H5T__conv_struct_subset executes.
  ScopedType file_type = BuildCompound(4, {{"x", 0, H5T_STD_I32LE}});
  ScopedType mem_wide = BuildCompound(8, {{"x", 0, H5T_NATIVE_INT64}});
  ASSERT_TRUE(file_type.valid());
  ASSERT_TRUE(mem_wide.valid());

  const std::array<hsize_t, 1> dims = {2};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset1(H5Dcreate2(file.get(), "grow", file_type.get(),
                                 space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
  ASSERT_TRUE(dset1.valid());
  constexpr auto kIn = std::to_array<int32_t>({11, -22});
  ASSERT_GE(H5Dwrite(dset1.get(), file_type.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kIn.data()),
            0);
  std::array<int64_t, 2> out_wide{};
  ASSERT_GE(H5Dread(dset1.get(), mem_wide.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out_wide.data()),
            0);
  EXPECT_THAT(out_wide, ElementsAre(11, -22));

  // 2. Reading into a superset struct with H5Pset_preserve preserves extra
  // field.
  struct DstRecord {
    int32_t x;
    int32_t keep;
  };
  ScopedType mem_superset =
      BuildCompound(sizeof(DstRecord),
                    {{"x", HOFFSET(DstRecord, x), H5T_NATIVE_INT32},
                     {"keep", HOFFSET(DstRecord, keep), H5T_NATIVE_INT32}});
  ASSERT_TRUE(mem_superset.valid());
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_preserve(dxpl.get(), true), 0);

  std::array<DstRecord, 2> out_rec = {{{0, 501}, {0, 502}}};
  ASSERT_GE(H5Dread(dset1.get(), mem_superset.get(), H5S_ALL, H5S_ALL,
                    dxpl.get(), out_rec.data()),
            0);
  EXPECT_EQ(out_rec[0].x, 11);
  EXPECT_EQ(out_rec[0].keep, 501);
  EXPECT_EQ(out_rec[1].x, -22);
  EXPECT_EQ(out_rec[1].keep, 502);
}

TEST(H5TconvCompoundTest, NestedGrowingCompoundExercisesStridedNoOptPath) {
  // Inner conversion grows (4 -> 8 bytes), so inner uses H5T__conv_struct.
  // Outer conversion keeps dst_outer->size <= src_outer->size, so outer uses
  // H5T__conv_struct_opt and invokes inner H5T__conv_struct with non-zero
  // buf_stride and bkg_stride!
  ScopedType inner_src = BuildCompound(4, {{"v", 0, H5T_NATIVE_INT32}});
  ScopedType inner_dst = BuildCompound(8, {{"v", 0, H5T_NATIVE_INT64}});
  ScopedType outer_src = BuildCompound(
      12, {{"inner", 0, inner_src.get()}, {"tag", 4, H5T_NATIVE_INT64}});
  ScopedType outer_dst = BuildCompound(
      12, {{"inner", 0, inner_dst.get()}, {"tag", 8, H5T_NATIVE_INT32}});
  ASSERT_TRUE(inner_src.valid());
  ASSERT_TRUE(inner_dst.valid());
  ASSERT_TRUE(outer_src.valid());
  ASSERT_TRUE(outer_dst.valid());

  constexpr size_t kCount = 2;
  std::vector<uint8_t> buf(kCount * 12, 0);
  std::vector<uint8_t> bkg(kCount * 12, 0);
  WriteField<int32_t>(buf, 0, 12, 0, 101);
  WriteField<int64_t>(buf, 0, 12, 4, 1);
  WriteField<int32_t>(buf, 1, 12, 0, 202);
  WriteField<int64_t>(buf, 1, 12, 4, 2);

  ASSERT_GE(H5Tconvert(outer_src.get(), outer_dst.get(), kCount, buf.data(),
                       bkg.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(ReadField<int64_t>(buf, 0, 12, 0), 101);
  EXPECT_EQ(ReadField<int32_t>(buf, 0, 12, 8), 1);
  EXPECT_EQ(ReadField<int64_t>(buf, 1, 12, 0), 202);
  EXPECT_EQ(ReadField<int32_t>(buf, 1, 12, 8), 2);
}

TEST(H5TconvCompoundTest, MemberIdsRecalcAndFailuresInBothConverters) {
  // 1. INIT with exception callback (`cdata->command == H5T_CONV_INIT &&
  //    conv_ctx->u.init.cb_struct.func`) registers member IDs and aborts on
  //    member conversion failure inside H5T__conv_struct_opt loop 1.
  {
    ScopedType s = BuildCompound(8, {{"init_cb_a", 0, H5T_NATIVE_INT64}});
    ScopedType d = BuildCompound(2, {{"init_cb_a", 0, H5T_NATIVE_INT16}});
    ASSERT_TRUE(s.valid());
    ASSERT_TRUE(d.valid());

    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ConvCbState cb_state{.action = H5T_CONV_HANDLED};
    ASSERT_GE(
        H5Pset_type_conv_cb(dxpl.get(), CompoundMemberExceptionCb, &cb_state),
        0);

    std::vector<uint8_t> buf(8, 0);
    std::vector<uint8_t> bkg(2, 0);
    WriteField<int64_t>(buf, 0, 8, 0, 100000);
    ASSERT_GE(
        H5Tconvert(s.get(), d.get(), 1, buf.data(), bkg.data(), dxpl.get()), 0);
    EXPECT_EQ(cb_state.calls, 1);
    EXPECT_EQ(ReadField<int16_t>(buf, 0, 2, 0), -321);

    cb_state.action = H5T_CONV_ABORT;
    WriteField<int64_t>(buf, 0, 8, 0, 100000);
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Tconvert(s.get(), d.get(), 1, buf.data(), bkg.data(), dxpl.get()), 0);
  }

  // 2. CONV with recalc + exception callback (`cdata->command == H5T_CONV_CONV
  //    && conv_ctx->u.conv.cb_struct.func`) promotes previously invalid IDs.
  {
    ScopedType s = BuildCompound(4, {{"conv_cb_x", 0, H5T_NATIVE_INT32}});
    ScopedType d = BuildCompound(2, {{"conv_cb_x", 0, H5T_NATIVE_INT16}});
    ScopedType dummy_a(H5Tcreate(H5T_OPAQUE, 1));
    ScopedType dummy_b(H5Tcreate(H5T_OPAQUE, 1));
    ASSERT_TRUE(s.valid());
    ASSERT_TRUE(d.valid());
    ASSERT_TRUE(dummy_a.valid());
    ASSERT_TRUE(dummy_b.valid());
    ASSERT_GE(H5Tset_tag(dummy_a.get(), "recalc_trigger_a"), 0);
    ASSERT_GE(H5Tset_tag(dummy_b.get(), "recalc_trigger_b"), 0);

    H5T_cdata_t* pcdata = nullptr;
    ASSERT_NE(H5Tfind(s.get(), d.get(), &pcdata), nullptr);
    ASSERT_NE(pcdata, nullptr);

    // Register & unregister an unrelated conversion to flip recalc = true.
    constexpr char kDummyName[] = "compound_test_dummy_trigger";
    absl::Cleanup unreg_dummy = [&] {
      H5Tunregister(H5T_PERS_HARD, kDummyName, H5I_INVALID_HID, H5I_INVALID_HID,
                    CustomOpaqueConv);
    };
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, kDummyName, dummy_a.get(),
                          dummy_b.get(), CustomOpaqueConv),
              0);

    ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
    ASSERT_TRUE(dxpl.valid());
    ConvCbState cb_state{.action = H5T_CONV_HANDLED};
    ASSERT_GE(
        H5Pset_type_conv_cb(dxpl.get(), CompoundMemberExceptionCb, &cb_state),
        0);

    std::vector<uint8_t> buf(4, 0);
    std::vector<uint8_t> bkg(2, 0);
    WriteField<int32_t>(buf, 0, 4, 0, 100000);
    ASSERT_GE(
        H5Tconvert(s.get(), d.get(), 1, buf.data(), bkg.data(), dxpl.get()), 0);
    EXPECT_EQ(cb_state.calls, 1);
    EXPECT_EQ(ReadField<int16_t>(buf, 0, 2, 0), -321);
  }

  // 3. Application member conversion (`tpath->conv.is_app`), ID reuse across
  //    recalc, member conversion failures in loop 1 and loop 2 of BOTH opt and
  //    no-opt converters, and recalc failure after unregistering member path.
  {
    ScopedType op_s(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType op_d(H5Tcreate(H5T_OPAQUE, 8));
    ScopedType op_big(H5Tcreate(H5T_OPAQUE, 8));
    ScopedType op_small(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(op_s.valid());
    ASSERT_TRUE(op_d.valid());
    ASSERT_TRUE(op_big.valid());
    ASSERT_TRUE(op_small.valid());
    ASSERT_GE(H5Tset_tag(op_s.get(), "comp_app_s4"), 0);
    ASSERT_GE(H5Tset_tag(op_d.get(), "comp_app_d8"), 0);
    ASSERT_GE(H5Tset_tag(op_big.get(), "comp_app_b8"), 0);
    ASSERT_GE(H5Tset_tag(op_small.get(), "comp_app_s4_dst"), 0);

    constexpr char kAppConv[] = "compound_test_app_opaque";
    bool registered_s_to_d = false;
    bool registered_big_to_small = false;
    absl::Cleanup unreg_all = [&] {
      if (registered_s_to_d) {
        H5Tunregister(H5T_PERS_HARD, kAppConv, H5I_INVALID_HID, H5I_INVALID_HID,
                      CustomOpaqueConv);
      }
      if (registered_big_to_small) {
        H5Tunregister(H5T_PERS_HARD, kAppConv, H5I_INVALID_HID, H5I_INVALID_HID,
                      CustomOpaqueConv);
      }
    };
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, kAppConv, op_s.get(), op_d.get(),
                          CustomOpaqueConv),
              0);
    registered_s_to_d = true;
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, kAppConv, op_big.get(), op_small.get(),
                          CustomOpaqueConv),
              0);
    registered_big_to_small = true;

    // Case A (opt): loop 1 (op_big->op_small) & loop 2 (op_s->op_d fits in 12).
    ScopedType c_opt_s = BuildCompound(
        12, {{"shrink", 0, op_big.get()}, {"grow", 8, op_s.get()}});
    ScopedType c_opt_d = BuildCompound(
        12, {{"shrink", 0, op_small.get()}, {"grow", 4, op_d.get()}});
    ASSERT_TRUE(c_opt_s.valid());
    ASSERT_TRUE(c_opt_d.valid());

    std::vector<uint8_t> buf_opt(12, 0x11);
    std::vector<uint8_t> bkg_opt(12, 0);
    ASSERT_GE(H5Tconvert(c_opt_s.get(), c_opt_d.get(), 1, buf_opt.data(),
                         bkg_opt.data(), H5P_DEFAULT),
              0);

    // Trigger recalc while keeping member IDs alive (ID reuse branch).
    {
      ScopedType trig1(H5Tcreate(H5T_OPAQUE, 2));
      ScopedType trig2(H5Tcreate(H5T_OPAQUE, 2));
      ASSERT_GE(H5Tset_tag(trig1.get(), "trig1"), 0);
      ASSERT_GE(H5Tset_tag(trig2.get(), "trig2"), 0);
      constexpr char kTrig[] = "compound_test_trig";
      absl::Cleanup unreg_trig = [&] {
        H5Tunregister(H5T_PERS_HARD, kTrig, H5I_INVALID_HID, H5I_INVALID_HID,
                      CustomOpaqueConv);
      };
      ASSERT_GE(H5Tregister(H5T_PERS_HARD, kTrig, trig1.get(), trig2.get(),
                            CustomOpaqueConv),
                0);
    }
    std::fill(buf_opt.begin(), buf_opt.end(), 0x22);
    ASSERT_GE(H5Tconvert(c_opt_s.get(), c_opt_d.get(), 1, buf_opt.data(),
                         bkg_opt.data(), H5P_DEFAULT),
              0);

    // Fail inside opt loop 2 (`grow` at offset 8).
    {
      ScopedErrorSilence silence;
      std::fill(buf_opt.begin(), buf_opt.end(), 0x11);
      buf_opt[8] = kFailSentinel;
      EXPECT_LT(H5Tconvert(c_opt_s.get(), c_opt_d.get(), 1, buf_opt.data(),
                           bkg_opt.data(), H5P_DEFAULT),
                0);
    }

    // Case B (no-opt): op_s->op_d (4->8) exceeds src size (4), forcing
    // H5T__conv_struct. Also test loop 1 & loop 2 failures in no-opt.
    ScopedType c_noopt_s = BuildCompound(
        12, {{"shrink", 0, op_big.get()}, {"grow", 8, op_s.get()}});
    // Shrink 8->4, grow 4->8? That fits in 12! Use 16-byte dst with two
    // growing op_s->op_d members plus shrinking op_big->op_small? Wait:
    // src size 16: {op_big @0 (8), op_s1 @8 (4), op_s2 @12 (4)} -> dst size 20:
    // {op_small @0 (4), op_d1 @4 (8), op_d2 @12 (8)}.
    // Opt check: loop1 offset = 4+4 = 8; loop2 op_s2: offset=4, 8 > 16-4 = 12
    // is false! To make opt reject: src={op_s1 @0 (4), op_s2 @4 (4)} size 8, OR
    // add another field? Wait: src={op_big @0 (8), op_s1 @8 (4), op_s2 @12 (4)}
    // still has src_size 16 >= 8+4! We need dst_memb > src_size - offset.
    // Just use a 20-byte dst opaque type or 4-byte + 4-byte + 4-byte:
    ScopedType op_tiny(H5Tcreate(H5T_OPAQUE, 2));
    ScopedType op_huge(H5Tcreate(H5T_OPAQUE, 12));
    ASSERT_TRUE(op_tiny.valid());
    ASSERT_TRUE(op_huge.valid());
    ASSERT_GE(H5Tset_tag(op_tiny.get(), "comp_app_t2"), 0);
    ASSERT_GE(H5Tset_tag(op_huge.get(), "comp_app_h12"), 0);
    bool registered_tiny_to_huge = false;
    absl::Cleanup unreg_tiny = [&] {
      if (registered_tiny_to_huge) {
        H5Tunregister(H5T_PERS_HARD, kAppConv, H5I_INVALID_HID, H5I_INVALID_HID,
                      CustomOpaqueConv);
      }
    };
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, kAppConv, op_tiny.get(), op_huge.get(),
                          CustomOpaqueConv),
              0);
    registered_tiny_to_huge = true;

    // src size = 12: {op_big @0 (8), op_tiny1 @8 (2), op_tiny2 @10 (2)}.
    // dst size = 28: {op_small @0 (4), op_huge1 @4 (12), op_huge2 @16 (12)}.
    // Opt check: loop1 offset = 2+2 = 4; loop2 tiny2: offset=2,
    // dst_memb->size (12) > src->size (12) - 2 (10) -> opt REJECTS!
    c_noopt_s = BuildCompound(12, {{"shrink", 0, op_big.get()},
                                   {"g1", 8, op_tiny.get()},
                                   {"g2", 10, op_tiny.get()}});
    ScopedType c_noopt_d = BuildCompound(28, {{"shrink", 0, op_small.get()},
                                              {"g1", 4, op_huge.get()},
                                              {"g2", 16, op_huge.get()}});
    ASSERT_TRUE(c_noopt_s.valid());
    ASSERT_TRUE(c_noopt_d.valid());

    std::vector<uint8_t> buf_noopt(28, 0x33);
    std::vector<uint8_t> bkg_noopt(28, 0);
    ASSERT_GE(H5Tconvert(c_noopt_s.get(), c_noopt_d.get(), 1, buf_noopt.data(),
                         bkg_noopt.data(), H5P_DEFAULT),
              0);

    // Trigger recalc on c_noopt_s -> c_noopt_d (success path).
    {
      ScopedType trig1(H5Tcreate(H5T_OPAQUE, 3));
      ScopedType trig2(H5Tcreate(H5T_OPAQUE, 3));
      ASSERT_GE(H5Tset_tag(trig1.get(), "trig3"), 0);
      ASSERT_GE(H5Tset_tag(trig2.get(), "trig4"), 0);
      constexpr char kTrig2[] = "compound_test_trig2";
      absl::Cleanup unreg_trig = [&] {
        H5Tunregister(H5T_PERS_HARD, kTrig2, H5I_INVALID_HID, H5I_INVALID_HID,
                      CustomOpaqueConv);
      };
      ASSERT_GE(H5Tregister(H5T_PERS_HARD, kTrig2, trig1.get(), trig2.get(),
                            CustomOpaqueConv),
                0);
    }
    std::fill(buf_noopt.begin(), buf_noopt.end(), 0x44);
    ASSERT_GE(H5Tconvert(c_noopt_s.get(), c_noopt_d.get(), 1, buf_noopt.data(),
                         bkg_noopt.data(), H5P_DEFAULT),
              0);

    // Fail inside no-opt loop 1 (`shrink` at offset 0).
    {
      ScopedErrorSilence silence;
      std::fill(buf_noopt.begin(), buf_noopt.end(), 0x33);
      buf_noopt[0] = kFailSentinel;
      EXPECT_LT(H5Tconvert(c_noopt_s.get(), c_noopt_d.get(), 1,
                           buf_noopt.data(), bkg_noopt.data(), H5P_DEFAULT),
                0);

      // Fail inside no-opt loop 2 (`g1` at offset 8).
      std::fill(buf_noopt.begin(), buf_noopt.end(), 0x33);
      buf_noopt[8] = kFailSentinel;
      EXPECT_LT(H5Tconvert(c_noopt_s.get(), c_noopt_d.get(), 1,
                           buf_noopt.data(), bkg_noopt.data(), H5P_DEFAULT),
                0);
    }
  }
}

TEST(H5TconvCompoundTest, IncompatibleMemberFailsInitialization) {
  ScopedType str4(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str4.valid());
  ASSERT_GE(H5Tset_size(str4.get(), 4), 0);

  ScopedType src = BuildCompound(4, {{"bad", 0, str4.get()}});
  ScopedType dst_same = BuildCompound(4, {{"bad", 0, H5T_NATIVE_INT32}});
  ScopedType dst_grow = BuildCompound(8, {{"bad", 0, H5T_NATIVE_INT64}});
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst_same.valid());
  ASSERT_TRUE(dst_grow.valid());

  ScopedErrorSilence silence;
  std::array<uint8_t, 8> buf{};
  std::array<uint8_t, 8> bkg{};
  EXPECT_LT(H5Tconvert(src.get(), dst_same.get(), 1, buf.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tconvert(src.get(), dst_grow.get(), 1, buf.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
}

TEST(H5TconvCompoundTest, DirectInternalCompoundApisCoverAllBranches) {
  ScopedType c_shrink_s = BuildCompound(
      8, {{"a", 0, H5T_NATIVE_INT32}, {"b", 4, H5T_NATIVE_INT32}});
  ScopedType c_shrink_d = BuildCompound(12, {{"a", 0, H5T_NATIVE_INT32},
                                             {"b", 4, H5T_NATIVE_INT32},
                                             {"c", 8, H5T_NATIVE_INT32}});
  ScopedType c_smaller_d = BuildCompound(4, {{"a", 0, H5T_NATIVE_INT32}});
  ASSERT_TRUE(c_shrink_s.valid());
  ASSERT_TRUE(c_shrink_d.valid());
  ASSERT_TRUE(c_smaller_d.valid());

  const auto* t_s = static_cast<const H5T_t*>(
      H5I_object_verify(c_shrink_s.get(), H5I_DATATYPE));
  const auto* t_d = static_cast<const H5T_t*>(
      H5I_object_verify(c_shrink_d.get(), H5I_DATATYPE));
  const auto* t_sd = static_cast<const H5T_t*>(
      H5I_object_verify(c_smaller_d.get(), H5I_DATATYPE));
  const auto* t_int = static_cast<const H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT, H5I_DATATYPE));
  ASSERT_NE(t_s, nullptr);
  ASSERT_NE(t_d, nullptr);
  ASSERT_NE(t_sd, nullptr);
  ASSERT_NE(t_int, nullptr);

  ScopedErrorSilence silence;
  H5T_conv_ctx_t ctx{};

  // 1. Error paths in H5T__conv_struct.
  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_struct(nullptr, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_struct(t_s, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_struct(t_int, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(
      H5T__conv_struct(t_s, t_int, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(
      H5T__conv_struct(nullptr, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_struct(t_s, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_struct(t_s, t_d, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_struct(t_s, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // 2. Direct H5T__conv_struct init, H5T__conv_struct_subset inspection,
  //    strided (buf_stride != 0, bkg_stride == 0), packed (dst <= src), and
  //    free.
  {
    H5T_cdata_t noopt_cd{};
    absl::Cleanup free_noopt = [&] {
      if (noopt_cd.priv != nullptr) {
        noopt_cd.command = H5T_CONV_FREE;
        H5T__conv_struct(t_s, t_d, &noopt_cd, &ctx, 0, 0, 0, nullptr, nullptr);
      }
    };
    noopt_cd.command = H5T_CONV_INIT;
    ASSERT_GE(
        H5T__conv_struct(t_s, t_d, &noopt_cd, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    const H5T_subset_info_t* sub_info = H5T__conv_struct_subset(&noopt_cd);
    ASSERT_NE(sub_info, nullptr);
    EXPECT_EQ(sub_info->subset, H5T_SUBSET_SRC);
    EXPECT_EQ(sub_info->copy_size, 8);

    std::vector<uint8_t> buf(2 * 16, 0);
    std::vector<uint8_t> bkg(2 * 12, 0);
    WriteField<int32_t>(buf, 0, 16, 0, 5);
    WriteField<int32_t>(buf, 0, 16, 4, 6);
    WriteField<int32_t>(buf, 1, 16, 0, 7);
    WriteField<int32_t>(buf, 1, 16, 4, 8);
    WriteField<int32_t>(bkg, 0, 12, 8, 91);
    WriteField<int32_t>(bkg, 1, 12, 8, 92);

    noopt_cd.command = H5T_CONV_CONV;
    ASSERT_GE(H5T__conv_struct(t_s, t_d, &noopt_cd, &ctx, 2, 16, 0, buf.data(),
                               bkg.data()),
              0);
    EXPECT_EQ(ReadField<int32_t>(buf, 0, 16, 0), 5);
    EXPECT_EQ(ReadField<int32_t>(buf, 0, 16, 8), 91);
    EXPECT_EQ(ReadField<int32_t>(buf, 1, 16, 0), 7);
    EXPECT_EQ(ReadField<int32_t>(buf, 1, 16, 8), 92);

    noopt_cd.command = H5T_CONV_FREE;
    EXPECT_GE(
        H5T__conv_struct(t_s, t_d, &noopt_cd, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    EXPECT_EQ(noopt_cd.priv, nullptr);
  }

  // Packed (buf_stride == 0, bkg_stride == 0) with dst->size <= src->size on
  // H5T__conv_struct.
  {
    H5T_cdata_t noopt_cd{};
    absl::Cleanup free_noopt = [&] {
      if (noopt_cd.priv != nullptr) {
        noopt_cd.command = H5T_CONV_FREE;
        H5T__conv_struct(t_s, t_sd, &noopt_cd, &ctx, 0, 0, 0, nullptr, nullptr);
      }
    };
    noopt_cd.command = H5T_CONV_INIT;
    ASSERT_GE(
        H5T__conv_struct(t_s, t_sd, &noopt_cd, &ctx, 0, 0, 0, nullptr, nullptr),
        0);
    std::vector<uint8_t> buf(2 * 8, 0);
    std::vector<uint8_t> bkg(2 * 4, 0);
    WriteField<int32_t>(buf, 0, 8, 0, 31);
    WriteField<int32_t>(buf, 1, 8, 0, 32);
    noopt_cd.command = H5T_CONV_CONV;
    ASSERT_GE(H5T__conv_struct(t_s, t_sd, &noopt_cd, &ctx, 2, 0, 0, buf.data(),
                               bkg.data()),
              0);
    EXPECT_EQ(ReadField<int32_t>(buf, 0, 4, 0), 31);
    EXPECT_EQ(ReadField<int32_t>(buf, 1, 4, 0), 32);
  }

  // 3. Error paths and strided execution in H5T__conv_struct_opt.
  cdata = {};
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_struct_opt(nullptr, t_d, &cdata, &ctx, 0, 0, 0, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(H5T__conv_struct_opt(t_s, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(
      H5T__conv_struct_opt(t_int, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_struct_opt(t_s, t_int, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_struct_opt(nullptr, t_d, &cdata, &ctx, 0, 0, 0, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(H5T__conv_struct_opt(t_s, nullptr, &cdata, &ctx, 0, 0, 0, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(H5T__conv_struct_opt(t_s, t_d, &cdata, nullptr, 0, 0, 0, nullptr,
                                 nullptr),
            0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(
      H5T__conv_struct_opt(t_s, t_d, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  {
    H5T_cdata_t opt_cd{};
    absl::Cleanup free_opt = [&] {
      if (opt_cd.priv != nullptr) {
        opt_cd.command = H5T_CONV_FREE;
        H5T__conv_struct_opt(t_s, t_sd, &opt_cd, &ctx, 0, 0, 0, nullptr,
                             nullptr);
      }
    };
    opt_cd.command = H5T_CONV_INIT;
    ASSERT_GE(H5T__conv_struct_opt(t_s, t_sd, &opt_cd, &ctx, 0, 0, 0, nullptr,
                                   nullptr),
              0);

    std::vector<uint8_t> buf(2 * 16, 0);
    std::vector<uint8_t> bkg(2 * 8, 0);
    WriteField<int32_t>(buf, 0, 16, 0, 41);
    WriteField<int32_t>(buf, 1, 16, 0, 42);
    opt_cd.command = H5T_CONV_CONV;
    // Non-zero buf_stride with zero bkg_stride exercises !bkg_stride branch.
    ASSERT_GE(H5T__conv_struct_opt(t_s, t_sd, &opt_cd, &ctx, 2, 16, 0,
                                   buf.data(), bkg.data()),
              0);
    EXPECT_EQ(ReadField<int32_t>(buf, 0, 16, 0), 41);
    EXPECT_EQ(ReadField<int32_t>(buf, 1, 16, 0), 42);

    opt_cd.command = H5T_CONV_FREE;
    EXPECT_GE(H5T__conv_struct_opt(t_s, t_sd, &opt_cd, &ctx, 0, 0, 0, nullptr,
                                   nullptr),
              0);
    EXPECT_EQ(opt_cd.priv, nullptr);
  }

  // 4. Local recalc failure on both H5T__conv_struct and H5T__conv_struct_opt
  //    without leaving a NULL-priv path in HDF5's global path table.
  {
    ScopedType op_local_a(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType op_local_b(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(op_local_a.valid());
    ASSERT_TRUE(op_local_b.valid());
    ASSERT_GE(H5Tset_tag(op_local_a.get(), "local_recalc_a"), 0);
    ASSERT_GE(H5Tset_tag(op_local_b.get(), "local_recalc_b"), 0);

    constexpr char kLocalConv[] = "compound_test_local_recalc";
    bool local_reg = false;
    absl::Cleanup unreg_local = [&] {
      if (local_reg) {
        H5Tunregister(H5T_PERS_HARD, kLocalConv, H5I_INVALID_HID,
                      H5I_INVALID_HID, CustomOpaqueConv);
      }
    };
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, kLocalConv, op_local_a.get(),
                          op_local_b.get(), CustomOpaqueConv),
              0);
    local_reg = true;

    ScopedType cs = BuildCompound(4, {{"m", 0, op_local_a.get()}});
    ScopedType cd = BuildCompound(4, {{"m", 0, op_local_b.get()}});
    ASSERT_TRUE(cs.valid());
    ASSERT_TRUE(cd.valid());
    const auto* t_cs =
        static_cast<const H5T_t*>(H5I_object_verify(cs.get(), H5I_DATATYPE));
    const auto* t_cd =
        static_cast<const H5T_t*>(H5I_object_verify(cd.get(), H5I_DATATYPE));
    ASSERT_NE(t_cs, nullptr);
    ASSERT_NE(t_cd, nullptr);

    H5T_cdata_t local_noopt{};
    H5T_cdata_t local_opt{};
    absl::Cleanup free_locals = [&] {
      if (local_noopt.priv != nullptr) {
        local_noopt.command = H5T_CONV_FREE;
        H5T__conv_struct(t_cs, t_cd, &local_noopt, &ctx, 0, 0, 0, nullptr,
                         nullptr);
      }
      if (local_opt.priv != nullptr) {
        local_opt.command = H5T_CONV_FREE;
        H5T__conv_struct_opt(t_cs, t_cd, &local_opt, &ctx, 0, 0, 0, nullptr,
                             nullptr);
      }
    };

    local_noopt.command = H5T_CONV_INIT;
    ASSERT_GE(H5T__conv_struct(t_cs, t_cd, &local_noopt, &ctx, 0, 0, 0, nullptr,
                               nullptr),
              0);
    local_opt.command = H5T_CONV_INIT;
    ASSERT_GE(H5T__conv_struct_opt(t_cs, t_cd, &local_opt, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
              0);

    ASSERT_GE(H5Tunregister(H5T_PERS_HARD, kLocalConv, op_local_a.get(),
                            op_local_b.get(), CustomOpaqueConv),
              0);
    local_reg = false;

    std::array<uint8_t, 4> buf{};
    std::array<uint8_t, 4> bkg{};
    local_noopt.recalc = true;
    local_noopt.command = H5T_CONV_CONV;
    EXPECT_LT(H5T__conv_struct(t_cs, t_cd, &local_noopt, &ctx, 1, 0, 0,
                               buf.data(), bkg.data()),
              0);
    EXPECT_EQ(local_noopt.priv, nullptr);

    local_opt.recalc = true;
    local_opt.command = H5T_CONV_CONV;
    EXPECT_LT(H5T__conv_struct_opt(t_cs, t_cd, &local_opt, &ctx, 1, 0, 0,
                                   buf.data(), bkg.data()),
              0);
    EXPECT_EQ(local_opt.priv, nullptr);
  }
}

}  // namespace
}  // namespace hdf5
