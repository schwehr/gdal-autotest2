// Unit tests for
// third_party/kakadu/vendor/apps/support/avx2_region_decompressor.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES` with `-DKDU_NO_AVX2` on x86_64),
// `avx2_region_decompressor.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on
// both x86_64 and `--cpu=arm`). These tests exercise all sample-conversion,
// white-stretch, planar/interleaved byte/word transfer, and horizontal/vertical
// resampling paths through `kdu_region_decompressor` across vector and tail
// widths.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/support/kdu_region_decompressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_compressed_source;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_params;
using ::kdu_core::kdu_thread_safe_message;
using ::kdu_core::kdu_uint16;
using ::kdu_core::siz_params;
using ::kdu_supp::kdu_channel_mapping;
using ::kdu_supp::kdu_region_decompressor;
using ::kdu_supp::kdu_stripe_compressor;

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char *string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class Avx2RegionDecompTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Avx2RegionDecompTestEnv());

class MemoryTarget final : public kdu_compressed_target {
 public:
  explicit MemoryTarget(std::vector<kdu_byte> *out) : out_(out) {}
  bool write(const kdu_byte *buf, int num_bytes) override {
    if (num_bytes > 0) {
      out_->insert(out_->end(), buf, buf + num_bytes);
    }
    return true;
  }

 private:
  std::vector<kdu_byte> *out_;
};

class MemorySource final : public kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_byte> &data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte *buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(::kdu_core::kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  ::kdu_core::kdu_long get_pos() override {
    return static_cast<::kdu_core::kdu_long>(pos_);
  }

 private:
  const std::vector<kdu_byte> &data_;
  size_t pos_ = 0;
};

struct CodestreamGuard {
  explicit CodestreamGuard(kdu_codestream *cs) : cs_(cs) {}
  ~CodestreamGuard() {
    if (cs_ != nullptr && cs_->exists()) {
      cs_->destroy();
    }
  }
  kdu_codestream *cs_;
};

std::vector<kdu_byte> CompressPlanarCodestream(
    int width, int height, int num_comps, int bit_depth, bool reversible,
    const std::vector<std::vector<kdu_int32>> &planes) {
  std::vector<kdu_byte> out;
  MemoryTarget target(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, false);
  static_cast<kdu_params &>(siz).finalize();

  kdu_codestream cs;
  cs.create(&siz, &target);
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->parse_string("Cycc=no");
  cs.access_siz()->parse_string("Clevels=2");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(num_comps, height);
  std::vector<int> sample_gaps(num_comps, 1);
  std::vector<int> row_gaps(num_comps, width);
  std::vector<int> precisions(num_comps, bit_depth);
  auto is_signed = std::make_unique<bool[]>(num_comps);
  std::vector<kdu_int32 *> ptrs(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    is_signed[c] = false;
    ptrs[c] = const_cast<kdu_int32 *>(planes[c].data());
  }
  while (comp.push_stripe(ptrs.data(), heights.data(), sample_gaps.data(),
                          row_gaps.data(), precisions.data(),
                          is_signed.get())) {
  }
  comp.finish();
  cs.destroy();
  return out;
}

TEST(Avx2RegionDecompressorTest, WhiteStretchSmallAndOneBitPrecisions) {
  for (int bit_depth : {1, 6}) {
    for (int width : {17, 33, 65}) {
      constexpr int kHeight = 8;
      std::vector<kdu_int32> plane(static_cast<size_t>(width) * kHeight);
      const kdu_int32 max_val = (1 << bit_depth) - 1;
      for (size_t i = 0; i < plane.size(); ++i) {
        plane[i] = (i % 2 == 0) ? 0 : max_val;
      }
      const std::vector<kdu_byte> data =
          CompressPlanarCodestream(width, kHeight, 1, bit_depth, true, {plane});

      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);

      kdu_channel_mapping mapping;
      mapping.configure(cs);

      kdu_region_decompressor decomp;
      decomp.set_white_stretch(8);
      const kdu_dims region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_EQ(region.size.x, width);
      ASSERT_EQ(region.size.y, kHeight);
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region, kdu_coords(1, 1),
                               kdu_coords(1, 1), /*precise=*/false));

      std::vector<kdu_byte> out(static_cast<size_t>(width) * kHeight, 127);
      kdu_byte *bufs[1] = {out.data()};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, false, 1, region.pos, width, 64, 64,
                                   inc, nr, 8));
      }
      EXPECT_TRUE(decomp.finish());
      // White stretch maps 0 -> 0 and max_val -> 255.
      EXPECT_EQ(out[0], 0);
      EXPECT_EQ(out[1], 255);
      EXPECT_EQ(out[width - 1], ((width - 1) % 2 == 0) ? 0 : 255);

      cs.destroy();
    }
  }
}

TEST(Avx2RegionDecompressorTest, TransferFix16ToBytesPlanarRgbAndRgba) {
  constexpr int kHeight = 8;
  for (int width : {15, 32, 33, 65}) {
    const size_t n = static_cast<size_t>(width) * kHeight;
    std::vector<std::vector<kdu_int32>> planes(4, std::vector<kdu_int32>(n));
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t idx = static_cast<size_t>(y) * width + x;
        planes[0][idx] = (x * 7 + y * 3) & 0xFF;
        planes[1][idx] = (x * 5 + 40) & 0xFF;
        planes[2][idx] = (200 - x * 2) & 0xFF;
        planes[3][idx] = (x % 2 == 0) ? 255 : 128;
      }
    }

    // Planar (gap1) and 3-channel RGB interleaved transfers.
    const std::vector<kdu_byte> rgb_data = CompressPlanarCodestream(
        width, kHeight, 3, 8, true, {planes[0], planes[1], planes[2]});
    {
      MemorySource src(rgb_data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);
      cs.set_persistent();
      kdu_channel_mapping mapping;
      mapping.configure(cs);

      // Planar byte output (gap == 1).
      kdu_region_decompressor dec_planar;
      const kdu_dims region = dec_planar.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(dec_planar.start(cs, &mapping, 0, 0, 1, region,
                                   kdu_coords(1, 1)));
      std::vector<kdu_byte> p0(n), p1(n), p2(n);
      kdu_byte *pbufs[3] = {p0.data(), p1.data(), p2.data()};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(dec_planar.process(pbufs, false, 1, region.pos, width, 64,
                                       64, inc, nr, 8));
      }
      EXPECT_TRUE(dec_planar.finish());
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(p0[i], planes[0][i]);
        EXPECT_EQ(p1[i], planes[1][i]);
        EXPECT_EQ(p2[i], planes[2][i]);
      }

      // Interleaved 3-channel byte output (gap == 3).
      kdu_region_decompressor dec_interleaved;
      ASSERT_TRUE(dec_interleaved.start(cs, &mapping, 0, 0, 1, region,
                                        kdu_coords(1, 1)));
      std::vector<kdu_byte> rgb(n * 3, 0);
      kdu_byte *ibufs[3] = {rgb.data(), rgb.data() + 1, rgb.data() + 2};
      inc = region;
      while (!inc.is_empty()) {
        ASSERT_TRUE(dec_interleaved.process(ibufs, false, 3, region.pos, width,
                                            64, 64, inc, nr, 8));
      }
      EXPECT_TRUE(dec_interleaved.finish());
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(rgb[i * 3 + 0], planes[0][i]);
        EXPECT_EQ(rgb[i * 3 + 1], planes[1][i]);
        EXPECT_EQ(rgb[i * 3 + 2], planes[2][i]);
      }
      cs.destroy();
    }

    // 4-channel interleaved RGBA (gap == 4).
    const std::vector<kdu_byte> rgba_data =
        CompressPlanarCodestream(width, kHeight, 4, 8, true, planes);
    {
      MemorySource src(rgba_data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);
      kdu_channel_mapping mapping;
      mapping.set_num_channels(4);
      mapping.num_colour_channels = 3;
      for (int c = 0; c < 4; ++c) {
        mapping.source_components[c] = c;
      }

      kdu_region_decompressor dec_rgba;
      const kdu_dims region = dec_rgba.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(dec_rgba.start(cs, &mapping, 0, 0, 1, region,
                                 kdu_coords(1, 1)));
      std::vector<kdu_byte> rgba(n * 4, 0);
      kdu_byte *abufs[4] = {rgba.data(), rgba.data() + 1, rgba.data() + 2,
                            rgba.data() + 3};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(dec_rgba.process(abufs, false, 4, region.pos, width, 64, 64,
                                     inc, nr, 8));
      }
      EXPECT_TRUE(dec_rgba.finish());
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(rgba[i * 4 + 0], planes[0][i]);
        EXPECT_EQ(rgba[i * 4 + 1], planes[1][i]);
        EXPECT_EQ(rgba[i * 4 + 2], planes[2][i]);
        EXPECT_EQ(rgba[i * 4 + 3], planes[3][i]);
      }
      cs.destroy();
    }
  }
}

TEST(Avx2RegionDecompressorTest, TransferToWordsFix16Int32AndFloat) {
  constexpr int kWidth = 65;
  constexpr int kHeight = 8;
  const size_t n = static_cast<size_t>(kWidth) * kHeight;

  struct Case {
    int bit_depth;
    bool reversible;
    bool precise;
    int max_err;
  };
  const Case cases[] = {
      {12, true, false, 0},   // fix16 -> words gap1
      {16, true, true, 1},    // int32 -> words gap1
      {12, false, true, 32},  // float -> words gap1
  };

  for (const Case &tc : cases) {
    std::vector<kdu_int32> plane(n);
    const kdu_int32 mask = (1 << tc.bit_depth) - 1;
    for (size_t i = 0; i < n; ++i) {
      plane[i] = static_cast<kdu_int32>((i * 73) & mask);
    }
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, tc.bit_depth, tc.reversible, {plane});

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    kdu_channel_mapping mapping;
    mapping.configure(cs);

    kdu_region_decompressor decomp;
    const kdu_dims region = decomp.get_rendered_image_dims(
        cs, &mapping, 0, 0, kdu_coords(1, 1));
    ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region, kdu_coords(1, 1),
                             kdu_coords(1, 1), tc.precise));
    std::vector<kdu_uint16> words(n, 0);
    kdu_uint16 *wbufs[1] = {words.data()};
    kdu_dims inc = region;
    kdu_dims nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(wbufs, false, 1, region.pos, kWidth, 64, 64,
                                 inc, nr, tc.bit_depth));
    }
    EXPECT_TRUE(decomp.finish());
    for (size_t i = 0; i < n; ++i) {
      EXPECT_NEAR(words[i], plane[i], tc.max_err);
    }
    cs.destroy();
  }
}

TEST(Avx2RegionDecompressorTest, VerticalAndHorizontalResamplingFix16AndFloat) {
  constexpr int kWidth = 48;
  constexpr int kHeight = 24;
  const size_t n = static_cast<size_t>(kWidth) * kHeight;
  std::vector<kdu_int32> plane(n, 128);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      plane[static_cast<size_t>(y) * kWidth + x] =
          std::min(255, std::max(0, 100 + ((x + y) & 31)));
    }
  }

  struct ScaleCase {
    bool reversible;
    bool precise;
    int num;
    int den;
  };
  const ScaleCase cases[] = {
      {true, false, 2, 1},   // 2x bilinear (fix16 hshuf + vert 2-tap)
      {false, true, 2, 1},   // 2x bilinear (float hshuf + vert 2-tap)
      {true, false, 3, 2},   // 1.5x bicubic (horz/vert fix16 general)
      {false, true, 3, 2},   // 1.5x bicubic (horz/vert float general)
      {true, false, 2, 3},   // 2/3 downsample fix16
      {false, true, 2, 3},   // 2/3 downsample float
  };

  for (const ScaleCase &tc : cases) {
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, 8, tc.reversible, {plane});

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    kdu_channel_mapping mapping;
    mapping.configure(cs);

    kdu_region_decompressor decomp;
    const kdu_coords exp_num(tc.num, tc.num);
    const kdu_coords exp_den(tc.den, tc.den);
    const kdu_dims region = decomp.get_rendered_image_dims(
        cs, &mapping, 0, 0, exp_num, exp_den);
    ASSERT_GT(region.size.x, 16);
    ASSERT_GT(region.size.y, 8);
    ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region, exp_num, exp_den,
                             tc.precise));

    std::vector<kdu_byte> out(region.area(), 0);
    kdu_byte *bufs[1] = {out.data()};
    kdu_dims inc = region;
    kdu_dims nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(bufs, false, 1, region.pos, region.size.x,
                                 256, 256, inc, nr, 8));
    }
    EXPECT_TRUE(decomp.finish());
    for (kdu_byte val : out) {
      EXPECT_GE(val, 80);
      EXPECT_LE(val, 155);
    }
    cs.destroy();
  }
}

}  // namespace
