// Unit tests for third_party/kakadu/vendor/coresys/transform/synthesis2.cpp.
//
// Under Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// defines `KDU_NO_AVX2` on x86_64, and `kdu_arch.h` gates NEON on
// `__ARM_NEON__` rather than `__ARM_NEON` so `--cpu=arm` compiles without
// `KDU_NEON_INTRINSICS`), `KD_SYNTHESIS2_CHECK_SUPPORT` leaves
// `synthesis2_supported` false at compile time. Consequently only
// `kd_synthesis2::check_support()` (lines 118-147, 166) and the pre-check
// prefix of `kd_synthesis2::get_recommended_wkg_precision()` (lines 172-191,
// 214) are reachable (`34/813 = 4.18%` ceiling of compiled LCOV `DA:` lines);
// all body methods (`kd_synthesis2`, `~kd_synthesis2`, `install_accelerators*`,
// `init`, `start`, `pull`) have no scalar fallback (`init()` asserts
// `log2_vector_len > 0`). This suite covers 100% of the reachable lines and
// branch outcomes in `synthesis2.cpp`.

#include <algorithm>
#include <cmath>
#include <cstddef>
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
  void put_text(const char* string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class Synthesis2TestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kSynthesis2Env =
    ::testing::AddGlobalTestEnvironment(new Synthesis2TestEnvironment());

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

void SetupEncoderCodestream(ScopedCodestream& ctx,
                            kdu_compressed_target& target, int width,
                            int height, int bit_depth,
                            const std::vector<std::string>& extra_params) {
  ASSERT_TRUE(ctx.siz.parse_string("Scomponents=1"));
  // kdu_dims/params pairs use {height, width} (vertical coordinate first).
  ctx.siz.set(Sdims, 0, 0, height);
  ctx.siz.set(Sdims, 0, 1, width);
  ctx.siz.set(Ssize, 0, 0, height);
  ctx.siz.set(Ssize, 0, 1, width);
  ctx.siz.set(Ssampling, 0, 0, 1);
  ctx.siz.set(Ssampling, 0, 1, 1);
  ctx.siz.set(Sprecision, 0, 0, bit_depth);
  ctx.siz.set(Ssigned, 0, 0, false);
  static_cast<kdu_params*>(&ctx.siz)->finalize();

  ctx.codestream.create(&ctx.siz, &target);
  siz_params* params = ctx.codestream.access_siz();
  for (const std::string& opt : extra_params) {
    ASSERT_TRUE(params->parse_string(opt.c_str())) << opt;
  }
  params->finalize_all();
}

std::vector<kdu_byte> EncodeTestBitstream(
    int width, int height, int bit_depth, bool reversible,
    const std::vector<std::string>& extra_params, const std::vector<int>& in) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream ctx;
  SetupEncoderCodestream(ctx, target, width, height, bit_depth, extra_params);

  {
    ScopedTile tile;
    tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);

    const kdu_byte sample_prec = KDU_SAMPLE_PREC16;
    const kdu_byte line_flags =
        sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
    kdu_sample_allocator allocator;
    kdu_push_pull_params pp_params;
    ScopedAnalysis analysis;
    analysis.engine = kdu_analysis(top_res, &allocator, pp_params, sample_prec);
    ScopedLineBuf line;
    line.line.pre_create(&allocator, width, line_flags, 0, 0);
    allocator.finalize(ctx.codestream);
    line.line.create();

    for (int y = 0; y < height; ++y) {
      kdu_sample16* buf = line.line.get_buf16();
      for (int x = 0; x < width; ++x) {
        const int val = in[static_cast<size_t>(y * width + x)];
        buf[x].ival = static_cast<kdu_int16>(
            reversible ? val : (val << (KDU_FIX_POINT - 8)));
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
  return bitstream;
}

std::vector<int> DecodeWithSynthesis(const std::vector<kdu_byte>& bitstream,
                                     bool reversible, kdu_byte sample_prec,
                                     bool transpose, int out_w, int out_h) {
  VectorSource source(bitstream);
  ScopedCodestream ctx;
  ctx.codestream.create(&source);
  if (transpose) {
    ctx.codestream.change_appearance(/*transpose=*/true, /*vflip=*/false,
                                     /*hflip=*/false);
  }

  ScopedTile tile;
  tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
  kdu_node top_node = top_res.access_node();
  kdu_dims dims;
  top_node.get_dims(dims);
  EXPECT_EQ(dims.size.x, out_w);
  EXPECT_EQ(dims.size.y, out_h);

  const kdu_byte line_flags =
      sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  kdu_sample_allocator allocator;
  kdu_push_pull_params pp_params;
  ScopedSynthesis synth;
  synth.engine = kdu_synthesis(top_node, &allocator, pp_params, sample_prec);
  ScopedLineBuf line;
  line.line.pre_create(&allocator, dims.size.x, line_flags, 0, 0);
  allocator.finalize(ctx.codestream);
  line.line.create();

  std::vector<int> out(static_cast<size_t>(dims.size.x * dims.size.y));
  for (int y = 0; y < dims.size.y; ++y) {
    synth.engine.pull(line.line);
    if (sample_prec == KDU_LINE_BUF_SHORTS) {
      const kdu_sample16* buf = line.line.get_buf16();
      for (int x = 0; x < dims.size.x; ++x) {
        out[static_cast<size_t>(y * dims.size.x + x)] =
            reversible ? buf[x].ival : (buf[x].ival >> (KDU_FIX_POINT - 8));
      }
    } else if (sample_prec == KDU_LINE_BUF_VPRECISE) {
      const kdu_sample64* buf = line.line.get_buf64();
      for (int x = 0; x < dims.size.x; ++x) {
        out[static_cast<size_t>(y * dims.size.x + x)] =
            static_cast<int>(buf[x].ival);
      }
    } else {
      const kdu_sample32* buf = line.line.get_buf32();
      for (int x = 0; x < dims.size.x; ++x) {
        out[static_cast<size_t>(y * dims.size.x + x)] =
            reversible ? buf[x].ival
                       : static_cast<int>(std::lround(buf[x].fval * 256.0F));
      }
    }
  }
  synth.engine.destroy();
  tile.tile.close();
  ctx.codestream.destroy();
  return out;
}

int QuerySynthesisWorkingPrecision(int width, int height, int bit_depth,
                                   const std::vector<std::string>& extra_params,
                                   bool& flexible_out) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream ctx;
  SetupEncoderCodestream(ctx, target, width, height, bit_depth, extra_params);

  ScopedTile tile;
  tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  return kdu_transform_attributes::get_recommended_working_precision(
      tc, flexible_out, /*analysis=*/false);
}

TEST(Synthesis2Test, GetRecommendedWorkingPrecisionPaths) {
  bool flexible = false;

  // Non-both decomposition directions (`Cdecomp=V(-)` / `Cdecomp=H(-)`).
  EXPECT_EQ(
      QuerySynthesisWorkingPrecision(
          16, 16, 8, {"Clevels=1", "Ckernels=W5X3", "Cdecomp=V(-)"}, flexible),
      12);
  EXPECT_FALSE(flexible);

  EXPECT_EQ(
      QuerySynthesisWorkingPrecision(
          16, 16, 8, {"Clevels=1", "Ckernels=W5X3", "Cdecomp=H(-)"}, flexible),
      12);
  EXPECT_FALSE(flexible);

  // Non-W5X3/W9X7/I5X3 kernel (`R1X1`) rejects before support check.
  EXPECT_EQ(QuerySynthesisWorkingPrecision(
                16, 16, 8,
                {"Clevels=1", "Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
                flexible),
            15);
  EXPECT_FALSE(flexible);

  // 2D W5X3, irreversible I5X3, and W9X7 reach KD_SYNTHESIS2_CHECK_SUPPORT.
  EXPECT_EQ(QuerySynthesisWorkingPrecision(
                16, 16, 8, {"Clevels=2", "Ckernels=W5X3"}, flexible),
            12);
  EXPECT_FALSE(flexible);

  EXPECT_EQ(
      QuerySynthesisWorkingPrecision(16, 16, 8,
                                     {"Clevels=2", "Ckernels=ATK", "Catk=2",
                                      "Kkernels:I2=I5X3", "Creversible=no"},
                                     flexible),
      14);
  EXPECT_TRUE(flexible);

  EXPECT_EQ(QuerySynthesisWorkingPrecision(
                16, 16, 8, {"Clevels=2", "Ckernels=W9X7", "Creversible=no"},
                flexible),
            15);
  EXPECT_TRUE(flexible);
}

TEST(Synthesis2Test, CheckSupportBranchesAndTransformFallbackRoundTrips) {
  std::vector<int> samples_12x12(12 * 12);
  for (int y = 0; y < 12; ++y) {
    for (int x = 0; x < 12; ++x) {
      samples_12x12[static_cast<size_t>(y * 12 + x)] =
          ((y * 23 + x * 17) % 201) - 100;
    }
  }
  const std::vector<int> samples_1d = {10, -20, 30, -40, 50, -60, 70, -80};

  // Non-W5X3/W9X7/I5X3 kernel (`R1X1`).
  {
    const std::vector<kdu_byte> bits = EncodeTestBitstream(
        12, 12, 8, /*reversible=*/true,
        {"Clevels=1", "Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
        samples_12x12);
    EXPECT_EQ(
        DecodeWithSynthesis(bits, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/false, 12, 12),
        samples_12x12);
  }

  // Degenerate dimensions (`dims.size.x < 2` and `dims.size.y < 2`).
  {
    const std::vector<kdu_byte> bits_1x8 =
        EncodeTestBitstream(1, 8, 8, /*reversible=*/true,
                            {"Clevels=1", "Ckernels=W5X3"}, samples_1d);
    EXPECT_EQ(
        DecodeWithSynthesis(bits_1x8, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/false, 1, 8),
        samples_1d);

    const std::vector<kdu_byte> bits_8x1 =
        EncodeTestBitstream(8, 1, 8, /*reversible=*/true,
                            {"Clevels=1", "Ckernels=W5X3"}, samples_1d);
    EXPECT_EQ(
        DecodeWithSynthesis(bits_8x1, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/false, 8, 1),
        samples_1d);
  }

  // 2D W5X3: normal and transposed reversible (line 132-135) paths.
  {
    const std::vector<kdu_byte> bits =
        EncodeTestBitstream(12, 12, 8, /*reversible=*/true,
                            {"Clevels=2", "Ckernels=W5X3"}, samples_12x12);
    EXPECT_EQ(
        DecodeWithSynthesis(bits, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/false, 12, 12),
        samples_12x12);
    EXPECT_EQ(
        DecodeWithSynthesis(bits, /*reversible=*/true, KDU_LINE_BUF_VPRECISE,
                            /*transpose=*/false, 12, 12),
        samples_12x12);

    const std::vector<int> transposed =
        DecodeWithSynthesis(bits, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/true, 12, 12);
    for (int y = 0; y < 12; ++y) {
      for (int x = 0; x < 12; ++x) {
        EXPECT_EQ(transposed[static_cast<size_t>(x * 12 + y)],
                  samples_12x12[static_cast<size_t>(y * 12 + x)]);
      }
    }
  }

  // Missing HL or LH child (`Cdecomp=V(-)` / `Cdecomp=H(-)`).
  for (const char* decomp : {"Cdecomp=V(-)", "Cdecomp=H(-)"}) {
    const std::vector<kdu_byte> bits = EncodeTestBitstream(
        12, 12, 8, /*reversible=*/true, {"Clevels=1", "Ckernels=W5X3", decomp},
        samples_12x12);
    EXPECT_EQ(
        DecodeWithSynthesis(bits, /*reversible=*/true, KDU_LINE_BUF_SHORTS,
                            /*transpose=*/false, 12, 12),
        samples_12x12);
  }

  // 2D irreversible W9X7 (including transposed decode) and I5X3.
  {
    const std::vector<kdu_byte> w9x7_bits = EncodeTestBitstream(
        12, 12, 8, /*reversible=*/false,
        {"Clevels=2", "Ckernels=W9X7", "Creversible=no"}, samples_12x12);
    const std::vector<int> out = DecodeWithSynthesis(
        w9x7_bits, /*reversible=*/false, 0, /*transpose=*/false, 12, 12);
    const std::vector<int> out_t = DecodeWithSynthesis(
        w9x7_bits, /*reversible=*/false, 0, /*transpose=*/true, 12, 12);
    for (int y = 0; y < 12; ++y) {
      for (int x = 0; x < 12; ++x) {
        const int ref = samples_12x12[static_cast<size_t>(y * 12 + x)];
        EXPECT_NEAR(out[static_cast<size_t>(y * 12 + x)], ref, 4);
        EXPECT_NEAR(out_t[static_cast<size_t>(x * 12 + y)], ref, 4);
      }
    }

    const std::vector<kdu_byte> i5x3_bits =
        EncodeTestBitstream(12, 12, 8, /*reversible=*/false,
                            {"Clevels=2", "Ckernels=ATK", "Catk=2",
                             "Kkernels:I2=I5X3", "Creversible=no"},
                            samples_12x12);
    const std::vector<int> i5x3_out =
        DecodeWithSynthesis(i5x3_bits, /*reversible=*/false,
                            KDU_LINE_BUF_SHORTS, /*transpose=*/false, 12, 12);
    for (int i = 0; i < 12 * 12; ++i) {
      EXPECT_NEAR(i5x3_out[static_cast<size_t>(i)],
                  samples_12x12[static_cast<size_t>(i)], 4);
    }
  }
}

}  // namespace
}  // namespace kdu_core
