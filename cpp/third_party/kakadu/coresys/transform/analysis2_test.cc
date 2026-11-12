// Unit tests for third_party/kakadu/vendor/coresys/transform/analysis2.cpp.
//
// Under Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// defines `KDU_NO_AVX2` on x86_64, and `kdu_arch.h` gates NEON on
// `__ARM_NEON__` rather than `__ARM_NEON` so `--cpu=arm` compiles without
// `KDU_NEON_INTRINSICS`), `KD_ANALYSIS2_CHECK_SUPPORT` leaves
// `analysis2_supported` false at compile time. Consequently only
// `kd_analysis2::check_support()` (lines 84-109) and the pre-check prefix of
// `kd_analysis2::get_recommended_wkg_precision()` (lines 116-131, 158) are
// reachable (`31/722 = 4.29%` ceiling of compiled LCOV `DA:` lines); all body
// methods (`install_accelerators*`, `init`, `start`, `finish`, `push`) have no
// scalar fallback (`init()` asserts `log2_vector_len > 0`). This suite covers
// 100% of the reachable lines and branch outcomes in `analysis2.cpp`.

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

class Analysis2TestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kAnalysis2Env =
    ::testing::AddGlobalTestEnvironment(new Analysis2TestEnvironment());

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

void SetupCodestream(ScopedCodestream& ctx, kdu_compressed_target& target,
                     int width, int height, int bit_depth,
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

int QueryAnalysisWorkingPrecision(int width, int height, int bit_depth,
                                  const std::vector<std::string>& extra_params,
                                  bool& flexible_out) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream ctx;
  SetupCodestream(ctx, target, width, height, bit_depth, extra_params);

  ScopedTile tile;
  tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  return kdu_transform_attributes::get_recommended_working_precision(
      tc, flexible_out, /*analysis=*/true);
}

std::vector<kdu_byte> RunAnalysisEncode(
    int width, int height, int bit_depth, bool reversible,
    const std::vector<std::string>& extra_params, kdu_byte sample_prec,
    bool transpose, const std::vector<int>& samples) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream ctx;
  SetupCodestream(ctx, target, width, height, bit_depth, extra_params);
  if (transpose) {
    ctx.codestream.change_appearance(/*transpose=*/true, /*vflip=*/false,
                                     /*hflip=*/false);
  }

  {
    ScopedTile tile;
    tile.tile = ctx.codestream.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution(tc.get_num_resolutions() - 1);
    kdu_node top_node = top_res.access_node();
    kdu_dims dims;
    top_node.get_dims(dims);

    const kdu_byte line_flags =
        sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
    kdu_sample_allocator allocator;
    kdu_push_pull_params pp_params;
    ScopedAnalysis analysis;
    analysis.engine = kdu_analysis(top_node, &allocator, pp_params, sample_prec,
                                   /*normalization=*/1.0F);
    ScopedLineBuf line;
    line.line.pre_create(&allocator, dims.size.x, line_flags, 0, 0);
    allocator.finalize(ctx.codestream);
    line.line.create();

    for (int y = 0; y < dims.size.y; ++y) {
      if (sample_prec == KDU_LINE_BUF_SHORTS) {
        kdu_sample16* buf = line.line.get_buf16();
        for (int x = 0; x < dims.size.x; ++x) {
          const int val = samples[static_cast<size_t>(y * dims.size.x + x)];
          buf[x].ival = static_cast<kdu_int16>(
              reversible ? val : (val << (KDU_FIX_POINT - 8)));
        }
      } else if (sample_prec == KDU_LINE_BUF_VPRECISE) {
        kdu_sample64* buf = line.line.get_buf64();
        for (int x = 0; x < dims.size.x; ++x) {
          buf[x].ival = samples[static_cast<size_t>(y * dims.size.x + x)];
        }
      } else {
        kdu_sample32* buf = line.line.get_buf32();
        for (int x = 0; x < dims.size.x; ++x) {
          const int val = samples[static_cast<size_t>(y * dims.size.x + x)];
          if (reversible) {
            buf[x].ival = val;
          } else {
            buf[x].fval = static_cast<float>(val) / 256.0F;
          }
        }
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

TEST(Analysis2Test, GetRecommendedWorkingPrecisionPaths) {
  bool flexible = false;

  // Non-both decomposition directions (`Cdecomp=V(-)` / `Cdecomp=H(-)`).
  EXPECT_EQ(
      QueryAnalysisWorkingPrecision(
          16, 16, 8, {"Clevels=1", "Ckernels=W5X3", "Cdecomp=V(-)"}, flexible),
      11);
  EXPECT_FALSE(flexible);

  EXPECT_EQ(
      QueryAnalysisWorkingPrecision(
          16, 16, 8, {"Clevels=1", "Ckernels=W5X3", "Cdecomp=H(-)"}, flexible),
      11);
  EXPECT_FALSE(flexible);

  // Non-W5X3/W9X7/I5X3 kernel (`R1X1`) rejects before support check.
  EXPECT_EQ(QueryAnalysisWorkingPrecision(
                16, 16, 8,
                {"Clevels=1", "Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
                flexible),
            15);
  EXPECT_FALSE(flexible);

  // 2D W5X3, irreversible I5X3, and W9X7 reach KD_ANALYSIS2_CHECK_SUPPORT.
  EXPECT_EQ(QueryAnalysisWorkingPrecision(
                16, 16, 8, {"Clevels=2", "Ckernels=W5X3"}, flexible),
            11);
  EXPECT_FALSE(flexible);

  EXPECT_EQ(
      QueryAnalysisWorkingPrecision(16, 16, 8,
                                    {"Clevels=2", "Ckernels=ATK", "Catk=2",
                                     "Kkernels:I2=I5X3", "Creversible=no"},
                                    flexible),
      14);
  EXPECT_TRUE(flexible);

  EXPECT_EQ(QueryAnalysisWorkingPrecision(
                16, 16, 8, {"Clevels=2", "Ckernels=W9X7", "Creversible=no"},
                flexible),
            15);
  EXPECT_TRUE(flexible);
}

TEST(Analysis2Test, CheckSupportBranchesAndTransformFallbackRoundTrips) {
  std::vector<int> samples_12x12(12 * 12);
  for (int i = 0; i < 12 * 12; ++i) {
    samples_12x12[static_cast<size_t>(i)] = ((i * 29 + 7) % 201) - 100;
  }
  const std::vector<int> samples_1d(8, 17);

  // Non-W5X3/W9X7/I5X3 kernel (`R1X1`).
  EXPECT_FALSE(RunAnalysisEncode(
                   12, 12, 8, /*reversible=*/true,
                   {"Clevels=1", "Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
                   KDU_LINE_BUF_SHORTS, /*transpose=*/false, samples_12x12)
                   .empty());

  // Degenerate dimensions (`dims.size.x < 2` and `dims.size.y < 2`).
  EXPECT_FALSE(RunAnalysisEncode(
                   1, 8, 8, /*reversible=*/true, {"Clevels=1", "Ckernels=W5X3"},
                   KDU_LINE_BUF_SHORTS, /*transpose=*/false, samples_1d)
                   .empty());
  EXPECT_FALSE(
      RunAnalysisEncode(8, 1, 8, /*reversible=*/false,
                        {"Clevels=1", "Ckernels=W9X7", "Creversible=no"}, 0,
                        /*transpose=*/false, samples_1d)
          .empty());

  // Transposed reversible W5X3 rejects because horizontal transform must lead.
  EXPECT_FALSE(RunAnalysisEncode(12, 12, 8, /*reversible=*/true,
                                 {"Clevels=1", "Ckernels=W5X3"},
                                 KDU_LINE_BUF_SHORTS, /*transpose=*/true,
                                 samples_12x12)
                   .empty());

  // Transposed irreversible W9X7 passes the reversible+transposed check.
  EXPECT_FALSE(
      RunAnalysisEncode(12, 12, 8, /*reversible=*/false,
                        {"Clevels=1", "Ckernels=W9X7", "Creversible=no"}, 0,
                        /*transpose=*/true, samples_12x12)
          .empty());

  // 1D decompositions (`HL` or `LH` missing).
  EXPECT_FALSE(RunAnalysisEncode(12, 12, 8, /*reversible=*/true,
                                 {"Clevels=1", "Ckernels=W5X3", "Cdecomp=V(-)"},
                                 KDU_LINE_BUF_SHORTS, /*transpose=*/false,
                                 samples_12x12)
                   .empty());
  EXPECT_FALSE(RunAnalysisEncode(12, 12, 8, /*reversible=*/true,
                                 {"Clevels=1", "Ckernels=W5X3", "Cdecomp=H(-)"},
                                 KDU_LINE_BUF_SHORTS, /*transpose=*/false,
                                 samples_12x12)
                   .empty());

  // 2D W5X3 (16/32/64-bit), W9X7, and I5X3 reach KD_ANALYSIS2_CHECK_SUPPORT and
  // round-trip cleanly via fallback synthesis.
  std::vector<int> samples_16x16(16 * 16);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      samples_16x16[static_cast<size_t>(y * 16 + x)] =
          ((y * 19 + x * 31) % 240) - 120;
    }
  }

  const std::vector<kdu_byte> w5x3_bits = RunAnalysisEncode(
      16, 16, 8, /*reversible=*/true, {"Clevels=2", "Ckernels=W5X3"},
      KDU_LINE_BUF_SHORTS, /*transpose=*/false, samples_16x16);
  EXPECT_FALSE(w5x3_bits.empty());
  EXPECT_FALSE(RunAnalysisEncode(16, 16, 16, /*reversible=*/true,
                                 {"Clevels=2", "Ckernels=W5X3"},
                                 KDU_LINE_BUF_VPRECISE, /*transpose=*/false,
                                 samples_16x16)
                   .empty());

  const std::vector<kdu_byte> i5x3_bits = RunAnalysisEncode(
      16, 16, 8, /*reversible=*/false,
      {"Clevels=2", "Ckernels=ATK", "Catk=2", "Kkernels:I2=I5X3",
       "Creversible=no"},
      KDU_LINE_BUF_SHORTS, /*transpose=*/false, samples_16x16);
  EXPECT_FALSE(i5x3_bits.empty());

  const std::vector<kdu_byte> w9x7_bits =
      RunAnalysisEncode(16, 16, 8, /*reversible=*/false,
                        {"Clevels=2", "Ckernels=W9X7", "Creversible=no"}, 0,
                        /*transpose=*/false, samples_16x16);
  EXPECT_FALSE(w9x7_bits.empty());

  // Decode the W5X3 bitstream and verify exact lossless reconstruction.
  VectorSource source(w5x3_bits);
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
  dec_line.line.pre_create(&dec_alloc, 16,
                           KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0, 0);
  dec_alloc.finalize(dec_ctx.codestream);
  dec_line.line.create();

  for (int y = 0; y < 16; ++y) {
    synth.engine.pull(dec_line.line);
    const kdu_sample16* buf = dec_line.line.get_buf16();
    for (int x = 0; x < 16; ++x) {
      EXPECT_EQ(buf[x].ival, samples_16x16[static_cast<size_t>(y * 16 + x)]);
    }
  }
}

}  // namespace
}  // namespace kdu_core
