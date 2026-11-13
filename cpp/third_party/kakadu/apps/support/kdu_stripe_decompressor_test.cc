// Unit tests for
// third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.{h,cpp}.

#include "third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

class MemoryCompressedTarget : public kdu_core::kdu_compressed_target {
 public:
  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes > 0) {
      data_.insert(data_.end(), buf, buf + num_bytes);
    }
    return true;
  }

  const std::vector<kdu_core::kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_core::kdu_byte> data_;
};

class MemoryCompressedSource : public kdu_core::kdu_compressed_source {
 public:
  explicit MemoryCompressedSource(const std::vector<kdu_core::kdu_byte>& data)
      : data_(data) {}

  int get_capabilities() override { return KDU_SOURCE_CAP_SEEKABLE; }

  bool seek(kdu_core::kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_core::kdu_long get_pos() override {
    return static_cast<kdu_core::kdu_long>(pos_);
  }

  int read(kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail = data_.size() - pos_;
    const size_t n = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

 private:
  const std::vector<kdu_core::kdu_byte>& data_;
  size_t pos_ = 0;
};

class ThrowingErrorHandler : public kdu_core::kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char* s) override {
    if (s != nullptr) msg_ += s;
  }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      last_msg_ = std::move(msg_);
      msg_.clear();
      kdu_core::kdu_thread_safe_message::flush(true);
      throw kdu_core::kdu_exception(1);
    }
  }
  const std::string& last_msg() const { return last_msg_; }

 private:
  std::string msg_;
  std::string last_msg_;
};

class NoopWarningHandler : public kdu_core::kdu_thread_safe_message {
 public:
  ~NoopWarningHandler() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      kdu_core::kdu_thread_safe_message::flush(true);
    }
  }
};

void ParseMultiParams(kdu_core::kdu_params* params, std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
      ++i;
    }
    if (i >= text.size()) break;
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t' &&
           text[j] != '\n') {
      ++j;
    }
    const std::string token(text.substr(i, j - i));
    ASSERT_TRUE(params->parse_string(token.c_str())) << "token=" << token;
    i = j;
  }
}

std::vector<kdu_core::kdu_byte> EncodeTestCodestream(
    int width, int height, int num_components, int bit_depth, bool reversible,
    bool is_signed = false, const char* siz_extra = nullptr,
    const char* cod_extra = nullptr) {
  kdu_core::siz_params siz;
  const std::string siz_str =
      std::string(siz_extra != nullptr ? siz_extra : "");
  const std::string dims_key =
      (siz_str.find("Ssampling") != std::string::npos) ? "Ssize" : "Sdims";
  siz.parse_string(("Scomponents=" + std::to_string(num_components)).c_str());
  siz.parse_string((dims_key + "={" + std::to_string(height) + "," +
                    std::to_string(width) + "}")
                       .c_str());
  siz.parse_string(("Sprecision=" + std::to_string(bit_depth)).c_str());
  siz.parse_string(is_signed ? "Ssigned=yes" : "Ssigned=no");
  if (siz_extra != nullptr && siz_extra[0] != '\0') {
    ParseMultiParams(&siz, siz_extra);
  }
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  MemoryCompressedTarget target;
  kdu_core::kdu_codestream cs;
  cs.create(&siz, &target);
  absl::Cleanup cs_cleanup = [&] {
    if (cs.exists()) cs.destroy();
  };

  kdu_core::kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
  if (cod == nullptr) {
    ADD_FAILURE() << "Missing COD cluster";
    return {};
  }
  cod->parse_string(reversible ? "Creversible=yes" : "Creversible=no");
  if (num_components >= 3 && reversible) {
    cod->parse_string("Cycc=no");
  }
  if (cod_extra != nullptr && cod_extra[0] != '\0') {
    ParseMultiParams(cod, cod_extra);
  }
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(num_components);
  std::vector<int> widths(num_components);
  for (int c = 0; c < num_components; ++c) {
    kdu_core::kdu_dims dims;
    cs.get_dims(c, dims);
    heights[c] = dims.size.y;
    widths[c] = dims.size.x;
  }

  if (bit_depth <= 8 && !is_signed) {
    std::vector<std::vector<kdu_core::kdu_byte>> planes(num_components);
    std::vector<kdu_core::kdu_byte*> ptrs(num_components);
    for (int c = 0; c < num_components; ++c) {
      planes[c].resize(static_cast<size_t>(widths[c]) * heights[c]);
      for (int y = 0; y < heights[c]; ++y) {
        for (int x = 0; x < widths[c]; ++x) {
          planes[c][y * widths[c] + x] =
              static_cast<kdu_core::kdu_byte>((x * 7 + y * 11 + c * 37) & 0xff);
        }
      }
      ptrs[c] = planes[c].data();
    }
    comp.push_stripe(ptrs.data(), heights.data());
  } else {
    std::vector<std::vector<kdu_core::kdu_int32>> planes(num_components);
    std::vector<kdu_core::kdu_int32*> ptrs(num_components);
    std::vector<int> precs(num_components, bit_depth);
    std::vector<bool> signed_vec(num_components, is_signed);
    const kdu_core::kdu_int32 max_val =
        (bit_depth >= 31) ? 0x3fffffff
                          : ((1 << (bit_depth - (is_signed ? 1 : 0))) - 1);
    for (int c = 0; c < num_components; ++c) {
      planes[c].resize(static_cast<size_t>(widths[c]) * heights[c]);
      for (int y = 0; y < heights[c]; ++y) {
        for (int x = 0; x < widths[c]; ++x) {
          const int pattern = (x * 13 + y * 5 + c * 17) & 0x7f;
          planes[c][y * widths[c] + x] =
              is_signed ? ((pattern % 17) - 8) : (pattern % (max_val + 1));
        }
      }
      ptrs[c] = planes[c].data();
    }
    const bool signed_arr[4] = {is_signed, is_signed, is_signed, is_signed};
    if (num_components <= 4) {
      comp.push_stripe(ptrs.data(), heights.data(), nullptr, nullptr,
                       precs.data(), signed_arr);
    } else {
      comp.push_stripe(ptrs.data(), heights.data(), nullptr, nullptr,
                       precs.data(), nullptr);
    }
  }
  comp.finish();
  cs.destroy();
  return target.data();
}

class RecordingNotifier : public kdu_stripe_decompressor_notifier {
 public:
  void last_tile_started(kdu_core::kdu_long queue_sequence_idx,
                         kdu_core::kdu_thread_env* caller) override {
    ++call_count;
    last_seq_idx = queue_sequence_idx;
    if (on_notify_comp != nullptr && on_notify_cs != nullptr &&
        on_notify_cs->exists()) {
      std::vector<int> rec(on_notify_cs->get_num_components(true), 4);
      on_notify_comp->prestage(rec.data(), queue_sequence_idx, next_notifier,
                               caller);
    }
  }

  int call_count = 0;
  kdu_core::kdu_long last_seq_idx = -1;
  kdu_stripe_decompressor* on_notify_comp = nullptr;
  kdu_core::kdu_codestream* on_notify_cs = nullptr;
  kdu_stripe_decompressor_notifier* next_notifier = nullptr;
};

void ReopenCodestream(MemoryCompressedSource& src,
                      kdu_core::kdu_codestream& cs) {
  if (cs.exists()) cs.destroy();
  src.seek(0);
  cs.create(&src);
}

class KduStripeDecompressorTest : public testing::Test {
 protected:
  void SetUp() override {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }
  void TearDown() override {
    kdu_core::kdu_customize_errors(nullptr);
    kdu_core::kdu_customize_warnings(nullptr);
  }

  ThrowingErrorHandler error_handler_;
  NoopWarningHandler warning_handler_;
};

TEST_F(KduStripeDecompressorTest, AllEightPullStripeOverloadsAndSimdPaths) {
  constexpr int kW = 40;
  constexpr int kH = 12;
  const auto data_rgb = EncodeTestCodestream(kW, kH, 3, 8, /*reversible=*/true);
  const auto data_rgba =
      EncodeTestCodestream(kW, kH, 4, 8, /*reversible=*/true);

  // 1. kdu_byte* stripe_bufs[] planar (sample_gap=1 SIMD + streaming stores)
  {
    MemoryCompressedSource src(data_rgb);
    kdu_core::kdu_codestream cs;
    ReopenCodestream(src, cs);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    EXPECT_FALSE(decomp.is_active());
    decomp.start(cs);
    EXPECT_TRUE(decomp.is_active());

    std::array<std::vector<kdu_core::kdu_byte>, 3> planes = {
        std::vector<kdu_core::kdu_byte>(kW * kH),
        std::vector<kdu_core::kdu_byte>(kW * kH),
        std::vector<kdu_core::kdu_byte>(kW * kH),
    };
    kdu_core::kdu_byte* ptrs[3] = {planes[0].data(), planes[1].data(),
                                   planes[2].data()};
    const int heights[3] = {kH, kH, kH};
    EXPECT_FALSE(decomp.pull_stripe(ptrs, heights, nullptr, nullptr, nullptr,
                                    nullptr, KDU_STRIPE_STORE_PREF_STREAMING));
    EXPECT_TRUE(decomp.finish());
    EXPECT_FALSE(decomp.is_active());
  }

  // 2. kdu_byte* single buffer overloads: NULL offsets (tight 3-comp SIMD),
  //    explicit sample_offsets + row_gaps + clamped precisions.
  {
    MemoryCompressedSource src(data_rgb);
    kdu_core::kdu_codestream cs;
    ReopenCodestream(src, cs);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs);
    std::vector<kdu_core::kdu_byte> interleaved(kW * kH * 3);
    const int heights[3] = {kH, kH, kH};
    EXPECT_FALSE(decomp.pull_stripe(interleaved.data(), heights));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_byte> plane_buf(kW * kH * 3);
    const int offsets[3] = {0, kW * kH, 2 * kW * kH};
    const int sgaps[3] = {1, 1, 1};
    const int rgaps[3] = {kW, kW, kW};
    const int precs[3] = {0, 8, 12};
    EXPECT_FALSE(decomp.pull_stripe(plane_buf.data(), heights, offsets, sgaps,
                                    rgaps, precs));
    EXPECT_TRUE(decomp.finish());
  }

  // 3. 4-comp tight interleaved SIMD (sample_gap=4) for bytes, int16, int32,
  //    float.
  {
    MemoryCompressedSource src(data_rgba);
    kdu_core::kdu_codestream cs;
    ReopenCodestream(src, cs);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int heights[4] = {kH, kH, kH, kH};

    decomp.start(cs);
    std::vector<kdu_core::kdu_byte> buf_b(kW * kH * 4);
    EXPECT_FALSE(decomp.pull_stripe(buf_b.data(), heights));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_int16> buf_16(kW * kH * 4);
    EXPECT_FALSE(decomp.pull_stripe(buf_16.data(), heights));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_int32> buf_32(kW * kH * 4);
    EXPECT_FALSE(decomp.pull_stripe(buf_32.data(), heights));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<float> buf_f(kW * kH * 4);
    EXPECT_FALSE(decomp.pull_stripe(buf_f.data(), heights));
    EXPECT_TRUE(decomp.finish());
  }

  // 4. int16*, int32*, float* multi-buffer and single-buffer with explicit
  //    offsets/row_gaps/is_signed/precisions.
  {
    MemoryCompressedSource src(data_rgb);
    kdu_core::kdu_codestream cs;
    ReopenCodestream(src, cs);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int heights[3] = {kH, kH, kH};
    const int offsets[3] = {0, kW * kH, 2 * kW * kH};

    decomp.start(cs);
    std::vector<kdu_core::kdu_int16> b16(kW * kH * 3);
    kdu_core::kdu_int16* p16[3] = {b16.data(), b16.data() + offsets[1],
                                   b16.data() + offsets[2]};
    const int precs16[3] = {0, 12, 20};
    const bool signed16[3] = {true, false, true};
    EXPECT_FALSE(
        decomp.pull_stripe(p16, heights, nullptr, nullptr, precs16, signed16));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_int16> i16(kW * kH * 3);
    EXPECT_FALSE(decomp.pull_stripe(i16.data(), heights, offsets, nullptr,
                                    nullptr, precs16, signed16));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_int32> b32(kW * kH * 3);
    kdu_core::kdu_int32* p32[3] = {b32.data(), b32.data() + offsets[1],
                                   b32.data() + offsets[2]};
    const int precs32[3] = {0, 16, 40};
    EXPECT_FALSE(
        decomp.pull_stripe(p32, heights, nullptr, nullptr, precs32, signed16));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<kdu_core::kdu_int32> i32(kW * kH * 3);
    EXPECT_FALSE(decomp.pull_stripe(i32.data(), heights, offsets, nullptr,
                                    nullptr, precs32, signed16));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<float> bf(kW * kH * 3);
    float* pf[3] = {bf.data(), bf.data() + offsets[1], bf.data() + offsets[2]};
    const int precsf[3] = {-100, 0, 100};
    EXPECT_FALSE(
        decomp.pull_stripe(pf, heights, nullptr, nullptr, precsf, signed16));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs);
    std::vector<float> fi(kW * kH * 3);
    EXPECT_FALSE(decomp.pull_stripe(fi.data(), heights, offsets, nullptr,
                                    nullptr, precsf, signed16));
    EXPECT_TRUE(decomp.finish());
  }
}

TEST_F(KduStripeDecompressorTest, BytesWordsDwordsFloatsAllPrecisionPaths) {
  struct PrecisionCase {
    int orig_bits;
    bool reversible;
    bool is_signed;
    bool force_precise;
    int dst_bits;
    int float_prec;
  };
  static constexpr PrecisionCase kCases[] = {
      // Irreversible (buf16 when !force_precise, buf32 when force_precise).
      {8, false, false, false, 4, 8},
      {8, false, false, false, 8, 0},
      {8, false, false, false, 12, -4},
      {12, false, true, false, 16, 12},
      {12, false, false, true, 8, 8},
      {12, false, false, true, 12, 12},
      {12, false, false, true, 24, 16},
      // Reversible 8-bit (buf16 is_absolute()).
      {8, true, false, false, 6, 6},
      {8, true, false, false, 8, 8},
      {8, true, true, false, 16, 12},
      // Reversible 16-bit (buf32 is_absolute()).
      {16, true, false, false, 8, 8},
      {16, true, false, false, 16, 16},
      {16, true, true, false, 24, 24},
      // Reversible 28-bit (buf64 is_absolute()).
      {28, true, false, true, 12, 8},
      {28, true, false, true, 28, 28},
      {28, true, true, true, 32, 32},
  };

  constexpr int kW = 34;  // Non-multiple of 16 for SIMD tail loops.
  constexpr int kH = 4;

  for (const auto& tc : kCases) {
    const auto data = EncodeTestCodestream(kW, kH, 1, tc.orig_bits,
                                           tc.reversible, tc.is_signed);
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int heights[1] = {kH};
    const int byte_prec = std::clamp(tc.dst_bits, 1, 8);
    const int word_prec = std::clamp(tc.dst_bits, 1, 16);
    const int dword_prec = std::clamp(tc.dst_bits, 1, 32);

    for (int sample_gap : {1, 2}) {
      for (bool want_signed : {false, true}) {
        const int row_gap = kW * sample_gap;

        ReopenCodestream(src, cs);
        decomp.start(cs, tc.force_precise);
        std::vector<kdu_core::kdu_byte> bb(row_gap * kH);
        kdu_core::kdu_byte* pb[1] = {bb.data()};
        EXPECT_FALSE(
            decomp.pull_stripe(pb, heights, &sample_gap, &row_gap, &byte_prec));
        EXPECT_TRUE(decomp.finish());

        ReopenCodestream(src, cs);
        decomp.start(cs, tc.force_precise);
        std::vector<kdu_core::kdu_int16> bw(row_gap * kH);
        kdu_core::kdu_int16* pw[1] = {bw.data()};
        EXPECT_FALSE(decomp.pull_stripe(pw, heights, &sample_gap, &row_gap,
                                        &word_prec, &want_signed));
        EXPECT_TRUE(decomp.finish());

        ReopenCodestream(src, cs);
        decomp.start(cs, tc.force_precise);
        std::vector<kdu_core::kdu_int32> bd(row_gap * kH);
        kdu_core::kdu_int32* pd[1] = {bd.data()};
        EXPECT_FALSE(decomp.pull_stripe(pd, heights, &sample_gap, &row_gap,
                                        &dword_prec, &want_signed));
        EXPECT_TRUE(decomp.finish());

        ReopenCodestream(src, cs);
        decomp.start(cs, tc.force_precise);
        std::vector<float> bf(row_gap * kH);
        float* pf[1] = {bf.data()};
        EXPECT_FALSE(decomp.pull_stripe(pf, heights, &sample_gap, &row_gap,
                                        &tc.float_prec, &want_signed));
        EXPECT_TRUE(decomp.finish());
      }
    }
  }
}

TEST_F(KduStripeDecompressorTest, PadFlagsSimdAndScalarAndHighPrecision) {
  constexpr int kW = 36;
  constexpr int kH = 4;

  struct PadConfig {
    std::array<int, 3> flags;
    int offset;  // Index within 4-channel pixel where dummy channel lands.
  };
  static constexpr PadConfig kPadConfigs[] = {
      {{0, 0, KDU_STRIPE_PAD_AFTER | KDU_STRIPE_PAD_HIGH}, 3},
      {{0, 0, KDU_STRIPE_PAD_AFTER | KDU_STRIPE_PAD_LOW}, 3},
      {{0, 0, KDU_STRIPE_PAD_AFTER}, 3},
      {{0, 0, KDU_STRIPE_PAD_AFTER | KDU_STRIPE_PAD_HIGH | KDU_STRIPE_PAD_LOW},
       3},
      {{KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_HIGH, 0, 0}, 0},
      {{KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_LOW, 0, 0}, 0},
      {{KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_AFTER, 0, 0}, 0},
  };

  for (int bit_depth : {8, 16, 28}) {
    const auto data =
        EncodeTestCodestream(kW, kH, 3, bit_depth, /*reversible=*/true);
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int heights[3] = {kH, kH, kH};

    for (const auto& cfg : kPadConfigs) {
      const int sgaps4[3] = {4, 4, 4};
      const int rgaps4[3] = {kW * 4, kW * 4, kW * 4};
      const int base_off = (cfg.offset == 0) ? 1 : 0;

      // Byte RGBA padded SIMD (for 8-bit) and scalar fallback (for >8-bit).
      ReopenCodestream(src, cs);
      decomp.start(cs);
      std::vector<kdu_core::kdu_byte> bb(kW * kH * 4, 42);
      kdu_core::kdu_byte* pb[3] = {bb.data() + base_off,
                                   bb.data() + base_off + 1,
                                   bb.data() + base_off + 2};
      EXPECT_FALSE(decomp.pull_stripe(pb, heights, sgaps4, rgaps4, nullptr,
                                      cfg.flags.data()));
      EXPECT_TRUE(decomp.finish());
      if (cfg.flags[cfg.offset == 0 ? 0 : 2] & KDU_STRIPE_PAD_HIGH) {
        EXPECT_EQ(bb[cfg.offset], 255);
      } else if (cfg.flags[cfg.offset == 0 ? 0 : 2] & KDU_STRIPE_PAD_LOW) {
        EXPECT_EQ(bb[cfg.offset], 0);
      } else {
        EXPECT_EQ(bb[cfg.offset], 128);
      }

      // int16 with signed & unsigned padding.
      for (bool want_signed : {true, false}) {
        ReopenCodestream(src, cs);
        decomp.start(cs);
        std::vector<kdu_core::kdu_int16> bw(kW * kH * 4);
        kdu_core::kdu_int16* pw[3] = {bw.data() + base_off,
                                      bw.data() + base_off + 1,
                                      bw.data() + base_off + 2};
        const int p16[3] = {12, 12, 12};
        const bool s16[3] = {want_signed, want_signed, want_signed};
        EXPECT_FALSE(decomp.pull_stripe(pw, heights, sgaps4, rgaps4, p16, s16,
                                        cfg.flags.data()));
        EXPECT_TRUE(decomp.finish());
      }

      // int32 with signed & unsigned padding + 32-bit prec.
      for (bool want_signed : {true, false}) {
        ReopenCodestream(src, cs);
        decomp.start(cs);
        std::vector<kdu_core::kdu_int32> bd(kW * kH * 4);
        kdu_core::kdu_int32* pd[3] = {bd.data() + base_off,
                                      bd.data() + base_off + 1,
                                      bd.data() + base_off + 2};
        const int p32[3] = {20, 20, 20};
        const bool s32[3] = {want_signed, want_signed, want_signed};
        EXPECT_FALSE(decomp.pull_stripe(pd, heights, sgaps4, rgaps4, p32, s32,
                                        cfg.flags.data()));
        EXPECT_TRUE(decomp.finish());
      }

      // float with signed & unsigned padding.
      for (bool want_signed : {true, false}) {
        ReopenCodestream(src, cs);
        decomp.start(cs);
        std::vector<float> bf(kW * kH * 4);
        float* pf[3] = {bf.data() + base_off, bf.data() + base_off + 1,
                        bf.data() + base_off + 2};
        const int pf_prec[3] = {8, 8, 8};
        const bool sf[3] = {want_signed, want_signed, want_signed};
        EXPECT_FALSE(decomp.pull_stripe(pf, heights, sgaps4, rgaps4, pf_prec,
                                        sf, cfg.flags.data()));
        EXPECT_TRUE(decomp.finish());
      }
    }

    // Scalar non-4 sample_gap padding (sample_gap=2 -> scalar pad_* paths) and
    // explicit prec=32 signed/unsigned padding on int32.
    {
      const int sgaps5[3] = {5, 5, 5};
      const int rgaps5[3] = {kW * 5, kW * 5, kW * 5};
      const int flags_hi[3] = {KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_HIGH, 0,
                               KDU_STRIPE_PAD_AFTER | KDU_STRIPE_PAD_LOW};
      ReopenCodestream(src, cs);
      decomp.start(cs);
      std::vector<kdu_core::kdu_int32> bd(kW * kH * 5);
      kdu_core::kdu_int32* pd[3] = {bd.data() + 1, bd.data() + 2,
                                    bd.data() + 3};
      const int p32[3] = {32, 32, 32};
      const bool s32[3] = {true, true, false};
      EXPECT_FALSE(
          decomp.pull_stripe(pd, heights, sgaps5, rgaps5, p32, s32, flags_hi));
      EXPECT_TRUE(decomp.finish());
    }
  }
}

TEST_F(KduStripeDecompressorTest, PullStripePck32PathsAndErrors) {
  constexpr int kH = 4;

  // 1. v210 10-bit 4:2:2 packed 32-bit pattern (SIMD + partial trailing
  // period).
  for (int width : {24, 26}) {
    const auto data =
        EncodeTestCodestream(width, kH, 3, 10, /*reversible=*/true, false,
                             "Ssampling={1,1},{1,2},{1,2}", "Clevels=2");
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs);
    const int offsets[3] = {1, 0, 2};
    const int gaps[3] = {2, 4, 4};
    const int words_per_row = ((width + 5) / 6) * 4;
    std::vector<kdu_core::kdu_int32> pck_buf(words_per_row * 2);
    EXPECT_TRUE(decomp.pull_stripe_pck32(pck_buf.data(), 2, offsets, gaps,
                                         /*period_words=*/4,
                                         /*fld_bits=*/10, words_per_row));
    EXPECT_FALSE(decomp.pull_stripe_pck32(pck_buf.data(), 2, offsets, gaps,
                                          /*period_words=*/4,
                                          /*fld_bits=*/10, words_per_row));
    EXPECT_TRUE(decomp.finish());
  }

  // 2. r210 10-bit packed (period_words=1, SIMD + tail), fld_bits=12 scalar
  //    with skipped_samples, plus buf32 and buf64 precisions.
  for (int bit_depth : {10, 16, 28}) {
    const int width = 17;
    const auto data =
        EncodeTestCodestream(width, kH, 3, bit_depth, /*reversible=*/true);
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;

    // r210 with NULL skipped_samples/precisions.
    decomp.start(cs);
    const int r_offsets[3] = {2, 1, 0};
    const int r_gaps[3] = {3, 3, 3};
    std::vector<kdu_core::kdu_int32> r_buf(width * kH);
    EXPECT_FALSE(decomp.pull_stripe_pck32(r_buf.data(), kH, r_offsets, r_gaps,
                                          /*period_words=*/1,
                                          /*fld_bits=*/10, width));
    EXPECT_TRUE(decomp.finish());

    // Scalar 12-bit fld_bits with skipped_samples and precisions.
    ReopenCodestream(src, cs);
    decomp.start(cs);
    const int s_offsets[3] = {0, 1, 2};
    const int s_gaps[3] = {4, 4, 4};
    const int s_skips[3] = {1, 0, 0};
    const int s_precs[3] = {12, 10, 8};
    const int words_per_row = (width + 1) * 4 + 8;
    std::vector<kdu_core::kdu_int32> s_buf(words_per_row * kH);
    EXPECT_FALSE(decomp.pull_stripe_pck32(
        s_buf.data(), kH, s_offsets, s_gaps, /*period_words=*/4,
        /*fld_bits=*/12, words_per_row, s_skips, s_precs));
    EXPECT_TRUE(decomp.finish());
  }

  // 3. Multi-tile pck32 and pck32 validation error paths.
  {
    const auto data = EncodeTestCodestream(
        32, 32, 3, 10, /*reversible=*/true, false,
        "Stiles={16,16}\nSsampling={1,1},{1,2},{1,2}", "Clevels=2");
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int offsets[3] = {1, 0, 2};
    const int gaps[3] = {2, 4, 4};
    const int words_per_row = ((32 + 5) / 6) * 4;
    std::vector<kdu_core::kdu_int32> pck_buf(words_per_row * 32);

    // Invalid fld_bits.
    decomp.start(cs);
    EXPECT_THROW(decomp.pull_stripe_pck32(pck_buf.data(), 32, offsets, gaps, 4,
                                          /*fld_bits=*/20, words_per_row),
                 kdu_core::kdu_exception);
    decomp.reset();

    // period_flds not divisible by sample_gaps[c].
    ReopenCodestream(src, cs);
    decomp.start(cs);
    const int bad_gaps[3] = {5, 5, 5};
    EXPECT_THROW(decomp.pull_stripe_pck32(pck_buf.data(), 32, offsets, bad_gaps,
                                          4, 10, words_per_row),
                 kdu_core::kdu_exception);
    decomp.reset();

    // skipped_samples out of range (7 * 2 = 14 > period_flds=12).
    ReopenCodestream(src, cs);
    decomp.start(cs);
    const int bad_skips[3] = {7, 0, 0};
    EXPECT_THROW(decomp.pull_stripe_pck32(pck_buf.data(), 32, offsets, gaps, 4,
                                          10, words_per_row, bad_skips),
                 kdu_core::kdu_exception);
    decomp.reset();

    // Multi-tile v210 incremental pull.
    ReopenCodestream(src, cs);
    decomp.start(cs);
    EXPECT_TRUE(decomp.pull_stripe_pck32(pck_buf.data(), 16, offsets, gaps, 4,
                                         10, words_per_row));
    EXPECT_FALSE(decomp.pull_stripe_pck32(pck_buf.data(), 16, offsets, gaps, 4,
                                          10, words_per_row));
    EXPECT_TRUE(decomp.finish());

    // Non-reversible (float source) scalar pck32 12-bit pull.
    const auto data_444 =
        EncodeTestCodestream(24, 4, 3, /*precision=*/10, /*is_signed=*/false,
                             /*reversible=*/false, "", "");
    MemoryCompressedSource src_444(data_444);
    kdu_core::kdu_codestream cs_444;
    cs_444.create(&src_444);
    absl::Cleanup cs_444_cleanup = [&] {
      if (cs_444.exists()) cs_444.destroy();
    };
    const int rgbi_offsets[3] = {0, 1, 2};
    const int rgbi_gaps[3] = {2, 2, 2};
    std::vector<kdu_core::kdu_int32> rgbi_buf(4 * 48, 0);
    decomp.start(cs_444);
    EXPECT_FALSE(decomp.pull_stripe_pck32(rgbi_buf.data(), 4, rgbi_offsets,
                                          rgbi_gaps, 4, 12, 48));
    EXPECT_TRUE(decomp.finish());
  }
}

TEST_F(KduStripeDecompressorTest, PrestageNotifierMtMultiTileReuseAndErrors) {
  constexpr int kW = 48;
  constexpr int kH = 48;
  const auto mt_data = EncodeTestCodestream(
      kW, kH, 3, 8, /*reversible=*/true, false,
      "Stiles={16,16}\nSsampling={1,1},{2,2},{2,2}", "Clayers=2\nClevels=2");

  // 1. Recommended stripe heights and errors before start / disproportionate.
  {
    MemoryCompressedSource src(mt_data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_core::kdu_membroker broker(4LL << 20);
    kdu_stripe_decompressor decomp;
    int rec[3] = {};
    int max_h[3] = {};
    EXPECT_THROW(decomp.get_recommended_stripe_heights(4, 8, rec, max_h),
                 kdu_core::kdu_exception);

    decomp.mem_configure(&broker, 16);
    decomp.start(cs);
    EXPECT_TRUE(decomp.get_recommended_stripe_heights(0, 0, rec, max_h));
    EXPECT_EQ(rec[0], 1);
    decomp.finish();
    ReopenCodestream(src, cs);
    decomp.start(cs);
    EXPECT_TRUE(decomp.get_recommended_stripe_heights(8, 12, rec, max_h));
    EXPECT_EQ(rec[0], 9);
    EXPECT_EQ(max_h[0], 9);

    // Disproportionate push on multi-tile subsampled components throws.
    std::vector<kdu_core::kdu_byte> c0(kW * 32);
    kdu_core::kdu_byte* bufs[3] = {c0.data(), nullptr, nullptr};
    const int bad_h[3] = {32, 0, 0};
    EXPECT_THROW(decomp.pull_stripe(bufs, bad_h), kdu_core::kdu_exception);
    decomp.finish();
    decomp.reset(/*free_memory=*/true);
  }

  // 2. Single-threaded prestage + notifier via finish() before last tile.
  {
    MemoryCompressedSource src(mt_data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs);
    RecordingNotifier notifier;
    const int rec[3] = {16, 8, 8};
    EXPECT_TRUE(decomp.prestage(rec, 0, &notifier, nullptr));
    EXPECT_FALSE(decomp.prestage(rec, 0, &notifier, nullptr));
    EXPECT_FALSE(decomp.finish());
    EXPECT_EQ(notifier.call_count, 1);
  }

  // 3. Multi-threaded D1 -> D2 prestage pipelining, set_thread_env, finish,
  //    extra pull_stripe at EOF, reset(false) reuse, and reset(true).
  {
    kdu_core::kdu_thread_env env;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    kdu_core::kdu_thread_env foreign_env;
    foreign_env.create();
    auto env_cleanup = absl::MakeCleanup([&] {
      env.terminate(nullptr, true);
      if (foreign_env.exists()) foreign_env.destroy();
      env.destroy();
    });

    MemoryCompressedSource src1(mt_data);
    MemoryCompressedSource src2(mt_data);
    kdu_core::kdu_codestream cs1;
    kdu_core::kdu_codestream cs2;
    cs1.create(&src1);
    cs2.create(&src2);
    absl::Cleanup cs_cleanup = [&] {
      if (cs2.exists()) cs2.destroy();
      if (cs1.exists()) cs1.destroy();
    };

    kdu_stripe_decompressor d1;
    kdu_stripe_decompressor d2;
    kdu_core::kdu_push_pull_params extra_pp;
    d1.start(cs1, /*force_precise=*/true, /*want_fastest=*/false, &env, nullptr,
             /*env_dbuf_height=*/-1, /*env_tile_concurrency=*/0, &extra_pp);
    d2.start(cs2, false, true, &env);

    // Mismatched caller thread env throws (prestage sets notifier before
    // check).
    const int heights[3] = {16, 8, 8};
    RecordingNotifier n1;
    RecordingNotifier n2;
    EXPECT_THROW(d1.prestage(heights, 0, &n1, &foreign_env),
                 kdu_core::kdu_exception);
    d1.reset();
    d1.start(cs1, /*force_precise=*/true, /*want_fastest=*/false, &env, nullptr,
             /*env_dbuf_height=*/-1, /*env_tile_concurrency=*/0, &extra_pp);

    n1.on_notify_comp = &d2;
    n1.on_notify_cs = &cs2;
    n1.next_notifier = &n2;
    EXPECT_TRUE(d1.prestage(heights, 0, &n1, &env));

    std::array<std::vector<kdu_core::kdu_byte>, 3> out1 = {
        std::vector<kdu_core::kdu_byte>(kW * 16),
        std::vector<kdu_core::kdu_byte>((kW / 2) * 8),
        std::vector<kdu_core::kdu_byte>((kW / 2) * 8),
    };
    kdu_core::kdu_byte* p1[3] = {out1[0].data(), out1[1].data(),
                                 out1[2].data()};
    EXPECT_TRUE(d1.pull_stripe(p1, heights));
    EXPECT_TRUE(d1.pull_stripe(p1, heights));
    EXPECT_FALSE(d1.pull_stripe(p1, heights));
    EXPECT_FALSE(d1.pull_stripe(p1, heights));  // Extra call past EOF.
    EXPECT_TRUE(d1.finish());
    EXPECT_EQ(n1.call_count, 1);

    EXPECT_FALSE(d2.set_thread_env(nullptr));
    EXPECT_FALSE(d2.set_thread_env(&foreign_env));
    EXPECT_TRUE(d2.set_thread_env(&env));
    // Finish d2 before completing all rows using alt_env (&env).
    EXPECT_FALSE(d2.finish(&env));
    EXPECT_EQ(n2.call_count, 1);

    // Reuse d1 after finish() with reset(false) -> second untiled MT decode ->
    // reset(true).
    const auto untiled_data = EncodeTestCodestream(32, 16, 1, 8, true);
    MemoryCompressedSource src_u(untiled_data);
    kdu_core::kdu_codestream csu;
    csu.create(&src_u);
    absl::Cleanup csu_cleanup = [&] {
      if (csu.exists()) csu.destroy();
    };
    d1.reset(/*free_memory=*/false);
    d1.start(csu, false, false, &env);
    int rec_u[1] = {};
    int max_u[1] = {};
    EXPECT_FALSE(d1.get_recommended_stripe_heights(4, 8, rec_u, max_u));
    std::vector<kdu_core::kdu_byte> bu(32 * 8);
    kdu_core::kdu_byte* pu[1] = {bu.data()};
    while (d1.pull_stripe(pu, rec_u)) {
    }
    EXPECT_TRUE(d1.finish());
    d1.reset(/*free_memory=*/true);
    foreign_env.destroy();
  }

  // 4. Multi-threaded abort via explicit abort_queue + cs_terminate + reset.
  {
    kdu_core::kdu_thread_env abort_env;
    abort_env.create();
    ASSERT_TRUE(abort_env.add_thread());
    kdu_core::kdu_thread_queue abort_queue;
    abort_env.attach_queue(&abort_queue, nullptr, "decomp_abort_q");
    MemoryCompressedSource src(mt_data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
      if (abort_env.exists()) abort_env.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs, false, false, &abort_env, &abort_queue);
    std::array<std::vector<kdu_core::kdu_byte>, 3> out = {
        std::vector<kdu_core::kdu_byte>(kW * 16),
        std::vector<kdu_core::kdu_byte>((kW / 2) * 8),
        std::vector<kdu_core::kdu_byte>((kW / 2) * 8),
    };
    kdu_core::kdu_byte* p[3] = {out[0].data(), out[1].data(), out[2].data()};
    const int heights[3] = {16, 8, 8};
    EXPECT_TRUE(decomp.pull_stripe(p, heights));
    abort_env.terminate(&abort_queue, false);
    abort_env.cs_terminate(cs);
    abort_env.destroy();
    decomp.reset(/*free_memory=*/false);
    EXPECT_FALSE(decomp.finish());
  }
}

TEST_F(KduStripeDecompressorTest, TransferEdgeAndSimdPadVariants) {
  // 1. buf32 (force_precise=true) with dst_bits > original_bits and == for
  //    bytes, words, and pck32.
  {
    constexpr int kW = 34;
    constexpr int kH = 4;
    const auto data =
        EncodeTestCodestream(kW, kH, 3, /*bit_depth=*/6, /*reversible=*/true);
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int heights[3] = {kH, kH, kH};

    for (int dst_prec : {6, 8}) {
      ReopenCodestream(src, cs);
      decomp.start(cs, /*force_precise=*/true);
      std::array<std::vector<kdu_core::kdu_byte>, 3> planes = {
          std::vector<kdu_core::kdu_byte>(kW * kH),
          std::vector<kdu_core::kdu_byte>(kW * kH),
          std::vector<kdu_core::kdu_byte>(kW * kH),
      };
      kdu_core::kdu_byte* pb[3] = {planes[0].data(), planes[1].data(),
                                   planes[2].data()};
      const int precs[3] = {dst_prec, dst_prec, dst_prec};
      EXPECT_FALSE(decomp.pull_stripe(pb, heights, nullptr, nullptr, precs));
      EXPECT_TRUE(decomp.finish());
    }

    for (int dst_prec : {6, 12}) {
      ReopenCodestream(src, cs);
      decomp.start(cs, /*force_precise=*/true);
      std::array<std::vector<kdu_core::kdu_int16>, 3> planes = {
          std::vector<kdu_core::kdu_int16>(kW * kH),
          std::vector<kdu_core::kdu_int16>(kW * kH),
          std::vector<kdu_core::kdu_int16>(kW * kH),
      };
      kdu_core::kdu_int16* pw[3] = {planes[0].data(), planes[1].data(),
                                    planes[2].data()};
      const int precs[3] = {dst_prec, dst_prec, dst_prec};
      const bool signed_flags[3] = {false, false, false};
      EXPECT_FALSE(decomp.pull_stripe(pw, heights, nullptr, nullptr, precs,
                                      signed_flags));
      EXPECT_TRUE(decomp.finish());
    }

    const int offsets[3] = {0, 1, 2};
    const int gaps[3] = {4, 4, 4};
    for (int dst_prec : {6, 10}) {
      ReopenCodestream(src, cs);
      decomp.start(cs, /*force_precise=*/true);
      std::vector<kdu_core::kdu_int32> pck(kH * kW * 4, 0);
      const int precs[3] = {dst_prec, dst_prec, dst_prec};
      EXPECT_FALSE(decomp.pull_stripe_pck32(pck.data(), kH, offsets, gaps, 4,
                                            12, kW * 4, nullptr, precs));
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 2. transfer_pck32 buf16 irreversible paths (want_fastest=true) with
  //    shift >= 0 (fld_bits=12) and shift < 0 (fld_bits=16), plus
  //    transfer_words buf16 !is_absolute() shift < 0 (dst_bits=16 >
  //    KDU_FIX_POINT).
  {
    constexpr int kW = 24;
    constexpr int kH = 4;
    const auto data = EncodeTestCodestream(kW, kH, 3, 8, /*reversible=*/false);
    MemoryCompressedSource src(data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int offsets[3] = {0, 1, 2};
    const int gaps[3] = {4, 4, 4};

    decomp.start(cs, /*force_precise=*/false, /*want_fastest=*/true);
    std::vector<kdu_core::kdu_int32> pck12(kH * kW * 4, 0);
    EXPECT_FALSE(decomp.pull_stripe_pck32(pck12.data(), kH, offsets, gaps, 4,
                                          12, kW * 4));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs, /*force_precise=*/false, /*want_fastest=*/true);
    std::vector<kdu_core::kdu_int32> pck16(kH * kW * 4, 0);
    EXPECT_FALSE(decomp.pull_stripe_pck32(pck16.data(), kH, offsets, gaps, 4,
                                          16, kW * 4));
    EXPECT_TRUE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs, /*force_precise=*/false, /*want_fastest=*/true);
    const int heights[3] = {kH, kH, kH};
    std::array<std::vector<kdu_core::kdu_int16>, 3> words = {
        std::vector<kdu_core::kdu_int16>(kW * kH),
        std::vector<kdu_core::kdu_int16>(kW * kH),
        std::vector<kdu_core::kdu_int16>(kW * kH),
    };
    kdu_core::kdu_int16* pw[3] = {words[0].data(), words[1].data(),
                                  words[2].data()};
    const int p16[3] = {16, 16, 16};
    EXPECT_FALSE(decomp.pull_stripe(pw, heights, nullptr, nullptr, p16));
    EXPECT_TRUE(decomp.finish());
  }

  // 3. SIMD pad_buf64 via KDSD_BUF8 (sample_gap=4) across buf16 !abs
  //    (want_fastest=true) and buf32 !abs (force_precise=true), plus
  //    simd_pad_handle reallocation across width change (32 -> 40), and
  //    aliased pointer fallback.
  {
    const auto data_irrev32 =
        EncodeTestCodestream(32, 4, 3, 8, /*reversible=*/false);
    const auto data_irrev40 =
        EncodeTestCodestream(40, 4, 3, 8, /*reversible=*/false);
    const auto data_rev32 =
        EncodeTestCodestream(32, 4, 3, 8, /*reversible=*/true);

    const std::array<int, 3> pad_variants[3] = {
        {KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_HIGH, 0, 0},
        {KDU_STRIPE_PAD_BEFORE | KDU_STRIPE_PAD_LOW, 0, 0},
        {0, 0, KDU_STRIPE_PAD_AFTER},
    };
    kdu_stripe_decompressor decomp;
    const int sgaps4[3] = {4, 4, 4};

    for (int mode = 0; mode < 2; ++mode) {
      MemoryCompressedSource src(data_irrev32);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      const bool force_precise = (mode == 1);
      const bool want_fastest = (mode == 0);
      for (const auto& flags : pad_variants) {
        ReopenCodestream(src, cs);
        decomp.start(cs, force_precise, want_fastest);
        const int base_off = (flags[0] != 0) ? 1 : 0;
        const int rgaps4[3] = {32 * 4, 32 * 4, 32 * 4};
        const int heights[3] = {4, 4, 4};
        std::vector<kdu_core::kdu_byte> bb(32 * 4 * 4);
        kdu_core::kdu_byte* pb[3] = {bb.data() + base_off,
                                     bb.data() + base_off + 1,
                                     bb.data() + base_off + 2};
        EXPECT_FALSE(decomp.pull_stripe(pb, heights, sgaps4, rgaps4, nullptr,
                                        flags.data()));
        EXPECT_TRUE(decomp.finish());
      }
    }

    // Reuse decomp across width 32 -> 40 with reset(false) to exercise
    // simd_pad_handle free+realloc on width increase.
    {
      MemoryCompressedSource src(data_irrev40);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      decomp.reset(/*free_memory=*/false);
      decomp.start(cs, /*force_precise=*/true, /*want_fastest=*/false);
      const int rgaps4[3] = {40 * 4, 40 * 4, 40 * 4};
      const int heights[3] = {4, 4, 4};
      std::vector<kdu_core::kdu_byte> bb(40 * 4 * 4);
      kdu_core::kdu_byte* pb[3] = {bb.data() + 1, bb.data() + 2, bb.data() + 3};
      EXPECT_FALSE(decomp.pull_stripe(pb, heights, sgaps4, rgaps4, nullptr,
                                      pad_variants[0].data()));
      EXPECT_TRUE(decomp.finish());
    }

    // Aliased component pointers reject SIMD interleave cleanly.
    {
      MemoryCompressedSource src(data_rev32);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      decomp.reset(/*free_memory=*/true);
      decomp.start(cs);
      const int sgaps3[3] = {3, 3, 3};
      const int rgaps3[3] = {32 * 3, 32 * 3, 32 * 3};
      const int heights[3] = {4, 4, 4};
      std::vector<kdu_core::kdu_byte> bb(32 * 4 * 3);
      kdu_core::kdu_byte* pb[3] = {bb.data(), bb.data(), bb.data() + 2};
      EXPECT_FALSE(decomp.pull_stripe(pb, heights, sgaps3, rgaps3));
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 4. Mid-tile-row partial_tiles cleanup in finish() and reset(), MT
  //    prestage-then-finish closing partial row trange, large-tile
  //    env_dbuf_height clamp (> 64), and taller second tile row.
  {
    const auto mt_data = EncodeTestCodestream(48, 48, 1, 8, /*reversible=*/true,
                                              false, "Stiles={16,16}", "");
    kdu_core::kdu_thread_env env;
    env.create();
    ASSERT_TRUE(env.add_thread());
    MemoryCompressedSource src(mt_data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
      if (env.exists()) env.destroy();
    };
    kdu_stripe_decompressor decomp;
    const int h8[1] = {8};
    const int h16[1] = {16};
    std::vector<kdu_core::kdu_byte> buf(48 * 16);
    kdu_core::kdu_byte* pb[1] = {buf.data()};

    // MT prestage followed immediately by finish() leaves trange.pos.x <
    // scheduled_lim.x, exercising partial-row close_tiles.
    decomp.start(cs, false, false, &env);
    RecordingNotifier n;
    EXPECT_TRUE(decomp.prestage(h16, 0, &n, &env));
    EXPECT_FALSE(decomp.finish());

    ReopenCodestream(src, cs);
    decomp.start(cs, false, false, &env);
    EXPECT_TRUE(decomp.pull_stripe(pb, h8));
    EXPECT_FALSE(decomp.finish());  // Releases remaining tile rows.

    ReopenCodestream(src, cs);
    decomp.start(cs);
    EXPECT_TRUE(decomp.pull_stripe(pb, h8));
    decomp.reset(/*free_memory=*/true);  // Releases active partial_tiles.
  }

  // Large tile height (>128) MT dbuf cap (env_dbuf_height == 64) + taller
  // second tile row (Sorigin={8,8}, Stiles={16,16}).
  {
    const auto tall_data = EncodeTestCodestream(
        32, 140, 1, 8, /*reversible=*/true, false, "Stiles={16,140}", "");
    kdu_core::kdu_thread_env env;
    env.create();
    ASSERT_TRUE(env.add_thread());
    MemoryCompressedSource src(tall_data);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
      if (env.exists()) env.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs, false, false, &env, nullptr, /*env_dbuf_height=*/-1);
    const int h140[1] = {140};
    std::vector<kdu_core::kdu_byte> buf(32 * 140);
    kdu_core::kdu_byte* pb[1] = {buf.data()};
    EXPECT_FALSE(decomp.pull_stripe(pb, h140));
    EXPECT_TRUE(decomp.finish());
  }
  {
    const auto staggered = EncodeTestCodestream(
        24, 24, 1, 8, /*reversible=*/true, false,
        "Stiles={16,16}\nSorigin={8,8}\nStile_origin={0,0}", "");
    MemoryCompressedSource src(staggered);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    kdu_stripe_decompressor decomp;
    decomp.start(cs);
    int rec[1] = {};
    int max_h[1] = {};
    EXPECT_TRUE(decomp.get_recommended_stripe_heights(4, 16, rec, max_h));
    EXPECT_EQ(rec[0], 8);
    EXPECT_EQ(max_h[0], 16);
    decomp.finish();
  }
}

}  // namespace
}  // namespace kdu_supp
