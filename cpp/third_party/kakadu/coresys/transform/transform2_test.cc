// Unit tests for third_party/kakadu/vendor/coresys/transform/transform2.cpp.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

class Transform2TestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kTransform2Env =
    ::testing::AddGlobalTestEnvironment(new Transform2TestEnvironment());

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
  ScopedAnalysis(kdu_resolution res, kdu_sample_allocator* allocator,
                 kdu_push_pull_params& params, kdu_byte sample_prec)
      : engine(res, allocator, params, sample_prec) {}
  kdu_analysis engine;
  ~ScopedAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedSynthesis {
  ScopedSynthesis(kdu_resolution res, kdu_sample_allocator* allocator,
                  kdu_push_pull_params& params, kdu_byte sample_prec)
      : engine(res, allocator, params, sample_prec) {}
  kdu_synthesis engine;
  ~ScopedSynthesis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

template <typename FillLineFn, typename ReadLineFn>
void RunRoundTrip(int width, int height, int precision, bool reversible,
                  const std::vector<std::string>& extra_params,
                  kdu_byte sample_prec, FillLineFn&& fill_line,
                  ReadLineFn&& read_line) {
  const kdu_byte line_flags =
      (sample_prec == KDU_SAMPLE_PREC16 ? KDU_LINE_BUF_SHORTS : 0) |
      (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);

  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream ctx;
    EXPECT_TRUE(ctx.siz.parse_string("Scomponents=1"));
    ctx.siz.set(Sdims, 0, 0, height);
    ctx.siz.set(Sdims, 0, 1, width);
    ctx.siz.set(Ssize, 0, 0, height);
    ctx.siz.set(Ssize, 0, 1, width);
    ctx.siz.set(Ssampling, 0, 0, 1);
    ctx.siz.set(Ssampling, 0, 1, 1);
    ctx.siz.set(Sprecision, 0, 0, precision);
    ctx.siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params*>(&ctx.siz)->finalize();

    ctx.codestream.create(&ctx.siz, &target);
    siz_params* params = ctx.codestream.access_siz();
    for (const std::string& opt : extra_params) {
      EXPECT_TRUE(params->parse_string(opt.c_str())) << opt;
    }
    params->finalize_all();

    {
      kdu_sample_allocator allocator;
      ScopedTile scoped_tile;
      scoped_tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
      kdu_tile_comp tc = scoped_tile.tile.access_component(0);
      kdu_resolution res = tc.access_resolution();

      bool flexible = !reversible;
      const int prec_ana =
          kdu_transform_attributes::get_recommended_working_precision(
              tc, flexible, /*for_analysis=*/true);
      EXPECT_EQ(flexible, !reversible);
      EXPECT_GE(prec_ana, precision);

      kdu_push_pull_params pp_params;
      ScopedAnalysis scoped_ana(res, &allocator, pp_params, sample_prec);

      kdu_line_buf line;
      line.pre_create(&allocator, width, line_flags, 0, 0);
      allocator.finalize(ctx.codestream);
      line.create();

      for (int y = 0; y < height; ++y) {
        fill_line(line, y);
        while (!scoped_ana.engine.push(line)) {
        }
      }
      while (!scoped_ana.engine.finish(nullptr)) {
      }
      scoped_ana.engine.destroy();
      scoped_tile.tile.close();
    }

    kdu_long max_bytes = 1 << 20;
    ctx.codestream.flush(&max_bytes, 1);
    ctx.codestream.destroy();
  }

  {
    VectorSource source(bitstream);
    ScopedCodestream ctx;
    ctx.codestream.create(&source);
    kdu_sample_allocator allocator;
    ScopedTile scoped_tile;
    scoped_tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = scoped_tile.tile.access_component(0);
    kdu_resolution res = tc.access_resolution();

    bool flexible = !reversible;
    const int prec_syn =
        kdu_transform_attributes::get_recommended_working_precision(
            tc, flexible, /*for_analysis=*/false);
    EXPECT_EQ(flexible, !reversible);
    EXPECT_GE(prec_syn, precision);

    kdu_push_pull_params pp_params;
    ScopedSynthesis scoped_syn(res, &allocator, pp_params, sample_prec);

    kdu_line_buf line;
    line.pre_create(&allocator, width, line_flags, 0, 0);
    allocator.finalize(ctx.codestream);
    line.create();

    for (int y = 0; y < height; ++y) {
      scoped_syn.engine.pull(line);
      read_line(line, y);
    }
    scoped_syn.engine.destroy();
    scoped_tile.tile.close();
    ctx.codestream.destroy();
  }
}

std::vector<int> EncodeAndDecodeReversible(
    int width, int height, int precision,
    const std::vector<std::string>& extra_params, kdu_byte sample_prec,
    const std::vector<int>& input) {
  std::vector<int> output(static_cast<size_t>(width * height));
  RunRoundTrip(
      width, height, precision, /*reversible=*/true, extra_params, sample_prec,
      [&](kdu_line_buf& line, int y) {
        if (sample_prec == KDU_SAMPLE_PREC16) {
          kdu_sample16* buf = line.get_buf16();
          for (int x = 0; x < width; ++x) {
            buf[x].ival = static_cast<kdu_int16>(input[y * width + x]);
          }
        } else {
          kdu_sample32* buf = line.get_buf32();
          for (int x = 0; x < width; ++x) {
            buf[x].ival = input[y * width + x];
          }
        }
      },
      [&](kdu_line_buf& line, int y) {
        if (sample_prec == KDU_SAMPLE_PREC16) {
          const kdu_sample16* buf = line.get_buf16();
          for (int x = 0; x < width; ++x) {
            output[y * width + x] = buf[x].ival;
          }
        } else {
          const kdu_sample32* buf = line.get_buf32();
          for (int x = 0; x < width; ++x) {
            output[y * width + x] = buf[x].ival;
          }
        }
      });
  return output;
}

std::vector<float> EncodeAndDecodeIrreversible(
    int width, int height, const std::vector<std::string>& extra_params,
    kdu_byte sample_prec, const std::vector<float>& input) {
  std::vector<float> output(static_cast<size_t>(width * height));
  RunRoundTrip(
      width, height, /*precision=*/8, /*reversible=*/false, extra_params,
      sample_prec,
      [&](kdu_line_buf& line, int y) {
        if (sample_prec == KDU_SAMPLE_PREC16) {
          kdu_sample16* buf = line.get_buf16();
          for (int x = 0; x < width; ++x) {
            buf[x].ival = static_cast<kdu_int16>(
                std::lround(input[y * width + x] * (1 << KDU_FIX_POINT)));
          }
        } else {
          kdu_sample32* buf = line.get_buf32();
          for (int x = 0; x < width; ++x) {
            buf[x].fval = input[y * width + x];
          }
        }
      },
      [&](kdu_line_buf& line, int y) {
        if (sample_prec == KDU_SAMPLE_PREC16) {
          const kdu_sample16* buf = line.get_buf16();
          for (int x = 0; x < width; ++x) {
            output[y * width + x] =
                static_cast<float>(buf[x].ival) / (1 << KDU_FIX_POINT);
          }
        } else {
          const kdu_sample32* buf = line.get_buf32();
          for (int x = 0; x < width; ++x) {
            output[y * width + x] = buf[x].fval;
          }
        }
      });
  return output;
}

TEST(Transform2Test, W5X3LosslessRoundTrip16And32Bit) {
  for (const auto& [w, h] :
       std::vector<std::pair<int, int>>{{32, 32}, {33, 31}}) {
    std::vector<int> input(static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        input[y * w + x] = ((y * 19 + x * 37) % 256) - 128;
      }
    }

    const std::vector<std::string> opts = {"Clevels=2", "Rlevels=2",
                                           "Ckernels=W5X3"};
    const std::vector<int> out16 =
        EncodeAndDecodeReversible(w, h, 8, opts, KDU_SAMPLE_PREC16, input);
    EXPECT_EQ(out16, input);

    const std::vector<int> out32 =
        EncodeAndDecodeReversible(w, h, 8, opts, KDU_SAMPLE_PREC32, input);
    EXPECT_EQ(out32, input);
  }
}

TEST(Transform2Test, R1X1FallbackAndW9X7IrreversibleRoundTrips) {
  constexpr int kW = 24;
  constexpr int kH = 24;
  std::vector<int> int_input(kW * kH);
  std::vector<float> flt_input(kW * kH);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int v = ((y * 23 + x * 11) % 200) - 100;
      int_input[y * kW + x] = v;
      flt_input[y * kW + x] = static_cast<float>(v) / 256.0F;
    }
  }

  // Non-W5X3/W9X7 ATK R1X1 exercises unsupported check_support() fallback.
  const std::vector<int> r1x1_out = EncodeAndDecodeReversible(
      kW, kH, 8,
      {"Clevels=2", "Rlevels=2", "Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
      KDU_SAMPLE_PREC16, int_input);
  EXPECT_EQ(r1x1_out, int_input);

  // Irreversible W9X7 across 16-bit fixed-point and 32-bit floating-point.
  const std::vector<std::string> w9x7_opts = {
      "Clevels=2", "Rlevels=2", "Ckernels=W9X7", "Creversible=no"};
  const std::vector<float> w9x7_out16 = EncodeAndDecodeIrreversible(
      kW, kH, w9x7_opts, KDU_SAMPLE_PREC16, flt_input);
  const std::vector<float> w9x7_out32 = EncodeAndDecodeIrreversible(
      kW, kH, w9x7_opts, KDU_SAMPLE_PREC32, flt_input);
  for (int i = 0; i < kW * kH; ++i) {
    EXPECT_NEAR(w9x7_out16[i], flt_input[i], 0.05F);
    EXPECT_NEAR(w9x7_out32[i], flt_input[i], 0.05F);
  }
}

}  // namespace
}  // namespace kdu_core
