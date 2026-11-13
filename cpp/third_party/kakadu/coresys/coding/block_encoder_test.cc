// Unit tests for third_party/kakadu/vendor/coresys/coding/block_encoder.cpp.

#include <algorithm>
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

class IgnoringWarningHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
  }
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

 private:
  ThrowingErrorHandler error_handler_;
  IgnoringWarningHandler warning_handler_;
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
    if (width == 4 && height == 1024) {
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
      orig_cplex_start_ = block_->cplex_start;
      orig_cplex_passes_ = block_->cplex_passes;
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
      block_->cplex_start = orig_cplex_start_;
      block_->cplex_passes = orig_cplex_passes_;
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
  kdu_core::kdu_byte orig_cplex_start_ = 0;
  kdu_core::kdu_byte orig_cplex_passes_ = 0;
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
  uint32_t state = seed ? seed : 0x2468ACE0U;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if ((y < 4) && (x >= 2 && x <= 7) && (seed & 1U)) {
        continue;
      }
      state = state * 1664525U + 1013904223U;
      int32_t mag = static_cast<int32_t>((state >> 6) %
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
  uint64_t state = seed ? seed : 0x517CC1B727220A95ULL;
  uint64_t mask = (uint64_t{1} << k_max_prime) - 1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
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

void PrepareBlock32(kdu_core::kdu_block* block,
                    const std::vector<int32_t>& vals, int k_max_prime,
                    int modes, int orientation) {
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
}

void PrepareBlock64(kdu_core::kdu_block* block,
                    const std::vector<int64_t>& vals, int k_max_prime,
                    int modes, int orientation) {
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
}

std::vector<float> MakePerPlaneWmse(float base_wmse) {
  std::vector<float> wmse(64, 0.0f);
  float val = base_wmse;
  for (int i = 0; i < 64; ++i) {
    wmse[i] = val;
    val *= 0.25f;
  }
  return wmse;
}

TEST(BlockEncoderTest, CapabilitiesInitSpeedpackAndOptionalEntryPoints) {
  ScopedMessageHandlers handlers;
  int cap_flags = 0;
  const char* caps = kdu_core::kdu_block_encoder::get_capabilities(cap_flags);
  ASSERT_NE(caps, nullptr);
  EXPECT_EQ(cap_flags, KDU_BLOCK_CODER_CAP_J2K1);
  EXPECT_EQ(std::string(caps), "J2K1");

  kdu_core::kdu_block_encoder encoder;
  encoder.init(/*allow_optimizations=*/false);
  encoder.init(/*allow_optimizations=*/true);
  encoder.speedpack_config(kdu_core::kdu_coords(64, 64), 16);

  ScopedOutputBlock fixture(16, 16);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  std::vector<kdu_core::kdu_uint16> data16(256, 1);
  std::vector<kdu_core::kdu_uint32> data32(256, 1);
  block->modes = 0;
  EXPECT_FALSE(encoder.encode16(block, data16.data(), /*reversible=*/true));
  EXPECT_FALSE(encoder.encode32(block, data32.data(), /*reversible=*/true));

  // Exercise HT stub dispatch under Google3's active configuration.
  block->modes = Cmodes_HT;
  EXPECT_FALSE(encoder.encode16(block, data16.data(), /*reversible=*/false));
  EXPECT_FALSE(encoder.encode32(block, data32.data(), /*reversible=*/false));

  block->num_passes = 3;
  encoder.encode(block, /*reversible=*/true);
  EXPECT_EQ(block->num_passes, 0);

  std::vector<float> weights(16, 1.0f);
  const auto wmse = MakePerPlaneWmse(1.0f);
  block->modes = Cmodes_HT;
  block->num_passes = 3;
  encoder.cellular_encode(block, /*reversible=*/true, wmse.data(),
                          weights.data(), 4, 4);
  EXPECT_EQ(block->num_passes, 0);

  encoder.init(/*allow_optimizations=*/false);
  block->modes = Cmodes_HT;
  block->num_passes = 3;
  encoder.encode(block, /*reversible=*/false);
  EXPECT_EQ(block->num_passes, 0);
}

TEST(BlockEncoderTest, Encode32BitOrientationsPartialStripesAndAllModes) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_encoder encoder;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime = 14;
  const int orientations[] = {LL_BAND, LH_BAND, HL_BAND, HH_BAND};

  const int heights[] = {1, 2, 3, 4, 9, 10, 11};
  for (int h_idx = 0; h_idx < 7; ++h_idx) {
    const int height = heights[h_idx];
    const int width = (height <= 4) ? 16 : 8;
    ScopedOutputBlock fixture(width, height);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);

    const auto vals = MakePattern32(width, height, k_max_prime,
                                    static_cast<uint32_t>(h_idx + 1));
    PrepareBlock32(block, vals, k_max_prime, /*modes=*/0,
                   orientations[h_idx % 4]);
    const auto wmse = MakePerPlaneWmse(1024.0f);
    encoder.encode(block, /*reversible=*/true, wmse.data());
    ASSERT_EQ(block->num_passes, 3 * k_max_prime - 2);
    EXPECT_GT(block->pass_slopes[block->num_passes - 1], 0);

    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime), vals[i]);
    }
  }

  // Exercise all mode switches on a multi-stripe block.
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
      Cmodes_CAUSAL | Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2,
      Cmodes_CAUSAL | Cmodes_RESET | Cmodes_RESTART | Cmodes_SEGMARK |
          Cmodes_ERTERM | Cmodes_BYPASS,
  };
  ScopedOutputBlock fixture(16, 12);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);
  const auto dense = MakePattern32(16, 12, k_max_prime, 8U);
  const auto sparse = MakePattern32(16, 12, k_max_prime, 9U);
  const auto wmse = MakePerPlaneWmse(4096.0f);

  for (int idx = 0; idx < 12; ++idx) {
    const auto& pat = (idx & 1) ? dense : sparse;
    PrepareBlock32(block, pat, k_max_prime, mode_cases[idx], HL_BAND);
    encoder.encode(block, /*reversible=*/false, wmse.data());
    ASSERT_EQ(block->num_passes, 3 * k_max_prime - 2);
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    for (int i = 0; i < 16 * 12; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime), pat[i]);
    }
  }
}

TEST(BlockEncoderTest, Encode32BitBufferAugmentationSlopesAndClamping) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_encoder encoder;
  kdu_core::kdu_block_decoder decoder;

  // High-entropy 64x64 block forces byte buffer growth (> 4KB).
  {
    ScopedOutputBlock fixture(64, 64);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat = MakePattern32(64, 64, 24, 12345U);
    PrepareBlock32(block, pat, 24, Cmodes_RESTART, HL_BAND);
    const int initial_max_bytes = block->max_bytes;
    encoder.encode(block, /*reversible=*/true);
    EXPECT_GT(block->max_bytes, initial_max_bytes);
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    for (int i = 0; i < 64 * 64; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], 24), pat[i]);
    }
  }

  // Test slope clamp (> 0xFFFF and < 2), reversible final-pass slope forcing,
  // early threshold termination (open segment + restart), and pass clamping.
  {
    ScopedOutputBlock fixture(16, 16);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat = MakePattern32(16, 16, 14, 77U);

    // Extreme low WMSE => slope_to_log clamps < 2.0f -> 2, and reversible
    // forced last pass slope >= 1 if zeroed by convex hull.
    PrepareBlock32(block, pat, 14, /*modes=*/0, HL_BAND);
    std::vector<float> tiny_wmse(64, 1.0e-35f);
    encoder.encode(block, /*reversible=*/true, tiny_wmse.data());
    EXPECT_GE(block->pass_slopes[block->num_passes - 1], 1);

    // High WMSE (below float infinity) => slope_to_log clamps to 0xFFFF.
    PrepareBlock32(block, pat, 14, /*modes=*/0, HL_BAND);
    std::vector<float> huge_wmse(64, 1.0e25f);
    encoder.encode(block, /*reversible=*/false, huge_wmse.data());
    ASSERT_GT(block->num_passes, 0);
    EXPECT_EQ(*std::max_element(block->pass_slopes,
                                block->pass_slopes + block->num_passes),
              0xFFFF);

    // High min_slope_threshold stops early inside an open segment and under
    // Cmodes_RESTART.
    PrepareBlock32(block, pat, 14, /*modes=*/0, HL_BAND);
    const int orig_passes = block->num_passes;
    const auto normal_wmse = MakePerPlaneWmse(1.0f);
    encoder.encode(block, /*reversible=*/false, normal_wmse.data(),
                   /*min_slope_threshold=*/0xF000);
    EXPECT_LT(block->num_passes, orig_passes);

    PrepareBlock32(block, pat, 14, Cmodes_RESTART, HL_BAND);
    encoder.encode(block, /*reversible=*/false, normal_wmse.data(),
                   /*min_slope_threshold=*/0xF000);
    EXPECT_LT(block->num_passes, orig_passes);

    // Negative num_passes, oversized num_passes, and cplex clamping.
    PrepareBlock32(block, pat, 14, /*modes=*/0, LL_BAND);
    block->num_passes = -5;
    encoder.encode(block, /*reversible=*/true);
    EXPECT_EQ(block->num_passes, 0);

    PrepareBlock32(block, pat, 14, /*modes=*/0, LL_BAND);
    block->cplex_start = 1;
    block->cplex_passes = 8;
    block->num_passes = 200;
    encoder.encode(block, /*reversible=*/false);
    EXPECT_LE(block->num_passes, 12);
  }
}

TEST(BlockEncoderTest, Encode64BitAllPassTypesPartialStripesAndSlopes) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_encoder encoder;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime = 35;  // > 91 passes => 64-bit coding passes.

  // Cover partial stripes (heights 1, 2, 3) plus multi-stripe 16x12 in 64-bit.
  const int heights[] = {1, 2, 3, 12};
  for (int h_idx = 0; h_idx < 4; ++h_idx) {
    const int height = heights[h_idx];
    const int width = 16;
    ScopedOutputBlock fixture(width, height);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);

    const auto pat = MakePattern64(width, height, k_max_prime,
                                   static_cast<uint64_t>(h_idx + 1));
    const int modes =
        (h_idx == 3) ? (Cmodes_CAUSAL | Cmodes_BYPASS | Cmodes_BYPASS_E1 |
                        Cmodes_BYPASS_E2)
                     : (Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2);
    PrepareBlock64(block, pat, k_max_prime, modes, HH_BAND);
    const auto wmse = MakePerPlaneWmse(1024.0f);
    encoder.encode(block, /*reversible=*/false, wmse.data());
    ASSERT_GT(block->num_passes, 91);

    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    EXPECT_EQ(block->sample_buffer, nullptr);
    ASSERT_NE(block->sample_buffer64, nullptr);
    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag64(block->sample_buffer64[i], k_max_prime), pat[i]);
    }
  }

  // Non-bypass 64-bit MQ passes + causal + early threshold stop.
  ScopedOutputBlock fixture(16, 12);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);
  const auto pat = MakePattern64(16, 12, k_max_prime, 2U);
  for (int modes : {0, Cmodes_CAUSAL}) {
    PrepareBlock64(block, pat, k_max_prime, modes, LH_BAND);
    encoder.encode(block, /*reversible=*/false);
    ASSERT_GT(block->num_passes, 91);
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/false));
    for (int i = 0; i < 16 * 12; ++i) {
      EXPECT_EQ(FromSignMag64(block->sample_buffer64[i], k_max_prime), pat[i]);
    }
  }
}

TEST(BlockEncoderTest, CellularEncodePartialStripesModesWeightsAndErrors) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_encoder encoder;
  kdu_core::kdu_block_decoder decoder;
  const int k_max_prime = 14;
  const auto wmse = MakePerPlaneWmse(2048.0f);
  std::vector<float> cell_weights(256, 1.0f);
  for (int i = 0; i < 256; ++i) {
    cell_weights[i] = 0.5f + 0.02f * static_cast<float>(i & 15);
  }

  // Partial stripes & first_cell_cols / first_cell_rows != 4 bilinear shifts.
  const int heights[] = {1, 2, 3, 9, 10, 11};
  for (int h_idx = 0; h_idx < 6; ++h_idx) {
    const int height = heights[h_idx];
    const int width = 12;
    ScopedOutputBlock fixture(width, height);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);

    std::vector<float> local_weights = cell_weights;
    const auto pat = MakePattern32(width, height, k_max_prime,
                                   static_cast<uint32_t>(h_idx + 1));
    PrepareBlock32(block, pat, k_max_prime, /*modes=*/0, HL_BAND);
    const int first_cols = (h_idx % 3) + 1;  // 1..3 triggers horizontal shift.
    const int first_rows =
        ((h_idx + 1) % 3) + 1;  // 1..3 triggers vertical shift.
    encoder.cellular_encode(block, /*reversible=*/true, wmse.data(),
                            local_weights.data(), first_cols, first_rows);
    ASSERT_EQ(block->num_passes, 3 * k_max_prime - 2);
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    for (int i = 0; i < width * height; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime), pat[i]);
    }
  }

  // Exercise all pass functions in cellular mode + early threshold cutoff.
  {
    ScopedOutputBlock fixture(16, 12, /*timing_iterations=*/2);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat_sparse = MakePattern32(16, 12, k_max_prime, 11U);
    const auto pat_dense = MakePattern32(16, 12, k_max_prime, 12U);

    const int modes_list[] = {
        0,
        Cmodes_CAUSAL,
        Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2,
        Cmodes_CAUSAL | Cmodes_BYPASS | Cmodes_BYPASS_E1 | Cmodes_BYPASS_E2 |
            Cmodes_RESET | Cmodes_RESTART | Cmodes_SEGMARK | Cmodes_ERTERM,
    };
    for (int m_idx = 0; m_idx < 4; ++m_idx) {
      const auto& pat = (m_idx & 1) ? pat_dense : pat_sparse;
      PrepareBlock32(block, pat, k_max_prime, modes_list[m_idx], LH_BAND);
      encoder.cellular_encode(block, /*reversible=*/(m_idx == 0), wmse.data(),
                              cell_weights.data(), 4, 4);
      EXPECT_TRUE(decoder.decode(block, /*reversible=*/(m_idx == 0)));
      for (int i = 0; i < 16 * 12; ++i) {
        EXPECT_EQ(FromSignMag32(block->sample_buffer[i], k_max_prime), pat[i]);
      }
    }

    // Early threshold termination (open segment + restart) in cellular_encode.
    const auto small_wmse = MakePerPlaneWmse(1.0f);
    for (int mode : {0, Cmodes_RESTART}) {
      PrepareBlock32(block, pat_dense, k_max_prime, mode, HL_BAND);
      const int orig_passes = block->num_passes;
      encoder.cellular_encode(block, /*reversible=*/false, small_wmse.data(),
                              cell_weights.data(), 4, 4,
                              /*min_slope_threshold=*/0xF000);
      EXPECT_LT(block->num_passes, orig_passes);
    }

    // Negative / oversized pass clamping in cellular_encode.
    PrepareBlock32(block, pat_dense, k_max_prime, /*modes=*/0, LL_BAND);
    block->num_passes = -1;
    encoder.cellular_encode(block, /*reversible=*/true, nullptr,
                            cell_weights.data(), 4, 4);
    EXPECT_EQ(block->num_passes, 0);

    PrepareBlock32(block, pat_dense, k_max_prime, /*modes=*/0, LL_BAND);
    block->num_passes = 200;
    encoder.cellular_encode(block, /*reversible=*/false, small_wmse.data(),
                            cell_weights.data(), 4, 4);
    EXPECT_LE(block->num_passes, 31 * 3 - 2);

    // Null sample_buffer throws KDU_ERROR_EXCEPTION in
    // kd_cellular_block_encode.
    block->set_max_samples64(16 * 12);
    block->hide_sample_buffer();
    block->num_passes = 10;
    EXPECT_THROW(
        encoder.cellular_encode(block, /*reversible=*/false, wmse.data(),
                                cell_weights.data(), 4, 4),
        kdu_core::kdu_exception);
    block->set_max_samples(16 * 12);
  }

  // Large 64x64 cellular encode forces byte buffer reallocation.
  {
    ScopedOutputBlock fixture(64, 64);
    kdu_core::kdu_block* block = fixture.block();
    ASSERT_NE(block, nullptr);
    const auto pat = MakePattern32(64, 64, 24, 9999U);
    PrepareBlock32(block, pat, 24, Cmodes_RESTART, HL_BAND);
    const int initial_max_bytes = block->max_bytes;
    encoder.cellular_encode(block, /*reversible=*/true, wmse.data(),
                            cell_weights.data(), 4, 4);
    EXPECT_GT(block->max_bytes, initial_max_bytes);
    EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
    for (int i = 0; i < 64 * 64; ++i) {
      EXPECT_EQ(FromSignMag32(block->sample_buffer[i], 24), pat[i]);
    }
  }
}

TEST(BlockEncoderTest, LargeContextBufferAndTimingStatsLoop) {
  ScopedMessageHandlers handlers;
  kdu_core::kdu_block_encoder encoder;
  kdu_core::kdu_block_decoder decoder;

  ScopedOutputBlock fixture(4, 1024, /*timing_iterations=*/2);
  kdu_core::kdu_block* block = fixture.block();
  ASSERT_NE(block, nullptr);

  const auto pat = MakePattern32(4, 1024, 10, 42U);
  PrepareBlock32(block, pat, 10, /*modes=*/0, LL_BAND);
  encoder.encode(block, /*reversible=*/true);
  EXPECT_TRUE(decoder.decode(block, /*reversible=*/true));
  for (int i = 0; i < 4 * 1024; ++i) {
    EXPECT_EQ(FromSignMag32(block->sample_buffer[i], 10), pat[i]);
  }
}

}  // namespace
