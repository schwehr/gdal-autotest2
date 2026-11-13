// Unit tests for
// third_party/kakadu/vendor/apps/support/sse4_region_decompressor.cpp.
//
// In Google3's x86_64 configuration (`KAKADU_X86_SIMD_DEFINES`),
// `sse4_region_decompressor.cpp` IS compiled and executed on `k8`. These
// tests exercise both `sse4_reinterpret_and_copy_to_unsigned_floats` and
// `sse4_reinterpret_and_copy_to_signed_floats` across `KDRD_INT32_TYPE` and
// `KDRD_INT64_TYPE` tile-line representations, aligned and unaligned
// destination pointers (`lead` scalar loop), positive/negative
// `missing_src_samples` across multi-tile codestreams, vector main loops,
// tails, and right-edge padding.

#include <algorithm>
#include <cmath>
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

class Sse4RegionDecompressorTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Sse4RegionDecompressorTestEnv());

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

// Encodes finite custom-precision floating-point bit patterns (without NaN or
// Inf exponents) as reversible integers so Kakadu decodes `KDRD_INT32_TYPE` or
// `KDRD_INT64_TYPE` line buffers directly into float reinterpretation.
std::vector<kdu_byte> CompressFloatBitPatternCodestream(int width, int height,
                                                        int bit_depth,
                                                        bool is_signed,
                                                        int exp_bits,
                                                        int tile_width) {
  std::vector<kdu_byte> out;
  MemoryTarget target(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, 1);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, is_signed);
  siz.set(Stiles, 0, 0, height);
  siz.set(Stiles, 0, 1, tile_width);
  static_cast<kdu_params &>(siz).finalize();

  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  cs.create(&siz, &target);
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->parse_string("Cycc=no");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();

  const int mant_bits =
      is_signed ? (bit_depth - 1 - exp_bits) : (bit_depth - exp_bits);
  const int max_exp = (1 << exp_bits) - 2;  // Avoid all-ones Inf/NaN exponent.
  const int bias = (1 << (exp_bits - 1)) - 1;
  const kdu_int32 offset =
      is_signed ? 0 : (kdu_int32{1} << (bit_depth - 1));
  const size_t n = static_cast<size_t>(width) * height;
  std::vector<kdu_int32> plane(n);
  for (size_t i = 0; i < n; ++i) {
    // Include zero (exp=0, mant=0) and finite normal exponents around bias.
    const int raw_exp = bias - 2 + static_cast<int>(i % 5);
    const int exp = (i % 7 == 0) ? 0 : std::clamp(raw_exp, 1, max_exp);
    const int mant_mask = (1 << mant_bits) - 1;
    const uint32_t mant =
        (exp == 0) ? 0 : static_cast<uint32_t>((i * 37 + 13) & mant_mask);
    const uint32_t mag_bits =
        (static_cast<uint32_t>(exp) << mant_bits) | mant;
    if (is_signed) {
      kdu_int32 raw = static_cast<kdu_int32>(mag_bits);
      if (i % 2 == 1) {
        // S_MAG / signed-magnitude encoding matching float reinterpretation.
        raw = -raw;
      }
      plane[i] = raw;
    } else {
      plane[i] = static_cast<kdu_int32>(mag_bits) - offset;
    }
  }

  kdu_stripe_compressor comp;
  comp.start(cs);
  const int heights[1] = {height};
  const int ogaps[1] = {0};
  const int sgaps[1] = {1};
  const int rgaps[1] = {width};
  const int precs[1] = {bit_depth};
  const bool sflags[1] = {true};
  while (comp.push_stripe(plane.data(), heights, ogaps, sgaps, rgaps, precs,
                          sflags)) {
  }
  comp.finish();
  cs.destroy();
  return out;
}

TEST(Sse4RegionDecompressorTest, UnsignedAndSignedFloatReinterpretation) {
  struct Case {
    int bit_depth;  // 20 -> KDRD_INT32_TYPE; 28 -> KDRD_INT64_TYPE.
    bool is_signed;
    int exp_bits;
  };
  const std::vector<Case> cases = {
      {20, false, 5},
      {20, true, 5},
      {28, false, 8},
      {28, true, 8},
  };
  // Width 45 with 12-pixel tiles covers multi-tile traversal (widths
  // [12, 12, 12, 9]), while width 14 with 13-pixel tiles yields a 1-pixel
  // second tile at offset 13 where `src_len < lead`.
  constexpr int kWidth = 45;
  constexpr int kHeight = 8;
  constexpr int kTileWidth = 12;

  for (const auto &tc : cases) {
    const std::vector<kdu_byte> data = CompressFloatBitPatternCodestream(
        kWidth, kHeight, tc.bit_depth, tc.is_signed, tc.exp_bits, kTileWidth);

    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    mapping.configure(cs);
    ASSERT_TRUE(mapping.channel_interp[0].init(
        tc.bit_depth, tc.is_signed, 0.0f, JP2_CHANNEL_FORMAT_FLOAT,
        &tc.exp_bits));

    kdu_region_decompressor decomp;

    // 1. Full region at 1x1 expansion: crosses 4 tiles, non-multiple-of-4
    // width tail, aligned dst.
    {
      const kdu_dims full_region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, full_region,
                               kdu_coords(1, 1)));
      std::vector<float> out(full_region.area(), 0.0f);
      float *bufs[1] = {out.data()};
      kdu_dims inc = full_region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, false, 1, full_region.pos, kWidth, 64,
                                   64, inc, nr, /*force_normalized=*/false));
      }
      EXPECT_TRUE(decomp.finish());
      for (float v : out) {
        EXPECT_TRUE(std::isfinite(v));
      }
    }

    // 2. Full region at 2x1 expansion: `in_line_start = -2` forces unaligned
    // `dp` (`lead` scalar loop), positive `missing.x` (left-edge padding), and
    // extended `in_line_length` (right-edge padding).
    {
      const kdu_dims exp_region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(2, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, exp_region,
                               kdu_coords(2, 1)));
      std::vector<float> out(exp_region.area(), 0.0f);
      float *bufs[1] = {out.data()};
      kdu_dims inc = exp_region;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, false, 1, exp_region.pos,
                                   exp_region.size.x, 64, 64, inc, nr,
                                   /*force_normalized=*/false));
      }
      EXPECT_TRUE(decomp.finish());
      for (float v : out) {
        EXPECT_TRUE(std::isfinite(v));
      }
    }

    // 3. Process row 0 across the full width so `comp` retains all tiles from
    // x=0, then immediately advance `inc` past tile 0 boundary (pos.x = 18 >
    // tile_width = 12, size.x = 5 < tile_width) within the same tile row so
    // `missing_src_samples < 0` skips an entire tile (`n >= src_len`) and
    // truncates `src_len > num_samples`.
    {
      const kdu_dims full_region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, full_region,
                               kdu_coords(1, 1)));
      std::vector<float> out(full_region.area(), 0.0f);
      float *bufs[1] = {out.data()};
      kdu_dims inc;
      inc.pos = kdu_coords(0, 0);
      inc.size = kdu_coords(kWidth, 1);
      kdu_dims nr;
      ASSERT_TRUE(decomp.process(bufs, false, 1, full_region.pos, kWidth,
                                 kWidth, kWidth, inc, nr,
                                 /*force_normalized=*/false));
      kdu_dims tail_inc;
      tail_inc.pos = kdu_coords(18, 1);
      tail_inc.size = kdu_coords(5, 1);
      while (!tail_inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, false, 1, full_region.pos, kWidth,
                                   64, 64, tail_inc, nr,
                                   /*force_normalized=*/false));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // 4. Cropped 2x1 region across tiles plus a 14x8 codestream with
    // tile_width=13 so tile 1 starts at unaligned x=13 with `src_len = 1 <
    // lead`.
    {
      const kdu_dims full_region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_coords(2, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, full_region,
                               kdu_coords(2, 1)));
      kdu_dims crop = full_region;
      crop.pos.x = 37;
      crop.size.x = full_region.size.x - crop.pos.x;
      std::vector<float> out(crop.area(), 0.0f);
      float *bufs[1] = {out.data()};
      kdu_dims inc = crop;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, false, 1, crop.pos, crop.size.x, 64,
                                   64, inc, nr, /*force_normalized=*/false));
      }
      EXPECT_TRUE(decomp.finish());
    }
    cs.destroy();

    {
      const std::vector<kdu_byte> unaligned_data =
          CompressFloatBitPatternCodestream(14, kHeight, tc.bit_depth,
                                            tc.is_signed, tc.exp_bits, 13);
      MemorySource usrc(unaligned_data);
      kdu_codestream ucs;
      CodestreamGuard uguard(&ucs);
      ucs.create(&usrc);
      kdu_channel_mapping umapping;
      umapping.configure(ucs);
      ASSERT_TRUE(umapping.channel_interp[0].init(
          tc.bit_depth, tc.is_signed, 0.0f, JP2_CHANNEL_FORMAT_FLOAT,
          &tc.exp_bits));
      kdu_region_decompressor udecomp;
      const kdu_dims uregion = udecomp.get_rendered_image_dims(
          ucs, &umapping, 0, 0, kdu_coords(1, 1));
      ASSERT_TRUE(udecomp.start(ucs, &umapping, 0, 0, 1, uregion,
                                kdu_coords(1, 1)));
      std::vector<float> out(uregion.area(), 0.0f);
      float *bufs[1] = {out.data()};
      kdu_dims inc = uregion;
      kdu_dims nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(udecomp.process(bufs, false, 1, uregion.pos, 14, 128, 128,
                                    inc, nr, /*force_normalized=*/false));
      }
      EXPECT_TRUE(udecomp.finish());
    }
  }
}

}  // namespace
