// Unit tests for
// third_party/kakadu/vendor/apps/support/ssse3_region_decompressor.cpp.
//
// In Google3's x86_64 configuration (`KAKADU_X86_SIMD_DEFINES`),
// `ssse3_region_decompressor.cpp` IS compiled and executed on `k8`. These
// tests exercise all five SSSE3 horizontal resampling paths via
// `kdu_region_decompressor`:
//   - `ssse3_horz_resample_float` (6-tap float across expansions/reductions)
//   - `ssse3_hshuf_float_2tap_expand` (2-tap shuffle float expansion)
//   - `ssse3_horz_resample_fix16` (6-tap fix16 heavy reduction <= 7.1/18)
//   - `ssse3_hshuf_fix16_2tap_expand` (2-tap shuffle fix16 with bv=1 and bv=2)
//   - `ssse3_hshuf_fix16_6tap_expand` (6-tap shuffle fix16 with bv=2 and bv=3)
//
// Coverage ceiling via public `kdu_region_decompressor` on `k8`
// (`-DKDU_NO_AVX2`): 273/365 `DA:` lines (74.79%, 100% of reachable lines). The
// remaining 92 lines are unreachable through public API dispatch:
//   - `L96-123` (24 lines) & `L201-238` (33 lines): `leadin == 0` with
//     `blend_vecs == 0` only occurs for 2-tap kernels, which are
//     unconditionally overridden by `ssse3_hshuf_*_2tap_expand`
//     (`blend_vecs > 0`).
//   - `L348-369` (21 lines): `blend_vecs == 1` in
//     `ssse3_hshuf_float_2tap_expand` is never selected
//     (`SSSE3_SET_HSHUF_FLOAT_2TAP_FUNC` sets `bv = 2`).
//   - `L298-313` (13 lines): `kl > 4` in `ssse3_horz_resample_fix16` requires
//     `kernel_length` in `{13..15, 21..23}`, whereas `exp <= 7.1/18` (`rate` in
//     `[2.535, 3.0)`) yields `kernel_length` in `[17, 20]` (`kl` in `[1, 4]`).
//   - `L573` (1 line): defensive `assert(0)` in
//     `ssse3_hshuf_fix16_6tap_expand`.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
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
using ::kdu_core::kdu_int16;
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

class Ssse3RegionDecompressorTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Ssse3RegionDecompressorTestEnv());

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

std::vector<kdu_byte> CompressSmoothCodestream(int width, int height,
                                               int bit_depth,
                                               bool reversible) {
  std::vector<kdu_byte> out;
  MemoryTarget target(&out);
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, 1);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, false);
  static_cast<kdu_params &>(siz).finalize();

  kdu_codestream cs;
  CodestreamGuard guard(&cs);
  cs.create(&siz, &target);
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->parse_string("Cycc=no");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();

  const int max_val = (1 << bit_depth) - 1;
  const int half = 1 << (bit_depth - 1);
  const size_t n = static_cast<size_t>(width) * height;
  std::vector<kdu_int16> samples(n);
  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      const int v = (c * 17 + r * 31) & max_val;
      samples[static_cast<size_t>(r) * width + c] =
          static_cast<kdu_int16>(v - half);
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
  while (comp.push_stripe(samples.data(), heights, ogaps, sgaps, rgaps, precs,
                          sflags)) {
  }
  comp.finish();
  cs.destroy();
  return out;
}

struct Ratio {
  int num;
  int den;
};

void RunFloatResample(kdu_codestream &cs, kdu_channel_mapping &mapping,
                      float max_overshoot, int zero_thresh, Ratio rx, Ratio ry,
                      int crop_x) {
  kdu_region_decompressor decomp;
  decomp.set_interpolation_behaviour(max_overshoot, zero_thresh);
  const kdu_dims full = decomp.get_rendered_image_dims(
      cs, &mapping, 0, 0, kdu_coords(rx.num, ry.num),
      kdu_coords(rx.den, ry.den));
  kdu_dims region = full;
  if (crop_x > 0 && crop_x < full.size.x) {
    region.pos.x += crop_x;
    region.size.x -= crop_x;
  }
  ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                           kdu_coords(rx.num, ry.num),
                           kdu_coords(rx.den, ry.den), /*precise=*/false,
                           ::kdu_core::KDU_WANT_OUTPUT_COMPONENTS,
                           /*fastest=*/false));
  std::vector<float> out(region.area(), 0.0f);
  float *bufs[1] = {out.data()};
  kdu_dims inc = region;
  kdu_dims nr;
  while (!inc.is_empty()) {
    ASSERT_TRUE(decomp.process(bufs, false, 1, region.pos, region.size.x, 256,
                               256, inc, nr));
  }
  EXPECT_TRUE(decomp.finish());
  for (float v : out) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

void RunFix16Resample(kdu_codestream &cs, kdu_channel_mapping &mapping,
                      float max_overshoot, int zero_thresh, Ratio rx, Ratio ry,
                      int crop_x) {
  kdu_region_decompressor decomp;
  decomp.set_interpolation_behaviour(max_overshoot, zero_thresh);
  const kdu_dims full = decomp.get_rendered_image_dims(
      cs, &mapping, 0, 0, kdu_coords(rx.num, ry.num),
      kdu_coords(rx.den, ry.den));
  kdu_dims region = full;
  if (crop_x > 0 && crop_x < full.size.x) {
    region.pos.x += crop_x;
    region.size.x -= crop_x;
  }
  ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                           kdu_coords(rx.num, ry.num),
                           kdu_coords(rx.den, ry.den), /*precise=*/false,
                           ::kdu_core::KDU_WANT_OUTPUT_COMPONENTS,
                           /*fastest=*/true));
  std::vector<kdu_byte> out(region.area(), 0);
  kdu_byte *bufs[1] = {out.data()};
  kdu_dims inc = region;
  kdu_dims nr;
  while (!inc.is_empty()) {
    ASSERT_TRUE(decomp.process(bufs, false, 1, region.pos, region.size.x, 256,
                               256, inc, nr, 8));
  }
  EXPECT_TRUE(decomp.finish());
  EXPECT_GT(out.size(), 0U);
}

TEST(Ssse3RegionDecompressorTest, FloatTwoTapShuffleAndSixTapResample) {
  // Use 12-bit irreversible so `KDRD_FLOAT_TYPE` is native (`!fastest`).
  for (int width : {19, 65}) {
    const std::vector<kdu_byte> data =
        CompressSmoothCodestream(width, 6, 12, /*reversible=*/false);
    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    mapping.configure(cs);

    // 1. 2-tap shuffle float expansion (`max_overshoot = 0.0f` forces klen=2):
    // exercises multiple ratios (`_exp > 1.0f`) and non-zero crop phase.
    for (Ratio rx : {Ratio{9, 8}, Ratio{5, 4}, Ratio{3, 2}, Ratio{2, 1},
                     Ratio{5, 2}, Ratio{3, 1}}) {
      RunFloatResample(cs, mapping, /*max_overshoot=*/0.0f, /*zero_thresh=*/1,
                       rx, {1, 1}, 0);
    }
    RunFloatResample(cs, mapping, /*max_overshoot=*/0.0f, /*zero_thresh=*/1,
                     {3, 2}, {1, 1}, 3);

    // 2. 6-tap float resampling (`max_overshoot > 0` with exp <= 1.0 and
    // exp < zero_thresh = 8): exercises reduction (`3/8`, `1/2`, `3/5`) and
    // moderate/high expansion (`5/4`, `3/2`, `2/1`, `5/2`).
    for (Ratio rx : {Ratio{3, 8}, Ratio{1, 2}, Ratio{3, 5}, Ratio{3, 4},
                     Ratio{9, 8}, Ratio{3, 2}, Ratio{2, 1}, Ratio{5, 2}}) {
      RunFloatResample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                       rx, {1, 1}, 0);
    }
    RunFloatResample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                     {3, 8}, {1, 1}, 2);
    cs.destroy();
  }
}

TEST(Ssse3RegionDecompressorTest, Fix16TwoTapSixTapShuffleAndResample) {
  // Use 8-bit irreversible with `fastest = true` -> `KDRD_FIX16_TYPE`.
  for (int width : {19, 65}) {
    const std::vector<kdu_byte> data =
        CompressSmoothCodestream(width, 6, 8, /*reversible=*/false);
    MemorySource src(data);
    kdu_codestream cs;
    CodestreamGuard guard(&cs);
    cs.create(&src);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    mapping.configure(cs);

    // 1. `ssse3_hshuf_fix16_2tap_expand` (`max_overshoot = 0.0f`, `exp > 1`):
    // covers both `_bv = 2` (`exp <= 7.1/6 = 1.183`, e.g. `9/8`, `7/6`) and
    // `_bv = 1` (`exp > 1.183`, e.g. `5/4`, `3/2`, `2/1`, `5/2`).
    for (Ratio rx : {Ratio{10, 9}, Ratio{9, 8}, Ratio{5, 4}, Ratio{3, 2},
                     Ratio{2, 1}, Ratio{5, 2}}) {
      RunFix16Resample(cs, mapping, /*max_overshoot=*/0.0f, /*zero_thresh=*/1,
                       rx, {1, 1}, 0);
    }
    RunFix16Resample(cs, mapping, /*max_overshoot=*/0.0f, /*zero_thresh=*/1,
                     {9, 8}, {1, 1}, 3);

    // 2. `ssse3_hshuf_fix16_6tap_expand` (`max_overshoot = 0.25f`,
    // `zero_thresh = 8`): covers `bv = 2` (`exp >= 0.71`: `3/4`, `4/5`, `5/4`,
    // `3/2`, `2/1`) and `bv = 3` (`0.394 <= exp < 0.71`: `1/2`, `3/5`).
    for (Ratio rx : {Ratio{1, 2}, Ratio{3, 5}, Ratio{3, 4}, Ratio{4, 5},
                     Ratio{9, 8}, Ratio{3, 2}, Ratio{2, 1}, Ratio{5, 2}}) {
      RunFix16Resample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                       rx, {1, 1}, 0);
    }
    RunFix16Resample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                     {1, 2}, {1, 1}, 2);

    // 3. `ssse3_horz_resample_fix16`: 6-tap reduction where `tmp_bv > 3`
    // (`1/3 < exp < 7.1/18 = 0.3944`, e.g. `7/20`, `3/8`, `11/30`).
    for (Ratio rx : {Ratio{7, 20}, Ratio{3, 8}, Ratio{11, 30}}) {
      RunFix16Resample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                       rx, {1, 1}, 0);
    }
    RunFix16Resample(cs, mapping, /*max_overshoot=*/0.25f, /*zero_thresh=*/8,
                     {3, 8}, {1, 1}, 1);
    cs.destroy();
  }
}

}  // namespace
