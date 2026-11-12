// Unit tests for
// third_party/kakadu/vendor/coresys/transform/avx2_dwt_local.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES` with `-DKDU_NO_AVX2` on x86_64),
// `avx2_dwt_local.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on both
// x86_64 and `--cpu=arm`). This suite verifies that `kdu_analysis` and
// `kdu_synthesis` 16-bit and 32-bit `W5X3` and `W9X7` DWT paths across varied
// subband origins and wide odd/even scanlines fall back cleanly and
// accurately.

#include <algorithm>
#include <array>
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

class Avx2DwtLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kAvx2DwtLocalEnv =
    ::testing::AddGlobalTestEnvironment(new Avx2DwtLocalTestEnvironment());

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

std::vector<int> RoundTripDwt(int width, int height, int origin_x, int origin_y,
                              bool reversible, bool precise,
                              const std::vector<std::string>& extra_params,
                              const std::vector<int>& input) {
  constexpr int kBitDepth = 8;
  constexpr int kDc = 1 << (kBitDepth - 1);
  constexpr double kScale = static_cast<double>(1 << kBitDepth);

  std::vector<kdu_byte> bitstream;
  {
    VectorTarget target(bitstream);
    ScopedCodestream enc;
    EXPECT_TRUE(enc.siz.parse_string("Scomponents=1"));
    enc.siz.set(Sorigin, 0, 0, origin_y);
    enc.siz.set(Sorigin, 0, 1, origin_x);
    enc.siz.set(Ssize, 0, 0, origin_y + height);
    enc.siz.set(Ssize, 0, 1, origin_x + width);
    enc.siz.set(Sdims, 0, 0, height);
    enc.siz.set(Sdims, 0, 1, width);
    enc.siz.set(Ssampling, 0, 0, 1);
    enc.siz.set(Ssampling, 0, 1, 1);
    enc.siz.set(Sprecision, 0, 0, kBitDepth);
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
    kdu_resolution res = tc.access_resolution();

    const kdu_byte sample_prec = precise ? 0 : KDU_LINE_BUF_SHORTS;
    const kdu_byte line_flags =
        sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
    kdu_sample_allocator alloc;
    kdu_push_pull_params pp_params;
    ScopedAnalysis engine;
    engine.engine = kdu_analysis(res, &alloc, pp_params, sample_prec);
    ScopedLineBuf line;
    line.line.pre_create(&alloc, width, line_flags, 0, 0);
    alloc.finalize(enc.codestream);
    line.line.create();

    for (int y = 0; y < height; ++y) {
      if (precise && !reversible) {
        kdu_sample32* dst = line.line.get_buf32();
        for (int x = 0; x < width; ++x) {
          dst[x].fval = static_cast<float>(
                            input[static_cast<size_t>(y) * width + x] - kDc) /
                        static_cast<float>(kScale);
        }
      } else if (precise) {
        kdu_sample32* dst = line.line.get_buf32();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = input[static_cast<size_t>(y) * width + x] - kDc;
        }
      } else if (reversible) {
        kdu_sample16* dst = line.line.get_buf16();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = static_cast<kdu_int16>(
              input[static_cast<size_t>(y) * width + x] - kDc);
        }
      } else {
        kdu_sample16* dst = line.line.get_buf16();
        for (int x = 0; x < width; ++x) {
          dst[x].ival = static_cast<kdu_int16>(std::llround(
              static_cast<double>(input[static_cast<size_t>(y) * width + x] -
                                  kDc) *
              (1 << KDU_FIX_POINT) / kScale));
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
  kdu_resolution res = tc.access_resolution();

  const kdu_byte sample_prec = precise ? 0 : KDU_LINE_BUF_SHORTS;
  const kdu_byte line_flags =
      sample_prec | (reversible ? KDU_LINE_BUF_ABSOLUTE : 0);
  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  ScopedSynthesis syn;
  syn.engine = kdu_synthesis(res, &alloc, pp_params, sample_prec);
  ScopedLineBuf line;
  line.line.pre_create(&alloc, width, line_flags, 0, 0);
  alloc.finalize(dec.codestream);
  line.line.create();

  std::vector<int> out(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    syn.engine.pull(line.line, nullptr);
    if (precise && !reversible) {
      const kdu_sample32* p = line.line.get_buf32();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] =
            static_cast<int>(
                std::llround(static_cast<double>(p[x].fval) * kScale)) +
            kDc;
      }
    } else if (precise) {
      const kdu_sample32* p = line.line.get_buf32();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] = p[x].ival + kDc;
      }
    } else if (reversible) {
      const kdu_sample16* p = line.line.get_buf16();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] = p[x].ival + kDc;
      }
    } else {
      const kdu_sample16* p = line.line.get_buf16();
      for (int x = 0; x < width; ++x) {
        out[static_cast<size_t>(y) * width + x] =
            static_cast<int>(std::llround(static_cast<double>(p[x].ival) *
                                          kScale / (1 << KDU_FIX_POINT))) +
            kDc;
      }
    }
  }
  syn.engine.destroy();
  tile.tile.close();
  dec.codestream.destroy();
  return out;
}

std::vector<int> MakePattern(int width, int height) {
  std::vector<int> data(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      data[static_cast<size_t>(y) * width + x] =
          128 + static_cast<int>(85.0 * std::sin(0.11 * x + 0.07 * y));
    }
  }
  return data;
}

TEST(Avx2DwtLocalTest, W5x3ReversibleFallbackAcrossOriginsAndPrecisions) {
  constexpr std::array<int, 4> kOrigins = {0, 1, 2, 3};
  const std::vector<int> even_input = MakePattern(96, 40);
  for (int ox : kOrigins) {
    for (bool precise : {false, true}) {
      EXPECT_EQ(RoundTripDwt(96, 40, ox, ox & 1, /*reversible=*/true, precise,
                             {"Creversible=yes", "Clevels=2", "Ckernels=W5X3"},
                             even_input),
                even_input)
          << "ox=" << ox << " precise=" << precise;
    }
  }
  const std::vector<int> odd_input = MakePattern(97, 41);
  EXPECT_EQ(RoundTripDwt(97, 41, 3, 2, /*reversible=*/true, /*precise=*/false,
                         {"Creversible=yes", "Clevels=2", "Ckernels=W5X3"},
                         odd_input),
            odd_input);
}

TEST(Avx2DwtLocalTest, W9x7IrreversibleFallbackAcrossOriginsAndPrecisions) {
  constexpr std::array<int, 4> kOrigins = {0, 1, 2, 3};
  const std::vector<int> even_input = MakePattern(96, 40);
  for (int ox : kOrigins) {
    for (bool precise : {false, true}) {
      const std::vector<int> out = RoundTripDwt(
          96, 40, ox, ox & 1, /*reversible=*/false, precise,
          {"Creversible=no", "Clevels=2", "Ckernels=W9X7", "Qstep=0.0005"},
          even_input);
      for (size_t i = 0; i < even_input.size(); ++i) {
        EXPECT_NEAR(out[i], even_input[i], 3)
            << "ox=" << ox << " precise=" << precise << " i=" << i;
      }
    }
  }
  const std::vector<int> odd_input = MakePattern(97, 41);
  const std::vector<int> odd_out = RoundTripDwt(
      97, 41, 3, 2, /*reversible=*/false, /*precise=*/true,
      {"Creversible=no", "Clevels=2", "Ckernels=W9X7", "Qstep=0.0005"},
      odd_input);
  for (size_t i = 0; i < odd_input.size(); ++i) {
    EXPECT_NEAR(odd_out[i], odd_input[i], 3) << "i=" << i;
  }
}

}  // namespace
}  // namespace kdu_core
