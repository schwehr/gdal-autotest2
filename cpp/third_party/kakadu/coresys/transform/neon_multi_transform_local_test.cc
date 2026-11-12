// Unit tests for
// third_party/kakadu/vendor/coresys/transform/neon_multi_transform_local.cpp.
//
// In Google3's active build configuration (`kdu_arch.h` gates
// `KDU_NEON_INTRINSICS` on `#ifdef __ARM_NEON__`),
// `neon_multi_transform_local.cpp` is compiled out (`0/0` LCOV `DA:` ceiling
// on both x86_64 and `--cpu=arm`). This suite verifies that Part-2 NLT
// (`Sextensions=NLT`, `NLType=SMAG`/`UMAG`) and Part-2 MCT (`Sextensions=MCT`,
// reversible/irreversible `MATRIX` + line copy) codestream round-trips across
// 16-bit default flags, 32-bit `KDU_MULTI_XFORM_PRECISE`, and 64-bit
// `KDU_MULTI_XFORM_VPRECISE` with wide scanlines fall back cleanly and
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
  void put_text(const char* string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class NeonMultiTransformLocalTestEnvironment final
    : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kNeonMultiTransformEnv =
    ::testing::AddGlobalTestEnvironment(
        new NeonMultiTransformLocalTestEnvironment());

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

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;
  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ScopedMultiSynthesis {
  kdu_multi_synthesis engine;
  ~ScopedMultiSynthesis() {
    if (engine.exists()) {
      engine.destroy();
    }
  }
};

struct ComponentData {
  int precision = 8;
  bool is_signed = false;
  std::vector<int32_t> samples;
};

void WriteSampleRow(kdu_line_buf* line, int width, int precision,
                    bool is_signed, const int32_t* row) {
  kdu_sample64* dst64 = line->get_buf64();
  kdu_sample32* dst32 = line->get_buf32();
  kdu_sample16* dst16 = line->get_buf16();
  ASSERT_TRUE(dst64 != nullptr || dst32 != nullptr || dst16 != nullptr);

  const int32_t dc = is_signed ? 0 : (int32_t{1} << (precision - 1));
  const bool is_abs = line->is_absolute();
  if (dst64 != nullptr) {
    for (int x = 0; x < width; ++x) {
      dst64[x].ival = static_cast<kdu_int64>(row[x] - dc);
    }
  } else if (dst32 != nullptr && is_abs) {
    for (int x = 0; x < width; ++x) {
      dst32[x].ival = static_cast<kdu_int32>(row[x] - dc);
    }
  } else if (dst32 != nullptr) {
    const double inv_scale = 1.0 / static_cast<double>(int32_t{1} << precision);
    for (int x = 0; x < width; ++x) {
      dst32[x].fval =
          static_cast<float>(static_cast<double>(row[x] - dc) * inv_scale);
    }
  } else if (is_abs) {
    for (int x = 0; x < width; ++x) {
      dst16[x].ival = static_cast<kdu_int16>(row[x] - dc);
    }
  } else {
    const double scale =
        static_cast<double>(1 << KDU_FIX_POINT) / (int32_t{1} << precision);
    for (int x = 0; x < width; ++x) {
      dst16[x].ival = static_cast<kdu_int16>(
          std::llround(static_cast<double>(row[x] - dc) * scale));
    }
  }
}

void ReadSampleRow(kdu_line_buf* line, int width, int precision, bool is_signed,
                   std::vector<int32_t>& out) {
  const kdu_sample64* src64 = line->get_buf64();
  const kdu_sample32* src32 = line->get_buf32();
  const kdu_sample16* src16 = line->get_buf16();
  ASSERT_TRUE(src64 != nullptr || src32 != nullptr || src16 != nullptr);

  const int32_t dc = is_signed ? 0 : (int32_t{1} << (precision - 1));
  const bool is_abs = line->is_absolute();
  if (src64 != nullptr) {
    for (int x = 0; x < width; ++x) {
      out.push_back(static_cast<int32_t>(src64[x].ival) + dc);
    }
  } else if (src32 != nullptr && is_abs) {
    for (int x = 0; x < width; ++x) {
      out.push_back(static_cast<int32_t>(src32[x].ival) + dc);
    }
  } else if (src32 != nullptr) {
    const double scale = static_cast<double>(int32_t{1} << precision);
    for (int x = 0; x < width; ++x) {
      out.push_back(static_cast<int32_t>(std::llround(
                        static_cast<double>(src32[x].fval) * scale)) +
                    dc);
    }
  } else if (is_abs) {
    for (int x = 0; x < width; ++x) {
      out.push_back(static_cast<int32_t>(src16[x].ival) + dc);
    }
  } else {
    const double scale =
        static_cast<double>(int32_t{1} << precision) / (1 << KDU_FIX_POINT);
    for (int x = 0; x < width; ++x) {
      out.push_back(static_cast<int32_t>(std::llround(
                        static_cast<double>(src16[x].ival) * scale)) +
                    dc);
    }
  }
}

std::vector<kdu_byte> EncodeMulti(int width, int height, bool reversible,
                                  int xform_flags,
                                  const std::vector<ComponentData>& components,
                                  const std::vector<std::string>& siz_ext,
                                  const std::vector<std::string>& cod_ext) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  const int num_comps = static_cast<int>(components.size());
  EXPECT_TRUE(cs.siz.parse_string(
      ("Scomponents=" + std::to_string(num_comps)).c_str()));
  cs.siz.set(Sorigin, 0, 0, 0);
  cs.siz.set(Sorigin, 0, 1, 0);
  cs.siz.set(Ssize, 0, 0, height);
  cs.siz.set(Ssize, 0, 1, width);
  for (int c = 0; c < num_comps; ++c) {
    cs.siz.set(Sdims, c, 0, height);
    cs.siz.set(Sdims, c, 1, width);
    cs.siz.set(Ssampling, c, 0, 1);
    cs.siz.set(Ssampling, c, 1, 1);
    cs.siz.set(Sprecision, c, 0, components[c].precision);
    cs.siz.set(Ssigned, c, 0, components[c].is_signed);
  }
  for (const std::string& p : siz_ext) {
    EXPECT_TRUE(cs.siz.parse_string(p.c_str())) << p;
  }
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(
      params->parse_string(reversible ? "Creversible=yes" : "Creversible=no"));
  EXPECT_TRUE(params->parse_string("Cycc=no"));
  EXPECT_TRUE(params->parse_string("Clevels=1"));
  for (const std::string& p : cod_ext) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  ScopedMultiAnalysis ana;
  ana.engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
  std::vector<int> rows_done(num_comps, 0);
  bool any_left = true;
  while (any_left) {
    any_left = false;
    for (int c = 0; c < num_comps; ++c) {
      if (rows_done[c] >= height) {
        continue;
      }
      any_left = true;
      kdu_line_buf* line = ana.engine.exchange_line(c, nullptr);
      if (line != nullptr) {
        const int32_t* row = components[c].samples.data() +
                             static_cast<size_t>(rows_done[c]) * width;
        WriteSampleRow(line, width, components[c].precision,
                       components[c].is_signed, row);
        ana.engine.exchange_line(c, line);
        rows_done[c]++;
      }
    }
  }
  ana.engine.destroy();
  tile.tile.close();
  kdu_long max_bytes = 1 << 20;
  cs.codestream.flush(&max_bytes, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

std::vector<std::vector<int32_t>> DecodeMulti(
    const std::vector<kdu_byte>& bitstream, int xform_flags) {
  VectorSource source(bitstream);
  ScopedCodestream cs;
  cs.codestream.create(&source);
  const int num_comps = cs.codestream.get_num_components(true);
  std::vector<int> precisions(num_comps);
  std::vector<bool> is_signed(num_comps);
  std::vector<int> widths(num_comps);
  std::vector<int> heights(num_comps);
  std::vector<std::vector<int32_t>> output(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    kdu_dims dims;
    cs.codestream.get_dims(c, dims, true);
    widths[c] = dims.size.x;
    heights[c] = dims.size.y;
    precisions[c] = cs.codestream.get_bit_depth(c, true, false);
    is_signed[c] = cs.codestream.get_signed(c, true);
    output[c].reserve(static_cast<size_t>(widths[c]) * heights[c]);
  }

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  ScopedMultiSynthesis syn;
  syn.engine.create(cs.codestream, tile.tile, nullptr, nullptr, xform_flags);
  std::vector<int> rows_left(heights);
  bool any_left = true;
  while (any_left) {
    any_left = false;
    for (int c = 0; c < num_comps; ++c) {
      if (rows_left[c] <= 0) {
        continue;
      }
      any_left = true;
      kdu_line_buf* line = syn.engine.get_line(c);
      if (line != nullptr) {
        ReadSampleRow(line, widths[c], precisions[c], is_signed[c], output[c]);
        rows_left[c]--;
      }
    }
  }
  syn.engine.destroy();
  tile.tile.close();
  cs.codestream.destroy();
  return output;
}

TEST(NeonMultiTransformLocalTest, ReversibleSmagAndUmag32BitAnd64BitRoundTrip) {
  constexpr int kWidth = 48;
  constexpr int kHeight = 16;
  std::vector<int32_t> smag_samples(static_cast<size_t>(kWidth) * kHeight);
  std::vector<int32_t> umag_samples(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const size_t idx = static_cast<size_t>(y) * kWidth + x;
      smag_samples[idx] = ((x * 23 - y * 19 + 300) % 256) - 128;
      umag_samples[idx] = (x * 17 + y * 31) % 256;
    }
  }
  const std::vector<ComponentData> comps = {
      {8, true, smag_samples},
      {8, false, umag_samples},
  };
  const std::vector<std::string> siz_ext = {
      "Sextensions=NLT",
      "Ncomponents=2",
      "Nprecision=8",
      "Nsigned=yes,no",
  };
  const std::vector<std::string> cod_ext = {
      "NLType:C0=SMAG",
      "NLType:C1=UMAG",
  };
  for (int flags : {KDU_MULTI_XFORM_PRECISE, KDU_MULTI_XFORM_VPRECISE}) {
    const std::vector<kdu_byte> bs = EncodeMulti(
        kWidth, kHeight, /*reversible=*/true, flags, comps, siz_ext, cod_ext);
    const std::vector<std::vector<int32_t>> dec = DecodeMulti(bs, flags);
    ASSERT_EQ(dec.size(), 2u);
    EXPECT_EQ(dec[0], smag_samples) << "flags=" << flags;
    EXPECT_EQ(dec[1], umag_samples) << "flags=" << flags;
  }
}

TEST(NeonMultiTransformLocalTest, IrreversibleSmagAndUmagRoundTrip) {
  constexpr int kWidth = 48;
  constexpr int kHeight = 16;
  std::vector<int32_t> smag_smooth(static_cast<size_t>(kWidth) * kHeight);
  std::vector<int32_t> umag_smooth(static_cast<size_t>(kWidth) * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const size_t idx = static_cast<size_t>(y) * kWidth + x;
      const int32_t mag = (x * 40 + y * 20) % 1500;
      smag_smooth[idx] = ((x / 16) % 2 == 0) ? mag : (-2048 + (mag / 2));
      umag_smooth[idx] = 500 + mag;
    }
  }
  const std::vector<ComponentData> comps = {
      {12, true, smag_smooth},
      {12, false, umag_smooth},
  };
  const std::vector<kdu_byte> bs = EncodeMulti(
      kWidth, kHeight, /*reversible=*/false, KDU_MULTI_XFORM_PRECISE, comps,
      {"Sextensions=NLT", "Ncomponents=2", "Nprecision=12", "Nsigned=yes,no"},
      {"NLType:C0=SMAG", "NLType:C1=UMAG", "Qstep=0.0005"});
  const std::vector<std::vector<int32_t>> dec =
      DecodeMulti(bs, KDU_MULTI_XFORM_PRECISE);
  ASSERT_EQ(dec.size(), 2u);
  double umag_sum_abs = 0.0;
  for (size_t i = 0; i < umag_smooth.size(); ++i) {
    EXPECT_GE(dec[0][i], -2048);
    EXPECT_LE(dec[0][i], 2047);
    EXPECT_GE(dec[1][i], 0);
    EXPECT_LE(dec[1][i], 4095);
    umag_sum_abs += std::fabs(static_cast<double>(dec[1][i] - umag_smooth[i]));
  }
  EXPECT_LT(umag_sum_abs / umag_smooth.size(), 25.0);
}

TEST(NeonMultiTransformLocalTest, Part2MctMatrixAndCopyRoundTrip) {
  constexpr int kWidth = 48;
  constexpr int kHeight = 16;

  // Reversible 2-component MATRIX + copy across 16-bit, 32-bit, and 64-bit.
  {
    std::vector<int32_t> c0(static_cast<size_t>(kWidth) * kHeight);
    std::vector<int32_t> c1(static_cast<size_t>(kWidth) * kHeight);
    for (size_t i = 0; i < c0.size(); ++i) {
      c0[i] = static_cast<int32_t>((i * 19 + 7) % 256);
      c1[i] = static_cast<int32_t>((i * 31 + 13) % 256);
    }
    const std::vector<ComponentData> comps = {{8, false, c0}, {8, false, c1}};
    const std::vector<std::string> siz_ext = {
        "Sextensions=MCT", "Mcomponents=2", "Mprecision=8", "Msigned=no"};
    const std::vector<std::string> cod_ext = {
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,2,2,-1,0,-1",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=3,-2",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
                      KDU_MULTI_XFORM_VPRECISE}) {
      const std::vector<kdu_byte> bs = EncodeMulti(
          kWidth, kHeight, /*reversible=*/true, flags, comps, siz_ext, cod_ext);
      const std::vector<std::vector<int32_t>> dec = DecodeMulti(bs, flags);
      ASSERT_EQ(dec.size(), 2u);
      EXPECT_EQ(dec[0], c0) << "flags=" << flags;
      EXPECT_EQ(dec[1], c1) << "flags=" << flags;
    }
  }

  // Irreversible 3-component MATRIX across 16-bit and 32-bit modes.
  {
    std::vector<int32_t> c0(static_cast<size_t>(kWidth) * kHeight);
    std::vector<int32_t> c1(static_cast<size_t>(kWidth) * kHeight);
    std::vector<int32_t> c2(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const size_t idx = static_cast<size_t>(y) * kWidth + x;
        c0[idx] =
            128 + static_cast<int32_t>(50.0 * std::sin(0.15 * x + 0.1 * y));
        c1[idx] =
            128 + static_cast<int32_t>(45.0 * std::cos(0.12 * x - 0.1 * y));
        c2[idx] =
            128 + static_cast<int32_t>(40.0 * std::sin(0.08 * x + 0.2 * y));
      }
    }
    const std::vector<ComponentData> comps = {
        {8, false, c0}, {8, false, c1}, {8, false, c2}};
    const std::vector<std::string> siz_ext = {
        "Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no"};
    const std::vector<std::string> cod_ext = {
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.25,0.0,0.0,1.0,-0.25,0.25,0.0,1.0",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
        "Qstep=0.0005",
    };
    for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
      const std::vector<kdu_byte> bs =
          EncodeMulti(kWidth, kHeight, /*reversible=*/false, flags, comps,
                      siz_ext, cod_ext);
      const std::vector<std::vector<int32_t>> dec = DecodeMulti(bs, flags);
      ASSERT_EQ(dec.size(), 3u);
      for (int c = 0; c < 3; ++c) {
        for (size_t i = 0; i < comps[c].samples.size(); ++i) {
          EXPECT_NEAR(dec[c][i], comps[c].samples[i], 6)
              << "flags=" << flags << " c=" << c << " i=" << i;
        }
      }
    }
  }
}

}  // namespace
}  // namespace kdu_core
