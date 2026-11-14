// Unit tests for third_party/kakadu/vendor/apps/image/neon_sample_reorg.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES`, and `kdu_arch.h` checks `__ARM_NEON__`),
// `neon_sample_reorg.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on both
// x86_64 and `--cpu=arm`). These tests exercise `kdu_sample_reorg_in` and
// `kdu_sample_reorg_out` across all paths accelerated by
// `neon_sample_reorg.cpp`: 8-bit de-interleave/interleave (2/3/4 channels,
// counts > 32) and 16-bit de-interleave/interleave/byte-reverse (1/2/3/4
// channels, counts > 16), including exact vector multiples, remainder
// backtrack tails, and unaligned offsets.

#include <array>
#include <cstddef>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/image/kdu_sample_reorg.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_uint16;

kdu_uint16 SwapBytes16(kdu_uint16 val) {
  return static_cast<kdu_uint16>((val >> 8U) | (val << 8U));
}

template <typename T>
T PatternSample(size_t pixel, int channel) {
  if constexpr (sizeof(T) == 1) {
    return static_cast<T>((pixel * 37U + channel * 71U + 13U) & 0xFFU);
  } else {
    return static_cast<T>((pixel * 257U + channel * 3571U + 0x2468U) & 0xFFFFU);
  }
}

template <typename T>
T ApplyReverse(T val, bool reverse_bytes) {
  if constexpr (sizeof(T) == 2) {
    return reverse_bytes ? SwapBytes16(val) : val;
  }
  return val;
}

template <typename T>
void VerifyReorgIn(int channels, bool reverse_bytes, size_t pixels,
                   size_t offset_elems) {
  std::vector<T> src_buf(offset_elems + pixels * static_cast<size_t>(channels) +
                         16);
  T* src = src_buf.data() + offset_elems;
  for (size_t p = 0; p < pixels; ++p) {
    for (int c = 0; c < channels; ++c) {
      src[p * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
          PatternSample<T>(p, c);
    }
  }

  std::array<std::vector<T>, 4> dst_vecs;
  std::array<void*, 4> dst_vptrs{};
  for (int c = 0; c < 4; ++c) {
    dst_vecs[static_cast<size_t>(c)].assign(offset_elems + pixels + 16, T{});
    dst_vptrs[static_cast<size_t>(c)] =
        dst_vecs[static_cast<size_t>(c)].data() + offset_elems;
  }

  kdu_sample_reorg_in reorg;
  const bool ok =
      reorg.init(static_cast<int>(sizeof(T)), channels, reverse_bytes, pixels);
  if (!ok) {
    EXPECT_FALSE(reorg.exists());
    EXPECT_FALSE(reorg.reorg(dst_vptrs.data(), src, pixels));
    return;
  }
  EXPECT_TRUE(reorg.exists());
  ASSERT_TRUE(reorg.reorg(dst_vptrs.data(), src, pixels));
  for (int c = 0; c < channels; ++c) {
    const T* dst = static_cast<const T*>(dst_vptrs[static_cast<size_t>(c)]);
    for (size_t p = 0; p < pixels; ++p) {
      EXPECT_EQ(dst[p], ApplyReverse<T>(PatternSample<T>(p, c), reverse_bytes));
    }
  }
}

template <typename T>
void VerifyReorgOut(int channels, bool reverse_bytes, size_t pixels,
                    size_t offset_elems) {
  std::array<std::vector<T>, 4> src_vecs;
  std::array<void*, 4> src_vptrs{};
  for (int c = 0; c < 4; ++c) {
    src_vecs[static_cast<size_t>(c)].assign(offset_elems + pixels + 16, T{});
    T* ptr = src_vecs[static_cast<size_t>(c)].data() + offset_elems;
    for (size_t p = 0; p < pixels; ++p) {
      ptr[p] = PatternSample<T>(p, c);
    }
    src_vptrs[static_cast<size_t>(c)] = ptr;
  }

  std::vector<T> dst_buf(offset_elems + pixels * static_cast<size_t>(channels) +
                         16);
  T* dst = dst_buf.data() + offset_elems;

  kdu_sample_reorg_out reorg;
  const bool ok =
      reorg.init(static_cast<int>(sizeof(T)), channels, reverse_bytes, pixels);
  if (!ok) {
    EXPECT_FALSE(reorg.exists());
    EXPECT_FALSE(reorg.reorg(dst, src_vptrs.data(), pixels));
    return;
  }
  EXPECT_TRUE(reorg.exists());
  ASSERT_TRUE(reorg.reorg(dst, src_vptrs.data(), pixels));
  for (size_t p = 0; p < pixels; ++p) {
    for (int c = 0; c < channels; ++c) {
      EXPECT_EQ(dst[p * static_cast<size_t>(channels) + static_cast<size_t>(c)],
                ApplyReverse<T>(PatternSample<T>(p, c), reverse_bytes));
    }
  }
}

TEST(NeonSampleReorgTest, Uint8MultiChannelAndBacktrackTails) {
  for (int ch : {2, 3, 4}) {
    for (size_t pixels : {size_t{32}, size_t{33}, size_t{64}, size_t{83}}) {
      for (size_t off : {size_t{0}, size_t{1}, size_t{5}}) {
        VerifyReorgIn<kdu_byte>(ch, false, pixels, off);
        VerifyReorgOut<kdu_byte>(ch, false, pixels, off);
      }
    }
  }
}

TEST(NeonSampleReorgTest, Uint16ChannelsRevBytesAndBacktrackTails) {
  for (int ch : {1, 2, 3, 4}) {
    for (bool rev : {false, true}) {
      for (size_t pixels : {size_t{16}, size_t{17}, size_t{32}, size_t{45}}) {
        for (size_t off : {size_t{0}, size_t{1}}) {
          VerifyReorgIn<kdu_uint16>(ch, rev, pixels, off);
          VerifyReorgOut<kdu_uint16>(ch, rev, pixels, off);
        }
      }
    }
  }
}

}  // namespace
}  // namespace kdu_supp
