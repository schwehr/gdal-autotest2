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
// Tests enumeration datatype conversions (enum-to-enum O(1) perfect-hash and
// O(log N) binary-search paths, exception callbacks, compound member recursive
// reuse, and enum-to-integer/float numeric conversions) in
// third_party/hdf5/src/H5Tconv_enum.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_enum.c

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
#define H5TCONV_ENUM_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Tconv_enum.h"
}  // extern "C"
#ifdef H5TCONV_ENUM_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_ENUM_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

template <typename T>
struct EnumMember {
  const char* name;
  T value;
};

template <typename T>
ScopedType BuildEnum(hid_t base_type,
                     const std::vector<EnumMember<T>>& members) {
  ScopedType enum_type(H5Tenum_create(base_type));
  if (!enum_type.valid()) {
    return ScopedType();
  }
  for (const auto& m : members) {
    if (H5Tenum_insert(enum_type.get(), m.name, &m.value) < 0) {
      return ScopedType();
    }
  }
  return enum_type;
}

struct ConvExceptionLog {
  int calls = 0;
  H5T_conv_except_t last_type = H5T_CONV_EXCEPT_NAN;
  H5T_conv_ret_t action = H5T_CONV_UNHANDLED;
  int64_t replacement = 0;
  size_t dst_bytes = 0;
};

H5T_conv_ret_t EnumConvExceptionCb(H5T_conv_except_t except_type,
                                   hid_t /*src_id*/, hid_t /*dst_id*/,
                                   void* /*src_buf*/, void* dst_buf,
                                   void* user_data) {
  auto* log = static_cast<ConvExceptionLog*>(user_data);
  ++log->calls;
  log->last_type = except_type;
  if (log->action == H5T_CONV_HANDLED && dst_buf != nullptr &&
      log->dst_bytes > 0 && log->dst_bytes <= sizeof(log->replacement)) {
    std::memcpy(dst_buf, &log->replacement, log->dst_bytes);
  }
  return log->action;
}

TEST(H5TconvEnumTest, EmptyEnumInitializationAndConversion) {
  // Different base byte orders ensure the path is not treated as a no-op.
  ScopedType src(H5Tenum_create(H5T_STD_I32LE));
  ScopedType dst(H5Tenum_create(H5T_STD_I32BE));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  H5T_cdata_t* cdata = nullptr;
  EXPECT_NE(H5Tfind(src.get(), dst.get(), &cdata), nullptr);
  ASSERT_NE(cdata, nullptr);
  EXPECT_EQ(cdata->need_bkg, H5T_BKG_NO);

  // Converting 0 elements on an empty enum returns success.
  int32_t dummy = 0;
  EXPECT_GE(H5Tconvert(src.get(), dst.get(), 0, &dummy, nullptr, H5P_DEFAULT),
            0);

  // Converting 1 element when nmembs == 0 falls into the unmapped miss path and
  // fills the destination element with 0xff bytes.
  int32_t val = 7;
  EXPECT_GE(H5Tconvert(src.get(), dst.get(), 1, &val, nullptr, H5P_DEFAULT), 0);
  EXPECT_EQ(val, -1);
}

TEST(H5TconvEnumTest, SingleMemberOneByteEnumUsesO1Path) {
  ScopedType src = BuildEnum<int8_t>(H5T_NATIVE_SCHAR, {{"ONLY", 42}});
  ScopedType dst = BuildEnum<int16_t>(H5T_NATIVE_SHORT, {{"ONLY", 1042}});
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Buffer sized for the larger destination type (nelmts * sizeof(int16_t)).
  alignas(2) std::array<uint8_t, 4> buf{};
  buf[0] = 42;
  buf[1] = 10;  // Unmapped value triggers default 0xff fill.
  ASSERT_GE(
      H5Tconvert(src.get(), dst.get(), 2, buf.data(), nullptr, H5P_DEFAULT), 0);

  int16_t out0 = 0;
  int16_t out1 = 0;
  std::memcpy(&out0, &buf[0], sizeof(out0));
  std::memcpy(&out1, &buf[2], sizeof(out1));
  EXPECT_EQ(out0, 1042);
  EXPECT_EQ(out1, -1);
}

TEST(H5TconvEnumTest, DenseOneByteEnumO1LookupWideningAndNarrowing) {
  // 6 members spanning [-2, 4] -> length = 7, 7/6 < 1.2 -> O(1) lookup with an
  // unused gap at value 3.
  const std::vector<EnumMember<int8_t>> kSrcMembers = {
      {"NEG_TWO", -2}, {"NEG_ONE", -1}, {"ZERO", 0},
      {"ONE", 1},      {"TWO", 2},      {"FOUR", 4},
  };
  const std::vector<EnumMember<int16_t>> kDstMembers = {
      {"ZERO", 100}, {"ONE", 101},     {"TWO", 102},
      {"FOUR", 104}, {"NEG_TWO", -20}, {"NEG_ONE", -10},
  };
  ScopedType src1 = BuildEnum(H5T_NATIVE_SCHAR, kSrcMembers);
  ScopedType dst2 = BuildEnum(H5T_NATIVE_SHORT, kDstMembers);
  ASSERT_TRUE(src1.valid());
  ASSERT_TRUE(dst2.valid());

  // 1. Widening conversion (dst size > src size) exercises reverse iteration
  //    and all O(1) miss conditions: below min (-3), gap (3), above max (5).
  constexpr auto kInputs = std::to_array<int8_t>({-2, -3, 0, 3, 4, 5});
  alignas(2) std::array<int16_t, 6> wide_buf{};
  std::memcpy(wide_buf.data(), kInputs.data(), kInputs.size());
  ASSERT_GE(H5Tconvert(src1.get(), dst2.get(), kInputs.size(), wide_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(wide_buf, ElementsAre(-20, -1, 100, -1, 104, -1));

  // 2. Narrowing conversion (dst size <= src size) exercises forward iteration.
  ScopedType src2 = BuildEnum(H5T_NATIVE_SHORT, kDstMembers);
  ScopedType dst1 = BuildEnum(H5T_NATIVE_SCHAR, kSrcMembers);
  ASSERT_TRUE(src2.valid());
  ASSERT_TRUE(dst1.valid());

  std::array<int16_t, 5> narrow_in = {-20, -10, 100, 999, 104};
  ASSERT_GE(H5Tconvert(src2.get(), dst1.get(), narrow_in.size(),
                       narrow_in.data(), nullptr, H5P_DEFAULT),
            0);
  auto* narrow_out = reinterpret_cast<const int8_t*>(narrow_in.data());
  EXPECT_EQ(narrow_out[0], -2);
  EXPECT_EQ(narrow_out[1], -1);
  EXPECT_EQ(narrow_out[2], 0);
  EXPECT_EQ(narrow_out[3], -1);
  EXPECT_EQ(narrow_out[4], 4);
}

TEST(H5TconvEnumTest, DenseTwoByteEnumO1LookupWidening) {
  // 6 dense short members spanning [10, 16] with gap at 15.
  const std::vector<EnumMember<int16_t>> kSrcMembers = {
      {"A", 10}, {"B", 11}, {"C", 12}, {"D", 13}, {"E", 14}, {"F", 16},
  };
  const std::vector<EnumMember<int32_t>> kDstMembers = {
      {"F", 600}, {"E", 500}, {"D", 400}, {"C", 300}, {"B", 200}, {"A", 100},
  };
  ScopedType src = BuildEnum(H5T_NATIVE_SHORT, kSrcMembers);
  ScopedType dst = BuildEnum(H5T_NATIVE_INT, kDstMembers);
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  constexpr auto kIn = std::to_array<int16_t>({10, 9, 12, 15, 16, 17});
  alignas(4) std::array<int32_t, 6> buf{};
  std::memcpy(buf.data(), kIn.data(), sizeof(kIn));
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), kIn.size(), buf.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_THAT(buf, ElementsAre(100, -1, 300, -1, 600, -1));
}

TEST(H5TconvEnumTest, DenseFourByteEnumO1LookupAndExceptionCallbacks) {
  // 6 dense 4-byte int members spanning [0, 6] with gap at 5.
  const std::vector<EnumMember<int32_t>> kSrcMembers = {
      {"M0", 0}, {"M1", 1}, {"M2", 2}, {"M3", 3}, {"M4", 4}, {"M6", 6},
  };
  const std::vector<EnumMember<int32_t>> kDstMembers = {
      {"M6", 60}, {"M4", 40}, {"M3", 30}, {"M2", 20}, {"M1", 10}, {"M0", 1},
  };
  ScopedType src = BuildEnum(H5T_NATIVE_INT, kSrcMembers);
  ScopedType dst = BuildEnum(H5T_NATIVE_INT, kDstMembers);
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  // 1. Handled exception writes custom sentinel value.
  ConvExceptionLog log{
      .action = H5T_CONV_HANDLED,
      .replacement = 777,
      .dst_bytes = sizeof(int32_t),
  };
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), EnumConvExceptionCb, &log), 0);
  std::array<int32_t, 5> buf = {0, -1, 5, 6, 99};
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), buf.size(), buf.data(), nullptr,
                       dxpl.get()),
            0);
  EXPECT_EQ(log.calls, 3);
  EXPECT_EQ(log.last_type, H5T_CONV_EXCEPT_RANGE_HI);
  EXPECT_THAT(buf, ElementsAre(1, 777, 777, 60, 777));

  // 2. Unhandled exception fills destination with 0xff bytes.
  log.calls = 0;
  log.action = H5T_CONV_UNHANDLED;
  std::array<int32_t, 3> unhandled_buf = {1, 5, 4};
  ASSERT_GE(H5Tconvert(src.get(), dst.get(), unhandled_buf.size(),
                       unhandled_buf.data(), nullptr, dxpl.get()),
            0);
  EXPECT_EQ(log.calls, 1);
  EXPECT_THAT(unhandled_buf, ElementsAre(10, -1, 40));

  // 3. Aborting callback causes H5Tconvert to return failure.
  log.calls = 0;
  log.action = H5T_CONV_ABORT;
  std::array<int32_t, 2> abort_buf = {2, 100};
  ScopedErrorSilence silence;
  EXPECT_LT(H5Tconvert(src.get(), dst.get(), abort_buf.size(), abort_buf.data(),
                       nullptr, dxpl.get()),
            0);
  EXPECT_EQ(log.calls, 1);
}

TEST(H5TconvEnumTest, SparseFourByteAndEightByteEnumsUseBinarySearch) {
  // Sparse 4-byte enum: length/nmembs >= 1.2 forces O(log N) binary search.
  const std::vector<EnumMember<int32_t>> kSparseSrc = {
      {"S1", 5},
      {"S2", 50},
      {"S3", 500},
      {"S4", 5000},
  };
  const std::vector<EnumMember<int32_t>> kSparseDst = {
      {"S4", 4},
      {"S3", 3},
      {"S2", 2},
      {"S1", 1},
  };
  ScopedType src4 = BuildEnum(H5T_NATIVE_INT, kSparseSrc);
  ScopedType dst4 = BuildEnum(H5T_NATIVE_INT, kSparseDst);
  ASSERT_TRUE(src4.valid());
  ASSERT_TRUE(dst4.valid());

  // 1. Default O(log N) conversion covering cmp < 0, cmp > 0, and misses.
  std::array<int32_t, 6> vals4 = {5, 5000, 1, 100, 9999, 500};
  ASSERT_GE(H5Tconvert(src4.get(), dst4.get(), vals4.size(), vals4.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(vals4, ElementsAre(1, 4, -1, -1, -1, 3));

  // 2. O(log N) exception callbacks: HANDLED, UNHANDLED, ABORT.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ConvExceptionLog log{
      .action = H5T_CONV_HANDLED,
      .replacement = 888,
      .dst_bytes = sizeof(int32_t),
  };
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), EnumConvExceptionCb, &log), 0);

  std::array<int32_t, 2> handled_vals = {50, 77};
  ASSERT_GE(H5Tconvert(src4.get(), dst4.get(), handled_vals.size(),
                       handled_vals.data(), nullptr, dxpl.get()),
            0);
  EXPECT_EQ(log.calls, 1);
  EXPECT_THAT(handled_vals, ElementsAre(2, 888));

  log.action = H5T_CONV_UNHANDLED;
  int32_t unmapped_val = 77;
  ASSERT_GE(
      H5Tconvert(src4.get(), dst4.get(), 1, &unmapped_val, nullptr, dxpl.get()),
      0);
  EXPECT_EQ(unmapped_val, -1);

  log.action = H5T_CONV_ABORT;
  unmapped_val = 77;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tconvert(src4.get(), dst4.get(), 1, &unmapped_val, nullptr,
                         dxpl.get()),
              0);
  }

  // 3. 8-byte enum skips O(1) optimization even when values are dense.
  const std::vector<EnumMember<int64_t>> kSrc64 = {
      {"A", 1},
      {"B", 2},
      {"C", 3},
  };
  const std::vector<EnumMember<int16_t>> kDst16 = {
      {"C", 30},
      {"B", 20},
      {"A", 10},
  };
  ScopedType src8 = BuildEnum(H5T_NATIVE_INT64, kSrc64);
  ScopedType dst2 = BuildEnum(H5T_NATIVE_INT16, kDst16);
  ASSERT_TRUE(src8.valid());
  ASSERT_TRUE(dst2.valid());

  std::array<int64_t, 4> vals8 = {1, 3, 2, 99};
  ASSERT_GE(H5Tconvert(src8.get(), dst2.get(), vals8.size(), vals8.data(),
                       nullptr, H5P_DEFAULT),
            0);
  auto* out16 = reinterpret_cast<const int16_t*>(vals8.data());
  EXPECT_EQ(out16[0], 10);
  EXPECT_EQ(out16[1], 30);
  EXPECT_EQ(out16[2], 20);
  EXPECT_EQ(out16[3], -1);
}

TEST(H5TconvEnumTest, EnumInsideCompoundExercisesRecursiveAndStridedPath) {
  ScopedType src_enum = BuildEnum<int32_t>(
      H5T_NATIVE_INT32, {{"RED", 0}, {"GREEN", 1}, {"BLUE", 2}});
  ScopedType dst_enum = BuildEnum<int32_t>(
      H5T_NATIVE_INT32, {{"BLUE", 200}, {"GREEN", 100}, {"RED", 10}});
  ASSERT_TRUE(src_enum.valid());
  ASSERT_TRUE(dst_enum.valid());

  struct Item {
    int32_t color;
    int32_t id;
  };
  ScopedType src_comp(H5Tcreate(H5T_COMPOUND, sizeof(Item)));
  ScopedType dst_comp(H5Tcreate(H5T_COMPOUND, sizeof(Item)));
  ASSERT_TRUE(src_comp.valid());
  ASSERT_TRUE(dst_comp.valid());
  ASSERT_GE(
      H5Tinsert(src_comp.get(), "color", HOFFSET(Item, color), src_enum.get()),
      0);
  ASSERT_GE(
      H5Tinsert(src_comp.get(), "id", HOFFSET(Item, id), H5T_NATIVE_INT32), 0);
  ASSERT_GE(
      H5Tinsert(dst_comp.get(), "color", HOFFSET(Item, color), dst_enum.get()),
      0);
  ASSERT_GE(
      H5Tinsert(dst_comp.get(), "id", HOFFSET(Item, id), H5T_NATIVE_INT32), 0);

  std::array<Item, 3> buf = {{{0, 1}, {1, 2}, {2, 3}}};
  std::array<Item, 3> bkg = {};
  ASSERT_GE(H5Tconvert(src_comp.get(), dst_comp.get(), buf.size(), buf.data(),
                       bkg.data(), H5P_DEFAULT),
            0);
  EXPECT_EQ(buf[0].color, 10);
  EXPECT_EQ(buf[0].id, 1);
  EXPECT_EQ(buf[1].color, 100);
  EXPECT_EQ(buf[1].id, 2);
  EXPECT_EQ(buf[2].color, 200);
  EXPECT_EQ(buf[2].id, 3);
}

TEST(H5TconvEnumTest, InMemoryDatasetRoundTripConvertsEnumValues) {
  ScopedType file_enum =
      BuildEnum<int32_t>(H5T_STD_I32LE, {{"LOW", 1}, {"MED", 2}, {"HIGH", 3}});
  ScopedType mem_enum = BuildEnum<int32_t>(
      H5T_NATIVE_INT32, {{"HIGH", 30}, {"MED", 20}, {"LOW", 10}});
  ASSERT_TRUE(file_enum.valid());
  ASSERT_TRUE(mem_enum.valid());

  ScopedFile file = CreateInMemoryFile("enum_roundtrip.h5");
  ASSERT_TRUE(file.valid());
  const std::array<hsize_t, 1> dims = {3};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "levels", file_enum.get(),
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  constexpr auto kWriteVals = std::to_array<int32_t>({1, 2, 3});
  ASSERT_GE(H5Dwrite(dset.get(), file_enum.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kWriteVals.data()),
            0);

  std::array<int32_t, 3> read_vals{};
  ASSERT_GE(H5Dread(dset.get(), mem_enum.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_vals.data()),
            0);
  EXPECT_THAT(read_vals, ElementsAre(10, 20, 30));
}

TEST(H5TconvEnumTest, EnumToNumericCoversIntegerFloatNoopAndAbort) {
  ScopedType enum_type =
      BuildEnum<int32_t>(H5T_NATIVE_INT32, {{"A", 7}, {"B", 1000}, {"C", -4}});
  ASSERT_TRUE(enum_type.valid());

  // 1. Enum -> identical parent integer type hits H5T_path_noop(tpath) == true.
  std::array<int32_t, 3> same_buf = {7, 1000, -4};
  ASSERT_GE(H5Tconvert(enum_type.get(), H5T_NATIVE_INT32, same_buf.size(),
                       same_buf.data(), nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(same_buf, ElementsAre(7, 1000, -4));

  // 2. Enum -> 64-bit integer widens cleanly.
  std::array<int64_t, 3> i64_buf{};
  std::memcpy(i64_buf.data(), same_buf.data(), sizeof(same_buf));
  ASSERT_GE(H5Tconvert(enum_type.get(), H5T_NATIVE_INT64, 3, i64_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(i64_buf, ElementsAre(7, 1000, -4));

  // 3. Enum -> double converts via enum_f path.
  std::array<double, 3> f64_buf{};
  std::memcpy(f64_buf.data(), same_buf.data(), sizeof(same_buf));
  ASSERT_GE(H5Tconvert(enum_type.get(), H5T_NATIVE_DOUBLE, 3, f64_buf.data(),
                       nullptr, H5P_DEFAULT),
            0);
  EXPECT_THAT(f64_buf, ElementsAre(7.0, 1000.0, -4.0));

  // 4. Enum -> int8 with aborting overflow callback causes H5T_convert to fail.
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ConvExceptionLog log{.action = H5T_CONV_ABORT};
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), EnumConvExceptionCb, &log), 0);
  std::array<int32_t, 2> overflow_buf = {7, 1000};
  ScopedErrorSilence silence;
  EXPECT_LT(H5Tconvert(enum_type.get(), H5T_NATIVE_INT8, 2, overflow_buf.data(),
                       nullptr, dxpl.get()),
            0);
  EXPECT_EQ(log.calls, 1);
}

TEST(H5TconvEnumTest, DirectInternalEnumApisHandleErrorsAndCacheRebuild) {
  ScopedType e1 =
      BuildEnum<int32_t>(H5T_NATIVE_INT32, {{"ONE", 1}, {"TWO", 2}});
  ScopedType e2 =
      BuildEnum<int32_t>(H5T_NATIVE_INT32, {{"TWO", 20}, {"ONE", 10}});
  ScopedType e3 =
      BuildEnum<int32_t>(H5T_NATIVE_INT32, {{"ONE", 100}, {"TWO", 200}});
  ASSERT_TRUE(e1.valid());
  ASSERT_TRUE(e2.valid());
  ASSERT_TRUE(e3.valid());

  const auto* t_e1 =
      static_cast<const H5T_t*>(H5I_object_verify(e1.get(), H5I_DATATYPE));
  const auto* t_e2 =
      static_cast<const H5T_t*>(H5I_object_verify(e2.get(), H5I_DATATYPE));
  const auto* t_e3 =
      static_cast<const H5T_t*>(H5I_object_verify(e3.get(), H5I_DATATYPE));
  const auto* t_int = static_cast<const H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT, H5I_DATATYPE));
  const auto* t_str =
      static_cast<const H5T_t*>(H5I_object_verify(H5T_C_S1, H5I_DATATYPE));
  ASSERT_NE(t_e1, nullptr);
  ASSERT_NE(t_e2, nullptr);
  ASSERT_NE(t_e3, nullptr);
  ASSERT_NE(t_int, nullptr);
  ASSERT_NE(t_str, nullptr);

  ScopedErrorSilence silence;
  H5T_conv_ctx_t ctx{};

  // 1. H5T__conv_enum error branches for INIT, CONV, and invalid commands.
  H5T_cdata_t cdata{};
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_enum(nullptr, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_enum(t_e1, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_enum(t_int, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(
      H5T__conv_enum(t_e1, t_int, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(
      H5T__conv_enum(nullptr, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_enum(t_e1, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_enum(t_e1, t_e2, &cdata, nullptr, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_enum(t_int, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_LT(
      H5T__conv_enum(t_e1, t_int, &cdata, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_enum(t_e1, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  // 2. Free with NULL priv is a safe no-op, then init + cache rebuild + free.
  cdata = {};
  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_enum(t_e1, t_e2, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
            0);

  H5T_cdata_t managed{};
  absl::Cleanup free_managed = [&] {
    if (managed.priv != nullptr) {
      managed.command = H5T_CONV_FREE;
      H5T__conv_enum(t_e1, t_e2, &managed, &ctx, 0, 0, 0, nullptr, nullptr);
    }
  };
  managed.command = H5T_CONV_INIT;
  ASSERT_GE(
      H5T__conv_enum(t_e1, t_e2, &managed, &ctx, 0, 0, 0, nullptr, nullptr), 0);

  // Passing a different dst (t_e3) on H5T_CONV_CONV with recursive=false forces
  // cache rebuild, closing previous priv->src_copy and priv->dst_copy.
  int32_t val = 2;
  managed.command = H5T_CONV_CONV;
  ctx.u.conv.recursive = false;
  ASSERT_GE(H5T__conv_enum(t_e1, t_e3, &managed, &ctx, 1, 0, 0, &val, nullptr),
            0);
  EXPECT_EQ(val, 200);

  managed.command = H5T_CONV_FREE;
  EXPECT_GE(
      H5T__conv_enum(t_e1, t_e3, &managed, &ctx, 0, 0, 0, nullptr, nullptr), 0);
  EXPECT_EQ(managed.priv, nullptr);

  // 3. H5T__conv_enum_numeric error branches, FREE, and invalid command.
  H5T_cdata_t num_cdata{};
  num_cdata.command = H5T_CONV_INIT;
  EXPECT_LT(H5T__conv_enum_numeric(nullptr, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_enum_numeric(t_e1, nullptr, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_enum_numeric(t_int, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_enum_numeric(t_e1, t_str, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  EXPECT_GE(H5T__conv_enum_numeric(t_e1, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);

  num_cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_enum_numeric(nullptr, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  EXPECT_LT(H5T__conv_enum_numeric(t_e1, nullptr, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
  int32_t num_dummy = 0;
  EXPECT_LT(H5T__conv_enum_numeric(t_e1, t_str, &num_cdata, &ctx, 1, 0, 0,
                                   &num_dummy, nullptr),
            0);

  num_cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_enum_numeric(t_e1, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);

  num_cdata.command = static_cast<H5T_cmd_t>(99);
  EXPECT_LT(H5T__conv_enum_numeric(t_e1, t_int, &num_cdata, &ctx, 0, 0, 0,
                                   nullptr, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
