// Unit tests for
// third_party/kakadu/vendor/coresys/transform/neon_analysis2.cpp.
//
// In Google3's active build configuration (`kdu_arch.h` gates
// `KDU_NEON_INTRINSICS` on `#ifdef __ARM_NEON__`), `KD_ANALYSIS2_NEON_EXISTS`
// is undefined at compile time on x86_64 (`0/0` LCOV `DA:` ceiling), and
// `--cpu=arm` builds without `__ARM_NEON__` defined. This suite verifies that
// `kdu_analysis` and `kdu_synthesis` multi-level `W5X3` and `W9X7` round-trips
// across wide 16-bit and 32-bit scanlines work cleanly and accurately.

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

class NeonAnalysis2TestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kNeonAnalysis2Env =
    ::testing::AddGlobalTestEnvironment(new NeonAnalysis2TestEnvironment());

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

std::vector<int> EncodeAndDecode1Comp(
    int width, int height, int bit_depth, bool reversible, bool precise,
    const std::vector<std::string>& extra_params,
    const std::vector<int>& input) {
  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream enc;
    EXPECT_TRUE(enc.siz.parse_string("Scomponents=1"));
    enc.siz.set(Sdims, 0, 0, height);
    enc.siz.set(Sdims, 0, 1, width);
    enc.siz.set(Ssize, 0, 0, height);
    enc.siz.set(Ssize, 0, 1, width);
    enc.siz.set(Ssampling, 0, 0, 1);
    enc.siz.set(Ssampling, 0, 1, 1);
    enc.siz.set(Sprecision, 0, 0, bit_depth);
    enc.siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params*>(&enc.siz)->finalize();

    enc.codestream.create(&enc.siz, &target);
    siz_params* params = enc.codestream.access_siz();
    for (const std::string& opt : extra_params) {
      EXPECT_TRUE(params->parse_string(opt.c_str())) << opt;
    }
    params->finalize_all();

    ScopedTile tile;
    tile.tile = enc.codestream.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile.tile.access_component(0);
    EXPECT_GT(tc.get_num_resolutions(), 1);
    kdu_node top_node =
        tc.access_resolution(tc.get_num_resolutions() - 1).access_node();

    const kdu_byte sample_prec = precise ? 0 : KDU_LINE_BUF_SHORTS;
    const kdu_byte line_flags =
        sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
    kdu_sample_allocator alloc;
    kdu_push_pull_params pp_params;
    ScopedAnalysis engine;
    engine.engine = kdu_analysis(top_node, &alloc, pp_params, sample_prec,
                                 /*normalization=*/1.0F);

    ScopedLineBuf line;
    line.line.pre_create(&alloc, width, line_flags, 0, 0);
    alloc.finalize(enc.codestream);
    line.line.create();
    engine.engine.start(nullptr);
    const int dc = 1 << (bit_depth - 1);
    const double scale = static_cast<double>(1 << bit_depth);
    for (int y = 0; y < height; ++y) {
      if (precise && !reversible) {
        kdu_sample32* dst = line.line.get_buf32();
        for (int x = 0; x < width; ++x) {
          dst[x].fval = static_cast<float>(
                            input[static_cast<size_t>(y) * width + x] - dc) /
                        static_cast<float>(scale);
        }
      } else if (precise) {
        kdu_sample32* dst = line.line.get_buf32();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = input[static_cast<size_t>(y) * width + x] - dc;
        }
      } else if (reversible) {
        kdu_sample16* dst = line.line.get_buf16();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = static_cast<kdu_int16>(
              input[static_cast<size_t>(y) * width + x] - dc);
        }
      } else {
        kdu_sample16* dst = line.line.get_buf16();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = static_cast<kdu_int16>(
              std::llround(static_cast<double>(
                               input[static_cast<size_t>(y) * width + x] - dc) *
                           (1 << KDU_FIX_POINT) / scale));
        }
      }
      while (!engine.engine.push(line.line, nullptr)) {
      }
    }
    while (!engine.engine.finish(nullptr)) {
    }
    engine.engine.destroy();
    tile.tile.close();
    kdu_long max_bytes = 1 << 20;
    enc.codestream.flush(&max_bytes, 1, nullptr, true, true);
    enc.codestream.destroy();
  }

  VectorSource src(bitstream);
  ScopedCodestream dec;
  dec.codestream.create(&src);
  ScopedTile tile;
  tile.tile = dec.codestream.open_tile(kdu_coords(0, 0));
  kdu_tile_comp tc = tile.tile.access_component(0);
  kdu_node top_node =
      tc.access_resolution(tc.get_num_resolutions() - 1).access_node();
  const kdu_byte sample_prec = precise ? 0 : KDU_LINE_BUF_SHORTS;
  const kdu_byte line_flags =
      sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  ScopedSynthesis syn;
  syn.engine = kdu_synthesis(top_node, &alloc, pp_params, sample_prec,
                             /*normalization=*/1.0F);

  ScopedLineBuf line;
  line.line.pre_create(&alloc, width, line_flags, 0, 0);
  alloc.finalize(dec.codestream);
  line.line.create();
  syn.engine.start(nullptr);
  std::vector<int> out(static_cast<size_t>(width) * height);
  const int dc = 1 << (bit_depth - 1);
  const double scale = static_cast<double>(1 << bit_depth);
  for (int y = 0; y < height; ++y) {
    syn.engine.pull(line.line, nullptr);
    if (precise && !reversible) {
      const kdu_sample32* p = line.line.get_buf32();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] =
            static_cast<int>(
                std::llround(static_cast<double>(p[x].fval) * scale)) +
            dc;
      }
    } else if (precise) {
      const kdu_sample32* p = line.line.get_buf32();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] = p[x].ival + dc;
      }
    } else if (reversible) {
      const kdu_sample16* p = line.line.get_buf16();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] = p[x].ival + dc;
      }
    } else {
      const kdu_sample16* p = line.line.get_buf16();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] =
            static_cast<int>(std::llround(static_cast<double>(p[x].ival) *
                                          scale / (1 << KDU_FIX_POINT))) +
            dc;
      }
    }
  }
  syn.engine.destroy();
  tile.tile.close();
  dec.codestream.destroy();
  return out;
}

TEST(NeonAnalysis2Test, Reversible5x3WideScanlinesFallbackRoundTrips) {
  constexpr int kWidth = 96;
  constexpr int kHeight = 40;
  std::vector<int> input(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      input[static_cast<size_t>(y) * kWidth + x] = (x * 7 + y * 13) % 256;
    }
  }
  for (bool precise : {false, true}) {
    const std::vector<int> output = EncodeAndDecode1Comp(
        kWidth, kHeight, 8, /*reversible=*/true, precise,
        {"Creversible=yes", "Clevels=2", "Corder=LRCP"}, input);
    EXPECT_EQ(output, input) << "precise=" << precise;
  }
}

TEST(NeonAnalysis2Test, Irreversible9x7WideScanlinesFallbackRoundTrips) {
  constexpr int kWidth = 96;
  constexpr int kHeight = 40;
  std::vector<int> input(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      input[static_cast<size_t>(y) * kWidth + x] =
          128 + static_cast<int>(90.0 * std::sin(0.12 * x + 0.08 * y));
    }
  }
  for (bool precise : {false, true}) {
    const std::vector<int> output = EncodeAndDecode1Comp(
        kWidth, kHeight, 8, /*reversible=*/false, precise,
        {"Creversible=no", "Clevels=2", "Qstep=0.0005"}, input);
    double max_err = 0.0;
    for (size_t i = 0; i < input.size(); ++i) {
      max_err = std::max(max_err,
                         std::fabs(static_cast<double>(output[i] - input[i])));
    }
    EXPECT_LT(max_err, 3.0) << "precise=" << precise;
  }
}

}  // namespace
}  // namespace kdu_core
