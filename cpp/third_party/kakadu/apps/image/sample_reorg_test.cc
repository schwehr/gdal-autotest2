// Unit tests for third_party/kakadu/vendor/apps/image/sample_reorg.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/image/kdu_sample_reorg.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

template <typename T>
T MakePatternSample(size_t pixel, int channel) {
  if constexpr (std::is_same_v<T, kdu_core::kdu_byte>) {
    return static_cast<kdu_core::kdu_byte>((pixel * 17U + channel * 53U + 11U) &
                                           0xFFU);
  } else if constexpr (std::is_same_v<T, kdu_core::kdu_uint16>) {
    return static_cast<kdu_core::kdu_uint16>(
        (pixel * 257U + channel * 4099U + 0x1234U) & 0xFFFFU);
  } else if constexpr (std::is_same_v<T, kdu_core::kdu_uint32>) {
    return static_cast<kdu_core::kdu_uint32>(
        pixel * 65537U + static_cast<size_t>(channel) * 0x01010101U +
        0x12345678U);
  } else {
    return static_cast<float>(pixel) * 0.25f +
           static_cast<float>(channel) * 10.0f;
  }
}

template <typename T>
T ExpectedSample(T value, bool reverse_bytes) {
  if (!reverse_bytes || sizeof(T) == 1) {
    return value;
  }
  if constexpr (sizeof(T) == 2) {
    const auto raw = static_cast<uint16_t>(value);
    return static_cast<T>(static_cast<uint16_t>((raw >> 8U) | (raw << 8U)));
  } else {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    raw = ((raw & 0x000000FFU) << 24U) | ((raw & 0x0000FF00U) << 8U) |
          ((raw & 0x00FF0000U) >> 8U) | ((raw & 0xFF000000U) >> 24U);
    T swapped{};
    std::memcpy(&swapped, &raw, sizeof(swapped));
    return swapped;
  }
}

template <typename T>
bool CallTypedReorgIn(kdu_sample_reorg_in& reorg, int channels,
                      std::array<std::vector<T>, 4>& dst, std::vector<T>& src,
                      size_t pixels) {
  switch (channels) {
    case 1:
      return reorg.reorg(dst[0].data(), src.data(), pixels);
    case 2:
      return reorg.reorg(dst[0].data(), dst[1].data(), src.data(), pixels);
    case 3:
      return reorg.reorg(dst[0].data(), dst[1].data(), dst[2].data(),
                         src.data(), pixels);
    case 4:
      return reorg.reorg(dst[0].data(), dst[1].data(), dst[2].data(),
                         dst[3].data(), src.data(), pixels);
    default:
      return false;
  }
}

template <typename T>
bool CallTypedReorgOut(kdu_sample_reorg_out& reorg, int channels,
                       std::vector<T>& dst, std::array<std::vector<T>, 4>& src,
                       size_t pixels) {
  switch (channels) {
    case 1:
      return reorg.reorg(dst.data(), src[0].data(), pixels);
    case 2:
      return reorg.reorg(dst.data(), src[0].data(), src[1].data(), pixels);
    case 3:
      return reorg.reorg(dst.data(), src[0].data(), src[1].data(),
                         src[2].data(), pixels);
    case 4:
      return reorg.reorg(dst.data(), src[0].data(), src[1].data(),
                         src[2].data(), src[3].data(), pixels);
    default:
      return false;
  }
}

template <typename T>
void RunReorgInCase(int channels, bool reverse_bytes, size_t min_pixels,
                    size_t pixels) {
  ASSERT_GE(channels, 1);
  ASSERT_LE(channels, 4);

  std::vector<T> src(pixels * static_cast<size_t>(channels));
  for (size_t p = 0; p < pixels; ++p) {
    for (int c = 0; c < channels; ++c) {
      src[p * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
          MakePatternSample<T>(p, c);
    }
  }

  std::array<std::vector<T>, 4> dst;
  std::array<void*, 4> dst_ptrs{};
  for (int c = 0; c < 4; ++c) {
    dst[static_cast<size_t>(c)].assign(pixels, T{});
    dst_ptrs[static_cast<size_t>(c)] = dst[static_cast<size_t>(c)].data();
  }

  kdu_sample_reorg_in reorg;
  EXPECT_FALSE(reorg.exists());
  EXPECT_TRUE(!reorg);
  EXPECT_FALSE(CallTypedReorgIn(reorg, channels, dst, src, pixels));
  EXPECT_FALSE(reorg.reorg(dst_ptrs.data(), src.data(), pixels));

  const bool initialized = reorg.init(static_cast<int>(sizeof(T)), channels,
                                      reverse_bytes, min_pixels);
  EXPECT_EQ(reorg.exists(), initialized);
  EXPECT_EQ(!reorg, !initialized);

  if (!initialized) {
    EXPECT_FALSE(CallTypedReorgIn(reorg, channels, dst, src, pixels));
    EXPECT_FALSE(reorg.reorg(dst_ptrs.data(), src.data(), pixels));
    return;
  }

  EXPECT_TRUE(CallTypedReorgIn(reorg, channels, dst, src, pixels));
  for (int c = 0; c < channels; ++c) {
    for (size_t p = 0; p < pixels; ++p) {
      EXPECT_EQ(dst[static_cast<size_t>(c)][p],
                ExpectedSample<T>(MakePatternSample<T>(p, c), reverse_bytes));
    }
  }

  for (int c = 0; c < channels; ++c) {
    dst[static_cast<size_t>(c)].assign(pixels, T{});
  }
  EXPECT_TRUE(reorg.reorg(dst_ptrs.data(), src.data(), pixels));
  for (int c = 0; c < channels; ++c) {
    for (size_t p = 0; p < pixels; ++p) {
      EXPECT_EQ(dst[static_cast<size_t>(c)][p],
                ExpectedSample<T>(MakePatternSample<T>(p, c), reverse_bytes));
    }
  }

  if (min_pixels > 0) {
    EXPECT_FALSE(CallTypedReorgIn(reorg, channels, dst, src, min_pixels - 1));
    EXPECT_FALSE(reorg.reorg(dst_ptrs.data(), src.data(), min_pixels - 1));
  }

  EXPECT_FALSE(CallTypedReorgIn(reorg, (channels % 4) + 1, dst, src, pixels));

  if constexpr (sizeof(T) == 4) {
    std::array<std::vector<kdu_core::kdu_byte>, 4> wrong_dst;
    for (auto& vec : wrong_dst) {
      vec.assign(pixels, 0);
    }
    std::vector<kdu_core::kdu_byte> wrong_src(
        pixels * static_cast<size_t>(channels), 0);
    EXPECT_FALSE(
        CallTypedReorgIn(reorg, channels, wrong_dst, wrong_src, pixels));
  } else {
    std::array<std::vector<kdu_core::kdu_uint32>, 4> wrong_dst;
    for (auto& vec : wrong_dst) {
      vec.assign(pixels, 0);
    }
    std::vector<kdu_core::kdu_uint32> wrong_src(
        pixels * static_cast<size_t>(channels), 0);
    EXPECT_FALSE(
        CallTypedReorgIn(reorg, channels, wrong_dst, wrong_src, pixels));
  }

  reorg.clear();
  EXPECT_FALSE(reorg.exists());
  EXPECT_TRUE(!reorg);
  EXPECT_FALSE(CallTypedReorgIn(reorg, channels, dst, src, pixels));
}

template <typename T>
void RunReorgOutCase(int channels, bool reverse_bytes, size_t min_pixels,
                     size_t pixels) {
  ASSERT_GE(channels, 1);
  ASSERT_LE(channels, 4);

  std::array<std::vector<T>, 4> src;
  std::array<void*, 4> src_ptrs{};
  for (int c = 0; c < 4; ++c) {
    src[static_cast<size_t>(c)].resize(pixels);
    for (size_t p = 0; p < pixels; ++p) {
      src[static_cast<size_t>(c)][p] = MakePatternSample<T>(p, c);
    }
    src_ptrs[static_cast<size_t>(c)] = src[static_cast<size_t>(c)].data();
  }
  std::vector<T> dst(pixels * static_cast<size_t>(channels), T{});

  kdu_sample_reorg_out reorg;
  EXPECT_FALSE(reorg.exists());
  EXPECT_TRUE(!reorg);
  EXPECT_FALSE(CallTypedReorgOut(reorg, channels, dst, src, pixels));
  EXPECT_FALSE(reorg.reorg(dst.data(), src_ptrs.data(), pixels));

  const bool initialized = reorg.init(static_cast<int>(sizeof(T)), channels,
                                      reverse_bytes, min_pixels);
  EXPECT_EQ(reorg.exists(), initialized);
  EXPECT_EQ(!reorg, !initialized);

  if (!initialized) {
    EXPECT_FALSE(CallTypedReorgOut(reorg, channels, dst, src, pixels));
    EXPECT_FALSE(reorg.reorg(dst.data(), src_ptrs.data(), pixels));
    return;
  }

  EXPECT_TRUE(CallTypedReorgOut(reorg, channels, dst, src, pixels));
  for (size_t p = 0; p < pixels; ++p) {
    for (int c = 0; c < channels; ++c) {
      EXPECT_EQ(dst[p * static_cast<size_t>(channels) + static_cast<size_t>(c)],
                ExpectedSample<T>(MakePatternSample<T>(p, c), reverse_bytes));
    }
  }

  dst.assign(dst.size(), T{});
  EXPECT_TRUE(reorg.reorg(dst.data(), src_ptrs.data(), pixels));
  for (size_t p = 0; p < pixels; ++p) {
    for (int c = 0; c < channels; ++c) {
      EXPECT_EQ(dst[p * static_cast<size_t>(channels) + static_cast<size_t>(c)],
                ExpectedSample<T>(MakePatternSample<T>(p, c), reverse_bytes));
    }
  }

  if (min_pixels > 0) {
    EXPECT_FALSE(CallTypedReorgOut(reorg, channels, dst, src, min_pixels - 1));
    EXPECT_FALSE(reorg.reorg(dst.data(), src_ptrs.data(), min_pixels - 1));
  }

  EXPECT_FALSE(CallTypedReorgOut(reorg, (channels % 4) + 1, dst, src, pixels));

  if constexpr (sizeof(T) == 4) {
    std::vector<kdu_core::kdu_byte> wrong_dst(
        pixels * static_cast<size_t>(channels), 0);
    std::array<std::vector<kdu_core::kdu_byte>, 4> wrong_src;
    for (auto& vec : wrong_src) {
      vec.assign(pixels, 0);
    }
    EXPECT_FALSE(
        CallTypedReorgOut(reorg, channels, wrong_dst, wrong_src, pixels));
  } else {
    std::vector<kdu_core::kdu_uint32> wrong_dst(
        pixels * static_cast<size_t>(channels), 0);
    std::array<std::vector<kdu_core::kdu_uint32>, 4> wrong_src;
    for (auto& vec : wrong_src) {
      vec.assign(pixels, 0);
    }
    EXPECT_FALSE(
        CallTypedReorgOut(reorg, channels, wrong_dst, wrong_src, pixels));
  }

  reorg.clear();
  EXPECT_FALSE(reorg.exists());
  EXPECT_TRUE(!reorg);
  EXPECT_FALSE(CallTypedReorgOut(reorg, channels, dst, src, pixels));
}

TEST(SampleReorgTest, InvalidInitAndReinitState) {
  kdu_sample_reorg_in reorg_in;
  kdu_sample_reorg_out reorg_out;

  constexpr int kBadSampleBytes[] = {0, -1, 3, 8};
  constexpr int kBadChannels[] = {0, -1, 5};

  for (int sb : kBadSampleBytes) {
    for (int ch : {1, 2, 3, 4}) {
      EXPECT_FALSE(reorg_in.init(sb, ch, false, 64));
      EXPECT_FALSE(reorg_in.exists());
      EXPECT_FALSE(reorg_out.init(sb, ch, false, 64));
      EXPECT_FALSE(reorg_out.exists());
    }
  }

  for (int ch : kBadChannels) {
    for (int sb : {1, 2, 4}) {
      EXPECT_FALSE(reorg_in.init(sb, ch, false, 64));
      EXPECT_FALSE(reorg_in.exists());
      EXPECT_FALSE(reorg_out.init(sb, ch, false, 64));
      EXPECT_FALSE(reorg_out.exists());
    }
  }

  // Re-initializing an existing engine resets cleanly even if re-init fails.
  reorg_in.init(1, 2, false, 64);
  EXPECT_FALSE(reorg_in.init(3, 2, false, 64));
  EXPECT_FALSE(reorg_in.exists());

  reorg_out.init(1, 2, false, 64);
  EXPECT_FALSE(reorg_out.init(3, 2, false, 64));
  EXPECT_FALSE(reorg_out.exists());
}

TEST(SampleReorgTest, TypedByteReorgInAndOut) {
  for (int ch : {1, 2, 3, 4}) {
    for (bool rev : {false, true}) {
      RunReorgInCase<kdu_core::kdu_byte>(ch, rev, 33, 67);
      RunReorgOutCase<kdu_core::kdu_byte>(ch, rev, 33, 67);
      RunReorgInCase<kdu_core::kdu_byte>(ch, rev, 64, 64);
      RunReorgOutCase<kdu_core::kdu_byte>(ch, rev, 64, 64);
    }
  }
}

TEST(SampleReorgTest, TypedUint16ReorgInAndOut) {
  for (int ch : {1, 2, 3, 4}) {
    for (bool rev : {false, true}) {
      RunReorgInCase<kdu_core::kdu_uint16>(ch, rev, 17, 39);
      RunReorgOutCase<kdu_core::kdu_uint16>(ch, rev, 17, 39);
      RunReorgInCase<kdu_core::kdu_uint16>(ch, rev, 32, 32);
      RunReorgOutCase<kdu_core::kdu_uint16>(ch, rev, 32, 32);
    }
  }
}

TEST(SampleReorgTest, TypedUint32AndFloatReorgInAndOut) {
  for (int ch : {1, 2, 3, 4}) {
    for (bool rev : {false, true}) {
      RunReorgInCase<kdu_core::kdu_uint32>(ch, rev, 64, 67);
      RunReorgOutCase<kdu_core::kdu_uint32>(ch, rev, 64, 67);
      RunReorgInCase<float>(ch, rev, 64, 67);
      RunReorgOutCase<float>(ch, rev, 64, 67);
    }
  }
}

TEST(SampleReorgTest, MinPixelsClampBoundary) {
  constexpr size_t kHugeMinPixels = sizeof(size_t) > 4
                                        ? static_cast<size_t>(0x100000000ULL)
                                        : static_cast<size_t>(0xFFFFFFFFUL);

  kdu_sample_reorg_in reorg_in;
  kdu_sample_reorg_out reorg_out;

  for (size_t min_pels : {size_t{0}, size_t{16}, size_t{32},
                          size_t{0xFFFFFFFFUL}, kHugeMinPixels}) {
    reorg_in.init(1, 2, false, min_pels);
    reorg_out.init(1, 2, false, min_pels);
    reorg_in.init(2, 1, true, min_pels);
    reorg_out.init(2, 1, true, min_pels);
  }

  // Clamped to 0xFFFFFFFF: even if init succeeds (e.g. NEON), 64 < min_pels.
  const bool in_init = reorg_in.init(1, 2, false, kHugeMinPixels);
  const bool out_init = reorg_out.init(1, 2, false, kHugeMinPixels);
  EXPECT_EQ(reorg_in.exists(), in_init);
  EXPECT_EQ(reorg_out.exists(), out_init);

  std::vector<kdu_core::kdu_byte> ch0(64, 0);
  std::vector<kdu_core::kdu_byte> ch1(64, 0);
  std::vector<kdu_core::kdu_byte> ilv(128, 0);
  std::array<void*, 2> ptrs{ch0.data(), ch1.data()};
  EXPECT_FALSE(reorg_in.reorg(ch0.data(), ch1.data(), ilv.data(), 64));
  EXPECT_FALSE(reorg_in.reorg(ptrs.data(), ilv.data(), 64));
  EXPECT_FALSE(reorg_out.reorg(ilv.data(), ch0.data(), ch1.data(), 64));
  EXPECT_FALSE(reorg_out.reorg(ilv.data(), ptrs.data(), 64));
}

}  // namespace
}  // namespace kdu_supp
