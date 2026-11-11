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

// Clean-room unit tests for //third_party/netcdf/libsrc/ncx.c.
//
// Exercises ncx_len_* macros, size_t/off_t/uint32/uint64 primitives, text/
// char_char/opaque codecs, IEEE-754 float/double boundaries, unaligned wire
// buffers, X_ALIGN and X_SIZEOF_SHORT padding, and all 10x11 unpadded and
// 4x11 padded external<->internal conversion matrices including NC_ERANGE.

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cfloat>
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
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

extern "C" {
// clang-format off
#include "third_party/netcdf/libsrc/ncx.h"
// clang-format on
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

struct ExtSchar {
  using Native = schar;
  static constexpr size_t kBytes = X_SIZEOF_CHAR;
};

struct ExtUchar {
  using Native = uchar;
  static constexpr size_t kBytes = X_SIZEOF_UBYTE;
};

struct ExtShort {
  using Native = short;
  static constexpr size_t kBytes = X_SIZEOF_SHORT;
};

struct ExtUshort {
  using Native = ushort;
  static constexpr size_t kBytes = X_SIZEOF_USHORT;
};

struct ExtInt {
  using Native = int;
  static constexpr size_t kBytes = X_SIZEOF_INT;
};

struct ExtUint {
  using Native = uint;
  static constexpr size_t kBytes = X_SIZEOF_UINT;
};

struct ExtFloat {
  using Native = float;
  static constexpr size_t kBytes = X_SIZEOF_FLOAT;
};

struct ExtDouble {
  using Native = double;
  static constexpr size_t kBytes = X_SIZEOF_DOUBLE;
};

struct ExtLonglong {
  using Native = longlong;
  static constexpr size_t kBytes = X_SIZEOF_LONGLONG;
};

struct ExtUlonglong {
  using Native = ulonglong;
  static constexpr size_t kBytes = X_SIZEOF_ULONGLONG;
};

template <typename ExtTag>
void WriteBigEndianValue(typename ExtTag::Native val, uint8_t* dst) {
  if constexpr (std::is_same_v<ExtTag, ExtSchar> ||
                std::is_same_v<ExtTag, ExtUchar>) {
    dst[0] = static_cast<uint8_t>(val);
  } else if constexpr (std::is_same_v<ExtTag, ExtShort> ||
                       std::is_same_v<ExtTag, ExtUshort>) {
    const uint16_t u = static_cast<uint16_t>(val);
    dst[0] = static_cast<uint8_t>((u >> 8) & 0xffU);
    dst[1] = static_cast<uint8_t>(u & 0xffU);
  } else if constexpr (std::is_same_v<ExtTag, ExtInt> ||
                       std::is_same_v<ExtTag, ExtUint>) {
    const uint32_t u = static_cast<uint32_t>(val);
    dst[0] = static_cast<uint8_t>((u >> 24) & 0xffU);
    dst[1] = static_cast<uint8_t>((u >> 16) & 0xffU);
    dst[2] = static_cast<uint8_t>((u >> 8) & 0xffU);
    dst[3] = static_cast<uint8_t>(u & 0xffU);
  } else if constexpr (std::is_same_v<ExtTag, ExtFloat>) {
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(val));
    std::memcpy(&u, &val, sizeof(u));
    dst[0] = static_cast<uint8_t>((u >> 24) & 0xffU);
    dst[1] = static_cast<uint8_t>((u >> 16) & 0xffU);
    dst[2] = static_cast<uint8_t>((u >> 8) & 0xffU);
    dst[3] = static_cast<uint8_t>(u & 0xffU);
  } else if constexpr (std::is_same_v<ExtTag, ExtDouble>) {
    uint64_t u = 0;
    static_assert(sizeof(u) == sizeof(val));
    std::memcpy(&u, &val, sizeof(u));
    for (size_t b = 0; b < 8; ++b) {
      dst[b] = static_cast<uint8_t>((u >> ((7U - b) * 8U)) & 0xffU);
    }
  } else {
    const uint64_t u = static_cast<uint64_t>(val);
    for (size_t b = 0; b < 8; ++b) {
      dst[b] = static_cast<uint8_t>((u >> ((7U - b) * 8U)) & 0xffU);
    }
  }
}

template <typename ExtTag, typename SrcT>
std::vector<uint8_t> EncodeCanonicalWire(const std::vector<SrcT>& vals) {
  std::vector<uint8_t> out(vals.size() * ExtTag::kBytes, 0);
  for (size_t i = 0; i < vals.size(); ++i) {
    WriteBigEndianValue<ExtTag>(static_cast<typename ExtTag::Native>(vals[i]),
                                &out[i * ExtTag::kBytes]);
  }
  return out;
}

template <typename ExtTag, typename IType>
void ExpectValueEqual(IType actual, IType expected, size_t index) {
  SCOPED_TRACE(::testing::Message() << "index=" << index);
  if constexpr (std::is_floating_point_v<IType>) {
    EXPECT_EQ(actual, expected);
    if constexpr (std::is_floating_point_v<typename ExtTag::Native>) {
      if (std::fpclassify(expected) == FP_ZERO) {
        EXPECT_EQ(std::signbit(actual), std::signbit(expected));
      }
    }
  } else {
    EXPECT_EQ(actual, expected);
  }
}

template <typename ExtTag, typename IType>
std::vector<IType> MakeValidValues() {
  using E = typename ExtTag::Native;
  if constexpr (std::is_floating_point_v<E> &&
                std::is_floating_point_v<IType>) {
    if constexpr (std::is_same_v<E, double> && std::is_same_v<IType, double>) {
      return {-DBL_MAX, -12.5, -DBL_MIN, -0.0, 0.0, DBL_MIN, 3.75, DBL_MAX};
    } else {
      return {static_cast<IType>(-FLT_MAX), static_cast<IType>(-12.5f),
              static_cast<IType>(-FLT_MIN), static_cast<IType>(-0.0f),
              static_cast<IType>(0.0f),     static_cast<IType>(FLT_MIN),
              static_cast<IType>(3.75f),    static_cast<IType>(FLT_MAX)};
    }
  } else if constexpr (std::is_floating_point_v<E> ||
                       std::is_floating_point_v<IType>) {
    using IntT = std::conditional_t<std::is_floating_point_v<E>, IType, E>;
    constexpr bool kUsesFloat =
        std::is_same_v<E, float> || std::is_same_v<IType, float>;
    if constexpr (std::is_same_v<IntT, schar>) {
      return {static_cast<IType>(SCHAR_MIN), static_cast<IType>(-42),
              static_cast<IType>(-1),        static_cast<IType>(0),
              static_cast<IType>(1),         static_cast<IType>(42),
              static_cast<IType>(SCHAR_MAX)};
    } else if constexpr (std::is_same_v<IntT, uchar>) {
      return {static_cast<IType>(0), static_cast<IType>(1),
              static_cast<IType>(42), static_cast<IType>(127),
              static_cast<IType>(UCHAR_MAX)};
    } else if constexpr (std::is_same_v<IntT, short>) {
      return {static_cast<IType>(SHRT_MIN), static_cast<IType>(-1024),
              static_cast<IType>(-1),       static_cast<IType>(0),
              static_cast<IType>(1),        static_cast<IType>(1024),
              static_cast<IType>(SHRT_MAX)};
    } else if constexpr (std::is_same_v<IntT, ushort>) {
      return {static_cast<IType>(0), static_cast<IType>(1),
              static_cast<IType>(42), static_cast<IType>(32768),
              static_cast<IType>(USHRT_MAX)};
    } else if constexpr (std::is_same_v<IntT, int>) {
      if constexpr (kUsesFloat) {
        return {static_cast<IType>(INT_MIN),   static_cast<IType>(-1000000),
                static_cast<IType>(-1),        static_cast<IType>(0),
                static_cast<IType>(1),         static_cast<IType>(1000000),
                static_cast<IType>(2147483520)};
      } else {
        return {static_cast<IType>(INT_MIN), static_cast<IType>(-1000000),
                static_cast<IType>(-1),      static_cast<IType>(0),
                static_cast<IType>(1),       static_cast<IType>(1000000),
                static_cast<IType>(INT_MAX)};
      }
    } else if constexpr (std::is_same_v<IntT, uint>) {
      if constexpr (kUsesFloat) {
        return {static_cast<IType>(0U), static_cast<IType>(1U),
                static_cast<IType>(1000000U), static_cast<IType>(4294967040U)};
      } else {
        return {static_cast<IType>(0U), static_cast<IType>(1U),
                static_cast<IType>(1000000U), static_cast<IType>(UINT_MAX)};
      }
    } else if constexpr (std::is_same_v<IntT, long> ||
                         std::is_same_v<IntT, longlong>) {
      constexpr long long kMin =
          static_cast<long long>(std::numeric_limits<IntT>::min());
      if constexpr (sizeof(IntT) == 4) {
        return {static_cast<IType>(kMin),
                static_cast<IType>(-1000000),
                static_cast<IType>(-1),
                static_cast<IType>(0),
                static_cast<IType>(1),
                static_cast<IType>(1000000),
                static_cast<IType>(kUsesFloat ? 2147483520 : INT_MAX)};
      } else if constexpr (kUsesFloat) {
        return {static_cast<IType>(kMin),
                static_cast<IType>(-1000000LL),
                static_cast<IType>(-1LL),
                static_cast<IType>(0LL),
                static_cast<IType>(1LL),
                static_cast<IType>(1000000LL),
                static_cast<IType>(9223371487098961920LL)};
      } else {
        return {static_cast<IType>(kMin),
                static_cast<IType>(-1000000000000LL),
                static_cast<IType>(-1LL),
                static_cast<IType>(0LL),
                static_cast<IType>(1LL),
                static_cast<IType>(1000000000000LL),
                static_cast<IType>(9223372036854774784LL)};
      }
    } else {
      if constexpr (kUsesFloat) {
        return {static_cast<IType>(0ULL), static_cast<IType>(1ULL),
                static_cast<IType>(1000000ULL),
                static_cast<IType>(18446742974197923840ULL)};
      } else {
        return {static_cast<IType>(0ULL), static_cast<IType>(1ULL),
                static_cast<IType>(1000000000000ULL),
                static_cast<IType>(18446744073709549568ULL)};
      }
    }
  } else {
    constexpr bool kBothSigned = std::is_signed_v<E> && std::is_signed_v<IType>;
    IType lo = static_cast<IType>(0);
    if constexpr (kBothSigned) {
      const long long min_val = std::max<long long>(
          static_cast<long long>(std::numeric_limits<E>::min()),
          static_cast<long long>(std::numeric_limits<IType>::min()));
      lo = static_cast<IType>(min_val);
    }
    const uint64_t hi_u = std::min<uint64_t>(
        static_cast<uint64_t>(std::numeric_limits<E>::max()),
        static_cast<uint64_t>(std::numeric_limits<IType>::max()));
    const IType hi = static_cast<IType>(hi_u);

    std::vector<IType> vals;
    vals.push_back(lo);
    if constexpr (kBothSigned) {
      vals.push_back(static_cast<IType>(-1));
      vals.push_back(static_cast<IType>(0));
    }
    vals.push_back(static_cast<IType>(1));
    vals.push_back(static_cast<IType>(42));
    if (hi_u > 127ULL) {
      vals.push_back(static_cast<IType>(127));
    }
    vals.push_back(hi);
    return vals;
  }
}

// Returns 3-element integer->integer out-of-range inputs for putn/pad_putn,
// or empty when no safe overflow exists. Floating-point inputs are
// intentionally skipped because without ERANGE_FILL ncx_put_*_<float/double>
// casts after setting NC_ERANGE.
template <typename ExtTag, typename IType>
std::vector<IType> PutnErangeInputs() {
  using E = typename ExtTag::Native;
  if constexpr (std::is_floating_point_v<E> ||
                std::is_floating_point_v<IType>) {
    return {};
  } else {
    constexpr uint64_t kExtMax =
        static_cast<uint64_t>(std::numeric_limits<E>::max());
    constexpr uint64_t kIntMax =
        static_cast<uint64_t>(std::numeric_limits<IType>::max());
    constexpr bool kCanHigh = kIntMax > kExtMax;
    constexpr bool kCanLow =
        std::is_signed_v<IType> &&
        (std::is_unsigned_v<E> ||
         static_cast<long long>(std::numeric_limits<IType>::min()) <
             static_cast<long long>(std::numeric_limits<E>::min()));
    if constexpr (!kCanHigh && !kCanLow) {
      return {};
    } else {
      std::vector<IType> out;
      out.push_back(static_cast<IType>(0));
      if constexpr (kCanHigh) {
        out.push_back(static_cast<IType>(kExtMax + 1ULL));
      }
      if constexpr (kCanLow) {
        if constexpr (std::is_unsigned_v<E>) {
          out.push_back(static_cast<IType>(-1));
        } else {
          out.push_back(static_cast<IType>(
              static_cast<long long>(std::numeric_limits<E>::min()) - 1LL));
        }
      }
      if constexpr (kCanHigh && !kCanLow) {
        out.push_back(std::numeric_limits<IType>::max());
      } else if constexpr (!kCanHigh && kCanLow) {
        out.push_back(std::numeric_limits<IType>::min());
      }
      return out;
    }
  }
}

// Returns 3-element external wire vectors [valid, high, low] that trigger
// NC_ERANGE when decoded via getn/pad_getn into IType.
template <typename ExtTag, typename IType>
std::vector<typename ExtTag::Native> GetnErangeWireInputs() {
  using E = typename ExtTag::Native;
  if constexpr (!std::is_floating_point_v<E> &&
                !std::is_floating_point_v<IType>) {
    constexpr uint64_t kExtMax =
        static_cast<uint64_t>(std::numeric_limits<E>::max());
    constexpr uint64_t kIntMax =
        static_cast<uint64_t>(std::numeric_limits<IType>::max());
    constexpr bool kCanHigh = kExtMax > kIntMax;
    constexpr bool kCanLow =
        std::is_signed_v<E> &&
        (std::is_unsigned_v<IType> ||
         static_cast<long long>(std::numeric_limits<E>::min()) <
             static_cast<long long>(std::numeric_limits<IType>::min()));
    if constexpr (!kCanHigh && !kCanLow) {
      return {};
    } else {
      std::vector<E> out;
      out.push_back(static_cast<E>(0));
      if constexpr (kCanHigh) {
        out.push_back(static_cast<E>(kIntMax + 1ULL));
      }
      if constexpr (kCanLow) {
        if constexpr (std::is_unsigned_v<IType>) {
          out.push_back(static_cast<E>(-1));
        } else {
          out.push_back(static_cast<E>(
              static_cast<long long>(std::numeric_limits<IType>::min()) - 1LL));
        }
      }
      if constexpr (kCanHigh && !kCanLow) {
        out.push_back(std::numeric_limits<E>::max());
      } else if constexpr (!kCanHigh && kCanLow) {
        out.push_back(std::numeric_limits<E>::min());
      }
      return out;
    }
  } else if constexpr (std::is_floating_point_v<E> &&
                       !std::is_floating_point_v<IType>) {
    if constexpr (std::is_same_v<IType, schar>) {
      return {static_cast<E>(42), static_cast<E>(128), static_cast<E>(-129)};
    } else if constexpr (std::is_same_v<IType, uchar>) {
      return {static_cast<E>(42), static_cast<E>(256), static_cast<E>(-1)};
    } else if constexpr (std::is_same_v<IType, short>) {
      return {static_cast<E>(42), static_cast<E>(32768),
              static_cast<E>(-32769)};
    } else if constexpr (std::is_same_v<IType, ushort>) {
      return {static_cast<E>(42), static_cast<E>(65536), static_cast<E>(-1)};
    } else if constexpr (std::is_same_v<IType, int>) {
      return {static_cast<E>(42), static_cast<E>(2147483648.0),
              static_cast<E>(-1e10)};
    } else if constexpr (std::is_same_v<IType, uint>) {
      return {static_cast<E>(42), static_cast<E>(4294967296.0),
              static_cast<E>(-1)};
    } else if constexpr (std::is_same_v<IType, long> ||
                         std::is_same_v<IType, longlong>) {
      return {static_cast<E>(42), static_cast<E>(1e20), static_cast<E>(-1e20)};
    } else {
      return {static_cast<E>(42), static_cast<E>(1e21), static_cast<E>(-1)};
    }
  } else if constexpr (std::is_same_v<E, double> &&
                       std::is_same_v<IType, float>) {
    return {42.0, 1e40, -1e40};
  } else {
    return {};
  }
}

template <typename ExtTag, typename IType>
using GetnFn = int (*)(const void**, size_t, IType*);

template <typename ExtTag, typename IType>
using PutnFn = int (*)(void**, size_t, const IType*, void*);

template <typename ExtTag>
size_t PaddedByteCount(size_t nelems) {
  if constexpr (ExtTag::kBytes == 1) {
    return ((nelems + 3U) / 4U) * 4U;
  } else {
    return (nelems + (nelems % 2U)) * 2U;
  }
}

template <typename ExtTag, typename IType>
void VerifyFractionalConversions(GetnFn<ExtTag, IType> getn_fn,
                                 PutnFn<ExtTag, IType> putn_fn) {
  using E = typename ExtTag::Native;
  if constexpr (!std::is_floating_point_v<E> &&
                std::is_floating_point_v<IType>) {
    const std::vector<IType> frac =
        std::is_signed_v<E> ? std::vector<IType>{static_cast<IType>(-12.5),
                                                 static_cast<IType>(-0.75),
                                                 static_cast<IType>(-0.0),
                                                 static_cast<IType>(0.0),
                                                 static_cast<IType>(0.75),
                                                 static_cast<IType>(3.75),
                                                 static_cast<IType>(126.9)}
                            : std::vector<IType>{static_cast<IType>(0.0),
                                                 static_cast<IType>(0.75),
                                                 static_cast<IType>(3.75),
                                                 static_cast<IType>(126.9),
                                                 static_cast<IType>(254.25)};
    const std::vector<uint8_t> expected_wire =
        EncodeCanonicalWire<ExtTag>(frac);
    std::vector<uint8_t> wire(expected_wire.size() + 2, 0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_NOERR, putn_fn(&wxp, frac.size(), frac.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + expected_wire.size());
    EXPECT_EQ(wire[expected_wire.size()], 0xAA);
    EXPECT_THAT(
        std::vector<uint8_t>(wire.begin(), wire.begin() + expected_wire.size()),
        ElementsAreArray(expected_wire));
  } else if constexpr (std::is_floating_point_v<E> &&
                       !std::is_floating_point_v<IType>) {
    const std::vector<E> frac =
        std::is_signed_v<IType>
            ? std::vector<E>{static_cast<E>(-12.5), static_cast<E>(-0.75),
                             static_cast<E>(-0.0),  static_cast<E>(0.0),
                             static_cast<E>(0.75),  static_cast<E>(3.75),
                             static_cast<E>(126.9)}
            : std::vector<E>{static_cast<E>(0.0), static_cast<E>(0.75),
                             static_cast<E>(3.75), static_cast<E>(126.9),
                             static_cast<E>(254.25)};
    const std::vector<uint8_t> wire = EncodeCanonicalWire<ExtTag>(frac);
    std::vector<IType> out(frac.size(), static_cast<IType>(99));
    const void* rxp = wire.data();
    EXPECT_EQ(NC_NOERR, getn_fn(&rxp, frac.size(), out.data()));
    EXPECT_EQ(rxp, wire.data() + wire.size());
    for (size_t i = 0; i < frac.size(); ++i) {
      EXPECT_EQ(out[i], static_cast<IType>(frac[i]));
    }
  }
}

template <typename ExtTag, typename IType>
void VerifyUnpaddedPair(GetnFn<ExtTag, IType> getn_fn,
                        PutnFn<ExtTag, IType> putn_fn, const char* pair_name) {
  SCOPED_TRACE(pair_name);
  const std::vector<IType> valid = MakeValidValues<ExtTag, IType>();
  const std::vector<uint8_t> expected_wire = EncodeCanonicalWire<ExtTag>(valid);
  const size_t nbytes = expected_wire.size();

  // Zero elements: neither pointer nor buffer may change.
  {
    std::array<uint8_t, 16> wire;
    wire.fill(0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_NOERR, putn_fn(&wxp, 0, valid.data(), nullptr));
    EXPECT_EQ(wxp, wire.data());
    EXPECT_EQ(wire[0], 0xAA);

    std::array<IType, 2> out{static_cast<IType>(99), static_cast<IType>(99)};
    const void* rxp = wire.data();
    EXPECT_EQ(NC_NOERR, getn_fn(&rxp, 0, out.data()));
    EXPECT_EQ(rxp, wire.data());
    EXPECT_EQ(out[0], static_cast<IType>(99));
  }

  // Aligned (offset=0, fillp=nullptr) and unaligned (offset=1, non-null fillp).
  for (size_t offset : {size_t{0}, size_t{1}}) {
    uint64_t fill_buf = 0;
    void* fillp = (offset == 0) ? nullptr : &fill_buf;
    std::vector<uint8_t> wire(offset + nbytes + 4, 0xAA);
    void* wxp = wire.data() + offset;

    ASSERT_EQ(NC_NOERR, putn_fn(&wxp, valid.size(), valid.data(), fillp));
    EXPECT_EQ(wxp, wire.data() + offset + nbytes);
    if (offset > 0) {
      EXPECT_EQ(wire[0], 0xAA);
    }
    EXPECT_EQ(wire[offset + nbytes], 0xAA);
    EXPECT_THAT(std::vector<uint8_t>(wire.begin() + offset,
                                     wire.begin() + offset + nbytes),
                ElementsAreArray(expected_wire));

    std::vector<IType> out(valid.size(), static_cast<IType>(99));
    const void* rxp = wire.data() + offset;
    ASSERT_EQ(NC_NOERR, getn_fn(&rxp, valid.size(), out.data()));
    EXPECT_EQ(rxp, wire.data() + offset + nbytes);
    for (size_t i = 0; i < valid.size(); ++i) {
      ExpectValueEqual<ExtTag>(out[i], valid[i], i);
    }
  }

  VerifyFractionalConversions<ExtTag, IType>(getn_fn, putn_fn);

  const std::vector<IType> bad_put = PutnErangeInputs<ExtTag, IType>();
  if (!bad_put.empty()) {
    const size_t bad_bytes = bad_put.size() * ExtTag::kBytes;
    std::vector<uint8_t> wire(bad_bytes + 4, 0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_ERANGE,
              putn_fn(&wxp, bad_put.size(), bad_put.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + bad_bytes);
    EXPECT_EQ(wire[bad_bytes], 0xAA);
  }

  const std::vector<typename ExtTag::Native> bad_get =
      GetnErangeWireInputs<ExtTag, IType>();
  if (!bad_get.empty()) {
    const std::vector<uint8_t> wire = EncodeCanonicalWire<ExtTag>(bad_get);
    std::vector<IType> out(bad_get.size(), static_cast<IType>(99));
    const void* rxp = wire.data();
    EXPECT_EQ(NC_ERANGE, getn_fn(&rxp, bad_get.size(), out.data()));
    EXPECT_EQ(rxp, wire.data() + wire.size());
  }
}

template <typename ExtTag, typename IType>
void VerifyPaddedPair(GetnFn<ExtTag, IType> pad_getn_fn,
                      PutnFn<ExtTag, IType> pad_putn_fn,
                      const char* pair_name) {
  SCOPED_TRACE(pair_name);
  const std::vector<IType> valid = MakeValidValues<ExtTag, IType>();
  ASSERT_GE(valid.size(), 4U);

  // Zero elements: neither pointer nor buffer may change.
  {
    std::array<uint8_t, 16> wire;
    wire.fill(0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_NOERR, pad_putn_fn(&wxp, 0, valid.data(), nullptr));
    EXPECT_EQ(wxp, wire.data());
    EXPECT_EQ(wire[0], 0xAA);

    std::array<IType, 2> out{static_cast<IType>(99), static_cast<IType>(99)};
    const void* rxp = wire.data();
    EXPECT_EQ(NC_NOERR, pad_getn_fn(&rxp, 0, out.data()));
    EXPECT_EQ(rxp, wire.data());
    EXPECT_EQ(out[0], static_cast<IType>(99));
  }

  // Test nelems=1, 3 (padded) and 4 (already aligned), aligned & unaligned.
  for (size_t nelems : {size_t{1}, size_t{3}, size_t{4}}) {
    std::vector<IType> slice(valid.begin(), valid.begin() + (nelems - 1));
    slice.push_back(valid.back());
    const std::vector<uint8_t> expected_payload =
        EncodeCanonicalWire<ExtTag>(slice);
    const size_t payload_bytes = expected_payload.size();
    const size_t total_bytes = PaddedByteCount<ExtTag>(nelems);

    for (size_t offset : {size_t{0}, size_t{1}}) {
      uint64_t fill_buf = 0;
      void* fillp = (offset == 0) ? nullptr : &fill_buf;
      std::vector<uint8_t> wire(offset + total_bytes + 4, 0xAA);
      void* wxp = wire.data() + offset;

      ASSERT_EQ(NC_NOERR, pad_putn_fn(&wxp, nelems, slice.data(), fillp));
      EXPECT_EQ(wxp, wire.data() + offset + total_bytes);
      if (offset > 0) {
        EXPECT_EQ(wire[0], 0xAA);
      }
      EXPECT_EQ(wire[offset + total_bytes], 0xAA);
      EXPECT_THAT(std::vector<uint8_t>(wire.begin() + offset,
                                       wire.begin() + offset + payload_bytes),
                  ElementsAreArray(expected_payload));
      for (size_t p = payload_bytes; p < total_bytes; ++p) {
        EXPECT_EQ(wire[offset + p], 0x00) << "pad byte index=" << p;
      }

      std::vector<IType> out(nelems, static_cast<IType>(99));
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, pad_getn_fn(&rxp, nelems, out.data()));
      EXPECT_EQ(rxp, wire.data() + offset + total_bytes);
      for (size_t i = 0; i < nelems; ++i) {
        ExpectValueEqual<ExtTag>(out[i], slice[i], i);
      }
    }
  }

  // Exercise fractional conversion in pad_putn_<byte/short>_<float/double>.
  if constexpr (std::is_floating_point_v<IType>) {
    using E = typename ExtTag::Native;
    const std::vector<IType> frac =
        std::is_signed_v<E> ? std::vector<IType>{static_cast<IType>(-12.5),
                                                 static_cast<IType>(0.75),
                                                 static_cast<IType>(3.75)}
                            : std::vector<IType>{static_cast<IType>(0.0),
                                                 static_cast<IType>(0.75),
                                                 static_cast<IType>(3.75)};
    const std::vector<uint8_t> expected_payload =
        EncodeCanonicalWire<ExtTag>(frac);
    const size_t total_bytes = PaddedByteCount<ExtTag>(frac.size());
    std::vector<uint8_t> wire(total_bytes + 4, 0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_NOERR, pad_putn_fn(&wxp, frac.size(), frac.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + total_bytes);
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(),
                                     wire.begin() + expected_payload.size()),
                ElementsAreArray(expected_payload));
    for (size_t p = expected_payload.size(); p < total_bytes; ++p) {
      EXPECT_EQ(wire[p], 0x00);
    }
  }

  // 3-element ERANGE vectors hit both loop error paths and non-zero rndup pad.
  const std::vector<IType> bad_put = PutnErangeInputs<ExtTag, IType>();
  if (!bad_put.empty()) {
    const size_t total_bytes = PaddedByteCount<ExtTag>(bad_put.size());
    std::vector<uint8_t> wire(total_bytes + 4, 0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_ERANGE,
              pad_putn_fn(&wxp, bad_put.size(), bad_put.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + total_bytes);
    for (size_t p = bad_put.size() * ExtTag::kBytes; p < total_bytes; ++p) {
      EXPECT_EQ(wire[p], 0x00);
    }
    EXPECT_EQ(wire[total_bytes], 0xAA);
  }

  const std::vector<typename ExtTag::Native> bad_get =
      GetnErangeWireInputs<ExtTag, IType>();
  if (!bad_get.empty()) {
    const size_t total_bytes = PaddedByteCount<ExtTag>(bad_get.size());
    std::vector<uint8_t> wire = EncodeCanonicalWire<ExtTag>(bad_get);
    wire.resize(total_bytes, 0x00);
    std::vector<IType> out(bad_get.size(), static_cast<IType>(99));
    const void* rxp = wire.data();
    EXPECT_EQ(NC_ERANGE, pad_getn_fn(&rxp, bad_get.size(), out.data()));
    EXPECT_EQ(rxp, wire.data() + total_bytes);
  }
}

#define NCX_TEST_PAIR(ExtTag, ext_name, IType, int_name)              \
  VerifyUnpaddedPair<ExtTag, IType>(ncx_getn_##ext_name##_##int_name, \
                                    ncx_putn_##ext_name##_##int_name, \
                                    #ext_name "_" #int_name)

#define NCX_TEST_PAD_PAIR(ExtTag, ext_name, IType, int_name)            \
  VerifyPaddedPair<ExtTag, IType>(ncx_pad_getn_##ext_name##_##int_name, \
                                  ncx_pad_putn_##ext_name##_##int_name, \
                                  #ext_name "_" #int_name)

#define NCX_TEST_EXT_ROW(ExtTag, ext_name)                 \
  do {                                                     \
    NCX_TEST_PAIR(ExtTag, ext_name, schar, schar);         \
    NCX_TEST_PAIR(ExtTag, ext_name, uchar, uchar);         \
    NCX_TEST_PAIR(ExtTag, ext_name, short, short);         \
    NCX_TEST_PAIR(ExtTag, ext_name, ushort, ushort);       \
    NCX_TEST_PAIR(ExtTag, ext_name, int, int);             \
    NCX_TEST_PAIR(ExtTag, ext_name, uint, uint);           \
    NCX_TEST_PAIR(ExtTag, ext_name, long, long);           \
    NCX_TEST_PAIR(ExtTag, ext_name, float, float);         \
    NCX_TEST_PAIR(ExtTag, ext_name, double, double);       \
    NCX_TEST_PAIR(ExtTag, ext_name, longlong, longlong);   \
    NCX_TEST_PAIR(ExtTag, ext_name, ulonglong, ulonglong); \
  } while (false)

#define NCX_TEST_PAD_ROW(ExtTag, ext_name)                     \
  do {                                                         \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, schar, schar);         \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, uchar, uchar);         \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, short, short);         \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, ushort, ushort);       \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, int, int);             \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, uint, uint);           \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, long, long);           \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, float, float);         \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, double, double);       \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, longlong, longlong);   \
    NCX_TEST_PAD_PAIR(ExtTag, ext_name, ulonglong, ulonglong); \
  } while (false)

TEST(NcxTest, LenMacrosComputeExpectedSizesAndAlignment) {
  for (size_t n = 0; n <= 8; ++n) {
    const size_t expected_aligned4 = (n + 3U) & ~size_t{3U};
    const size_t expected_short = (n + (n % 2U)) * 2U;
    EXPECT_EQ(ncx_len_char(n), expected_aligned4);
    EXPECT_EQ(ncx_len_ubyte(n), expected_aligned4);
    EXPECT_EQ(ncx_len_short(n), expected_short);
    EXPECT_EQ(ncx_len_ushort(n), expected_short);
    EXPECT_EQ(ncx_len_int(n), n * 4U);
    EXPECT_EQ(ncx_len_uint(n), n * 4U);
    EXPECT_EQ(ncx_len_float(n), n * 4U);
    EXPECT_EQ(ncx_len_double(n), n * 8U);
    EXPECT_EQ(ncx_len_int64(n), n * 8U);
    EXPECT_EQ(ncx_len_uint64(n), n * 8U);
  }
}

TEST(NcxTest, SizeTAndOffTRoundTripAndNegativeOffsetRangeError) {
  // size_t: 32-bit big-endian unsigned sizes up to X_SIZE_MAX.
  for (size_t offset : {size_t{0}, size_t{1}}) {
    const std::array<size_t, 4> kValues = {0U, 1U, 0x12345678U, X_SIZE_MAX};
    for (size_t val : kValues) {
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_put_size_t(&wxp, &val));
      EXPECT_EQ(wxp, wire.data() + offset + 4);
      EXPECT_EQ(wire[offset + 4], 0xAA);

      size_t decoded = 99;
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_get_size_t(&rxp, &decoded));
      EXPECT_EQ(rxp, wire.data() + offset + 4);
      EXPECT_EQ(decoded, val);
    }
  }
  {
    const size_t val = 0x12345678U;
    std::array<uint8_t, 8> wire;
    wire.fill(0xAA);
    void* wxp = wire.data();
    ASSERT_EQ(NC_NOERR, ncx_put_size_t(&wxp, &val));
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(), wire.begin() + 4),
                ElementsAre(0x12, 0x34, 0x56, 0x78));
  }

  // Negative off_t fails with NC_ERANGE before advancing pointer or writing.
  for (size_t off_bytes : {size_t{4}, size_t{8}}) {
    for (off_t neg : {off_t{-1}, off_t{-42}}) {
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data();
      EXPECT_EQ(NC_ERANGE, ncx_put_off_t(&wxp, &neg, off_bytes));
      EXPECT_EQ(wxp, wire.data());
      EXPECT_EQ(wire[0], 0xAA);
    }
  }

  // Valid non-negative 4-byte off_t (MSB <= 0x7f).
  for (size_t offset : {size_t{0}, size_t{3}}) {
    for (off_t val :
         {off_t{0}, off_t{1}, off_t{0x12345678}, off_t{X_OFF_MAX}}) {
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_put_off_t(&wxp, &val, 4));
      EXPECT_EQ(wxp, wire.data() + offset + 4);
      EXPECT_EQ(wire[offset + 4], 0xAA);

      off_t decoded = -99;
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_get_off_t(&rxp, &decoded, 4));
      EXPECT_EQ(rxp, wire.data() + offset + 4);
      EXPECT_EQ(decoded, val);
    }
  }

  // Valid non-negative 8-byte off_t (MSB <= 0x7f).
  for (size_t offset : {size_t{0}, size_t{1}}) {
    for (off_t val :
         {off_t{0}, off_t{0x0102030405060708LL}, off_t{0x7fffffffffffffffLL}}) {
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_put_off_t(&wxp, &val, 8));
      EXPECT_EQ(wxp, wire.data() + offset + 8);
      EXPECT_EQ(wire[offset + 8], 0xAA);

      off_t decoded = -99;
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_get_off_t(&rxp, &decoded, 8));
      EXPECT_EQ(rxp, wire.data() + offset + 8);
      EXPECT_EQ(decoded, val);
    }
  }
}

TEST(NcxTest, Uint32AndUint64PutAndGet) {
  // ncx_put_uint32 covers full unsigned 32-bit range.
  {
    std::array<uint8_t, 12> wire;
    wire.fill(0xAA);
    void* wxp = wire.data() + 1;
    ASSERT_EQ(NC_NOERR, ncx_put_uint32(&wxp, 0x01020304U));
    ASSERT_EQ(NC_NOERR, ncx_put_uint32(&wxp, 0xffffffffU));
    EXPECT_EQ(wxp, wire.data() + 9);
    EXPECT_EQ(wire[0], 0xAA);
    EXPECT_EQ(wire[9], 0xAA);
    EXPECT_THAT(std::vector<uint8_t>(wire.begin() + 1, wire.begin() + 9),
                ElementsAre(0x01, 0x02, 0x03, 0x04, 0xff, 0xff, 0xff, 0xff));
  }

  // ncx_get_uint32 shifts signed int before cast, so keep MSB <= 0x7f.
  for (size_t offset : {size_t{0}, size_t{1}}) {
    for (uint val : {0U, 1U, 0x01020304U, 0x7f8090a0U, 0x7fffffffU}) {
      std::array<uint8_t, 8> wire;
      wire.fill(0xAA);
      void* wxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_put_uint32(&wxp, val));

      uint decoded = 99U;
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_get_uint32(&rxp, &decoded));
      EXPECT_EQ(rxp, wire.data() + offset + 4);
      EXPECT_EQ(decoded, val);
    }
  }

  // ncx_put_uint64 and ncx_get_uint64 handle full 64-bit unsigned range.
  for (size_t offset : {size_t{0}, size_t{3}}) {
    for (unsigned long long val : {0ULL, 1ULL, 0x0102030405060708ULL,
                                   0x8000000000000001ULL, ULLONG_MAX}) {
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_put_uint64(&wxp, val));
      EXPECT_EQ(wxp, wire.data() + offset + 8);
      EXPECT_EQ(wire[offset + 8], 0xAA);

      unsigned long long decoded = 99ULL;
      const void* rxp = wire.data() + offset;
      ASSERT_EQ(NC_NOERR, ncx_get_uint64(&rxp, &decoded));
      EXPECT_EQ(rxp, wire.data() + offset + 8);
      EXPECT_EQ(decoded, val);
    }
  }
}

TEST(NcxTest, TextCharCharAndVoidUnpaddedAndPadded) {
  // Unpadded text and char_char alias.
  {
    const std::array<char, 5> src = {'n', 'c', 'x', '-', '5'};
    std::array<uint8_t, 12> wire;
    wire.fill(0xAA);

    void* wxp = wire.data();
    EXPECT_EQ(NC_NOERR, ncx_putn_text(&wxp, 0, src.data()));
    EXPECT_EQ(wxp, wire.data());
    EXPECT_EQ(wire[0], 0xAA);

    wxp = wire.data() + 1;
    ASSERT_EQ(NC_NOERR, ncx_putn_text(&wxp, src.size(), src.data()));
    EXPECT_EQ(wxp, wire.data() + 1 + src.size());
    EXPECT_EQ(wire[0], 0xAA);
    EXPECT_EQ(wire[1 + src.size()], 0xAA);

    std::array<char, 5> dst{};
    const void* rxp = wire.data() + 1;
    ASSERT_EQ(NC_NOERR, ncx_getn_text(&rxp, src.size(), dst.data()));
    EXPECT_EQ(rxp, wire.data() + 1 + src.size());
    EXPECT_EQ(dst, src);

    const std::array<char, 4> csrc = {'Z', 'e', 'd', '9'};
    wire.fill(0xAA);
    wxp = wire.data();
    ASSERT_EQ(NC_NOERR, ncx_putn_char_char(&wxp, csrc.size(), csrc.data()));
    EXPECT_EQ(wxp, wire.data() + csrc.size());
    std::array<char, 4> cdst{};
    rxp = wire.data();
    ASSERT_EQ(NC_NOERR, ncx_getn_char_char(&rxp, csrc.size(), cdst.data()));
    EXPECT_EQ(cdst, csrc);
  }

  // Padded text across modulo-4 lengths (0, 1, 3, 4, 5).
  {
    const std::array<char, 5> src = {'a', 'b', 'c', 'd', 'e'};
    for (size_t n : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{5}}) {
      const size_t aligned = (n + 3U) & ~size_t{3U};
      std::array<uint8_t, 16> wire;
      wire.fill(0xAA);
      void* wxp = wire.data();
      ASSERT_EQ(NC_NOERR, ncx_pad_putn_text(&wxp, n, src.data()));
      EXPECT_EQ(wxp, wire.data() + aligned);
      EXPECT_EQ(wire[aligned], 0xAA);
      for (size_t i = n; i < aligned; ++i) {
        EXPECT_EQ(wire[i], 0x00);
      }

      std::array<char, 5> dst{};
      dst.fill('?');
      const void* rxp = wire.data();
      ASSERT_EQ(NC_NOERR, ncx_pad_getn_text(&rxp, n, dst.data()));
      EXPECT_EQ(rxp, wire.data() + aligned);
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(dst[i], src[i]);
      }
      for (size_t i = n; i < dst.size(); ++i) {
        EXPECT_EQ(dst[i], '?');
      }
    }
  }

  // Unpadded and padded void (opaque byte arrays).
  {
    const std::array<uint8_t, 5> src = {0x00, 0x7f, 0x80, 0xff, 0x55};
    std::array<uint8_t, 16> wire;
    wire.fill(0xAA);
    void* wxp = wire.data() + 1;
    ASSERT_EQ(NC_NOERR, ncx_putn_void(&wxp, src.size(), src.data()));
    EXPECT_EQ(wxp, wire.data() + 1 + src.size());
    EXPECT_EQ(wire[0], 0xAA);
    EXPECT_EQ(wire[1 + src.size()], 0xAA);

    std::array<uint8_t, 5> dst{};
    const void* rxp = wire.data() + 1;
    ASSERT_EQ(NC_NOERR, ncx_getn_void(&rxp, src.size(), dst.data()));
    EXPECT_EQ(rxp, wire.data() + 1 + src.size());
    EXPECT_EQ(dst, src);

    for (size_t n : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{5}}) {
      const size_t aligned = (n + 3U) & ~size_t{3U};
      wire.fill(0xAA);
      wxp = wire.data();
      ASSERT_EQ(NC_NOERR, ncx_pad_putn_void(&wxp, n, src.data()));
      EXPECT_EQ(wxp, wire.data() + aligned);
      EXPECT_EQ(wire[aligned], 0xAA);
      for (size_t i = n; i < aligned; ++i) {
        EXPECT_EQ(wire[i], 0x00);
      }

      std::array<uint8_t, 5> pdst;
      pdst.fill(0x5A);
      rxp = wire.data();
      ASSERT_EQ(NC_NOERR, ncx_pad_getn_void(&rxp, n, pdst.data()));
      EXPECT_EQ(rxp, wire.data() + aligned);
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(pdst[i], src[i]);
      }
      for (size_t i = n; i < pdst.size(); ++i) {
        EXPECT_EQ(pdst[i], 0x5A);
      }
    }
  }
}

TEST(NcxTest, BigEndianWireBytesAndFloatDoubleBoundaryConversions) {
  // Known big-endian encodings verified explicitly against IEEE/two's-comp.
  {
    std::array<uint8_t, 16> wire;
    wire.fill(0xAA);

    const std::array<short, 2> s = {-2, 0x1234};
    void* wxp = wire.data();
    ASSERT_EQ(NC_NOERR,
              ncx_putn_short_short(&wxp, s.size(), s.data(), nullptr));
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(), wire.begin() + 4),
                ElementsAre(0xff, 0xfe, 0x12, 0x34));

    const std::array<int, 2> i = {-2, 0x12345678};
    wxp = wire.data();
    ASSERT_EQ(NC_NOERR, ncx_putn_int_int(&wxp, i.size(), i.data(), nullptr));
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(), wire.begin() + 8),
                ElementsAre(0xff, 0xff, 0xff, 0xfe, 0x12, 0x34, 0x56, 0x78));

    const std::array<float, 3> f = {1.5f, -12.5f, -0.0f};
    wxp = wire.data();
    ASSERT_EQ(NC_NOERR,
              ncx_putn_float_float(&wxp, f.size(), f.data(), nullptr));
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(), wire.begin() + 12),
                ElementsAre(0x3f, 0xc0, 0x00, 0x00, 0xc1, 0x48, 0x00, 0x00,
                            0x80, 0x00, 0x00, 0x00));

    const std::array<double, 2> d = {-12.5, -0.0};
    wxp = wire.data();
    ASSERT_EQ(NC_NOERR,
              ncx_putn_double_double(&wxp, d.size(), d.data(), nullptr));
    EXPECT_THAT(std::vector<uint8_t>(wire.begin(), wire.begin() + 16),
                ElementsAre(0xc0, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
  }

  // Exact IEEE-754 boundary checks in
  // ncx_get_{float,double}_{longlong,ulonglong}.
  {
    const std::vector<float> f64s = {9223372036854775808.0f,
                                     -9223372036854775808.0f, 0.0f};
    const std::vector<uint8_t> fw = EncodeCanonicalWire<ExtFloat>(f64s);
    std::array<longlong, 3> out_ll{};
    const void* rxp = fw.data();
    EXPECT_EQ(NC_NOERR,
              ncx_getn_float_longlong(&rxp, f64s.size(), out_ll.data()));
    EXPECT_EQ(out_ll[0], LLONG_MAX);
    EXPECT_EQ(out_ll[1], LLONG_MIN);
    EXPECT_EQ(out_ll[2], 0LL);

    const std::vector<float> f64u = {18446744073709551616.0f, 0.0f};
    const std::vector<uint8_t> fwu = EncodeCanonicalWire<ExtFloat>(f64u);
    std::array<ulonglong, 2> out_ull{};
    rxp = fwu.data();
    EXPECT_EQ(NC_NOERR,
              ncx_getn_float_ulonglong(&rxp, f64u.size(), out_ull.data()));
    EXPECT_EQ(out_ull[0], ULLONG_MAX);
    EXPECT_EQ(out_ull[1], 0ULL);

    const std::vector<double> d64s = {9223372036854775808.0,
                                      -9223372036854775808.0, 0.0};
    const std::vector<uint8_t> dw = EncodeCanonicalWire<ExtDouble>(d64s);
    rxp = dw.data();
    EXPECT_EQ(NC_NOERR,
              ncx_getn_double_longlong(&rxp, d64s.size(), out_ll.data()));
    EXPECT_EQ(out_ll[0], LLONG_MAX);
    EXPECT_EQ(out_ll[1], LLONG_MIN);
    EXPECT_EQ(out_ll[2], 0LL);

    const std::vector<double> d64u = {18446744073709551616.0, 0.0};
    const std::vector<uint8_t> dwu = EncodeCanonicalWire<ExtDouble>(d64u);
    rxp = dwu.data();
    EXPECT_EQ(NC_NOERR,
              ncx_getn_double_ulonglong(&rxp, d64u.size(), out_ull.data()));
    EXPECT_EQ(out_ull[0], ULLONG_MAX);
    EXPECT_EQ(out_ull[1], 0ULL);
  }

  // ncx_get_double_float clamps out-of-range values to +/-FLT_MAX with
  // NC_ERANGE.
  {
    const std::vector<double> dbl_vals = {1e40, -1e40, 2.25};
    const std::vector<uint8_t> wire = EncodeCanonicalWire<ExtDouble>(dbl_vals);
    std::array<float, 3> out{};
    const void* rxp = wire.data();
    EXPECT_EQ(NC_ERANGE,
              ncx_getn_double_float(&rxp, dbl_vals.size(), out.data()));
    EXPECT_EQ(rxp, wire.data() + wire.size());
    EXPECT_EQ(out[0], FLT_MAX);
    EXPECT_EQ(out[1], -FLT_MAX);
    EXPECT_EQ(out[2], 2.25f);
  }

  // ncx_put_double_float reports NC_ERANGE on +/-infinity without UB and
  // ncx_getn_double_float clamps those encoded wire infinities to +/-FLT_MAX.
  {
    const std::array<float, 3> flt_vals = {
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), 2.25f};
    std::vector<uint8_t> wire(flt_vals.size() * X_SIZEOF_DOUBLE + 4, 0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_ERANGE, ncx_putn_double_float(&wxp, flt_vals.size(),
                                               flt_vals.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + flt_vals.size() * X_SIZEOF_DOUBLE);
    EXPECT_EQ(wire[flt_vals.size() * X_SIZEOF_DOUBLE], 0xAA);

    std::array<float, 3> decoded{};
    const void* rxp = wire.data();
    EXPECT_EQ(NC_ERANGE,
              ncx_getn_double_float(&rxp, flt_vals.size(), decoded.data()));
    EXPECT_EQ(rxp, wire.data() + flt_vals.size() * X_SIZEOF_DOUBLE);
    EXPECT_EQ(decoded[0], FLT_MAX);
    EXPECT_EQ(decoded[1], -FLT_MAX);
    EXPECT_EQ(decoded[2], 2.25f);
  }

  // Aggregate conversions continue processing after an initial NC_ERANGE and
  // preserve the first error rather than overwriting it on valid elements.
  {
    const std::array<int, 3> src_int = {100000, 123, -100000};
    std::array<uint8_t, 8> wire;
    wire.fill(0xAA);
    void* wxp = wire.data();
    EXPECT_EQ(NC_ERANGE, ncx_putn_short_int(&wxp, src_int.size(),
                                            src_int.data(), nullptr));
    EXPECT_EQ(wxp, wire.data() + src_int.size() * X_SIZEOF_SHORT);
    EXPECT_EQ(wire[2], 0x00);
    EXPECT_EQ(wire[3], 0x7b);
    EXPECT_EQ(wire[src_int.size() * X_SIZEOF_SHORT], 0xAA);

    const std::vector<uint8_t> ext_int_wire = EncodeCanonicalWire<ExtInt>(
        std::vector<int>(src_int.begin(), src_int.end()));
    std::array<short, 3> out_short{99, 99, 99};
    const void* rxp = ext_int_wire.data();
    EXPECT_EQ(NC_ERANGE,
              ncx_getn_int_short(&rxp, src_int.size(), out_short.data()));
    EXPECT_EQ(rxp, ext_int_wire.data() + ext_int_wire.size());
    EXPECT_EQ(out_short[1], 123);
  }
}

TEST(NcxMatrixTest, ScharUnpaddedAndPadded) {
  NCX_TEST_EXT_ROW(ExtSchar, schar);
  NCX_TEST_PAD_ROW(ExtSchar, schar);
}

TEST(NcxMatrixTest, UcharUnpaddedAndPadded) {
  NCX_TEST_EXT_ROW(ExtUchar, uchar);
  NCX_TEST_PAD_ROW(ExtUchar, uchar);
}

TEST(NcxMatrixTest, ShortUnpaddedAndPadded) {
  NCX_TEST_EXT_ROW(ExtShort, short);
  NCX_TEST_PAD_ROW(ExtShort, short);
}

TEST(NcxMatrixTest, UshortUnpaddedAndPadded) {
  NCX_TEST_EXT_ROW(ExtUshort, ushort);
  NCX_TEST_PAD_ROW(ExtUshort, ushort);
}

TEST(NcxMatrixTest, IntUnpadded) { NCX_TEST_EXT_ROW(ExtInt, int); }

TEST(NcxMatrixTest, UintUnpadded) { NCX_TEST_EXT_ROW(ExtUint, uint); }

TEST(NcxMatrixTest, FloatUnpadded) { NCX_TEST_EXT_ROW(ExtFloat, float); }

TEST(NcxMatrixTest, DoubleUnpadded) { NCX_TEST_EXT_ROW(ExtDouble, double); }

TEST(NcxMatrixTest, LonglongUnpadded) {
  NCX_TEST_EXT_ROW(ExtLonglong, longlong);
}

TEST(NcxMatrixTest, UlonglongUnpadded) {
  NCX_TEST_EXT_ROW(ExtUlonglong, ulonglong);
}

#undef NCX_TEST_PAD_ROW
#undef NCX_TEST_EXT_ROW
#undef NCX_TEST_PAD_PAIR
#undef NCX_TEST_PAIR

}  // namespace
}  // namespace netcdf
