// Unit tests for
// third_party/kakadu/vendor/coresys/transform/ssse3_dwt_local.cpp.
//
// In Google3's active configuration, `vendor/BUILD` leaves
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE` undefined, so `08-simd_issues.patch` in
// `synthesis.cpp` clears `vlift_16_func` and `hlift_16_func` for
// `step->step_idx == 1 && step->kernel_id == Ckernels_W9X7`. As a result,
// `ssse3_vlift_16_9x7_synth_s1` (lines 118-152) and
// `ssse3_hlift_16_9x7_synth_s1` (lines 270-296) are unreachable without
// defining `KDU_SIMD_OPTIMIZATIONS_UNSAFE` (`173/213 = 81.22%` x86_64 coverage
// ceiling; on `--cpu=arm`, SSSE3 code is compiled out under
// `KDU_X86_INTRINSICS`). This suite exercises 100% of all reachable SSSE3
// analysis and synthesis DWT lifting paths and pointer-alignment branches.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char* string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class Ssse3DwtLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kSsse3DwtEnv =
    ::testing::AddGlobalTestEnvironment(new Ssse3DwtLocalTestEnvironment());

class VectorTarget final : public kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_byte>& out) : out_(out) {}
  bool write(const kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_byte>& out_;
};

class VectorSource final : public kdu_compressed_source {
 public:
  explicit VectorSource(const std::vector<kdu_byte>& data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
};

struct ScopedCodestream {
  siz_params siz;
  kdu_codestream codestream;

  ~ScopedCodestream() {
    if (codestream.exists()) {
      codestream.destroy();
    }
  }
};

struct ScopedTile {
  kdu_tile tile;
  ~ScopedTile() {
    if (tile.exists()) {
      tile.close();
    }
  }
};

struct ScopedAnalysis {
  kdu_push_ifc engine;
  ~ScopedAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedSynthesis {
  kdu_pull_ifc engine;
  ~ScopedSynthesis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedLineBuf {
  kdu_line_buf line;
  ~ScopedLineBuf() { line.destroy(); }
};

void RunW9X7RoundTrip16Bit(int width, int height, int origin_x, int origin_y) {
  std::vector<kdu_int16> input(static_cast<size_t>(width * height));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int val = ((y * 17 + x * 31) % 201) - 100;
      input[static_cast<size_t>(y * width + x)] =
          static_cast<kdu_int16>(val << (KDU_FIX_POINT - 8));
    }
  }

  // 1. Encode with 16-bit W9X7 analysis to trigger SSSE3 vlift/hlift paths.
  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream ctx;
    ASSERT_TRUE(ctx.siz.parse_string("Scomponents=1"));
    // kdu_dims/params pairs use {y, x} (vertical coordinate first).
    ctx.siz.set(Sorigin, 0, 0, origin_y);
    ctx.siz.set(Sorigin, 0, 1, origin_x);
    ctx.siz.set(Ssize, 0, 0, origin_y + height);
    ctx.siz.set(Ssize, 0, 1, origin_x + width);
    ctx.siz.set(Sdims, 0, 0, height);
    ctx.siz.set(Sdims, 0, 1, width);
    ctx.siz.set(Ssampling, 0, 0, 1);
    ctx.siz.set(Ssampling, 0, 1, 1);
    ctx.siz.set(Sprecision, 0, 0, 8);
    ctx.siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params*>(&ctx.siz)->finalize();

    ctx.codestream.create(&ctx.siz, &target);
    siz_params* params = ctx.codestream.access_siz();
    ASSERT_TRUE(params->parse_string("Clevels=2"));
    ASSERT_TRUE(params->parse_string("Ckernels=W9X7"));
    ASSERT_TRUE(params->parse_string("Creversible=no"));
    params->finalize_all();

    {
      ScopedTile tile;
      tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
      kdu_tile_comp tc = tile.tile.access_component(0);
      kdu_resolution res = tc.access_resolution();
      kdu_sample_allocator allocator;
      kdu_push_pull_params pp_params;
      ScopedAnalysis analysis;
      analysis.engine =
          kdu_analysis(res, &allocator, pp_params, KDU_SAMPLE_PREC16);
      ScopedLineBuf line;
      line.line.pre_create(&allocator, width, KDU_LINE_BUF_SHORTS, 0, 0);
      allocator.finalize(ctx.codestream);
      line.line.create();

      for (int y = 0; y < height; ++y) {
        kdu_sample16* buf = line.line.get_buf16();
        for (int x = 0; x < width; ++x) {
          buf[x].ival = input[static_cast<size_t>(y * width + x)];
        }
        while (!analysis.engine.push(line.line)) {
        }
      }
      while (!analysis.engine.finish(nullptr)) {
      }
      analysis.engine.destroy();
      tile.tile.close();
    }

    kdu_long max_bytes = 1 << 20;
    ctx.codestream.flush(&max_bytes, 1);
    ctx.codestream.destroy();
  }

  ASSERT_FALSE(bitstream.empty());

  // 2. Decode with 16-bit W9X7 synthesis to trigger SSSE3 s0 and s23 synth.
  VectorSource source(bitstream);
  ScopedCodestream dec_ctx;
  dec_ctx.codestream.create(&source);
  ScopedTile dec_tile;
  dec_tile.tile = dec_ctx.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = dec_tile.tile.access_component(0);
  kdu_resolution res = tc.access_resolution();

  kdu_sample_allocator dec_alloc;
  kdu_push_pull_params dec_params;
  ScopedSynthesis synth;
  synth.engine = kdu_synthesis(res, &dec_alloc, dec_params, KDU_SAMPLE_PREC16);
  ScopedLineBuf dec_line;
  dec_line.line.pre_create(&dec_alloc, width, KDU_LINE_BUF_SHORTS, 0, 0);
  dec_alloc.finalize(dec_ctx.codestream);
  dec_line.line.create();

  for (int y = 0; y < height; ++y) {
    synth.engine.pull(dec_line.line);
    const kdu_sample16* buf = dec_line.line.get_buf16();
    for (int x = 0; x < width; ++x) {
      const int expected =
          input[static_cast<size_t>(y * width + x)] >> (KDU_FIX_POINT - 8);
      const int actual = buf[x].ival >> (KDU_FIX_POINT - 8);
      EXPECT_NEAR(actual, expected, 8)
          << "width=" << width << " ox=" << origin_x << " oy=" << origin_y
          << " at (" << x << "," << y << ")";
    }
  }
}

TEST(Ssse3DwtLocalTest, W9X716BitAnalysisAndSynthesisAcrossOrigins) {
  // Varied widths and x/y origins cover both 16-byte-aligned and unaligned
  // `src` pointers inside ssse3_hlift_16_9x7_{analysis,synth}_s{0,1,23}.
  constexpr std::array<int, 4> kOrigins = {0, 1, 2, 3};
  for (int ox : kOrigins) {
    RunW9X7RoundTrip16Bit(/*width=*/40, /*height=*/20, ox,
                          /*origin_y=*/ox & 1);
  }
  RunW9X7RoundTrip16Bit(/*width=*/37, /*height=*/19, /*origin_x=*/5,
                        /*origin_y=*/1);
}

}  // namespace
}  // namespace kdu_core
