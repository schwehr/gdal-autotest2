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
// Tests fixed-length string conversion H5T__conv_s_s via public H5Tconvert,
// strided compound conversion, and direct internal INIT/CONV/FREE edge cases
// (third_party/hdf5/src/H5Tconv_string.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tconv_string.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TCONV_STRING_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
// H5private.h omits H5__Float16 under __cplusplus, while H5Tpkg.h still
// declares unreferenced extern INF globals with that type.
using H5__Float16 = uint16_t;
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5Tconv_string.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TCONV_STRING_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCONV_STRING_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;

ScopedType MakeFixedStringType(size_t size, H5T_str_t pad,
                               H5T_cset_t cset = H5T_CSET_ASCII) {
  ScopedType dt(H5Tcopy(H5T_C_S1));
  if (dt.valid()) {
    if (H5Tset_size(dt, size) < 0 || H5Tset_strpad(dt, pad) < 0 ||
        H5Tset_cset(dt, cset) < 0) {
      dt.reset();
    }
  }
  return dt;
}

std::string EncodeFixedString(std::string_view text, size_t width,
                              H5T_str_t pad) {
  std::string out(width, pad == H5T_STR_SPACEPAD ? ' ' : '\0');
  const size_t copy_len = std::min(text.size(), width);
  std::memcpy(out.data(), text.data(), copy_len);
  if (pad == H5T_STR_NULLTERM && width > 0) {
    out[width - 1] = '\0';
  }
  return out;
}

std::string ExpectedConvertedString(std::string_view raw_src,
                                    H5T_str_t src_pad, size_t dst_width,
                                    H5T_str_t dst_pad) {
  size_t nchars = 0;
  if (src_pad == H5T_STR_NULLTERM || src_pad == H5T_STR_NULLPAD) {
    while (nchars < dst_width && nchars < raw_src.size() &&
           raw_src[nchars] != '\0') {
      ++nchars;
    }
  } else {
    nchars = raw_src.size();
    while (nchars > 0 && raw_src[nchars - 1] == ' ') {
      --nchars;
    }
    nchars = std::min(nchars, dst_width);
  }
  std::string out(dst_width, dst_pad == H5T_STR_SPACEPAD ? ' ' : '\0');
  std::memcpy(out.data(), raw_src.data(), nchars);
  if (dst_pad == H5T_STR_NULLTERM && dst_width > 0) {
    out[dst_width - 1] = '\0';
  }
  return out;
}

TEST(H5TconvStringTest, AllPaddingCombinationsSameGrowAndShrink) {
  constexpr std::array<H5T_str_t, 3> kPads = {
      H5T_STR_NULLTERM,
      H5T_STR_NULLPAD,
      H5T_STR_SPACEPAD,
  };
  struct SizePair {
    size_t src_size;
    size_t dst_size;
    H5T_cset_t cset;
  };
  constexpr std::array<SizePair, 5> kSizePairs = {{
      {6, 6, H5T_CSET_ASCII},
      {6, 6, H5T_CSET_UTF8},
      {8, 4, H5T_CSET_ASCII},  // Shrink: olap = 1
      {6, 4, H5T_CSET_UTF8},   // Shrink: olap = 2
      {4, 7, H5T_CSET_ASCII},  // Grow: direction = -1, olap = 2
  }};
  constexpr std::array<std::string_view, 6> kInputs = {
      "",
      "a",
      "ab",
      "abcdef",
      "      ",  // Exercises all-spaces truncation loop down to 0
      "xy  z",   // Exercises internal vs trailing spaces
  };

  for (const auto& sizes : kSizePairs) {
    for (H5T_str_t src_pad : kPads) {
      for (H5T_str_t dst_pad : kPads) {
        ScopedType src = MakeFixedStringType(sizes.src_size, src_pad, sizes.cset);
        ScopedType dst = MakeFixedStringType(sizes.dst_size, dst_pad, sizes.cset);
        ASSERT_GE(src, 0);
        ASSERT_GE(dst, 0);

        const size_t nelmts = kInputs.size();
        const size_t max_elem = std::max(sizes.src_size, sizes.dst_size);
        std::vector<uint8_t> buf(nelmts * max_elem, 0xA5);

        std::vector<std::string> expected;
        expected.reserve(nelmts);
        for (size_t i = 0; i < nelmts; ++i) {
          std::string encoded =
              EncodeFixedString(kInputs[i], sizes.src_size, src_pad);
          std::memcpy(buf.data() + i * sizes.src_size, encoded.data(),
                      sizes.src_size);
          expected.push_back(ExpectedConvertedString(encoded, src_pad,
                                                     sizes.dst_size, dst_pad));
        }

        ASSERT_GE(H5Tconvert(src, dst, nelmts, buf.data(), nullptr, H5P_DEFAULT),
                  0);
        for (size_t i = 0; i < nelmts; ++i) {
          std::string actual(
              reinterpret_cast<const char*>(buf.data() + i * sizes.dst_size),
              sizes.dst_size);
          EXPECT_EQ(actual, expected[i])
              << "src_size=" << sizes.src_size << " dst_size=" << sizes.dst_size
              << " src_pad=" << src_pad << " dst_pad=" << dst_pad << " idx=" << i;
        }
      }
    }
  }
}

TEST(H5TconvStringTest, MismatchedCharsetsRejectedByPublicConvert) {
  ScopedType ascii = MakeFixedStringType(6, H5T_STR_NULLTERM, H5T_CSET_ASCII);
  ScopedType utf8 = MakeFixedStringType(6, H5T_STR_NULLTERM, H5T_CSET_UTF8);
  ASSERT_GE(ascii, 0);
  ASSERT_GE(utf8, 0);

  std::array<char, 6> buf = {'h', 'e', 'l', 'l', 'o', '\0'};
  ScopedErrorSilence silence;
  EXPECT_LT(H5Tconvert(ascii, utf8, 1, buf.data(), nullptr, H5P_DEFAULT), 0);
  EXPECT_LT(H5Tconvert(utf8, ascii, 1, buf.data(), nullptr, H5P_DEFAULT), 0);
}

TEST(H5TconvStringTest, StridedCompoundConversionExercisesBufStride) {
  struct Record {
    int32_t id;
    std::array<char, 6> text;
    int32_t tail;
  };
  constexpr size_t kTextOffset = offsetof(Record, text);

  ScopedType src_str = MakeFixedStringType(6, H5T_STR_SPACEPAD);
  ScopedType dst_str = MakeFixedStringType(6, H5T_STR_NULLTERM);
  ASSERT_GE(src_str, 0);
  ASSERT_GE(dst_str, 0);

  ScopedType src_cmp(H5Tcreate(H5T_COMPOUND, sizeof(Record)));
  ScopedType dst_cmp(H5Tcreate(H5T_COMPOUND, sizeof(Record)));
  ASSERT_GE(src_cmp, 0);
  ASSERT_GE(dst_cmp, 0);
  ASSERT_GE(H5Tinsert(src_cmp, "id", offsetof(Record, id), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(src_cmp, "text", kTextOffset, src_str), 0);
  ASSERT_GE(
      H5Tinsert(src_cmp, "tail", offsetof(Record, tail), H5T_NATIVE_INT32), 0);

  ASSERT_GE(H5Tinsert(dst_cmp, "id", offsetof(Record, id), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(dst_cmp, "text", kTextOffset, dst_str), 0);
  ASSERT_GE(
      H5Tinsert(dst_cmp, "tail", offsetof(Record, tail), H5T_NATIVE_INT32), 0);

  std::array<Record, 3> records = {{
      {10, {'a', 'b', ' ', ' ', ' ', ' '}, 100},
      {20, {'c', 'd', 'e', 'f', ' ', ' '}, 200},
      {30, {'1', '2', '3', '4', '5', '6'}, 300},
  }};
  std::vector<Record> bkg(records.size());
  ASSERT_GE(H5Tconvert(src_cmp, dst_cmp, records.size(), records.data(),
                       bkg.data(), H5P_DEFAULT),
            0);

  EXPECT_EQ(std::string_view(records[0].text.data(), 6),
            std::string_view("ab\0\0\0\0", 6));
  EXPECT_EQ(std::string_view(records[1].text.data(), 6),
            std::string_view("cdef\0\0", 6));
  EXPECT_EQ(std::string_view(records[2].text.data(), 6),
            std::string_view("12345\0", 6));
  EXPECT_EQ(records[0].tail, 100);
  EXPECT_EQ(records[1].tail, 200);
  EXPECT_EQ(records[2].tail, 300);
}

TEST(H5TconvStringTest, DirectInternalInitFreeConvAndValidationErrors) {
  ScopedType src_id = MakeFixedStringType(6, H5T_STR_NULLPAD);
  ScopedType dst_id = MakeFixedStringType(4, H5T_STR_SPACEPAD);
  ASSERT_GE(src_id, 0);
  ASSERT_GE(dst_id, 0);

  auto* src_dt = static_cast<H5T_t*>(H5I_object_verify(src_id, H5I_DATATYPE));
  auto* dst_dt = static_cast<H5T_t*>(H5I_object_verify(dst_id, H5I_DATATYPE));
  ASSERT_NE(src_dt, nullptr);
  ASSERT_NE(dst_dt, nullptr);

  H5T_conv_ctx_t ctx{};
  H5T_cdata_t cdata{};

  // Direct strided conversion via internal entry point.
  cdata.command = H5T_CONV_INIT;
  ASSERT_GE(H5T__conv_s_s(src_dt, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  EXPECT_EQ(cdata.need_bkg, H5T_BKG_NO);

  std::array<uint8_t, 16> strided{};
  std::memcpy(strided.data(), "hello\0", 6);
  std::memcpy(strided.data() + 8, "xy\0\0\0\0", 6);
  cdata.command = H5T_CONV_CONV;
  ASSERT_GE(H5T__conv_s_s(src_dt, dst_dt, &cdata, &ctx, 2, 8, 0, strided.data(),
                          nullptr),
            0);
  EXPECT_EQ(std::string_view(reinterpret_cast<char*>(strided.data()), 4),
            "hell");
  EXPECT_EQ(std::string_view(reinterpret_cast<char*>(strided.data() + 8), 4),
            "xy  ");

  cdata.command = H5T_CONV_FREE;
  EXPECT_GE(H5T__conv_s_s(src_dt, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);

  ScopedErrorSilence silence;

  // Unknown command -> default branch.
  cdata.command = static_cast<H5T_cmd_t>(999);
  EXPECT_LT(H5T__conv_s_s(src_dt, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);

  // Null src/dst checks in INIT and CONV.
  cdata.command = H5T_CONV_INIT;
  EXPECT_LT(
      H5T__conv_s_s(nullptr, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5T__conv_s_s(src_dt, nullptr, &cdata, &ctx, 0, 0, 0, nullptr, nullptr),
      0);

  cdata.command = H5T_CONV_CONV;
  EXPECT_LT(H5T__conv_s_s(nullptr, dst_dt, &cdata, &ctx, 1, 0, 0, strided.data(),
                          nullptr),
            0);
  EXPECT_LT(H5T__conv_s_s(src_dt, nullptr, &cdata, &ctx, 1, 0, 0, strided.data(),
                          nullptr),
            0);

  // Bad precision (src and dst).
  H5T_t* bad_src = H5T_copy(src_dt, H5T_COPY_TRANSIENT);
  ASSERT_NE(bad_src, nullptr);
  absl::Cleanup close_bad_src = [&] { H5T_close(bad_src); };
  H5T_t* bad_dst = H5T_copy(dst_dt, H5T_COPY_TRANSIENT);
  ASSERT_NE(bad_dst, nullptr);
  absl::Cleanup close_bad_dst = [&] { H5T_close(bad_dst); };

  cdata.command = H5T_CONV_INIT;
  bad_src->shared->u.atomic.prec = 16;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_src->shared->u.atomic.prec = 8 * bad_src->shared->size;

  bad_dst->shared->u.atomic.prec = 16;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_dst->shared->u.atomic.prec = 8 * bad_dst->shared->size;

  // Non-zero offset (src and dst).
  bad_src->shared->u.atomic.offset = 1;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_src->shared->u.atomic.offset = 0;

  bad_dst->shared->u.atomic.offset = 1;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_dst->shared->u.atomic.offset = 0;

  // Invalid character sets (src and dst).
  bad_src->shared->u.atomic.u.s.cset = H5T_NCSET;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_src->shared->u.atomic.u.s.cset = H5T_CSET_ASCII;

  bad_dst->shared->u.atomic.u.s.cset = H5T_CSET_ERROR;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_dst->shared->u.atomic.u.s.cset = H5T_CSET_ASCII;

  // Invalid padding in INIT (src and dst).
  bad_src->shared->u.atomic.u.s.pad = H5T_STR_ERROR;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_src->shared->u.atomic.u.s.pad = H5T_NSTR;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_src->shared->u.atomic.u.s.pad = H5T_STR_NULLPAD;

  bad_dst->shared->u.atomic.u.s.pad = H5T_STR_ERROR;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);
  bad_dst->shared->u.atomic.u.s.pad = H5T_NSTR;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 0, 0, 0, nullptr,
                          nullptr),
            0);

  // Reserved padding values in CONV switches (src and dst).
  std::array<uint8_t, 8> scratch = {'a', 'b', 'c', 'd', 0, 0, 0, 0};
  cdata.command = H5T_CONV_CONV;
  bad_src->shared->u.atomic.u.s.pad = H5T_STR_RESERVED_3;
  EXPECT_LT(H5T__conv_s_s(bad_src, dst_dt, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);
  bad_src->shared->u.atomic.u.s.pad = H5T_STR_NULLPAD;

  bad_dst->shared->u.atomic.u.s.pad = H5T_STR_RESERVED_3;
  EXPECT_LT(H5T__conv_s_s(src_dt, bad_dst, &cdata, &ctx, 1, 0, 0, scratch.data(),
                          nullptr),
            0);
  bad_dst->shared->u.atomic.u.s.pad = H5T_STR_SPACEPAD;
  // Unconverted tail bytes beyond the 4-byte destination width stay untouched.
  EXPECT_EQ(strided[4], 'o');
  EXPECT_THAT(std::vector<uint8_t>(strided.begin() + 5, strided.begin() + 8),
              Each('\0'));
}

}  // namespace
}  // namespace hdf5
