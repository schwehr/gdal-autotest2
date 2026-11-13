// Unit tests for
// third_party/kakadu/vendor/apps/support/neon_region_decompressor.cpp.
//
// In Google3's active build configuration, `kdu_arch.h` gates
// `KDU_NEON_INTRINSICS` on `#ifdef __ARM_NEON__`, so
// `neon_region_decompressor.cpp` compiles cleanly out (`0/0` LCOV `DA:` on
// both `k8` and `--cpu=arm`). These tests exercise all sample conversion,
// float reinterpretation, white-stretch, planar/interleaved byte transfer,
// and horizontal/vertical resampling behaviors via `kdu_region_decompressor`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
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

class NeonRegionDecompTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new NeonRegionDecompTestEnv());

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
    int width, int height, int num_comps, int bit_depth, bool is_signed,
    bool reversible, const std::vector<std::vector<kdu_int32>> &planes,
    int tile_width = 0) {
  std::vector<kdu_byte> out;
  MemoryTarget target(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, is_signed);
  if (tile_width > 0) {
    siz.set(Stiles, 0, 0, height);
    siz.set(Stiles, 0, 1, tile_width);
  }
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
  auto sflags = std::make_unique<bool[]>(num_comps);
  std::vector<kdu_int32 *> ptrs(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    sflags[c] = is_signed;
    ptrs[c] = const_cast<kdu_int32 *>(planes[c].data());
  }
  while (comp.push_stripe(ptrs.data(), heights.data(), sample_gaps.data(),
                          row_gaps.data(), precisions.data(), sflags.get())) {
  }
  comp.finish();
  cs.destroy();
  return out;
}

TEST(NeonRegionDecompressorTest, WhiteStretchSmallAndOneBitPrecisions) {
  constexpr int kWidth = 41;
  constexpr int kHeight = 8;
  for (int bit_depth : {1, 2, 4, 6}) {
    const int max_val = (1 << bit_depth) - 1;
    std::vector<kdu_int32> plane(static_cast<size_t>(kWidth) * kHeight);
    for (size_t i = 0; i < plane.size(); ++i) {
      plane[i] = (i % 2 == 0) ? 0 : max_val;
    }
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, bit_depth, false, /*reversible=*/true, {plane});

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
    ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region, kdu_coords(1, 1)));

    std::vector<kdu_byte> out(region.area(), 0);
    kdu_byte *bufs[1] = {out.data()};
    kdu_dims incomplete = region;
    kdu_dims new_region;
    while (!incomplete.is_empty()) {
      ASSERT_TRUE(decomp.process(bufs, false, 1, region.pos, kWidth, 64, 64,
                                 incomplete, new_region, 8));
    }
    EXPECT_TRUE(decomp.finish());
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 255);
    cs.destroy();
  }
}

TEST(NeonRegionDecompressorTest, TransferFix16ToBytesPlanarRgbAndRgba) {
  constexpr int kHeight = 8;
  for (int width : {17, 45}) {
    const size_t n = static_cast<size_t>(width) * kHeight;
    std::vector<std::vector<kdu_int32>> planes(4, std::vector<kdu_int32>(n));
    for (size_t i = 0; i < n; ++i) {
      planes[0][i] = static_cast<kdu_int32>((i * 5 + 11) & 0xFF);
      planes[1][i] = static_cast<kdu_int32>((i * 7 + 33) & 0xFF);
      planes[2][i] = static_cast<kdu_int32>((i * 11 + 55) & 0xFF);
      planes[3][i] = static_cast<kdu_int32>((i * 3 + 128) & 0xFF);
    }

    // 1. Planar (gap1) & 3-comp interleaved.
    {
      const std::vector<kdu_byte> data = CompressPlanarCodestream(
          width, kHeight, 3, 8, false, /*reversible=*/true,
          {planes[0], planes[1], planes[2]});
      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);
      cs.set_persistent();
      kdu_channel_mapping mapping;
      mapping.configure(cs);

      kdu_region_decompressor decomp;
      const kdu_dims region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));

      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                               kdu_coords(1, 1)));
      std::vector<kdu_byte> p0(n, 0), p1(n, 0), p2(n, 0);
      kdu_byte *pbufs[3] = {p0.data(), p1.data(), p2.data()};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(pbufs, false, 1, region.pos, width, 64, 64,
                                   inc, nr, 8));
      }
      EXPECT_TRUE(decomp.finish());
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(p0[i], planes[0][i]);
        EXPECT_EQ(p1[i], planes[1][i]);
        EXPECT_EQ(p2[i], planes[2][i]);
      }

      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                               kdu_coords(1, 1)));
      std::vector<kdu_byte> ilv3(n * 3, 0);
      kdu_byte *ibufs[3] = {ilv3.data(), ilv3.data() + 1, ilv3.data() + 2};
      inc = region;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ibufs, false, 3, region.pos, width, 64, 64,
                                   inc, nr, 8));
      }
      EXPECT_TRUE(decomp.finish());
      for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(ilv3[i * 3 + 0], planes[0][i]);
        EXPECT_EQ(ilv3[i * 3 + 1], planes[1][i]);
        EXPECT_EQ(ilv3[i * 3 + 2], planes[2][i]);
      }
      cs.destroy();
    }

    // 2. 4-component RGBA interleaved (gap4 & interleaved transfer).
    {
      const std::vector<kdu_byte> data = CompressPlanarCodestream(
          width, kHeight, 4, 8, false, /*reversible=*/true, planes);
      MemorySource src(data);
      kdu_codestream cs;
      CodestreamGuard guard(&cs);
      cs.create(&src);

      kdu_channel_mapping mapping;
      mapping.configure(cs);
      mapping.set_num_channels(4);
      mapping.source_components[3] = 3;

      kdu_region_decompressor decomp;
      const kdu_dims region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                               kdu_coords(1, 1)));
      std::vector<kdu_byte> rgba(n * 4, 0);
      kdu_byte *abufs[4] = {rgba.data(), rgba.data() + 1, rgba.data() + 2,
                            rgba.data() + 3};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(abufs, false, 4, region.pos, width, 64, 64,
                                   inc, nr, 8));
      }
      EXPECT_TRUE(decomp.finish());
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

TEST(NeonRegionDecompressorTest, TransferToWordsFix16Int32AndFloat) {
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
      {12, true, false, 0},
      {16, true, true, 1},
      {12, false, true, 32},
  };

  for (const Case &tc : cases) {
    std::vector<kdu_int32> plane(n);
    const kdu_int32 mask = (1 << tc.bit_depth) - 1;
    for (size_t i = 0; i < n; ++i) {
      plane[i] = static_cast<kdu_int32>((i * 73) & mask);
    }
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, tc.bit_depth, false, tc.reversible, {plane});

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

TEST(NeonRegionDecompressorTest, VerticalAndHorizontalResamplingFix16AndFloat) {
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
      {true, false, 2, 1},
      {false, true, 2, 1},
      {true, false, 3, 2},
      {false, true, 3, 2},
      {true, false, 2, 3},
      {false, true, 2, 3},
  };

  for (const ScaleCase &tc : cases) {
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, 8, false, tc.reversible, {plane});

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

TEST(NeonRegionDecompressorTest, ReinterpretAsUnsignedAndSignedFloats) {
  struct FloatCase {
    int bit_depth;
    int exp_bits;
    bool orig_signed;
    bool precise;
  };
  const FloatCase cases[] = {
      {16, 5, false, false},
      {16, 5, true, false},
      {28, 8, false, true},
      {28, 8, true, true},
  };
  constexpr int kWidth = 48;
  constexpr int kHeight = 8;

  for (const FloatCase &tc : cases) {
    const size_t n = static_cast<size_t>(kWidth) * kHeight;
    std::vector<kdu_int32> plane(n, 0);
    // Construct valid float bit patterns interpreted at bit_depth / exp_bits.
    const int exp_bias = (1 << (tc.exp_bits - 1)) - 1;
    const int mant_bits = tc.bit_depth - 1 - tc.exp_bits;
    const kdu_int32 pat =
        static_cast<kdu_int32>((exp_bias - 1) << mant_bits);  // ~0.5
    for (size_t i = 0; i < n; ++i) {
      plane[i] = (tc.orig_signed && (i % 3 == 0)) ? -pat : pat;
    }
    const std::vector<kdu_byte> data = CompressPlanarCodestream(
        kWidth, kHeight, 1, tc.bit_depth, tc.orig_signed, /*reversible=*/true,
        {plane}, /*tile_width=*/16);

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    cs.set_persistent();
    kdu_channel_mapping mapping;
    mapping.configure(cs);
    ASSERT_TRUE(mapping.channel_interp[0].init(
        tc.bit_depth, tc.orig_signed, 0.0f, JP2_CHANNEL_FORMAT_FLOAT,
        &tc.exp_bits));

    for (const kdu_coords scale : {kdu_coords(1, 1), kdu_coords(3, 2)}) {
      kdu_region_decompressor decomp;
      kdu_dims full = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, scale, kdu_coords(1, 1));
      kdu_dims region = full;
      // Offset horizontally across tile boundaries to hit skip & alignment.
      region.pos.x += 3;
      region.size.x = std::max(16, full.size.x - 5);
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region, scale,
                               kdu_coords(1, 1), tc.precise));
      std::vector<float> fbuf(region.area(), 0.0f);
      float *fptrs[1] = {fbuf.data()};
      kdu_dims inc = region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(fptrs, false, 1, region.pos, region.size.x,
                                   256, 256, inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }
    cs.destroy();
  }
}

}  // namespace
