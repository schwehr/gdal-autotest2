// Unit tests for
// third_party/kakadu/vendor/coresys/coding/avx_coder_local.cpp.
//
// In Google3's active configuration, `vendor/BUILD` leaves
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE` undefined, and `08-simd_issues.patch` in
// `decoder.cpp:320` gates `job->simd_block_xfer` behind
// `KDU_SIMD_OPTIMIZATIONS_UNSAFE`. Consequently, although
// `avx_coder_local.cpp` is compiled in (`avx_xfer_irrev_decoded_block16` and
// `avx_xfer_irrev_decoded_block32`, 108 instrumented lines on x86_64), both
// functions are unreachable without defining `KDU_SIMD_OPTIMIZATIONS_UNSAFE`
// (`0/108 = 0.00%` x86_64 coverage ceiling). This suite verifies that the
// corresponding irreversible 16-bit and 32-bit decoded-block transfer fallback
// paths (across aligned/unaligned origins and single/multi-block stripes) run
// accurately.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
  void put_text(const char* text) override { std::fputs(text, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class QuietWarningHandler final : public kdu_thread_safe_message {
 public:
  ~QuietWarningHandler() noexcept override = default;
  void put_text(const char* /*text*/) override {}
};

class AvxCoderTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const error_handler = new ThrowingErrorHandler();
    static auto* const warning_handler = new QuietWarningHandler();
    kdu_customize_errors(error_handler);
    kdu_customize_warnings(warning_handler);
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new AvxCoderTestEnvironment());

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
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
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

std::vector<int32_t> MakeInputSamples(int width, int height, int bit_depth) {
  std::vector<int32_t> out(static_cast<size_t>(width) * height);
  const int32_t half = 1 << (bit_depth - 1);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      out[static_cast<size_t>(y) * width + x] =
          ((x * 19 + y * 37 + (x ^ y) * 11) % (2 * half - 1)) - (half - 1);
    }
  }
  return out;
}

std::vector<kdu_byte> EncodeIrreversibleCodestream(
    int width, int height, int origin_x, int xform_flags,
    const std::vector<std::string>& cod_params,
    const std::vector<int32_t>& samples) {
  constexpr int kBitDepth = 10;
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  cs.siz.set(Sorigin, 0, 0, 0);
  cs.siz.set(Sorigin, 0, 1, origin_x);
  cs.siz.set(Ssize, 0, 0, height);
  cs.siz.set(Ssize, 0, 1, origin_x + width);
  cs.siz.set(Sdims, 0, 0, height);
  cs.siz.set(Sdims, 0, 1, width);
  cs.siz.set(Ssampling, 0, 0, 1);
  cs.siz.set(Ssampling, 0, 1, 1);
  cs.siz.set(Sprecision, 0, 0, kBitDepth);
  cs.siz.set(Ssigned, 0, 0, true);
  static_cast<kdu_params*>(&cs.siz)->finalize();
  cs.codestream.create(&cs.siz, &target);

  siz_params* params = cs.codestream.access_siz();
  for (const std::string& p : cod_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();

  const double inv_scale = std::ldexp(1.0, -kBitDepth);
  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_multi_analysis engine;
    engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
    for (int y = 0; y < height; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr, nullptr);
      EXPECT_NE(line, nullptr);
      const int32_t* row = samples.data() + static_cast<size_t>(y) * width;
      if (kdu_sample32* dst32 = line->get_buf32()) {
        for (int x = 0; x < width; ++x) {
          dst32[x].fval = static_cast<float>(row[x] * inv_scale);
        }
      } else if (kdu_sample16* dst16 = line->get_buf16()) {
        for (int x = 0; x < width; ++x) {
          dst16[x].ival = static_cast<kdu_int16>(std::llround(
              row[x] * std::ldexp(1.0, KDU_FIX_POINT - kBitDepth)));
        }
      }
      engine.exchange_line(0, line, nullptr);
    }
    engine.destroy();
  }
  kdu_long limit = 0;
  cs.codestream.flush(&limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

std::vector<int32_t> DecodeIrreversibleCodestream(
    const std::vector<kdu_byte>& bitstream, int xform_flags) {
  constexpr int kBitDepth = 10;
  VectorSource src(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&src);
  kdu_dims dims;
  cs.codestream.get_dims(0, dims);
  std::vector<int32_t> out(static_cast<size_t>(dims.size.x) * dims.size.y);
  const double scale = std::ldexp(1.0, kBitDepth);
  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_multi_synthesis engine;
    engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
    for (int y = 0; y < dims.size.y; ++y) {
      kdu_line_buf* line = engine.get_line(0, nullptr);
      EXPECT_NE(line, nullptr);
      int32_t* dst = out.data() + static_cast<size_t>(y) * dims.size.x;
      if (const kdu_sample32* src32 = line->get_buf32()) {
        for (int x = 0; x < dims.size.x; ++x) {
          dst[x] = static_cast<int32_t>(std::llround(src32[x].fval * scale));
        }
      } else if (const kdu_sample16* src16 = line->get_buf16()) {
        for (int x = 0; x < dims.size.x; ++x) {
          dst[x] = static_cast<int32_t>(
              std::llround(static_cast<double>(src16[x].ival) * scale /
                           (1 << KDU_FIX_POINT)));
        }
      }
    }
    engine.destroy();
  }
  cs.codestream.destroy();
  return out;
}

TEST(AvxCoderLocalTest, Irreversible16And32BitMultiSynthesisFallback) {
  for (int origin_x : {0, 3, 7}) {
    const int width = 96;
    const int height = 24;
    const std::vector<int32_t> input = MakeInputSamples(width, height, 10);
    const std::vector<kdu_byte> bs = EncodeIrreversibleCodestream(
        width, height, origin_x, KDU_MULTI_XFORM_PRECISE,
        {"Creversible=no", "Ckernels=W9X7", "Clevels=1", "Cblk={8,32}",
         "Qstep=0.0005"},
        input);
    ASSERT_FALSE(bs.empty());

    for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
      const std::vector<int32_t> decoded =
          DecodeIrreversibleCodestream(bs, flags);
      ASSERT_EQ(decoded.size(), input.size());
      int max_err = 0;
      for (size_t i = 0; i < input.size(); ++i) {
        max_err = std::max(max_err, std::abs(decoded[i] - input[i]));
      }
      EXPECT_LE(max_err, 6) << "origin_x=" << origin_x << " flags=" << flags;
    }
  }
}

TEST(AvxCoderLocalTest, DirectSubbandIrreversibleDecoderAlignedAndUnaligned) {
  for (int width : {16, 37, 48, 96}) {
    for (int origin_x : {0, 5}) {
      const int height = 16;
      const std::vector<int32_t> input = MakeInputSamples(width, height, 10);
      const std::vector<kdu_byte> bs = EncodeIrreversibleCodestream(
          width, height, origin_x, KDU_MULTI_XFORM_PRECISE,
          {"Creversible=no", "Clevels=0", "Cblk={8,32}", "Qstep=0.001"}, input);
      ASSERT_FALSE(bs.empty());

      for (bool use_shorts : {true, false}) {
        VectorSource src(bs);
        ScopedCodestream cs;
        cs.codestream.create(&src);
        ScopedTile tile;
        tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
        kdu_subband band =
            tile.tile.access_component(0).access_resolution().access_subband(
                LL_BAND);

        kdu_sample_allocator alloc;
        kdu_push_pull_params params;
        params.set_opt_flags(KDU_BLOCK_CODING_ACCELERATE);
        kdu_decoder decoder(band, &alloc, params,
                            use_shorts ? KDU_SAMPLE_PREC16 : KDU_SAMPLE_PREC32);
        kdu_line_buf line;
        line.pre_create(&alloc, width, use_shorts ? KDU_LINE_BUF_SHORTS : 0, 0,
                        0);
        alloc.finalize(cs.codestream);
        line.create();
        decoder.start(nullptr);

        const double scale = std::ldexp(1.0, 10);
        int max_err = 0;
        for (int y = 0; y < height; ++y) {
          decoder.pull(line, nullptr);
          const int32_t* ref = input.data() + static_cast<size_t>(y) * width;
          if (use_shorts) {
            const kdu_sample16* p = line.get_buf16();
            ASSERT_NE(p, nullptr);
            for (int x = 0; x < width; ++x) {
              const int32_t val = static_cast<int32_t>(
                  std::llround(static_cast<double>(p[x].ival) * scale /
                               (1 << KDU_FIX_POINT)));
              max_err = std::max(max_err, std::abs(val - ref[x]));
            }
          } else {
            const kdu_sample32* p = line.get_buf32();
            ASSERT_NE(p, nullptr);
            for (int x = 0; x < width; ++x) {
              const int32_t val =
                  static_cast<int32_t>(std::llround(p[x].fval * scale));
              max_err = std::max(max_err, std::abs(val - ref[x]));
            }
          }
        }
        decoder.destroy();
        line.destroy();
        EXPECT_LE(max_err, 6);
      }
    }
  }
}

}  // namespace
}  // namespace kdu_core
