// Unit tests for
// third_party/kakadu/vendor/coresys/coding/cplex_analysis.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

class CplexAnalysisTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const error_handler = new ThrowingErrorHandler();
    static auto* const warning_handler = new QuietWarningHandler();
    kdu_customize_errors(error_handler);
    kdu_customize_warnings(warning_handler);
  }
};

const ::testing::Environment* const kCplexTestEnv =
    ::testing::AddGlobalTestEnvironment(new CplexAnalysisTestEnvironment());

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
      tile.close(nullptr);
    }
  }
};

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;
  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy(nullptr);
    }
  }
};

struct EncodeConfig {
  int width = 96;
  int height = 96;
  int precision = 8;
  bool reversible = false;
  int clevels = 2;
  kdu_long max_bytes = 2048;
  bool use_scbr = false;
  bool use_fragments = false;
  int fragment_width = 32;
  int fragment_height = 2;
  int fragment_pad = 8;
  int pattern_variant = 0;
  bool early_destroy_tile = false;
  bool transpose = false;
  std::vector<std::string> extra_cod_params;
};

std::vector<int16_t> BuildPatternSamples(int width, int height, int variant) {
  std::vector<int16_t> samples(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      double val = 0.0;
      if (variant == 0) {
        val = 50.0 * std::sin(0.15 * x + 0.25 * y) +
              20.0 * std::cos(0.4 * x - 0.1 * y);
      } else if (variant == 1) {
        // Similar top, slightly shifted phase to keep temporal correlation.
        val = 52.0 * std::sin(0.15 * x + 0.25 * y + 0.05) +
              20.0 * std::cos(0.4 * x - 0.1 * y);
      } else {
        // Strong step + high-frequency modulation to trigger spatial branch.
        const int checker = (((x ^ y) & 1) != 0) ? 1 : -1;
        val = (y < height / 2) ? (110.0 * checker) : (4.0 * ((x + y) & 3));
      }
      samples[static_cast<size_t>(y) * width + x] = static_cast<int16_t>(val);
    }
  }
  return samples;
}

std::vector<kdu_byte> RunCplexEncode(const EncodeConfig& cfg,
                                     kdu_cplex_share* share,
                                     kdu_cplex_bkgnd* bkgnd) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  cs.siz.set(Scomponents, 0, 0, 1);
  cs.siz.set(Sdims, 0, 0, cfg.height);
  cs.siz.set(Sdims, 0, 1, cfg.width);
  cs.siz.set(Sprecision, 0, 0, cfg.precision);
  cs.siz.set(Ssigned, 0, 0, true);
  if (cfg.use_scbr) {
    EXPECT_TRUE(cs.siz.parse_string("Scbr={1,64}"));
  }
  static_cast<kdu_params*>(&cs.siz)->finalize();

  cs.codestream.create(&cs.siz, &target);
  if (cfg.transpose) {
    cs.codestream.change_appearance(true, false, false);
  }
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(cfg.reversible ? "Creversible=yes"
                                                  : "Creversible=no"));
  EXPECT_TRUE(
      params->parse_string(("Clevels=" + std::to_string(cfg.clevels)).c_str()));
  if (cfg.reversible) {
    EXPECT_TRUE(params->parse_string("Ckernels=W5X3"));
  }
  if (cfg.use_scbr) {
    EXPECT_TRUE(params->parse_string("Clayers=1"));
    EXPECT_TRUE(params->parse_string("Corder=PCRL"));
    EXPECT_TRUE(params->parse_string("Cprecincts={8,8}"));
    EXPECT_TRUE(params->parse_string("Cblk={8,8}"));
  }
  for (const std::string& p : cfg.extra_cod_params) {
    EXPECT_TRUE(params->parse_string(p.c_str())) << p;
  }
  params->finalize_all();

  if (cfg.use_fragments) {
    cs.codestream.configure_simultaneous_processing_fragments(
        cfg.fragment_width, cfg.fragment_height, cfg.fragment_pad);
  }
  if (cfg.max_bytes > 0) {
    cs.codestream.set_max_bytes(cfg.max_bytes);
  }

  const std::vector<int16_t> samples =
      BuildPatternSamples(cfg.width, cfg.height, cfg.pattern_variant);

  {
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    EXPECT_TRUE(tile.tile.exists());

    kdu_push_pull_params extra_pp;
    extra_pp.set_cplex_share(share);
    extra_pp.set_cplex_bkgnd(bkgnd);
    if (cfg.use_fragments) {
      extra_pp.set_spatial_xform_dbuf_rows(32);
    }

    ScopedMultiAnalysis multi;
    multi.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                        KDU_MULTI_XFORM_DEFAULT_FLAGS, nullptr, 4, nullptr,
                        &extra_pp);
    const int push_rows =
        cfg.early_destroy_tile ? std::min(4, cfg.height) : cfg.height;
    const double inv_scale = std::ldexp(1.0, -cfg.precision);
    for (int y = 0; y < push_rows; ++y) {
      kdu_line_buf* line = multi.engine.exchange_line(0, nullptr, nullptr);
      EXPECT_NE(line, nullptr);
      if (line == nullptr) {
        break;
      }
      const int16_t* row = samples.data() + static_cast<size_t>(y) * cfg.width;
      if (kdu_sample32* dst32 = line->get_buf32()) {
        for (int x = 0; x < cfg.width; ++x) {
          if (cfg.reversible) {
            dst32[x].ival = row[x];
          } else {
            dst32[x].fval = static_cast<float>(row[x] * inv_scale);
          }
        }
      } else if (kdu_sample16* dst16 = line->get_buf16()) {
        for (int x = 0; x < cfg.width; ++x) {
          if (cfg.reversible) {
            dst16[x].ival = row[x];
          } else {
            dst16[x].ival = static_cast<kdu_int16>(std::llround(
                row[x] * std::ldexp(1.0, KDU_FIX_POINT - cfg.precision)));
          }
        }
      }
      multi.engine.exchange_line(0, line, nullptr);
    }
    if (cfg.early_destroy_tile) {
      return bitstream;
    }
  }

  kdu_long flush_limit = (cfg.max_bytes > 0) ? cfg.max_bytes : 0;
  cs.codestream.flush(&flush_limit, 1, nullptr, true, true);
  cs.codestream.destroy();
  return bitstream;
}

TEST(CplexAnalysisTest, InactiveAndDirectCplexShareCfragAndTile) {
  // Default-constructed inactive kdu_cplex_analysis and !operator / service /
  // finish / link_descendant.
  {
    kdu_cplex_analysis cplex;
    kdu_cplex_analysis descendant;
    EXPECT_FALSE(cplex.is_active());
    EXPECT_TRUE(!cplex);
    cplex.link_descendant(&descendant);
    cplex.service(nullptr);
    cplex.finish(nullptr);
  }

  // init() when Cplex=NONE (and attach when inactive): stays inactive and
  // attach returns nullptr.
  {
    std::vector<kdu_byte> bs;
    VectorTarget target(bs);
    ScopedCodestream cs;
    cs.siz.set(Scomponents, 0, 0, 1);
    cs.siz.set(Sdims, 0, 0, 32);
    cs.siz.set(Sdims, 0, 1, 32);
    cs.siz.set(Sprecision, 0, 0, 8);
    cs.siz.set(Ssigned, 0, 0, true);
    static_cast<kdu_params*>(&cs.siz)->finalize();
    cs.codestream.create(&cs.siz, &target);
    cs.codestream.access_siz()->finalize_all();
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_tile_comp tc = tile.tile.access_component(0);
    kdu_resolution top_res = tc.access_resolution();
    kdu_node node = top_res.access_node();
    kdu_subband subband =
        top_res.access_subband((top_res.which() == 0) ? LL_BAND : HL_BAND);

    kdu_cplex_analysis cplex;
    cplex.init(node, nullptr, nullptr);
    EXPECT_FALSE(cplex.is_active());
    int early_delay = 0;
    int late_delay = 0;
    EXPECT_EQ(cplex.attach(subband, 16, early_delay, late_delay), nullptr);
  }

  // kdu_cplex_share bounds and clamping.
  {
    kdu_cplex_share share;
    share.set_max_tiles(4);
    share.set_max_tiles(2);  // Already <= max_tiles.
    EXPECT_NE(share.get_tile(0), nullptr);
    EXPECT_NE(share.get_tile(10), nullptr);  // Dynamic expansion.
    EXPECT_EQ(share.get_tile(10), share.get_tile(10));
    EXPECT_EQ(share.get_tile(1 << 16), nullptr);  // Exceeds 2^16 limit.
    share.set_max_tiles((1 << 16) + 5);           // Clamped to 2^16.
    EXPECT_NE(share.get_tile(100), nullptr);
  }

  // kdu_cplex_share_tile bounds growing and reuse.
  {
    kdu_cplex_share_tile tile_share;
    tile_share.configure_cfrag_bounds(2, 2);
    tile_share.configure_cfrag_bounds(1, 1);  // No-op when <= current bounds.
    kdu_cplex_share_cfrag* f0 = tile_share.get_cfrag(0, 0);
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(tile_share.get_cfrag(0, 0), f0);
    tile_share.configure_cfrag_bounds(3, 4);  // Regrows backing matrix cleanly.
    EXPECT_EQ(tile_share.get_cfrag(0, 0), f0);
    EXPECT_NE(tile_share.get_cfrag(2, 3), nullptr);
  }

  // kdu_cplex_share_cfrag: 2-attachment limit, slot replacement, and busy-slot
  // deferred cleanup.
  {
    kdu_cplex_share_cfrag cfrag;
    bool just_started = false;
    float* p1 = cfrag.attach(0, 3, just_started);
    ASSERT_NE(p1, nullptr);
    EXPECT_TRUE(just_started);

    float* p2 = cfrag.attach(0, 3, just_started);
    EXPECT_EQ(p2, p1);
    EXPECT_FALSE(just_started);

    // Third simultaneous compatible attachment exceeds slot cap of 2.
    EXPECT_EQ(cfrag.attach(0, 3, just_started), nullptr);
    cfrag.detach(p1);
    cfrag.detach(p2);

    // Incompatible layout after detaching all references cleans up immediately
    // on attach.
    float* p3 = cfrag.attach(0, 5, just_started);
    ASSERT_NE(p3, nullptr);
    EXPECT_TRUE(just_started);

    // Occupy second slot with another layout, then try a third distinct layout
    // while both slots have active attachments: returns nullptr and marks both
    // slots for deferred cleanup inside detach().
    float* p4 = cfrag.attach(1, 7, just_started);
    ASSERT_NE(p4, nullptr);
    EXPECT_TRUE(just_started);
    EXPECT_EQ(cfrag.attach(2, 9, just_started), nullptr);

    cfrag.detach(p3);
    cfrag.detach(p4);

    // Now slots are freed again and a fresh attach succeeds.
    float* p5 = cfrag.attach(2, 9, just_started);
    ASSERT_NE(p5, nullptr);
    EXPECT_TRUE(just_started);
    cfrag.detach(p5);
  }
}

TEST(CplexAnalysisTest, CplexBkgndLifecycleReadOnlyAndReadWriteStats) {
  // Read-only background store tests.
  {
    kdu_cplex_bkgnd bkgnd;
    EXPECT_FALSE(bkgnd.get_read_only());
    kdu_uint32 hash = 0;
    EXPECT_EQ(bkgnd.get_num_components(hash), 0);
    EXPECT_EQ(bkgnd.access_component(0), nullptr);

    kdu_uint32 num_levels = 0, inc_sub = 0, inc_comp = 0, inc_rel = 0;
    kdu_uint32 store_calls = 0, inc_store = 0;
    EXPECT_FALSE(bkgnd.get_info(num_levels, inc_sub, inc_comp, inc_rel,
                                store_calls, inc_store));

    EXPECT_FALSE(bkgnd.configure(0, 1));
    EXPECT_FALSE(bkgnd.configure(16385, 1));

    EXPECT_TRUE(bkgnd.set_read_only());
    EXPECT_TRUE(bkgnd.get_read_only());
    ASSERT_TRUE(bkgnd.configure(2, 0x1234U));
    EXPECT_FALSE(bkgnd.set_read_only());  // Already configured.
    EXPECT_FALSE(bkgnd.configure(3, 0x1234U));
    EXPECT_FALSE(bkgnd.configure(2, 0x9999U));
    EXPECT_EQ(bkgnd.get_num_components(hash), 2);
    EXPECT_EQ(hash, 0x1234U);

    EXPECT_EQ(bkgnd.access_component(-1), nullptr);
    EXPECT_EQ(bkgnd.access_component(2), nullptr);
    kdu_cplex_bkgnd_comp* comp0 = bkgnd.access_component(0);
    ASSERT_NE(comp0, nullptr);
    EXPECT_TRUE(comp0->get_read_only());
    EXPECT_EQ(comp0->access_level(-1), nullptr);

    // Test >32 clamping to 32 levels and clean destruction.
    ASSERT_TRUE(comp0->configure(35, 0xABCDU, 2, 0));
    EXPECT_FALSE(comp0->get_reversible());
    kdu_uint32 dwt_hash = 0;
    int gbits = 0;
    int rev_prec = 0;
    EXPECT_EQ(comp0->get_num_levels(dwt_hash, gbits, rev_prec), 32);
    EXPECT_EQ(dwt_hash, 0xABCDU);
    EXPECT_EQ(gbits, 2);
    EXPECT_EQ(rev_prec, 0);
    EXPECT_EQ(comp0->access_level(33), nullptr);
    EXPECT_FALSE(comp0->configure(32, 0xABCDU, 2, 8));
    EXPECT_FALSE(comp0->configure(-1, 0xABCDU, 2, 0));

    kdu_cplex_bkgnd_level* lev1 = comp0->access_level(1);
    ASSERT_NE(lev1, nullptr);
    kdu_uint32 style = 0;
    kdu_coords depth(-1, -1);
    EXPECT_EQ(lev1->get_num_bands(style, depth), 0);
    ASSERT_TRUE(lev1->configure(4, 0, kdu_coords(1, 1)));
    EXPECT_FALSE(lev1->configure(4, 1, kdu_coords(1, 1)));
    EXPECT_EQ(lev1->get_num_bands(style, depth), 4);
    EXPECT_EQ(depth.x, 1);
    EXPECT_EQ(depth.y, 1);

    lev1->note_incompatibility(0);   // LL band: ignored.
    lev1->note_incompatibility(4);   // Out of bounds: ignored.
    lev1->note_incompatibility(2);   // +1
    lev1->note_incompatibility(-1);  // +(num_bands - 1) = +3

    std::array<float, 30> stats{};
    std::array<float, 30> samples{};
    EXPECT_FALSE(
        lev1->store_bkgnd_stats(-1, 0, 8, stats.data(), samples.data()));
    EXPECT_FALSE(lev1->store_bkgnd_stats(1, 0, 8, stats.data(),
                                         samples.data()));  // Zero samples.

    for (int i = 0; i < 10; ++i) {
      stats[i] = static_cast<float>((i + 1) * 4);
      samples[i] = 100.0f;
    }
    EXPECT_TRUE(
        lev1->store_bkgnd_stats(1, 0, 30, stats.data(), samples.data()));
    EXPECT_FALSE(lev1->store_bkgnd_stats(1, 0, 30, stats.data(),
                                         samples.data()));  // Already set.

    EXPECT_EQ(lev1->get_rel_stats(-1, 0), nullptr);
    EXPECT_EQ(lev1->get_rel_stats(2, 0), nullptr);
    EXPECT_NE(lev1->get_rel_stats(1, 50), nullptr);
    EXPECT_EQ(lev1->get_rel_stats(1, 200), nullptr);  // |mu_diff| > 128.

    EXPECT_FALSE(lev1->store_new_stats(1, 0, 8, stats.data(), 100.0f));
    kdu_uint16 out_eps = 0;
    EXPECT_EQ(lev1->get_acc_stats(1, out_eps, 8, stats.data(), samples.data()),
              0);

    kdu_uint32 new_cnt = 0, inc_new_cnt = 0;
    EXPECT_EQ(lev1->get_compatibility_stats(new_cnt, inc_new_cnt), 1U);
    EXPECT_EQ(new_cnt, 0U);
    EXPECT_EQ(inc_new_cnt, 0U);
    EXPECT_TRUE(bkgnd.get_info(num_levels, inc_sub, inc_comp, inc_rel,
                               store_calls, inc_store));
    EXPECT_EQ(inc_sub, 4U);
    EXPECT_EQ(inc_rel, 1U);
  }

  // Read-write reversible background stats store, accumulation, overwrite,
  // and extraction.
  {
    kdu_cplex_bkgnd bkgnd;
    ASSERT_TRUE(bkgnd.configure(2, 0x4321U));
    kdu_cplex_bkgnd_comp* comp0 = bkgnd.access_component(0);
    ASSERT_NE(comp0, nullptr);
    ASSERT_TRUE(comp0->configure(2, 0x7777U, 2, 8));
    EXPECT_TRUE(comp0->get_reversible());

    kdu_cplex_bkgnd_level* lev1 = comp0->access_level(1);
    ASSERT_NE(lev1, nullptr);
    ASSERT_TRUE(lev1->configure(4, 0, kdu_coords(1, 1)));

    std::array<float, 30> stats{};
    std::array<float, 30> samples{};
    for (int i = 0; i < 30; ++i) {
      stats[i] = static_cast<float>(i + 1);
      samples[i] = 50.0f;
    }
    const kdu_uint16 kEps10 = static_cast<kdu_uint16>(10 << 11);
    const kdu_uint16 kEps11 = static_cast<kdu_uint16>(11 << 11);
    ASSERT_TRUE(
        lev1->store_bkgnd_stats(1, kEps10, 6, stats.data(), samples.data()));
    // In reversible mode, eps mismatch (eps_xor != 0) makes get_rel_stats
    // return nullptr.
    EXPECT_EQ(lev1->get_rel_stats(1, kEps11), nullptr);

    // Invalid store_new_stats & get_acc_stats checks.
    kdu_uint16 out_eps = 0;
    EXPECT_FALSE(lev1->store_new_stats(-1, kEps10, 4, stats.data(), 10.0f));
    EXPECT_FALSE(lev1->store_new_stats(1, kEps10, 4, stats.data(), 0.0f));
    EXPECT_EQ(lev1->get_acc_stats(-1, out_eps, 8, stats.data(), samples.data()),
              0);
    EXPECT_EQ(lev1->get_acc_stats(1, out_eps, 8, nullptr, samples.data()), 0);
    EXPECT_EQ(lev1->get_acc_stats(2, out_eps, 8, stats.data(), samples.data()),
              0);

    // Compatible store_new_stats accumulates and increments store count.
    EXPECT_TRUE(lev1->store_new_stats(1, kEps10, 30, stats.data(), 25.0f));
    // Incompatible eps_xor in reversible overwrites acc_stats and increments
    // incompatible_new_stats_count.
    EXPECT_TRUE(lev1->store_new_stats(1, kEps11, 10, stats.data(), 15.0f));
    // Fresh band (no prior rel_eps_mu): first store sets upper 16 bits.
    EXPECT_TRUE(lev1->store_new_stats(2, 0, 8, stats.data(), 10.0f));

    std::array<float, 30> out_stats{};
    std::array<float, 30> out_samples{};
    EXPECT_EQ(lev1->get_acc_stats(1, out_eps, 30, out_stats.data(),
                                  out_samples.data()),
              10);
    EXPECT_EQ(out_eps, kEps11);
    EXPECT_EQ(lev1->get_acc_stats(2, out_eps, 8, out_stats.data(),
                                  out_samples.data()),
              8);

    kdu_uint32 num_levels = 0, inc_sub = 0, inc_comp = 0, inc_rel = 0;
    kdu_uint32 store_calls = 0, inc_store = 0;
    EXPECT_TRUE(bkgnd.get_info(num_levels, inc_sub, inc_comp, inc_rel,
                               store_calls, inc_store));
    EXPECT_EQ(inc_rel, 1U);
    EXPECT_EQ(store_calls, 3U);
    EXPECT_EQ(inc_store, 1U);
  }
}

TEST(CplexAnalysisTest, PlainInterfacesSingleAndMultiSetScbr) {
  // Single CPLEX-set without share/bkgnd, positive delay (Cplex={6,EST,0.25,8})
  // and zero delay (Cplex={6,EST,0.25,0}).
  {
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,8}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, nullptr).empty());
  }

  // Multi-CPLEX-set via Scbr triggers `KDU_CPLEX_REC_MORE_SETS`,
  // `advance_first_incomplete_set`, `cum_weights > 0.25f` renormalization, and
  // multi-set forecast generation on plain_interfaces.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 128;
    cfg.clevels = 1;
    cfg.max_bytes = 16384;
    cfg.use_scbr = true;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, nullptr).empty());
  }

  // Positive delay + Scbr and full-frame negative delay (-1) so `finish()`
  // drains non-empty `completed_head` on plain_interfaces.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 128;
    cfg.clevels = 2;
    cfg.max_bytes = 16384;
    cfg.use_scbr = true;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,16}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, nullptr).empty());
  }
  {
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,-1}", "Qstep=0.002",
                            "Cblk={8,8}"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, nullptr).empty());
  }

  // Standalone active kdu_cplex_analysis: `link_descendant` forwarding during
  // `finish()` and destructor `share_cfrag->detach()` without calling
  // `finish()`.
  {
    std::vector<kdu_byte> bs;
    VectorTarget target(bs);
    ScopedCodestream cs;
    cs.siz.set(Scomponents, 0, 0, 3);
    for (int c = 0; c < 3; ++c) {
      cs.siz.set(Sdims, c, 0, 32);
      cs.siz.set(Sdims, c, 1, 32);
      cs.siz.set(Sprecision, c, 0, 8);
      cs.siz.set(Ssigned, c, 0, true);
    }
    static_cast<kdu_params*>(&cs.siz)->finalize();
    cs.codestream.create(&cs.siz, &target);
    siz_params* params = cs.codestream.access_siz();
    ASSERT_TRUE(params->parse_string("Creversible=no"));
    ASSERT_TRUE(params->parse_string("Clevels=1"));
    ASSERT_TRUE(params->parse_string("Cplex={6,EST,0.25,8}"));
    ASSERT_TRUE(params->parse_string("Qstep=0.002"));
    params->finalize_all();
    cs.codestream.set_max_bytes(2048);

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_node node0 =
        tile.tile.access_component(0).access_resolution().access_node();
    kdu_node node1 =
        tile.tile.access_component(1).access_resolution().access_node();
    kdu_node node2 =
        tile.tile.access_component(2).access_resolution().access_node();

    kdu_cplex_share share;
    kdu_cplex_share_tile* share_tile = share.get_tile(0);
    ASSERT_NE(share_tile, nullptr);
    share_tile->configure_cfrag_bounds(1, 1);
    kdu_cplex_share_cfrag* cfrag = share_tile->get_cfrag(0, 0);
    ASSERT_NE(cfrag, nullptr);

    {
      kdu_cplex_analysis active_parent;
      kdu_cplex_analysis descendant;
      active_parent.init(node0, cfrag, nullptr);
      descendant.init(node1, nullptr, nullptr);
      ASSERT_TRUE(active_parent.is_active());
      ASSERT_TRUE(descendant.is_active());
      active_parent.link_descendant(&descendant);
      active_parent.finish(nullptr);
    }

    // Active `kdu_cplex_analysis` destroyed without calling `finish()` cleans
    // up `share_cfrag` in `~kdu_cplex_analysis`.
    {
      kdu_cplex_analysis unfinish_cplex;
      unfinish_cplex.init(node2, cfrag, nullptr);
      ASSERT_TRUE(unfinish_cplex.is_active());
    }
  }
}

TEST(CplexAnalysisTest, SharedInterfacesMultiFrameScbrTinyStripsAndFusion) {
  kdu_cplex_share share;

  // Tiny height (17x17, Clevels=2) drives small strips where strip_heights<=0
  // compaction happens inside kdu_cplex_shared_ifc::do_transfer.
  {
    EncodeConfig cfg;
    cfg.width = 17;
    cfg.height = 17;
    cfg.clevels = 2;
    cfg.max_bytes = 512;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());
  }

  // Tall Scbr multi-set (512x128, Clevels=2, Cprecincts={8,8}, Cblk={8,8})
  // shared across 2 frames: exercises `shared_interfaces` `completed_head`
  // draining, multi-set forecasts, and `rem_rows < rem_strip_rows`.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 512;
    cfg.clevels = 2;
    cfg.max_bytes = 32768;
    cfg.use_scbr = true;
    cfg.pattern_variant = 0;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,16}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());

    cfg.pattern_variant = 1;
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());
  }

  // Shared negative delay (-1) so `completed_head != NULL` at `finish()` on
  // `shared_interfaces`.
  {
    kdu_cplex_share neg_share;
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,-1}", "Qstep=0.002",
                            "Cblk={8,8}"};
    EXPECT_FALSE(RunCplexEncode(cfg, &neg_share, nullptr).empty());
  }

  // Frame 1 (Scbr multi-set): establishes 6-strip baseline in `share`.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 128;
    cfg.clevels = 1;
    cfg.max_bytes = 16384;
    cfg.use_scbr = true;
    cfg.pattern_variant = 0;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());
  }

  // Frame 2 (correlated with Frame 1, Scbr): exercises temporal preference,
  // `total_rows_seen == 0` initial forecast, and `remaining_band_rows > 0`.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 128;
    cfg.clevels = 1;
    cfg.max_bytes = 16384;
    cfg.use_scbr = true;
    cfg.pattern_variant = 1;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,16}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());
  }

  // Frame 3 (high-contrast variant vs Frame 2, single set + Scbr): exercises
  // spatial preference and `remaining_band_rows == 0` path in
  // `kdu_cplex_shared_ifc::forecast_remaining_rows`.
  {
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 128;
    cfg.clevels = 1;
    cfg.max_bytes = 16384;
    cfg.use_scbr = false;
    cfg.pattern_variant = 2;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, nullptr).empty());
  }

  // Fragment fusion + early destroy (destructor calls share_cfrag->detach
  // without finish()) + linked_descendant chaining on normal frame.
  {
    kdu_cplex_share frag_share;
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.use_fragments = true;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &frag_share, nullptr).empty());
    // Second frame reuses fragment share, then tears down early before finish.
    cfg.early_destroy_tile = true;
    RunCplexEncode(cfg, &frag_share, nullptr);
  }
}

void CopyBkgndAccumulatedToStore(kdu_cplex_bkgnd& src, kdu_cplex_bkgnd& dst) {
  kdu_uint32 mct_hash = 0;
  const int num_comps = src.get_num_components(mct_hash);
  ASSERT_GT(num_comps, 0);
  ASSERT_TRUE(dst.configure(num_comps, mct_hash));
  for (int c = 0; c < num_comps; ++c) {
    kdu_cplex_bkgnd_comp* const src_comp = src.access_component(c);
    kdu_cplex_bkgnd_comp* const dst_comp = dst.access_component(c);
    ASSERT_NE(src_comp, nullptr);
    ASSERT_NE(dst_comp, nullptr);
    kdu_uint32 dwt_hash = 0;
    int guard_bits = 0;
    int rev_prec = 0;
    const int num_levels =
        src_comp->get_num_levels(dwt_hash, guard_bits, rev_prec);
    ASSERT_GE(num_levels, 0);
    ASSERT_TRUE(
        dst_comp->configure(num_levels, dwt_hash, guard_bits, rev_prec));
    for (int d = 0; d <= num_levels; ++d) {
      kdu_cplex_bkgnd_level* const src_lvl = src_comp->access_level(d);
      if (src_lvl == nullptr) {
        continue;
      }
      kdu_cplex_bkgnd_level* const dst_lvl = dst_comp->access_level(d);
      ASSERT_NE(dst_lvl, nullptr);
      kdu_uint32 style = 0;
      kdu_coords depth_coords;
      const int num_bands = src_lvl->get_num_bands(style, depth_coords);
      if (num_bands == 0) {
        continue;
      }
      ASSERT_TRUE(dst_lvl->configure(num_bands, style, depth_coords));
      for (int b = 0; b < num_bands; ++b) {
        kdu_uint16 eps_mu = 0;
        std::array<float, 256> stats{};
        std::array<float, 256> samples{};
        const int got =
            src_lvl->get_acc_stats(b, eps_mu, static_cast<int>(stats.size()),
                                   stats.data(), samples.data());
        if (got <= 0) {
          continue;
        }
        const int count = std::min(got, static_cast<int>(stats.size()));
        ASSERT_TRUE(dst_lvl->store_bkgnd_stats(b, eps_mu, count, stats.data(),
                                               samples.data()));
      }
    }
  }
}

TEST(CplexAnalysisTest, BkgndAnalysisPopulationReuseAndIncompatibility) {
  kdu_cplex_bkgnd bkgnd;

  // Frame 1: bkgnd only (shared_6strips == NULL), Clevels=2 (larger depth first
  // so max_depth covers 2).
  {
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.max_bytes = 2048;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, &bkgnd).empty());
  }

  kdu_cplex_bkgnd bkgnd_loaded;
  CopyBkgndAccumulatedToStore(bkgnd, bkgnd_loaded);

  // Frame 2: reuse populated rel_stats (`bkgnd_stats != NULL`,
  // `shared_6strips == NULL`) hits `strip_rows == 0` initial forecast and
  // `strip_rows > 0` spatial+bkgnd bound in
  // `kdu_cplex_shared_ifc::forecast_remaining_rows`.
  {
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.max_bytes = 2048;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, &bkgnd_loaded).empty());
  }

  // Frame 3: combine share + bkgnd + Scbr + fragments (`shared_6strips != NULL`
  // extraction loop inside `finish()` + `linked_descendant->finish`).
  {
    kdu_cplex_share share;
    EncodeConfig cfg;
    cfg.width = 128;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.max_bytes = 8192;
    cfg.use_fragments = true;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002"};
    EXPECT_FALSE(RunCplexEncode(cfg, &share, &bkgnd).empty());
  }

  // Frame 4: mismatched decomposition style (`Cdecomp:L1=V(-)`) hits
  // `bkgnd_level->configure(...) == false` -> `note_incompatibility(-1)`.
  {
    EncodeConfig cfg;
    cfg.width = 96;
    cfg.height = 96;
    cfg.clevels = 2;
    cfg.max_bytes = 2048;
    cfg.extra_cod_params = {"Cplex={6,EST,0.25,0}", "Qstep=0.002",
                            "Cdecomp=H(-),V(-)"};
    EXPECT_FALSE(RunCplexEncode(cfg, nullptr, &bkgnd).empty());
  }

  kdu_uint32 num_levels = 0, inc_sub = 0, inc_comp = 0, inc_rel = 0;
  kdu_uint32 store_calls = 0, inc_store = 0;
  EXPECT_TRUE(bkgnd.get_info(num_levels, inc_sub, inc_comp, inc_rel,
                             store_calls, inc_store));
  EXPECT_GT(store_calls, 0U);
  EXPECT_GT(inc_sub, 0U);
}

}  // namespace
}  // namespace kdu_core
