// Unit tests for third_party/kakadu/vendor/coresys/coding/block_decoder.cpp.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_block_coding.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace {

class ThrowingErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class CountingWarningHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      ++count_;
    }
  }
  int count() const { return count_; }

 private:
  int count_ = 0;
};

class ScopedMessageHandlers {
 public:
  ScopedMessageHandlers() {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }
  ~ScopedMessageHandlers() {
    kdu_core::kdu_customize_warnings(nullptr);
    kdu_core::kdu_customize_errors(nullptr);
  }
  int warning_count() const { return warning_handler_.count(); }

 private:
  ThrowingErrorHandler error_handler_;
  CountingWarningHandler warning_handler_;
};

class VectorTarget final : public kdu_core::kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_core::kdu_byte>& out) : out_(out) {}

  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_core::kdu_byte>& out_;
};

class ScopedOutputBlock {
 public:
  ScopedOutputBlock(int width, int height, int timing_iterations = 0)
      : target_(compressed_bytes_) {
    kdu_core::siz_params siz;
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, height);
    siz.set(Sdims, 0, 1, width);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_core::kdu_params&>(siz).finalize();

    codestream_.create(&siz, &target_);
    codestream_.access_siz()->parse_string("Clevels=0");
    if (width * height > 4096 || width > 1024 || height > 1024) {
      codestream_.access_siz()->parse_string("Cblk={64,64}");
    } else if (width == 4 && height == 1024) {
      codestream_.access_siz()->parse_string("Cblk={1024,4}");
    } else {
      codestream_.access_siz()->parse_string("Cblk={64,64}");
    }
    codestream_.access_siz()->finalize_all();
    if (timing_iterations > 0) {
      codestream_.collect_timing_stats(timing_iterations);
    }

    tile_ = codestream_.open_tile(kdu_core::kdu_coords(0, 0));
    kdu_core::kdu_tile_comp tc = tile_.access_component(0);
    kdu_core::kdu_resolution res = tc.access_resolution(0);
    subband_ = res.access_subband(LL_BAND);
    block_ = subband_.open_block(kdu_core::kdu_coords(0, 0));
    if (block_ != nullptr) {
      orig_modes_ = block_->modes;
      orig_orientation_ = block_->orientation;
      orig_k_max_prime_ = block_->K_max_prime;
      orig_fussy_ = block_->fussy;
      orig_resilient_ = block_->resilient;
      orig_errors_detected_ = block_->errors_detected;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
    }
  }

  ~ScopedOutputBlock() {
    if (block_ != nullptr) {
      if (block_->sample_buffer == nullptr) {
        block_->set_max_samples(1);
      }
      block_->modes = orig_modes_;
      block_->orientation = orig_orientation_;
      block_->K_max_prime = orig_k_max_prime_;
      block_->fussy = orig_fussy_;
      block_->resilient = orig_resilient_;
      block_->errors_detected = orig_errors_detected_;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
      subband_.close_block(block_);
    }
    if (tile_.exists()) {
      tile_.close();
    }
    if (codestream_.exists()) {
      codestream_.destroy();
    }
  }

  kdu_core::kdu_block* block() { return block_; }
  kdu_core::kdu_codestream& codestream() { return codestream_; }

 private:
  std::vector<kdu_core::kdu_byte> compressed_bytes_;
  VectorTarget target_;
  kdu_core::kdu_codestream codestream_;
  kdu_core::kdu_tile tile_;
  kdu_core::kdu_subband subband_;
  kdu_core::kdu_block* block_ = nullptr;
  int orig_modes_ = 0;
  int orig_orientation_ = LL_BAND;
  int orig_k_max_prime_ = 0;
  bool orig_fussy_ = false;
  bool orig_resilient_ = false;
  bool* orig_errors_detected_ = nullptr;
};

kdu_core::kdu_int32 ToSignMag32(int32_t value, int k_max_prime) {
  if (value == 0) {
    return 0;
  }
  uint32_t mag = static_cast<uint32_t>(std::abs(value));
  uint32_t shifted = mag << (31 - k_max_prime);
  if (value < 0) {
    shifted |= uint32_t{0x80000000U};
  }
  return static_cast<kdu_core::kdu_int32>(shifted);
}

int32_t FromSignMag32(kdu_core::kdu_int32 sample, int k_max_prime) {
  uint32_t raw = static_cast<uint32_t>(sample);
  uint32_t mag = (raw & uint32_t{0x7FFFFFFFU}) >> (31 - k_max_prime);
  int32_t signed_mag = static_cast<int32_t>(mag);
  return (raw & uint32_t{0x80000000U}) ? -signed_mag : signed_mag;
}

kdu_core::kdu_int64 ToSignMag64(int64_t value, int k_max_prime) {
  if (value == 0) {
    return 0;
  }
  uint64_t mag = static_cast<uint64_t>(value < 0 ? -value : value);
  uint64_t shifted = mag << (63 - k_max_prime);
  if (value < 0) {
    shifted |= uint64_t{1} << 63;
  }
  return static_cast<kdu_core::kdu_int64>(shifted);
}

int64_t FromSignMag64(kdu_core::kdu_int64 sample, int k_max_prime) {
  uint64_t raw = static_cast<uint64_t>(sample);
  uint64_t mag = (raw & uint64_t{0x7FFFFFFFFFFFFFFFULL}) >> (63 - k_max_prime);
  int64_t signed_mag = static_cast<int64_t>(mag);
  return (raw & (uint64_t{1} << 63)) ? -signed_mag : signed_mag;
}

std::vector<int32_t> MakePattern32(int width, int height, int k_max_prime,
                                   uint32_t seed) {
  std::vector<int32_t> vals(static_cast<size_t>(width) * height, 0);
  int32_t max_mag = (int32_t{1} << k_max_prime) - 1;
  uint32_t state = seed ? seed : 0x13579BDFU;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // Leave sparse zero runs in some columns so cleanup run-skipping
      // triggers, while making central/border rows dense with alternating
      // signs.
      if (((y & 3) == 0) && (x >= 2 && x <= 7) && (seed & 1U)) {
        continue;
      }
      state = state * 1664525U + 1013904223U;
      int32_t mag = static_cast<int32_t>((state >> 8) %
                                         static_cast<uint32_t>(max_mag + 1));
      if (x == 0 && y == 0 && mag == 0) {
        mag = max_mag;
      }
      vals[static_cast<size_t>(y) * width + x] = (state & 1U) ? -mag : mag;
    }
  }
  return vals;
}

std::vector<int64_t> MakePattern64(int width, int height, int k_max_prime,
                                   uint64_t seed) {
  std::vector<int64_t> vals(static_cast<size_t>(width) * height, 0);
  uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ULL;
  uint64_t mask = (uint64_t{1} << k_max_prime) - 1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // Keep an all-zero 4-column stripe run in rows 0..3 of the first stripe
      // when seed bit 0 is set, so 64-bit fast column-skipping executes.
      if ((y < 4) && (x >= 2 && x <= 7) && (seed & 1U)) {
        continue;
      }
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      int64_t mag = static_cast<int64_t>(state & mask);
      if (x == width - 1 && y == height - 1) {
        mag |= (uint64_t{1} << (k_max_prime - 1));
      }
      vals[static_cast<size_t>(y) * width + x] = (state & 2U) ? -mag : mag;
    }
  }
  return vals;
}

void EncodeBlock32(kdu_core::kdu_block* block, const std::vector<int32_t>& vals,
                   int k_max_prime, int modes, int orientation,
                   bool reversible) {
  const int width = block->size.x;
  const int height = block->size.y;
  const int num_stripes = (height + 3) >> 2;
  const int padded_samples = (num_stripes << 2) * width;
  block->set_max_samples(padded_samples);
  std::memset(
      block->sample_buffer, 0,
      static_cast<size_t>(padded_samples) * sizeof(kdu_core::kdu_int32));
  for (size_t i = 0; i < vals.size(); ++i) {
    block->sample_buffer[i] = ToSignMag32(vals[i], k_max_prime);
  }
  block->modes = modes;
  block->orientation = orientation;
  block->K_max_prime = k_max_prime;
  block->missing_msbs = 0;
  block->num_passes = 3 * k_max_prime - 2;
  block->fussy = false;
  block->resilient = false;

  kdu_core::kdu_block_encoder encoder;
  encoder.encode(block, reversible);
}

void EncodeBlock64(kdu_core::kdu_block* block, const std::vector<int64_t>& vals,
                   int k_max_prime, int modes, int orientation) {
  const int width = block->size.x;
  const int height = block->size.y;
  const int num_stripes = (height + 3) >> 2;
  const int padded_samples = (num_stripes << 2) * width;
  block->set_max_samples64(padded_samples);
  block->hide_sample_buffer();
  std::memset(
      block->sample_buffer64, 0,
      static_cast<size_t>(padded_samples) * sizeof(kdu_core::kdu_int64));
  for (size_t i = 0; i < vals.size(); ++i) {
    block->sample_buffer64[i] = ToSignMag64(vals[i], k_max_prime);
  }
  block->modes = modes;
  block->orientation = orientation;
  block->K_max_prime = k_max_prime;
  block->missing_msbs = 0;
  block->num_passes = 3 * k_max_prime - 2;
  block->fussy = false;
  block->resilient = false;

  kdu_core::kdu_block_encoder encoder;
  encoder.encode(block, /*reversible=*/false);
}

TEST(BlockDecoderTest, CapabilitiesInitSpeedpackAndDecode16) {
  ScopedMessageHandlers handlers;
  int cap_flags = 0;
  const char* caps = kdu_core::kdu_block_decoder::get_capabilities(cap_flags);
  ASSERT_NE(caps, nullptr);
  EXPECT_EQ(cap_flags, KDU_BLOCK_CODER_CAP_J2K1);
  EXPECT_EQ(std::string(caps), "J2K1");

  kdu_core::kdu_block_decoder decoder;
  decoder.init(/*allow_optimizations=*/false);
  decoder.init(/*allow_optimizations=*/true);
  decoder.speedpack_config(kdu_core::kdu_coords(64, 64), 16);

  ScopedOutputBlock fixture(16, 16);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  int stride = -1;
  block->modes = 0;
  EXPECT_EQ(decoder.decode16(block, stride), nullptr);

  // Under Google3 default config FBC_ENABLED is disabled, so HT decode16
  // returns nullptr on x86 and delegates to stub fbc_block_decode16 on ARM64.
  block->modes = Cmodes_HT;
  EXPECT_EQ(decoder.decode16(block, stride), nullptr);

  block->num_passes = 3;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  ASSERT_NE(block->sample_buffer, nullptr);
  EXPECT_EQ(block->sample_buffer[0], 0);

  decoder.init(/*allow_optimizations=*/false);
  block->modes = Cmodes_HT;
  block->num_passes = 3;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
  ASSERT_NE(block->sample_buffer, nullptr);
  EXPECT_EQ(block->sample_buffer[0], 0);
}

TEST(BlockDecoderTest, RoundTrip32BitOrientationsAndPartialStripes) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime = 12;

  const int heights[] = {1, 2, 3, 4, 9, 10, 11};
  const int orientations[] = {LL_BAND, LH_BAND, HL_BAND, HH_BAND};

  for (int h_idx = 0; h_idx < 7; ++h_idx) {
    const int height = heights[h_idx];
    const int width = (height == 1 || height == 4) ? 16 : 8;
    ScopedOutputBlock fixture(width, height);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);

    const int orient = orientations[h_idx % 4];
    const auto expected = MakePattern32(width, height, k_max_prime,
                                        static_cast<uint32_t>(h_idx + 1));
    EncodeBlock32(block, expected, k_max_prime, /*modes=*/0, orient,
                  /*reversible=*/true);

    const int encoded_passes = block->num_passes;
    ASSERT_GT(encoded_passes, 0);
    // Clear sample buffer before decoding to verify reconstruction.
    std::memset(block->sample_buffer, 0,
                static_cast<size_t>(width * ((height + 3) & ~3)) *
                    sizeof(kdu_core::kdu_int32));

    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    EXPECT_EQ(block->num_passes, encoded_passes);
    ASSERT_NE(block->sample_buffer, nullptr);
    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime),
                expected[i]);
    }
  }
}

TEST(BlockDecoderTest, RoundTrip32BitDenseSignsAndAllModes) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime = 14;
  const int width = 16;
  const int height = 16;

  // Craft a dense matrix covering positive/negative neighborhood combinations
  // plus zero-runs for run-length cases 0..3.
  std::vector<int32_t> pattern(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (y < 4 && x >= 1 && x <= 4) {
        // Leave exact run cases in successive columns when shifted down.
        if (x - 1 == y) {
          pattern[static_cast<size_t>(y) * width + x] =
              ((x + y) & 1) ? -8191 : 8191;
        }
        continue;
      }
      int32_t v = ((x * 97 + y * 53) & 0x3FFF);
      if ((x ^ y) & 1) {
        v = -v;
      }
      pattern[static_cast<size_t>(y) * width + x] = v;
    }
  }

  const int mode_cases[] = {
      0,
      Cmodes_CAUSAL,
      Cmodes_RESET,
      Cmodes_RESTART,
      Cmodes_SEGMARK,
      Cmodes_ERTERM,
      Cmodes_BYPASS,
      Cmodes_BYPASS | Cmodes_BYPASS_E1,
      Cmodes_BYPASS | Cmodes_BYPASS_E2,
      Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2,
      Cmodes_CAUSAL | Cmodes_RESET | Cmodes_RESTART | Cmodes_SEGMARK |
          Cmodes_ERTERM | Cmodes_BYPASS,
  };

  ScopedOutputBlock fixture(width, height);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  for (int modes : mode_cases) {
    EncodeBlock32(block, pattern, k_max_prime, modes, HL_BAND,
                  /*reversible=*/true);
    const int full_passes = block->num_passes;
    ASSERT_GT(full_passes, 12);

    block->fussy = (modes & Cmodes_ERTERM) != 0;
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    ASSERT_NE(block->sample_buffer, nullptr);
    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime),
                pattern[i]);
    }

    // Exercise segment_truncated = true (fewer passes than
    // max_original_passes).
    if (full_passes > 11) {
      block->num_passes = full_passes - 1;
      EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
      EXPECT_EQ(block->num_passes, full_passes - 1);
    }
  }
}

TEST(BlockDecoderTest, RoundTrip64BitDenseAndBypassCausal) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime =
      35;  // 3 * 35 - 2 = 103 passes > 91 => use_32bits=false.
  const int width = 16;
  const int height = 12;  // 3 stripes => inter-stripe causal & non-causal.

  const auto pattern_sparse_cols =
      MakePattern64(width, height, k_max_prime, /*seed=*/1U);
  const auto pattern_dense =
      MakePattern64(width, height, k_max_prime, /*seed=*/2U);

  const int modes_list[] = {
      0,
      Cmodes_CAUSAL,
      Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2,
      Cmodes_CAUSAL | Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2,
  };

  ScopedOutputBlock fixture(width, height);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  for (int m_idx = 0; m_idx < 4; ++m_idx) {
    const auto& pat = (m_idx & 1) ? pattern_dense : pattern_sparse_cols;
    EncodeBlock64(block, pat, k_max_prime, modes_list[m_idx], LH_BAND);
    ASSERT_GT(block->num_passes, 91);
    const int encoded_passes = block->num_passes;

    // Decode into 64-bit buffer. Note sample_buffer is set to nullptr by
    // decode.
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    EXPECT_EQ(block->sample_buffer, nullptr);
    ASSERT_NE(block->sample_buffer64, nullptr);
    EXPECT_EQ(block->num_passes, encoded_passes);

    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag64(block->sample_buffer64[i], k_max_prime), pat[i]);
    }
  }
}

TEST(BlockDecoderTest, ZeroPassesAndPrecisionLimitedDecoding) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;

  // max_original_passes <= 0 breaks out early after zeroing sample_buffer.
  {
    ScopedOutputBlock fixture(8, 8);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    block->set_max_samples(64);
    std::memset(block->sample_buffer, 0x5A, 64 * sizeof(kdu_core::kdu_int32));
    block->K_max_prime = 5;
    block->missing_msbs = 5;
    block->num_passes = 0;
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    ASSERT_NE(block->sample_buffer, nullptr);
    for (int i = 0; i < 64; ++i) {
      EXPECT_EQ(block->sample_buffer[i], 0);
    }
  }

  // 32-bit precision limit: reversible=false, K_max_prime=33, num_passes=91.
  {
    ScopedOutputBlock fixture(8, 8);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat = MakePattern32(8, 8, 12, 7U);
    EncodeBlock32(block, pat, 12, /*modes=*/0, LL_BAND, /*reversible=*/false);
    // Keep pass 0 valid and pad pass_lengths up to 91 so num_passes==91
    // triggers precision reduction (num_passes -= 3 -> 88) in 32-bit mode
    // cleanly.
    block->set_max_passes(96, true);
    int z = block->num_passes;
    block->set_max_bytes(block->max_bytes + 512, true);
    for (; z < 91; ++z) {
      block->pass_lengths[z] = 0;
    }
    block->K_max_prime = 33;
    block->missing_msbs = 0;
    block->num_passes = 91;
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    EXPECT_EQ(block->num_passes, 88);
    EXPECT_NE(block->sample_buffer, nullptr);
  }

  // 64-bit precision limit: K_max_prime=65 (max_original_passes=193),
  // 3*p_max+1 = 187 passes triggers num_passes -= 3 -> 184.
  {
    ScopedOutputBlock fixture(8, 8);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat = MakePattern64(8, 8, 63, 11U);
    EncodeBlock64(block, pat, 63, /*modes=*/0, LL_BAND);
    // kdu_block_encoder caps num_passes at 3*62+1 = 187 when
    // sample_buffer==null.
    ASSERT_EQ(block->num_passes, 187);
    block->K_max_prime = 65;
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    EXPECT_EQ(block->num_passes, 184);
    EXPECT_EQ(block->sample_buffer, nullptr);
  }
}

TEST(BlockDecoderTest, LargeContextBufferAndTimingStatsLoop) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;

  // 4 x 1024 block => num_stripes=256, context_row_gap=7 => 1807 context words
  // (> 1600 threshold) and enables timing_iterations=2 to re-run timing loop.
  ScopedOutputBlock fixture(4, 1024, /*timing_iterations=*/2);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  const auto pat = MakePattern32(4, 1024, 10, 42U);
  EncodeBlock32(block, pat, 10, /*modes=*/0, LL_BAND, /*reversible=*/true);
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  ASSERT_NE(block->sample_buffer, nullptr);
  for (int i = 0; i < 4 * 1024; ++i) {
    EXPECT_EQ(FromSignMag32(block->sample_buffer[i], 10), pat[i]);
  }
}

TEST(BlockDecoderTest, CorruptSegmarkFussyThrowsAndResilientRecovers) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;
  const int width = 8;
  const int height = 8;
  const auto pat = MakePattern32(width, height, 6, 9U);

  ScopedOutputBlock fixture(width, height);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  // Use RESTART so each pass is its own segment; corrupt pass 0 (first
  // cleanup).
  EncodeBlock32(block, pat, 6, Cmodes_SEGMARK | Cmodes_RESTART, LL_BAND,
                /*reversible=*/true);
  ASSERT_GE(block->num_passes, 7);
  const int pass0_len = block->pass_lengths[0];
  ASSERT_GT(pass0_len, 0);
  // Flip all bits in pass 0 segment to break SEGMARK (0x0A).
  for (int i = 0; i < pass0_len; ++i) {
    block->byte_buffer[i] ^= 0xFFU;
  }

  block->fussy = true;
  block->resilient = false;
  EXPECT_THROW(decoder.decode(block, /*reversible=*/true),
               kdu_core::kdu_exception);

  // Now re-encode cleanly and corrupt cleanup pass z=3 (4th pass, 0-based index
  // 3) while enabling ERTERM + RESTART + SEGMARK so segment_start_z (3) > z-2
  // (1) triggers line segment_start_z > block->num_passes adjustment cleanly.
  EncodeBlock32(block, pat, 6, Cmodes_SEGMARK | Cmodes_RESTART | Cmodes_ERTERM,
                LL_BAND,
                /*reversible=*/true);
  ASSERT_GE(block->num_passes, 7);
  int offset = 0;
  for (int z = 0; z < 3; ++z) {
    offset += block->pass_lengths[z];
  }
  const int pass3_len = block->pass_lengths[3];
  ASSERT_GT(pass3_len, 0);
  for (int i = 0; i < pass3_len; ++i) {
    block->byte_buffer[offset + i] ^= 0xFFU;
  }

  bool errors_detected = false;
  block->errors_detected = &errors_detected;
  block->fussy = false;
  block->resilient = true;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  EXPECT_TRUE(errors_detected);
  EXPECT_LE(block->num_passes, 3);
  EXPECT_EQ(handlers.warning_count(), 1);

  // Decode a second corrupted block while errors_detected is already true:
  // warning count must remain 1.
  EncodeBlock32(block, pat, 6, Cmodes_SEGMARK | Cmodes_RESTART, LL_BAND,
                /*reversible=*/true);
  ASSERT_GE(block->pass_lengths[0], 1);
  for (int i = 0; i < block->pass_lengths[0]; ++i) {
    block->byte_buffer[i] ^= 0xFFU;
  }
  block->errors_detected = &errors_detected;
  block->fussy = false;
  block->resilient = true;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  EXPECT_EQ(block->num_passes, 0);
  EXPECT_EQ(handlers.warning_count(), 1);
}

TEST(BlockDecoderTest, CorruptErtermWithPriorValidSegmarkTruncatesCleanly) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_decoder decoder;
  const int width = 8;
  const int height = 8;
  const auto pat = MakePattern32(width, height, 6, 17U);

  ScopedOutputBlock fixture(width, height);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  // Without RESTART, all passes share one MQ segment checked by ERTERM on the
  // last pass; valid SEGMARKs on earlier cleanup passes allow partial salvage.
  EncodeBlock32(block, pat, 6, Cmodes_SEGMARK | Cmodes_ERTERM, LL_BAND,
                /*reversible=*/true);
  ASSERT_GE(block->num_passes, 7);

  int total_bytes = 0;
  for (int z = 0; z < block->num_passes; ++z) {
    total_bytes += block->pass_lengths[z];
  }
  ASSERT_GT(total_bytes, 2);
  // Flip final byte to trigger ERTERM failure on the trailing passes while
  // keeping early cleanup pass SEGMARKs intact.
  block->byte_buffer[total_bytes - 1] ^= 0xFFU;

  bool errors_detected = false;
  block->errors_detected = &errors_detected;
  block->fussy = false;
  block->resilient = true;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  EXPECT_TRUE(errors_detected);
  EXPECT_LT(block->num_passes, 16);
  EXPECT_EQ(handlers.warning_count(), 1);

  // Also verify ERTERM-only corruption (no SEGMARK) fails coder.finish()
  // and truncates the corrupted segment cleanly to segment_start_z.
  EncodeBlock32(block, pat, 6, Cmodes_ERTERM | Cmodes_RESTART, LL_BAND,
                /*reversible=*/true);
  ASSERT_GE(block->num_passes, 7);
  int restart_offset = block->pass_lengths[0] + block->pass_lengths[1];
  ASSERT_GT(block->pass_lengths[2], 0);
  for (int i = 0; i < block->pass_lengths[2]; ++i) {
    block->byte_buffer[restart_offset + i] ^= 0xFFU;
  }
  bool erterm_error = false;
  block->errors_detected = &erterm_error;
  block->fussy = false;
  block->resilient = true;
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  EXPECT_TRUE(erterm_error);
  EXPECT_EQ(block->num_passes, 2);
  EXPECT_EQ(handlers.warning_count(), 2);
}

}  // namespace
