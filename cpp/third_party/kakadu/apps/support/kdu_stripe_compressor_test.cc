// Unit tests for
// third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.{h,cpp}.

#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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
  void flush(bool = false) override {}
};

void ParseMultiParams(kdu_core::kdu_params* params, const char* text) {
  if (text == nullptr) return;
  std::string_view sv(text);
  size_t pos = 0;
  while (pos < sv.size()) {
    size_t start = sv.find_first_not_of(" \t\n", pos);
    if (start == std::string_view::npos) break;
    size_t end = sv.find_first_of(" \t\n", start);
    std::string token(sv.substr(start, end - start));
    params->parse_string(token.c_str());
    if (end == std::string_view::npos) break;
    pos = end;
  }
}

// Configures SIZ (and optional tile/subsampling/COD strings) and creates a
// kdu_codestream ready for kdu_stripe_compressor::start.
kdu_core::kdu_codestream CreateEncoderCodestream(
    kdu_core::kdu_compressed_target* target, int width, int height,
    int num_components, int bit_depth, bool reversible,
    const char* extra_siz_text = nullptr, const char* extra_cod_text = nullptr,
    bool ycc = false) {
  kdu_core::siz_params siz;
  siz.set(Scomponents, 0, 0, num_components);
  const bool has_sampling =
      extra_siz_text != nullptr &&
      std::string_view(extra_siz_text).find("Ssampling") !=
          std::string_view::npos;
  const char* size_attr = has_sampling ? Ssize : Sdims;
  siz.set(size_attr, 0, 0, height);
  siz.set(size_attr, 0, 1, width);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, bit_depth);
  ParseMultiParams(&siz, extra_siz_text);
  kdu_core::kdu_params* siz_p = &siz;
  siz_p->finalize();

  kdu_core::kdu_codestream cs;
  cs.create(&siz, target);
  auto cod = cs.access_siz()->access_cluster(COD_params);
  cod->set(Creversible, 0, 0, reversible);
  cod->set(Cycc, 0, 0, ycc && num_components >= 3);
  ParseMultiParams(cs.access_siz(), extra_cod_text);
  return cs;
}

class KduStripeCompressorTest : public ::testing::Test {
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

TEST_F(KduStripeCompressorTest, BytesWordsDwordsFloatsAllPrecisionPaths) {
  // Exercise transfer_bytes, transfer_words, transfer_dwords, and
  // transfer_floats across:
  //   - buf16 irreversible (!is_absolute)
  //   - buf16 reversible (is_absolute): src<orig, src>orig, src==orig
  //   - buf32 irreversible (force_precise=true)
  //   - buf32 reversible (force_precise=true): src<orig, src>orig, src==orig
  //   - buf64 reversible (Sprecision=31 -> KDU_LINE_BUF_VPRECISE)
  struct Case {
    int codestream_bits;
    int buffer_bits;
    bool reversible;
    bool force_precise;
    bool want_fastest;
    bool is_signed;
  };
  constexpr std::array<Case, 15> kCases = {{
      {8, 8, false, false, true, false},
      {8, 12, false, false, false, true},
      {8, 6, true, false, false, false},
      {6, 8, true, false, false, false},
      {8, 8, true, false, false, false},
      {8, 8, false, true, false, false},
      {8, 6, true, true, false, false},
      {6, 8, true, true, false, false},
      {8, 8, true, true, false, false},
      {12, 12, false, true, false, true},
      {12, 12, false, true, false, false},
      {12, 10, true, true, false, false},
      {10, 12, true, true, false, true},
      {31, 8, true, true, false, false},
      {31, 12, true, true, false, true},
  }};

  constexpr int kW = 16;
  constexpr int kH = 8;

  for (const Case& cfg : kCases) {
    // Use sample_gap = 2 to ensure scalar transfer_* functions execute on both
    // x86_64 (where SIMD handles packed buffers) and ARM.
    constexpr int kGap = 2;
    const int prec = cfg.buffer_bits;
    const int heights[1] = {kH};
    const int gaps[1] = {kGap};
    const int precs[1] = {prec};
    const bool signed_arr[1] = {cfg.is_signed};

    // 1. Byte buffer (for <= 8-bit inputs)
    if (cfg.buffer_bits <= 8) {
      MemoryCompressedTarget target;
      kdu_core::kdu_codestream cs = CreateEncoderCodestream(
          &target, kW, kH, 1, cfg.codestream_bits, cfg.reversible);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      kdu_stripe_compressor comp;
      comp.start(cs, 0, nullptr, nullptr, 0, false, cfg.force_precise, false,
                 0.0, 0, cfg.want_fastest);
      std::vector<kdu_core::kdu_byte> buf(kW * kH * kGap, 17);
      kdu_core::kdu_byte* bufs[1] = {buf.data()};
      EXPECT_FALSE(comp.push_stripe(bufs, heights, gaps, nullptr, precs, 0));
      EXPECT_TRUE(comp.finish());
      EXPECT_FALSE(target.data().empty());
    }

    // 2. 16-bit word buffer (for <= 16-bit inputs)
    if (cfg.buffer_bits <= 16) {
      MemoryCompressedTarget target;
      kdu_core::kdu_codestream cs = CreateEncoderCodestream(
          &target, kW, kH, 1, cfg.codestream_bits, cfg.reversible);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      kdu_stripe_compressor comp;
      comp.start(cs, 0, nullptr, nullptr, 0, false, cfg.force_precise, false,
                 0.0, 0, cfg.want_fastest);
      const kdu_core::kdu_int16 word_val =
          (cfg.is_signed || prec <= 1)
              ? 15
              : static_cast<kdu_core::kdu_int16>(1 << (prec - 1));
      std::vector<kdu_core::kdu_int16> buf(kW * kH * kGap, word_val);
      kdu_core::kdu_int16* bufs[1] = {buf.data()};
      EXPECT_FALSE(
          comp.push_stripe(bufs, heights, gaps, nullptr, precs, signed_arr, 0));
      EXPECT_TRUE(comp.finish());
      EXPECT_FALSE(target.data().empty());
    }

    // 3. 32-bit dword buffer
    {
      MemoryCompressedTarget target;
      kdu_core::kdu_codestream cs = CreateEncoderCodestream(
          &target, kW, kH, 1, cfg.codestream_bits, cfg.reversible);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      kdu_stripe_compressor comp;
      comp.start(cs, 0, nullptr, nullptr, 0, false, cfg.force_precise, false,
                 0.0, 0, cfg.want_fastest);
      const kdu_core::kdu_int32 dword_val =
          (cfg.is_signed || prec <= 1) ? 21 : (1 << (prec - 1));
      std::vector<kdu_core::kdu_int32> buf(kW * kH * kGap, dword_val);
      kdu_core::kdu_int32* bufs[1] = {buf.data()};
      EXPECT_FALSE(
          comp.push_stripe(bufs, heights, gaps, nullptr, precs, signed_arr, 0));
      EXPECT_TRUE(comp.finish());
      EXPECT_FALSE(target.data().empty());
    }

    // 4. Float buffer
    {
      MemoryCompressedTarget target;
      kdu_core::kdu_codestream cs = CreateEncoderCodestream(
          &target, kW, kH, 1, cfg.codestream_bits, cfg.reversible);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      kdu_stripe_compressor comp;
      comp.start(cs, 0, nullptr, nullptr, 0, false, cfg.force_precise, false,
                 0.0, 0, cfg.want_fastest);
      std::vector<float> buf(kW * kH * kGap, cfg.is_signed ? -0.25f : 12.5f);
      float* bufs[1] = {buf.data()};
      EXPECT_FALSE(
          comp.push_stripe(bufs, heights, gaps, nullptr, precs, signed_arr, 0));
      EXPECT_TRUE(comp.finish());
      EXPECT_FALSE(target.data().empty());
    }
  }
}

TEST_F(KduStripeCompressorTest, SingleBufferOverloadsAndSimdInterleave) {
  constexpr int kW = 32;
  constexpr int kH = 16;

  // Test single-buffer push_stripe overloads for byte, int16, int32, and float,
  // including tightly interleaved RGB (SIMD group), reverse BGR (base_ptr
  // swap), duplicate offsets, gapped offsets, and null optional params.
  struct SingleBufPattern {
    bool use_offsets;
    std::array<int, 3> offsets;
    int sample_gap;
    bool subsampled;
    bool reversible;
  };
  constexpr std::array<SingleBufPattern, 5> kPatterns = {{
      {false, {0, 1, 2}, 0, false, false},  // auto offsets + sample_gap=3 SIMD
      {true, {2, 1, 0}, 3, false, true},    // reversed BGR SIMD interleave
      {true, {0, 0, 1}, 3, false, false},   // duplicate offset breaks SIMD grp
      {true, {0, 1, 4}, 3, false, false},   // out-of-range offset breaks grp
      {false, {0, 1, 2}, 0, true, false},   // Ssampling mismatch breaks grp
  }};

  for (const auto& pat : kPatterns) {
    const char* extra_siz =
        pat.subsampled ? "Ssampling={1,1},{2,2},{2,2}" : nullptr;
    const int heights[3] = {kH, pat.subsampled ? kH / 2 : kH,
                            pat.subsampled ? kH / 2 : kH};
    const int gaps[3] = {3, 3, 3};
    const int* gaps_ptr = pat.use_offsets ? gaps : nullptr;
    const int* off_ptr = pat.use_offsets ? pat.offsets.data() : nullptr;

    // Bytes single buffer + mem_configure
    {
      MemoryCompressedTarget target;
      kdu_core::kdu_codestream cs = CreateEncoderCodestream(
          &target, kW, kH, 3, 8, pat.reversible, extra_siz);
      absl::Cleanup cs_cleanup = [&] {
        if (cs.exists()) cs.destroy();
      };
      kdu_core::kdu_membroker broker(1LL << 22);
      kdu_stripe_compressor comp;
      comp.mem_configure(&broker, KDU_SAMPLE_ALLOCATOR_DEF_FRAG_BITS + 1);
      comp.start(cs);
      std::vector<kdu_core::kdu_byte> buf(kW * kH * 5 + 16, 128);
      EXPECT_FALSE(comp.push_stripe(buf.data(), heights, off_ptr, gaps_ptr));
      EXPECT_TRUE(comp.finish());
      // Reset before broker destruction.
      comp.mem_configure(nullptr, KDU_SAMPLE_ALLOCATOR_DEF_FRAG_BITS);
    }

    if (!pat.subsampled) {
      // Words single buffer with default null optional params
      {
        MemoryCompressedTarget target;
        kdu_core::kdu_codestream cs =
            CreateEncoderCodestream(&target, kW, kH, 3, 12, pat.reversible);
        absl::Cleanup cs_cleanup = [&] {
          if (cs.exists()) cs.destroy();
        };
        kdu_stripe_compressor comp;
        comp.start(cs);
        std::vector<kdu_core::kdu_int16> buf(kW * kH * 5 + 16, 100);
        EXPECT_FALSE(comp.push_stripe(buf.data(), heights, off_ptr, gaps_ptr));
        EXPECT_TRUE(comp.finish());
      }

      // Dwords single buffer
      {
        MemoryCompressedTarget target;
        kdu_core::kdu_codestream cs =
            CreateEncoderCodestream(&target, kW, kH, 3, 12, pat.reversible);
        absl::Cleanup cs_cleanup = [&] {
          if (cs.exists()) cs.destroy();
        };
        kdu_stripe_compressor comp;
        comp.start(cs);
        std::vector<kdu_core::kdu_int32> buf(kW * kH * 5 + 16, 200);
        EXPECT_FALSE(comp.push_stripe(buf.data(), heights, off_ptr, gaps_ptr));
        EXPECT_TRUE(comp.finish());
      }

      // Floats single buffer with clamped precision bounds (-64..64)
      {
        MemoryCompressedTarget target;
        kdu_core::kdu_codestream cs =
            CreateEncoderCodestream(&target, kW, kH, 3, 8, pat.reversible);
        absl::Cleanup cs_cleanup = [&] {
          if (cs.exists()) cs.destroy();
        };
        kdu_stripe_compressor comp;
        comp.start(cs);
        std::vector<float> buf(kW * kH * 5 + 16, 0.0f);
        const int extreme_precs[3] = {-99, 99, 8};
        EXPECT_FALSE(comp.push_stripe(buf.data(), heights, off_ptr, gaps_ptr,
                                      nullptr, extreme_precs));
        EXPECT_TRUE(comp.finish());
      }
    }
  }
}

TEST_F(KduStripeCompressorTest,
       MultiTileStripeHeightsIncrementalFlushAndOptions) {
  constexpr int kW = 48;
  constexpr int kH = 48;
  MemoryCompressedTarget target;
  // 3x3 tiles of 16x16 with subsampling on components 1 & 2,
  // Clayers=2 Corder=PCRL (incremental flushing), and explicit
  // num_components=3.
  kdu_core::kdu_codestream cs =
      CreateEncoderCodestream(&target, kW, kH, 3, 8, false,
                              "Stiles={16,16}\nSsampling={1,1},{2,2},{2,2}",
                              "Clayers=2 Corder=PCRL Clevels=2");
  absl::Cleanup cs_cleanup = [&] {
    if (cs.exists()) cs.destroy();
  };

  kdu_core::kdu_cplex_share cplex_share;
  kdu_core::kdu_push_pull_params extra;
  extra.set_cplex_share(&cplex_share);

  kdu_stripe_compressor comp;
  const kdu_core::kdu_long layer_sizes[3] = {800, 1600, 3200};
  const kdu_core::kdu_uint16 layer_slopes[3] = {50000, 45000, 40000};
  // num_layer_specs=3 exceeds Clayers=2 -> clamped to 2.
  comp.start(cs, 3, layer_sizes, layer_slopes, /*min_slope_threshold=*/0,
             /*no_auto_complexity_control=*/false, /*force_precise=*/false,
             /*record_layer_info_in_comment=*/true, /*size_tolerance=*/0.05,
             /*num_components=*/3, /*want_fastest=*/false, nullptr, nullptr, -1,
             -1, /*trim_to_rate=*/true, KDU_FLUSH_USES_THRESHOLDS_AND_SIZES,
             &extra);

  int rec[3] = {};
  int max_h[3] = {};
  // Exercise preferred_min < 1 and absolute_max < preferred_min clamping,
  // plus scaling down to limit on multi-tile width (num_tiles.x == 3).
  EXPECT_TRUE(comp.get_recommended_stripe_heights(0, -1, rec, max_h));
  EXPECT_EQ(rec[0], 1);
  EXPECT_EQ(max_h[0], 1);

  int next_rec[3] = {};
  EXPECT_TRUE(comp.get_next_stripe_heights(8, 12, rec, next_rec));
  EXPECT_EQ(next_rec[0], 1);

  std::vector<kdu_core::kdu_byte> c0(kW * 12, 50);
  std::vector<kdu_core::kdu_byte> c1(kW * 6, 128);
  std::vector<kdu_core::kdu_byte> c2(kW * 6, 200);
  kdu_core::kdu_byte* bufs[3] = {c0.data(), c1.data(), c2.data()};

  // Push small stripes with flush_period=8 across 3x3 tiles so incremental
  // flushing and flush_on_tile_boundary both trigger.
  int pushed_rows = 0;
  while (pushed_rows < kH) {
    const int rem0 = std::min(6, kH - pushed_rows);
    int heights[3] = {rem0, rem0 / 2, rem0 / 2};
    (void)comp.get_next_stripe_heights(8, 12, heights, next_rec);
    const bool more = comp.push_stripe(bufs, heights, nullptr, nullptr, nullptr,
                                       /*flush_period=*/8);
    pushed_rows += rem0;
    EXPECT_EQ(more, pushed_rows < kH);
  }
  // Extra push_stripe after all_done returns false immediately; also check
  // empty get_next_stripe_heights at EOF.
  int zero_h[3] = {0, 0, 0};
  EXPECT_FALSE(comp.get_next_stripe_heights(8, 12, zero_h, next_rec));
  EXPECT_FALSE(comp.push_stripe(bufs, zero_h));

  kdu_core::kdu_long out_sizes[4] = {1, 2, 3, 4};
  kdu_core::kdu_uint16 out_slopes[4] = {1, 2, 3, 4};
  EXPECT_TRUE(comp.finish(4, out_sizes, out_slopes));
  EXPECT_EQ(out_sizes[2], 0);
  EXPECT_EQ(out_slopes[2], 0);
  EXPECT_EQ(out_sizes[3], 0);
  EXPECT_EQ(out_slopes[3], 0);

  // Subsequent finish / get_next_stripe_heights when idle return false.
  EXPECT_FALSE(comp.finish());
  EXPECT_FALSE(comp.get_next_stripe_heights(4, 8, zero_h, next_rec));
  EXPECT_FALSE(target.data().empty());
}

TEST_F(KduStripeCompressorTest,
       MultiThreadedMultiTileAutoFlushReuseAndAbortReset) {
  constexpr int kW = 48;
  constexpr int kH = 48;

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&] {
    env.terminate(nullptr, true);
    env.destroy();
  });

  kdu_stripe_compressor comp;

  // Frame 1: multi-threaded 3x3 tiles with auto_flush, reused across
  // full tile rows (flush_period=4 triggers both tc_trigger and incr_trigger).
  {
    MemoryCompressedTarget target;
    kdu_core::kdu_codestream cs =
        CreateEncoderCodestream(&target, kW, kH, 1, 8, false, "Stiles={16,16}");
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    const kdu_core::kdu_long prev_seq = comp.get_set_next_queue_sequence(10);
    EXPECT_GE(comp.get_set_next_queue_sequence(0),
              std::max<kdu_core::kdu_long>(10, prev_seq));
    const kdu_core::kdu_uint16 slopes[1] = {42000};
    comp.start(cs, 1, nullptr, slopes, 40000, false, false, false, 0.0, 0,
               false, &env, nullptr, /*env_dbuf_height=*/-1,
               /*env_tile_concurrency=*/0);
    int rec[1] = {};
    // Exercise get_next_stripe_heights before get_recommended_stripe_heights,
    // including clamping preferred_min/absolute_max < 1.
    int cur_h[1] = {8};
    EXPECT_TRUE(comp.get_next_stripe_heights(0, 0, cur_h, rec));
    EXPECT_TRUE(comp.get_next_stripe_heights(16, 24, cur_h, rec));

    std::vector<kdu_core::kdu_byte> full_tile_row(kW * 16, 77);
    kdu_core::kdu_byte* bufs[1] = {full_tile_row.data()};
    const int row_h[1] = {16};
    EXPECT_TRUE(comp.push_stripe(bufs, row_h, nullptr, nullptr, nullptr, 4));
    EXPECT_TRUE(comp.push_stripe(bufs, row_h, nullptr, nullptr, nullptr, 4));
    EXPECT_FALSE(comp.push_stripe(bufs, row_h, nullptr, nullptr, nullptr, 4));
    EXPECT_TRUE(comp.finish(0, nullptr, nullptr, &env));
    EXPECT_FALSE(target.data().empty());
  }

  // Frame 2: multi-threaded partial push then terminate env + reset(false) to
  // close scheduled unopened tiles and keep free_tiles/free_queues for reuse.
  {
    kdu_core::kdu_thread_env abort_env;
    abort_env.create();
    ASSERT_TRUE(abort_env.add_thread());
    kdu_core::kdu_thread_queue abort_queue;
    abort_env.attach_queue(&abort_queue, nullptr, "abort_q");
    MemoryCompressedTarget target;
    kdu_core::kdu_codestream cs =
        CreateEncoderCodestream(&target, kW, kH, 1, 8, false, "Stiles={16,16}");
    absl::Cleanup cleanup = [&] {
      if (cs.exists()) cs.destroy();
      if (abort_env.exists()) abort_env.destroy();
    };
    comp.start(cs, 0, nullptr, nullptr, 0, true, false, false, 0.0, 0, false,
               &abort_env, &abort_queue);
    std::vector<kdu_core::kdu_byte> partial(kW * 16, 99);
    kdu_core::kdu_byte* bufs[1] = {partial.data()};
    const int row_h[1] = {16};
    EXPECT_TRUE(comp.push_stripe(bufs, row_h, nullptr, nullptr, nullptr, 0));
    abort_env.terminate(&abort_queue, false);
    abort_env.cs_terminate(cs);
    abort_env.destroy();
    // Abort early before rows 1 & 2 are processed.
    comp.reset(/*free_memory=*/false);
    // Calling finish without completing returns false.
    EXPECT_FALSE(comp.finish());
  }

  // Frame 3: reuse free_tiles & free_queues from Frame 2 with another
  // multi-threaded pass and reset(true) at end.
  {
    MemoryCompressedTarget target;
    kdu_core::kdu_codestream cs =
        CreateEncoderCodestream(&target, kW, kH, 1, 8, false, "Stiles={16,16}");
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    comp.start(cs, 0, nullptr, nullptr, 0, true, false, false, 0.0, 0, false,
               &env);
    std::vector<kdu_core::kdu_byte> row(kW * 16, 33);
    kdu_core::kdu_byte* bufs[1] = {row.data()};
    const int row_h[1] = {16};
    EXPECT_TRUE(comp.push_stripe(bufs, row_h));
    EXPECT_TRUE(comp.push_stripe(bufs, row_h));
    EXPECT_FALSE(comp.push_stripe(bufs, row_h));
    EXPECT_TRUE(comp.finish());
    comp.reset(/*free_memory=*/true);
  }
}

TEST_F(KduStripeCompressorTest, ErrorPathsAndDisproportionatePush) {
  kdu_stripe_compressor comp;
  int rec[2] = {};
  // Calling get_recommended_stripe_heights before start throws.
  EXPECT_THROW(comp.get_recommended_stripe_heights(4, 8, rec, nullptr),
               kdu_core::kdu_exception);

  // Calling start with num_components exceeding codestream components throws.
  {
    MemoryCompressedTarget target;
    kdu_core::kdu_codestream cs =
        CreateEncoderCodestream(&target, 16, 16, 1, 8, false);
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    EXPECT_THROW(
        comp.start(cs, 0, nullptr, nullptr, 0, false, false, false, 0.0, 2),
        kdu_core::kdu_exception);
    comp.reset(/*free_memory=*/true);
  }

  // Disproportionate component stripe heights crossing tile boundaries throws.
  {
    MemoryCompressedTarget target;
    kdu_core::kdu_codestream cs =
        CreateEncoderCodestream(&target, 16, 32, 2, 8, false, "Stiles={16,16}");
    absl::Cleanup cs_cleanup = [&] {
      if (cs.exists()) cs.destroy();
    };
    comp.start(cs);
    std::vector<kdu_core::kdu_byte> c0(16 * 32, 10);
    std::vector<kdu_core::kdu_byte> c1(16 * 4, 20);
    kdu_core::kdu_byte* bufs[2] = {c0.data(), c1.data()};
    const int disprop_heights[2] = {32, 4};
    EXPECT_THROW(comp.push_stripe(bufs, disprop_heights),
                 kdu_core::kdu_exception);
    comp.reset(/*free_memory=*/true);
  }
}

}  // namespace
}  // namespace kdu_supp
