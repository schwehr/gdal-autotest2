#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/synchronization/mutex.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  void put_text(const char* text) override { pending_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      std::fprintf(stderr, "KDU_ERROR: %s\n", pending_.c_str());
      last_message_ = pending_;
      pending_.clear();
      kdu_thread_safe_message::flush(end_of_message);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_thread_safe_message::flush(end_of_message);
  }

 private:
  std::string pending_;
  std::string last_message_;
};

kdu_thread_safe_message* GlobalErrorHandler() {
  static auto* handler = new ThrowingErrorHandler();
  return handler;
}

class FusionTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { kdu_customize_errors(GlobalErrorHandler()); }
};

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

struct ScopedThreadEnv {
  kdu_thread_env env;
  kdu_codestream* cs = nullptr;

  void Start(int num_workers, kdu_codestream* codestream) {
    cs = codestream;
    env.create();
    for (int i = 0; i < num_workers; ++i) {
      ASSERT_TRUE(env.add_thread());
    }
  }

  ~ScopedThreadEnv() {
    if (env.exists()) {
      env.terminate(nullptr, true);
    }
    if (cs != nullptr && cs->exists() && env.exists()) {
      env.cs_terminate(*cs);
    }
    if (env.exists()) {
      env.destroy();
    }
  }
};

struct ScopedTile {
  kdu_tile tile;
  kdu_thread_env* env = nullptr;

  ~ScopedTile() {
    if (tile.exists()) {
      tile.close(env);
    }
  }
};

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;
  kdu_thread_env* env = nullptr;

  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy(env);
    }
  }
};

struct ScopedPushIfc {
  kdu_push_ifc ifc;

  ~ScopedPushIfc() {
    if (ifc.exists()) {
      ifc.destroy();
    }
  }
};

struct ScopedLineBuf {
  kdu_line_buf line;

  ~ScopedLineBuf() { line.destroy(); }
};

// Minimal custom kdu_thread_queue that dispatches a single WaiterJob on a
// worker thread and calls all_done() when the job finishes or terminates.
class WaiterQueue final : public kdu_thread_queue {
 public:
  int get_max_jobs() override { return 1; }

  void request_termination(kdu_thread_entity* caller) override {
    absl::MutexLock lock(mu_);
    terminate_requested_ = true;
    if (!in_flight_) {
      all_done(caller);
    }
  }

  void Launch(kdu_thread_job* job, kdu_thread_entity* caller) {
    MarkInFlight();
    schedule_job(job, caller, true);
  }

  void MarkInFlight() {
    absl::MutexLock lock(mu_);
    in_flight_ = true;
  }

  void FinishJob(kdu_thread_entity* caller) {
    bool call_done = false;
    {
      absl::MutexLock lock(mu_);
      in_flight_ = false;
      call_done = true;
    }
    if (call_done) {
      all_done(caller);
    }
  }

 private:
  absl::Mutex mu_;
  bool in_flight_ = false;
  bool terminate_requested_ = false;
};

void FinishWaiterQueue(WaiterQueue& wq, kdu_thread_env& env,
                       bool descendants_only) {
  struct DoneJob : public kdu_thread_job {
    WaiterQueue* q = nullptr;
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      static_cast<DoneJob*>(base)->q->FinishJob(caller);
    }
  };
  DoneJob done_job;
  done_job.q = &wq;
  done_job.set_job_func(&DoneJob::Run);
  wq.Launch(&done_job, &env);
  env.join(&wq, descendants_only);
  env.join(nullptr, true);
}

void CreateCodestreamForCompressor(ScopedCodestream& cs,
                                   kdu_compressed_target& target, int width,
                                   int height, int num_comps, int precision,
                                   bool is_signed, bool reversible, int levels,
                                   const char* extra_params = nullptr) {
  ASSERT_TRUE(cs.siz.parse_string(
      ("Scomponents=" + std::to_string(num_comps)).c_str()));
  ASSERT_TRUE(cs.siz.parse_string(
      ("Ssize={" + std::to_string(height) + "," + std::to_string(width) + "}")
          .c_str()));
  ASSERT_TRUE(cs.siz.parse_string(
      ("Sdims={" + std::to_string(height) + "," + std::to_string(width) + "}")
          .c_str()));
  ASSERT_TRUE(
      cs.siz.parse_string(("Sprecision=" + std::to_string(precision)).c_str()));
  ASSERT_TRUE(cs.siz.parse_string(is_signed ? "Ssigned=yes" : "Ssigned=no"));
  ASSERT_TRUE(cs.siz.parse_string("Ssampling={1,1}"));
  static_cast<kdu_params&>(cs.siz).finalize();

  cs.codestream.create(&cs.siz, &target);
  kdu_params* root = cs.codestream.access_siz();
  ASSERT_NE(root, nullptr);
  ASSERT_TRUE(
      root->parse_string(reversible ? "Creversible=yes" : "Creversible=no"));
  ASSERT_TRUE(
      root->parse_string(("Clevels=" + std::to_string(levels)).c_str()));
  if (extra_params == nullptr ||
      std::string(extra_params).find("Cblk=") == std::string::npos) {
    ASSERT_TRUE(root->parse_string("Cblk={16,16}"));
  }
  if (extra_params != nullptr) {
    std::string remaining(extra_params);
    size_t start = 0;
    while (start < remaining.size()) {
      size_t space = remaining.find(' ', start);
      if (space == std::string::npos) {
        space = remaining.size();
      }
      if (space > start) {
        std::string token = remaining.substr(start, space - start);
        ASSERT_TRUE(root->parse_string(token.c_str()))
            << "Failed to parse: " << token;
      }
      start = space + 1;
    }
  }
}

TEST_F(FusionTest, SingleThreadedReversibleShortRoundtrip) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 64;
  std::vector<kdu_byte> encoded;
  std::vector<kdu_byte> src(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      src[y * kWidth + x] = static_cast<kdu_byte>((x * 7 + y * 13) & 0xFF);
    }
  }

  {
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, true, 2));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);

    kdu_supp::kdu_stripe_compressor compressor;
    compressor.start(cs.codestream);
    const int stripe_schedule[] = {17, 23, 24};
    int y = 0;
    for (int h : stripe_schedule) {
      kdu_byte* bufs[1] = {src.data() + y * kWidth};
      int heights[1] = {h};
      compressor.push_stripe(bufs, heights);
      y += h;
    }
    EXPECT_TRUE(compressor.finish());
  }

  VectorSource source(encoded);
  ScopedCodestream dec_cs;
  dec_cs.codestream.create(&source);
  kdu_supp::kdu_stripe_decompressor decompressor;
  decompressor.start(dec_cs.codestream);
  std::vector<kdu_byte> dst(kWidth * kHeight, 0);
  kdu_byte* out_bufs[1] = {dst.data()};
  int out_heights[1] = {kHeight};
  EXPECT_FALSE(decompressor.pull_stripe(out_bufs, out_heights));
  EXPECT_TRUE(decompressor.finish());
  EXPECT_EQ(dst, src);
}

TEST_F(FusionTest, MultiThreadedReversibleMultiComponentRoundtrip) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 64;
  constexpr int kComps = 3;
  std::vector<kdu_byte> encoded;
  std::vector<std::vector<kdu_int16>> src(kComps);
  for (int c = 0; c < kComps; ++c) {
    src[c].resize(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        src[c][y * kWidth + x] =
            static_cast<kdu_int16>(((x * 11 + y * 5 + c * 97) % 4096) - 2048);
      }
    }
  }

  {
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, kComps, 12, true, true, 3, "Cycc=yes"));
    cs.codestream.configure_simultaneous_processing_fragments(32, 2, 8);

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(3, &cs.codestream));
    // TODO(schwehr): Upstream `fusion.cpp:254,263,266,777` uses relaxed
    // `entry->status.set(0)` rather than release semantics when reusing ring
    // entries (`kdu_elementary.h:390`), triggering a TSan data race at
    // `fusion.cpp:814` on entry wrap-around across worker threads. Keep all
    // DWT levels <= 32 rows (`KD_FUSION_SYNC_HEAD_MASK + 1`) with
    // `dbuf_rows = 256` to avoid wrap-around.
    kdu_push_pull_params extra_params;
    extra_params.set_spatial_xform_dbuf_rows(256);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, &env.env, nullptr, 4, 0, false, 0,
                     &extra_params);
    const int precs[kComps] = {12, 12, 12};
    const bool is_signed[kComps] = {true, true, true};
    int y = 0;
    while (y < kHeight) {
      const int h = std::min(19, kHeight - y);
      kdu_int16* bufs[kComps] = {
          src[0].data() + y * kWidth,
          src[1].data() + y * kWidth,
          src[2].data() + y * kWidth,
      };
      int heights[kComps] = {h, h, h};
      compressor.push_stripe(bufs, heights, nullptr, nullptr, precs, is_signed);
      y += h;
    }
    EXPECT_TRUE(compressor.finish());
  }

  VectorSource source(encoded);
  ScopedCodestream dec_cs;
  dec_cs.codestream.create(&source);
  kdu_supp::kdu_stripe_decompressor decompressor;
  decompressor.start(dec_cs.codestream);
  std::vector<std::vector<kdu_int16>> dst(
      kComps, std::vector<kdu_int16>(kWidth * kHeight, 0));
  kdu_int16* out_bufs[kComps] = {dst[0].data(), dst[1].data(), dst[2].data()};
  int out_heights[kComps] = {kHeight, kHeight, kHeight};
  const int precs[kComps] = {12, 12, 12};
  const bool is_signed[kComps] = {true, true, true};
  EXPECT_FALSE(decompressor.pull_stripe(out_bufs, out_heights, nullptr, nullptr,
                                        precs, is_signed));
  EXPECT_TRUE(decompressor.finish());
  for (int c = 0; c < kComps; ++c) {
    EXPECT_EQ(dst[c], src[c]) << "Mismatch in component " << c;
  }
}

TEST_F(FusionTest, MultiThreadedReversible32BitIntsRoundtrip) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 64;
  std::vector<kdu_byte> encoded;
  std::vector<kdu_int32> src(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      src[y * kWidth + x] =
          static_cast<kdu_int32>(((x * 131 + y * 521) % 200000) - 100000);
    }
  }

  {
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 20, true, true, 2));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(2, &cs.codestream));
    // Avoid `kd_fusion_entry` wrap-around across worker threads under TSan
    // (`fusion.cpp:254,814`).
    kdu_push_pull_params extra_params;
    extra_params.set_spatial_xform_dbuf_rows(256);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, &env.env, nullptr, 8, 0, false, 0,
                     &extra_params);
    const int precs[1] = {20};
    const bool is_signed[1] = {true};
    for (int y = 0; y < kHeight; y += 16) {
      kdu_int32* bufs[1] = {src.data() + y * kWidth};
      int heights[1] = {16};
      compressor.push_stripe(bufs, heights, nullptr, nullptr, precs, is_signed);
    }
    EXPECT_TRUE(compressor.finish());
  }

  VectorSource source(encoded);
  ScopedCodestream dec_cs;
  dec_cs.codestream.create(&source);
  kdu_supp::kdu_stripe_decompressor decompressor;
  decompressor.start(dec_cs.codestream);
  std::vector<kdu_int32> dst(kWidth * kHeight, 0);
  kdu_int32* out_bufs[1] = {dst.data()};
  int out_heights[1] = {kHeight};
  // TODO(schwehr): Upstream bug in
  // vendor/apps/support/kdu_stripe_decompressor.cpp:637-670
  // (`transfer_dwords` handles `dst_bits < original_bits` and
  // `dst_bits > original_bits` for 32-bit absolute integers, omitting
  // `dst_bits == original_bits`). Request 21 bits and verify `val << 1`.
  const int dec_precs[1] = {21};
  const bool is_signed[1] = {true};
  EXPECT_FALSE(decompressor.pull_stripe(out_bufs, out_heights, nullptr, nullptr,
                                        dec_precs, is_signed));
  EXPECT_TRUE(decompressor.finish());
  for (size_t i = 0; i < src.size(); ++i) {
    EXPECT_EQ(dst[i], src[i] << 1) << "Mismatch at index " << i;
  }
}

TEST_F(FusionTest, IrreversibleFloatAndForcePreciseWithDbuf) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 64;
  std::vector<float> src(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      src[y * kWidth + x] =
          0.4f * std::sin(static_cast<float>(x) * 0.1f + y * 0.2f);
    }
  }

  for (bool force_precise : {false, true}) {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(
        CreateCodestreamForCompressor(cs, target, kWidth, kHeight, 1, 10, true,
                                      false, 3, "Ckernels=W9X7 Clayers=2"));
    cs.codestream.configure_simultaneous_processing_fragments(32, 2, 8);

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    // `force_precise == false` sets `dbuf_rows = 1` to wrap `kd_fusion_entry`;
    // run with 0 worker threads so `fusion.cpp:254,814` relaxed `set(0)` does
    // not race in TSan, while `force_precise == true` avoids wrap-around
    // (`dbuf_rows = 256` caps at 32 entries >= max DWT level height 32) with
    // 3 workers.
    ASSERT_NO_FATAL_FAILURE(env.Start(force_precise ? 3 : 0, &cs.codestream));

    kdu_push_pull_params extra_params;
    extra_params.set_spatial_xform_dbuf_rows(force_precise ? 256 : 1);

    // TODO(schwehr): Upstream `compressed_local.h:2757,2781` accesses
    // `block_slope_threshold` non-atomically across threads when rate control
    // (`layer_sizes != nullptr`) is active with `env != nullptr`, triggering a
    // TSan race. Omit `layer_sizes` in this multi-threaded test (`layer_sizes`
    // is covered single-threaded in `CplexShareWithEstimateSingleThreaded`).
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false,
                     force_precise, false, 0.0, 0, false, &env.env, nullptr, -1,
                     0, false, 0, &extra_params);
    const bool is_signed[1] = {true};
    int y = 0;
    while (y < kHeight) {
      const int h = std::min(13, kHeight - y);
      float* bufs[1] = {src.data() + y * kWidth};
      int heights[1] = {h};
      compressor.push_stripe(bufs, heights, nullptr, nullptr, nullptr,
                             is_signed);
      y += h;
    }
    EXPECT_TRUE(compressor.finish());
    EXPECT_FALSE(encoded.empty());
  }
}

// TODO(schwehr): Upstream UBSan nullptr-with-nonzero-offset bug in
// vendor/coresys/compressed/compressed.cpp:5936,9588 (`old_addr + 1` on null
// `rcp->submitted_records`) triggers under `--config=asan-chlor` when
// `Cplex={...,EST,...}` is active with `env != nullptr`; exercise `cplex_share`
// single-threaded here so sanitizer builds remain clean without modifying
// vendor/.
TEST_F(FusionTest, CplexShareWithEstimateSingleThreaded) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 96;
  std::vector<float> src(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      src[y * kWidth + x] =
          0.4f * std::sin(static_cast<float>(x) * 0.1f + y * 0.2f);
    }
  }

  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 10, true, false, 3,
      "Ckernels=W9X7 Clayers=2 Cplex={6,EST,0.25,0}"));
  cs.codestream.configure_simultaneous_processing_fragments(32, 2, 8);

  kdu_supp::kdu_stripe_compressor compressor;
  kdu_cplex_share cplex_share;
  kdu_push_pull_params extra_params;
  extra_params.set_cplex_share(&cplex_share);
  extra_params.set_spatial_xform_dbuf_rows(64);

  const kdu_long layer_sizes[2] = {500, 1500};
  compressor.start(cs.codestream, 2, layer_sizes, nullptr, 0, false, true,
                   false, 0.0, 0, false, nullptr, nullptr, -1, 0, false, 0,
                   &extra_params);
  const bool is_signed[1] = {true};
  int y = 0;
  while (y < kHeight) {
    const int h = std::min(13, kHeight - y);
    float* bufs[1] = {src.data() + y * kWidth};
    int heights[1] = {h};
    compressor.push_stripe(bufs, heights, nullptr, nullptr, nullptr, is_signed);
    y += h;
  }
  EXPECT_TRUE(compressor.finish());
  EXPECT_FALSE(encoded.empty());
}

TEST_F(FusionTest, ConfigureCopyAndHeightOneEdgeCase) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 1;
  std::vector<kdu_byte> src(kWidth * kHeight);
  for (int x = 0; x < kWidth; ++x) {
    src[x] = static_cast<kdu_byte>((x * 3) & 0xFF);
  }

  std::vector<kdu_byte> encoded1;
  std::vector<kdu_byte> encoded2;
  VectorTarget target1(encoded1);
  VectorTarget target2(encoded2);
  ScopedCodestream cs1;
  ScopedCodestream cs2;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs1, target1, kWidth, kHeight, 1, 8, false, true, 2));
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(cs2, target2, kWidth, 3,
                                                        1, 8, false, false, 2));
  cs1.codestream.configure_simultaneous_processing_fragments(64, 1, 4);
  cs2.codestream.configure_simultaneous_processing_fragments(cs1.codestream);

  {
    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(2, &cs1.codestream));
    compressor.start(cs1.codestream, 0, nullptr, nullptr, 0, false, false,
                     false, 0.0, 0, false, &env.env, nullptr, 2, 0);
    kdu_byte* bufs[1] = {src.data()};
    int heights[1] = {1};
    compressor.push_stripe(bufs, heights);
    EXPECT_TRUE(compressor.finish());
  }

  {
    std::vector<kdu_byte> src2(kWidth * 3, 42);
    kdu_supp::kdu_stripe_compressor compressor;
    compressor.start(cs2.codestream, 0, nullptr, nullptr, 0, false, true);
    for (int y = 0; y < 3; ++y) {
      kdu_byte* bufs[1] = {src2.data() + y * kWidth};
      int heights[1] = {1};
      compressor.push_stripe(bufs, heights);
    }
    EXPECT_TRUE(compressor.finish());
  }

  EXPECT_FALSE(encoded1.empty());
  EXPECT_FALSE(encoded2.empty());
}

TEST_F(FusionTest, EarlyResetTriggersTerminationCleanup) {
  constexpr int kWidth = 256;
  constexpr int kHeight = 128;
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 8, false, false, 3));
  cs.codestream.configure_simultaneous_processing_fragments(32, 2, 8);
  cs.codestream.access_siz()->finalize_all();

  ScopedThreadEnv env;
  // TODO(schwehr): Upstream `multi_transform.cpp:597,1153` reads/writes
  // non-atomic `terminate_asap` across threads during early teardown, firing
  // a TSan race; run with 0 extra worker threads.
  ASSERT_NO_FATAL_FAILURE(env.Start(0, &cs.codestream));
  ScopedTile tile;
  tile.env = &env.env;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
  ASSERT_TRUE(tile.tile.exists());

  ScopedMultiAnalysis engine;
  engine.env = &env.env;
  engine.engine.create(cs.codestream, tile.tile, &env.env, nullptr,
                       KDU_MULTI_XFORM_DEFAULT_FLAGS | KDU_MULTI_XFORM_MT_DWT,
                       nullptr, 4);
  for (int y = 0; y < 16; ++y) {
    kdu_line_buf* line = engine.engine.exchange_line(0, nullptr, &env.env);
    ASSERT_NE(line, nullptr);
    if (kdu_sample16* buf = line->get_buf16()) {
      for (int x = 0; x < kWidth; ++x) {
        buf[x].ival = static_cast<kdu_int16>(x - 128);
      }
    } else if (kdu_sample32* buf32 = line->get_buf32()) {
      for (int x = 0; x < kWidth; ++x) {
        buf32[x].fval = 0.25f;
      }
    }
    engine.engine.exchange_line(0, line, &env.env);
  }
  // Tear down mid-tile via engine.destroy(&env) before all height is pushed,
  // exercising kd_fusion_core::request_termination and unfinished cleanup.
}

TEST_F(FusionTest, CplexBackgroundAndReversibleVPrecise) {
  kdu_cplex_bkgnd bkgnd;
  constexpr int kWidth = 256;
  constexpr int kHeight = 128;

  // Frame 1: populate kdu_cplex_bkgnd with Clevels=2, fragments enabled.
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, false, 2,
        "Ckernels=W9X7 Clayers=2 Cplex={6,EST,0.25,0}"));
    cs.codestream.configure_simultaneous_processing_fragments(64, 2, 4);

    kdu_supp::kdu_stripe_compressor compressor;
    kdu_push_pull_params extra_params;
    extra_params.set_cplex_bkgnd(&bkgnd);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, nullptr, nullptr, -1, 0, false, 0,
                     &extra_params);
    std::vector<kdu_byte> src(kWidth * kHeight, 100);
    kdu_byte* bufs[1] = {src.data()};
    int heights[1] = {kHeight};
    compressor.push_stripe(bufs, heights);
    EXPECT_TRUE(compressor.finish());
  }

  // Frame 2: reuse compatible kdu_cplex_bkgnd (same Clevels=2, max_depth=1) so
  // `LL1` triggers `need_fusion = true` and `kdu_analysis_fusion` matches
  // `cplex_bkgnd`.
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, false, 2,
        "Ckernels=W9X7 Clayers=2 Cplex={6,EST,0.25,0}"));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    // Run with 0 extra worker threads so `LL1` wrap-around (`64 > 32`) does
    // not hit `fusion.cpp:254,814` relaxed `set(0)` TSan race.
    ASSERT_NO_FATAL_FAILURE(env.Start(0, &cs.codestream));
    kdu_push_pull_params extra_params;
    extra_params.set_cplex_bkgnd(&bkgnd);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, &env.env, nullptr, 4, 0, false, 0,
                     &extra_params);
    std::vector<kdu_byte> src(kWidth * kHeight, 90);
    kdu_byte* bufs[1] = {src.data()};
    int heights[1] = {kHeight};
    compressor.push_stripe(bufs, heights);
    EXPECT_TRUE(compressor.finish());
  }

  // Frame 3: incompatible Qguard (4 vs 2) with Clevels=2, max_depth=1 hits
  // `cplex_bkgnd_comp = NULL` inside `kdu_analysis_fusion` (`LL1`).
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, false, 2,
        "Ckernels=W9X7 Clayers=2 Qguard=4 Cplex={6,EST,0.25,0}"));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(0, &cs.codestream));
    kdu_push_pull_params extra_params;
    extra_params.set_cplex_bkgnd(&bkgnd);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, &env.env, nullptr, 4, 0, false, 0,
                     &extra_params);
    std::vector<kdu_byte> src(kWidth * kHeight, 75);
    kdu_byte* bufs[1] = {src.data()};
    int heights[1] = {kHeight};
    compressor.push_stripe(bufs, heights);
    EXPECT_TRUE(compressor.finish());
  }

  // Frame 4: Reversible + KDU_MULTI_XFORM_VPRECISE covers sample_prec==VPRECISE
  // clearance when res.get_reversible() is true.
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 16, true, true, 1));
    cs.codestream.configure_simultaneous_processing_fragments(64, 1, 4);
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());
    ScopedMultiAnalysis engine;
    engine.engine.create(cs.codestream, tile.tile, nullptr, nullptr,
                         KDU_MULTI_XFORM_VPRECISE, nullptr, 2);
    for (int y = 0; y < kHeight; ++y) {
      kdu_line_buf* line = engine.engine.exchange_line(0, nullptr, nullptr);
      ASSERT_NE(line, nullptr);
      kdu_sample32* buf32 = line->get_buf32();
      kdu_sample64* buf64 = line->get_buf64();
      if (buf64 != nullptr) {
        for (int x = 0; x < kWidth; ++x) {
          buf64[x].ival = static_cast<kdu_int64>(x - 64);
        }
      } else if (buf32 != nullptr) {
        for (int x = 0; x < kWidth; ++x) {
          buf32[x].ival = x - 64;
        }
      }
      engine.engine.exchange_line(0, line, nullptr);
    }
  }

  // Frame 5: Direct reversible and irreversible KDU_LINE_BUF_VPRECISE init and
  // early destroy (before start()) covers `sample_prec = 0` clearance on
  // reversible nodes, `KDU_ALIGN_SAMPLES64`, and `tmp_buffer` cleanup.
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 16, true, true, 1));
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());
    kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
    kdu_node ll_node = res0.access_node();
    ASSERT_TRUE(ll_node.exists());
    ASSERT_TRUE(ll_node.access_subband().exists());
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    ScopedPushIfc fusion;
    fusion.ifc =
        kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_VPRECISE,
                            1.0F, 0, ll_dims.size.x, 0, 0);
  }
  {
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, false, 2));
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());
    kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
    kdu_node ll_node = res0.access_node();
    ASSERT_TRUE(ll_node.exists());
    ASSERT_TRUE(ll_node.access_subband().exists());
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    ScopedPushIfc fusion;
    fusion.ifc =
        kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_VPRECISE,
                            1.0F, 0, ll_dims.size.x, 0, 0);
  }
}

TEST_F(FusionTest, BackpressureDependenciesTBlockAndSubbandFinish) {
  // Single-level decomposition (Clevels=1) puts kdu_encoder directly below
  // kdu_analysis_fusion (hitting band.exists() path). Small Cblk={4,4} makes
  // block_indices.size.y > num_buffer_stripes so kdu_encoder propagates
  // dependencies to kd_fusion_core, exercising update_dependencies, Dependers
  // protocol, sync.W advection, and T-BLOCK installation/removal.
  {
    constexpr int kWidth = 256;
    constexpr int kHeight = 64;
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, true, 1, "Cblk={4,4}"));
    cs.codestream.configure_simultaneous_processing_fragments(32, 1, 4);

    std::vector<kdu_byte> src(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        src[y * kWidth + x] = static_cast<kdu_byte>((x * 13 + y * 29) & 0xFF);
      }
    }

    kdu_supp::kdu_stripe_compressor compressor;
    ScopedThreadEnv env;
    // Use 1 worker thread so coding lags behind line pushes, creating real
    // encoder backpressure (positive and negative dependency transitions).
    ASSERT_NO_FATAL_FAILURE(env.Start(1, &cs.codestream));
    kdu_push_pull_params extra_params;
    extra_params.set_spatial_xform_dbuf_rows(256);
    extra_params.set_max_block_stripes(1, 0);
    compressor.start(cs.codestream, 0, nullptr, nullptr, 0, false, false, false,
                     0.0, 0, false, &env.env, nullptr, 2, 0, false, 0,
                     &extra_params);
    int y = 0;
    while (y < kHeight) {
      const int h = std::min(16, kHeight - y);
      kdu_byte* bufs[1] = {src.data() + y * kWidth};
      int heights[1] = {h};
      compressor.push_stripe(bufs, heights);
      y += h;
    }
    EXPECT_TRUE(compressor.finish());
    EXPECT_FALSE(encoded.empty());
  }

  // Direct kdu_analysis_fusion with num_entries == remaining_height (32) and
  // Cblk={4,4} (sync.MAX > 0) installs T-BLOCK inside start() and releases it
  // via update_dependencies / do_advance; also exercises positive
  // acc_new_dependencies and multi-poll output.finish().
  {
    constexpr int kWidth = 128;
    constexpr int kHeight = 64;
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, true, 1, "Cblk={4,4}"));
    cs.codestream.access_siz()->finalize_all();

    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(2, &cs.codestream));
    ScopedTile tile;
    tile.env = &env.env;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
    ASSERT_TRUE(tile.tile.exists());

    kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
    kdu_node ll_node = res0.access_node();
    ASSERT_TRUE(ll_node.exists());
    ASSERT_TRUE(ll_node.access_subband().exists());
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);
    ASSERT_EQ(ll_dims.size.x, 64);
    ASSERT_EQ(ll_dims.size.y, 32);

    // Note: upstream fusion.cpp:735 has `core->sync.set(KD_FUSION_SYNC_B_FLAG)`
    // which zeroes `KD_FUSION_SYNC_MAX_MASK` if `dbuf_rows >= remaining_height`
    // while `sync.MAX > 0`, so use `dbuf_rows = 16 < 32` to exercise the
    // `do_advance` T-BLOCK path instead without clobbering `MAX`.
    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    params.set_spatial_xform_dbuf_rows(16);
    params.set_max_block_stripes(2, 0);

    ScopedLineBuf line0, line1;
    line0.line.pre_create(&allocator, 36,
                          KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
    line1.line.pre_create(&allocator, 32,
                          KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

    ScopedPushIfc frag0, frag1;
    frag0.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                    KDU_LINE_BUF_SHORTS, 1.0F, 0, 36, 0, 2,
                                    nullptr, nullptr, &env.env, nullptr);
    frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                    KDU_LINE_BUF_SHORTS, 1.0F, -32, 32, 2, 0,
                                    nullptr, nullptr, &env.env, nullptr);

    allocator.finalize(cs.codestream);
    line0.line.create();
    line1.line.create();

    frag0.ifc.start(&env.env);
    frag1.ifc.start(&env.env);

    for (int y = 0; y < ll_dims.size.y; ++y) {
      kdu_sample16* p0 = line0.line.get_buf16();
      kdu_sample16* p1 = line1.line.get_buf16();
      for (int x = 0; x < 36; ++x) {
        p0[x].ival = static_cast<kdu_int16>(x + y - 64);
      }
      for (int x = 0; x < 32; ++x) {
        p1[x].ival = static_cast<kdu_int16>(x + 32 + y - 64);
      }
      frag0.ifc.push(line0.line, &env.env);
      frag1.ifc.push(line1.line, &env.env);
    }

    while (!frag0.ifc.finish(&env.env)) {
    }
    while (!frag1.ifc.finish(&env.env)) {
    }
    env.env.join(nullptr, true);
  }

  // Multi-threaded DWT via kdu_multi_analysis with Clevels=1, Cblk={4,128},
  // Cprecincts={1024,32},{1024,128}, and spf(128, 1, 8): res 1 (`log2_siz_x=7`)
  // fragments while LL0 (`log2_siz_x=8 > 128`) remains unfragmented, forcing
  // `need_fusion = true` directly above `kdu_encoder`.
  // TODO(schwehr): Upstream `fusion.cpp:151,177` declares `new_sync` unsigned
  // (`kdu_uint32`), causing `assert(MAX >= NUM)` at line 177 to fail if a fast
  // encoder worker calls `update_dependencies(-1, 0)` while `do_advance` still
  // holds `+1` in `acc_new_dependencies` (`sync.NUM == -1 -> 4095`), and
  // upstream `fusion.cpp:255,759,819` skips installing T-BLOCK once `sync.MAX`
  // drops to 0 (`encoder.cpp:1032-1037`), letting a second worker thread push
  // the last fragment line without `S_H` and call `kd_fusion_ifc::finish()` ->
  // `kd_encoder::finish()` (`encoder.cpp:3941`) before `do_advance` finishes
  // pushing the final lines. Run with 0 extra worker threads so queue jobs
  // execute cooperatively on the caller thread without multi-thread races.
  // With `buffer_rows = 2` (so `core->num_entries == 2`) on a 1024x128 tile and
  // `buffer_rows = 24` on a 1024x64 tile, `kd_encoder` registers `MAX`
  // dependencies and backpressure into `kd_fusion_core`, exercising
  // `update_dependencies`, `do_advance`, `remove_tblock_condition`, and
  // `Dependers` protocols via `kd_multi_queue`.
  for (int pass = 0; pass < 2; ++pass) {
    constexpr int kWidth = 1024;
    const int height = (pass == 0) ? 128 : 64;
    const int buffer_rows = (pass == 0) ? 2 : 24;
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, height, 1, 12, true, true, 1,
        "Cblk={4,128} Cprecincts={1024,32},{1024,128}"));
    cs.codestream.configure_simultaneous_processing_fragments(128, 1, 8);
    cs.codestream.access_siz()->finalize_all();

    ScopedThreadEnv env;
    ASSERT_NO_FATAL_FAILURE(env.Start(0, &cs.codestream));
    ScopedTile tile;
    tile.env = &env.env;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
    ASSERT_TRUE(tile.tile.exists());

    kdu_tile_comp tc = tile.tile.access_component(0);
    ASSERT_GT(
        tc.access_resolution(1).get_num_simultaneous_processing_fragments(), 1);
    ASSERT_EQ(
        tc.access_resolution(0).get_num_simultaneous_processing_fragments(), 1);

    kdu_thread_queue env_queue;
    ASSERT_TRUE(env.env.attach_queue(&env_queue, nullptr, nullptr));
    ScopedMultiAnalysis engine;
    engine.env = &env.env;
    engine.engine.create(cs.codestream, tile.tile, &env.env, &env_queue,
                         KDU_MULTI_XFORM_DEFAULT_FLAGS | KDU_MULTI_XFORM_MT_DWT,
                         nullptr, buffer_rows);
    for (int y = 0; y < height; ++y) {
      kdu_line_buf* line = engine.engine.exchange_line(0, nullptr, &env.env);
      ASSERT_NE(line, nullptr);
      kdu_sample32* buf32 = line->get_buf32();
      kdu_sample16* buf16 = line->get_buf16();
      for (int x = 0; x < kWidth; ++x) {
        int val = ((x * 131 + y * 977) & 0xFFF) - 2048;
        if (buf32 != nullptr) {
          buf32[x].ival = val;
        } else {
          ASSERT_NE(buf16, nullptr);
          buf16[x].ival = static_cast<kdu_int16>(val);
        }
      }
      engine.engine.exchange_line(0, line, &env.env);
    }
    env.env.join(&env_queue, true);
    engine.engine.destroy(&env.env);
    tile.tile.close(&env.env);
  }

  // Deferred complexity-controlled finish via direct `kdu_analysis_fusion` with
  // `env = nullptr` exercises multi-poll `core->output.finish(env)` returning
  // false (`fusion.cpp:760`) deterministically as each remaining stripe clears.
  {
    constexpr int kWidth = 128;
    constexpr int kHeight = 256;  // LL0 = 64x128 -> 32 stripes of 4
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, false, 1,
        "Ckernels=W9X7 Clayers=1 Cmodes=HT Cblk={4,4} Cplex={6,EST,0.25,32}"));
    cs.codestream.set_max_bytes(500);
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());

    kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
    kdu_node ll_node = res0.access_node();
    ASSERT_TRUE(ll_node.exists());
    ASSERT_TRUE(ll_node.access_subband().exists());
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);
    ASSERT_EQ(ll_dims.size.x, 64);
    ASSERT_EQ(ll_dims.size.y, 128);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    params.set_spatial_xform_dbuf_rows(4);
    kdu_cplex_analysis cplex;

    ScopedLineBuf line0, line1;
    line0.line.pre_create(&allocator, 32, KDU_LINE_BUF_SHORTS, 0, 0);
    line1.line.pre_create(&allocator, 32, KDU_LINE_BUF_SHORTS, 0, 0);

    ScopedPushIfc frag0, frag1;
    frag0.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                    KDU_LINE_BUF_SHORTS, 1.0F, 0, 32, 0, 0,
                                    nullptr, &cplex, nullptr, nullptr);
    frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                    KDU_LINE_BUF_SHORTS, 1.0F, -32, 32, 0, 0,
                                    nullptr, &cplex, nullptr, nullptr);

    allocator.finalize(cs.codestream);
    line0.line.create();
    line1.line.create();
    frag0.ifc.start(nullptr);
    frag1.ifc.start(nullptr);

    for (int y = 0; y < ll_dims.size.y; ++y) {
      kdu_sample16* p0 = line0.line.get_buf16();
      kdu_sample16* p1 = line1.line.get_buf16();
      for (int x = 0; x < 32; ++x) {
        p0[x].ival = static_cast<kdu_int16>(x + y - 32);
        p1[x].ival = static_cast<kdu_int16>(x + 32 + y - 32);
      }
      frag0.ifc.push(line0.line, nullptr);
      frag1.ifc.push(line1.line, nullptr);
    }
    EXPECT_TRUE(frag0.ifc.finish(nullptr));
    int extra_finishes = 0;
    while (!frag1.ifc.finish(nullptr)) {
      ++extra_finishes;
    }
    EXPECT_GT(extra_finishes, 0);
  }
}

TEST_F(FusionTest, DirectFragmentSortingAlignmentSlackAndClamping) {
  // Odd subband width (67) triggers reverse-order fragment sorting,
  // equal-write_pos sorting, dbuf_rows bounds clamping (< 2 and > 256), and
  // KDU_ALIGN_SAMPLES16 tight-slack mask halving loops.
  {
    constexpr int kWidth = 134;  // LL0 width = 67
    constexpr int kHeight = 16;  // LL0 height = 8
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, true, 1));
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());
    kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
    kdu_node ll_node = res0.access_node();
    ASSERT_TRUE(ll_node.exists());
    ASSERT_TRUE(ll_node.access_subband().exists());
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);
    ASSERT_EQ(ll_dims.size.x, 67);
    ASSERT_EQ(ll_dims.size.y, 8);

    // Subcase A: reverse-order insertion (right frag created before left),
    // dbuf_rows = 300 (clamped > 256 and > 31), and slack < 15.
    {
      kdu_sample_allocator allocator;
      kdu_push_pull_params params;
      params.set_spatial_xform_dbuf_rows(300);

      ScopedLineBuf line_l, line_r;
      line_l.line.pre_create(&allocator, 36,
                             KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
      line_r.line.pre_create(&allocator, 37,
                             KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

      ScopedPushIfc frag_r, frag_l;
      frag_r.ifc =
          kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_SHORTS,
                              1.0F, -30, 37, 4, 0);
      frag_l.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                       KDU_LINE_BUF_SHORTS, 1.0F, 0, 36, 0, 2);

      allocator.finalize(cs.codestream);
      line_l.line.create();
      line_r.line.create();
      frag_r.ifc.start(nullptr);
      frag_l.ifc.start(nullptr);

      for (int y = 0; y < ll_dims.size.y; ++y) {
        kdu_sample16* pl = line_l.line.get_buf16();
        kdu_sample16* pr = line_r.line.get_buf16();
        for (int x = 0; x < 36; ++x) pl[x].ival = static_cast<kdu_int16>(x);
        for (int x = 0; x < 37; ++x)
          pr[x].ival = static_cast<kdu_int16>(x + 30);
        frag_l.ifc.push(line_l.line, nullptr);
        frag_r.ifc.push(line_r.line, nullptr);
      }
      while (!frag_l.ifc.finish(nullptr)) {
      }
      while (!frag_r.ifc.finish(nullptr)) {
      }
    }
  }

  // Subcase B: equal write_pos sorting (longer xfer_len created first) and
  // dbuf_rows = 1 clamped up to 2.
  {
    constexpr int kWidth = 128;  // LL0 width = 64
    constexpr int kHeight = 8;   // LL0 height = 4
    std::vector<kdu_byte> encoded;
    VectorTarget target(encoded);
    ScopedCodestream cs;
    ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
        cs, target, kWidth, kHeight, 1, 8, false, true, 1));
    cs.codestream.access_siz()->finalize_all();

    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.tile.exists());
    kdu_resolution res = tile.tile.access_component(0).access_resolution();
    kdu_node ll_node = res.access_node().access_child(LL_BAND);
    kdu_dims ll_dims;
    ll_node.get_dims(ll_dims);
    ASSERT_EQ(ll_dims.size.x, 64);

    kdu_sample_allocator allocator;
    kdu_push_pull_params params;
    params.set_spatial_xform_dbuf_rows(1);

    ScopedLineBuf line_wide, line_narrow;
    line_wide.line.pre_create(
        &allocator, 64, KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
    line_narrow.line.pre_create(
        &allocator, 32, KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

    ScopedPushIfc frag_wide, frag_narrow;
    frag_wide.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                        KDU_LINE_BUF_SHORTS, 1.0F, 0, 64, 0, 0);
    frag_narrow.ifc = kdu_analysis_fusion(
        ll_node, &allocator, params, KDU_LINE_BUF_SHORTS, 1.0F, 0, 32, 0, 0);

    allocator.finalize(cs.codestream);
    line_wide.line.create();
    line_narrow.line.create();
    frag_wide.ifc.start(nullptr);
    frag_narrow.ifc.start(nullptr);

    for (int y = 0; y < ll_dims.size.y; ++y) {
      kdu_sample16* pw = line_wide.line.get_buf16();
      kdu_sample16* pn = line_narrow.line.get_buf16();
      for (int x = 0; x < 64; ++x) pw[x].ival = static_cast<kdu_int16>(x);
      for (int x = 0; x < 32; ++x) pn[x].ival = static_cast<kdu_int16>(x);
      frag_narrow.ifc.push(line_narrow.line, nullptr);
      frag_wide.ifc.push(line_wide.line, nullptr);
    }
    while (!frag_narrow.ifc.finish(nullptr)) {
    }
    while (!frag_wide.ifc.finish(nullptr)) {
    }
  }
}

TEST_F(FusionTest, DependersProtocolTriggeredAndReleasedSingleThreaded) {
  // With queue=&wq on frag0 and num_entries=2, pushing two consecutive lines
  // on frag0 before frag1 catches up enters the Dependers Protocol
  // (entry->dependers + update_dependencies(1, 0)), and frag1's push clears it
  // in do_advance() (update_dependencies(-1, 0)).
  // TODO(schwehr): Run single-threaded (env.Start(0)) rather than pushing
  // frag0 and frag1 concurrently from two threads because entry->status.set(0)
  // in vendor/coresys/transform/fusion.cpp uses memory_order_relaxed on queue
  // wrap-around, which triggers a TSan data race on entry->line.
  constexpr int kWidth = 128;   // LL0 width = 64
  constexpr int kHeight = 256;  // LL0 height = 128
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 8, false, true, 1, "Cblk={4,4}"));
  cs.codestream.access_siz()->finalize_all();

  ScopedThreadEnv env;
  ASSERT_NO_FATAL_FAILURE(env.Start(0, &cs.codestream));
  ScopedTile tile;
  tile.env = &env.env;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
  ASSERT_TRUE(tile.tile.exists());

  kdu_resolution res = tile.tile.access_component(0).access_resolution();
  kdu_node ll_node = res.access_node().access_child(LL_BAND);
  ASSERT_TRUE(ll_node.exists());
  kdu_dims ll_dims;
  ll_node.get_dims(ll_dims);
  ASSERT_EQ(ll_dims.size.y, 128);

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  params.set_spatial_xform_dbuf_rows(2);
  params.set_max_block_stripes(1, 0);

  ScopedLineBuf line0, line1;
  line0.line.pre_create(&allocator, 32,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
  line1.line.pre_create(&allocator, 32,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

  WaiterQueue wq;
  ASSERT_TRUE(env.env.attach_queue(&wq, nullptr, KDU_TRANSFORM_THREAD_DOMAIN));

  ScopedPushIfc frag0, frag1;
  frag0.ifc =
      kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_SHORTS,
                          1.0F, 0, 32, 0, 0, nullptr, nullptr, &env.env, &wq);
  frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                  KDU_LINE_BUF_SHORTS, 1.0F, -32, 32, 0, 0,
                                  nullptr, nullptr, &env.env, nullptr);

  allocator.finalize(cs.codestream);
  line0.line.create();
  line1.line.create();
  frag0.ifc.start(&env.env);
  frag1.ifc.start(&env.env);

  auto fill_line = [](kdu_line_buf& line, int base_x, int y) {
    kdu_sample16* p = line.get_buf16();
    for (int x = 0; x < 32; ++x) {
      p[x].ival = static_cast<kdu_int16>(x + base_x + y - 64);
    }
  };

  for (int y = 0; y < ll_dims.size.y; y += 2) {
    fill_line(line0.line, 0, y);
    frag0.ifc.push(line0.line, &env.env);
    fill_line(line0.line, 0, y + 1);
    frag0.ifc.push(line0.line, &env.env);

    fill_line(line1.line, 32, y);
    frag1.ifc.push(line1.line, &env.env);
    fill_line(line1.line, 32, y + 1);
    frag1.ifc.push(line1.line, &env.env);
  }

  while (!frag0.ifc.finish(&env.env)) {
  }
  while (!frag1.ifc.finish(&env.env)) {
  }
  FinishWaiterQueue(wq, env.env, /*descendants_only=*/true);
}

TEST_F(FusionTest, TBlockReleasedByUpdateDependenciesWakesWaiter) {
  // With queue=&wq on frag0 (so core_queue=core -> encoder tracks MAX/NUM),
  // queue=nullptr on frag1 (Waiters Protocol), dbuf_rows=16 (scaled by 10/20
  // -> 8 entries), Cblk={8,64}, and LL0 height=10 (stripe 0 ends at row 7):
  // rows 0..7 run do_advance() on the caller thread, row 1 installs T-BLOCK on
  // entry 1 (for terminal row 9) while MAX=1, row 7 starts stripe 0 on the
  // worker (NUM=1), row 8 sets sync.W=1 on entry 0 (already freed on caller),
  // frag1.push(row 9) blocks on T-BLOCK, and the worker finishing stripe 0
  // runs do_advance(row 8) clearing B_FLAG -> remove_tblock_condition ->
  // signal_condition. Using num_entries=8 avoids worker/caller entry reuse
  // (and the relaxed fusion.cpp:254 set(0) TSan race).
  // TODO(schwehr): Using kHeight=20 (LL0=10) so row H-2 (8) does not end a
  // stripe (avoiding upstream fusion.cpp:369 cur_sync vs new_sync bug) and
  // 12-bit reversible kWidth=4096 stripe 0 so the worker takes longer than
  // push, avoiding upstream unsigned wrap race at
  // vendor/coresys/transform/fusion.cpp:177.
  constexpr int kWidth = 4096;  // LL0 width = 2048 (32 64x8 blocks/stripe)
  constexpr int kHeight = 20;   // LL0 height = 10 (0..7, 8..9)
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 12, true, false, 1, "Cblk={8,64}"));
  cs.codestream.access_siz()->finalize_all();

  ScopedThreadEnv env;
  ASSERT_NO_FATAL_FAILURE(env.Start(1, &cs.codestream));
  ScopedTile tile;
  tile.env = &env.env;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
  ASSERT_TRUE(tile.tile.exists());

  kdu_resolution res0 = tile.tile.access_component(0).access_resolution(0);
  kdu_node ll_node = res0.access_node();
  ASSERT_TRUE(ll_node.exists());
  ASSERT_TRUE(ll_node.access_subband().exists());
  kdu_dims ll_dims;
  ll_node.get_dims(ll_dims);
  ASSERT_EQ(ll_dims.size.x, 2048);
  ASSERT_EQ(ll_dims.size.y, 10);

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  params.set_spatial_xform_dbuf_rows(16);
  params.set_max_block_stripes(1, 0);

  ScopedLineBuf line0, line1;
  line0.line.pre_create(&allocator, 1024,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
  line1.line.pre_create(&allocator, 1024,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

  WaiterQueue wq;
  ASSERT_TRUE(env.env.attach_queue(&wq, nullptr, KDU_TRANSFORM_THREAD_DOMAIN));

  ScopedPushIfc frag0, frag1;
  frag0.ifc =
      kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_SHORTS,
                          1.0F, 0, 1024, 0, 0, nullptr, nullptr, &env.env, &wq);
  frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                  KDU_LINE_BUF_SHORTS, 1.0F, -1024, 1024, 0, 0,
                                  nullptr, nullptr, &env.env, nullptr);

  allocator.finalize(cs.codestream);
  line0.line.create();
  line1.line.create();
  frag0.ifc.start(&env.env);
  frag1.ifc.start(&env.env);

  auto fill_noisy = [](kdu_line_buf& line, int base_x, int y) {
    kdu_sample16* p = line.get_buf16();
    for (int x = 0; x < 1024; ++x) {
      p[x].ival = static_cast<kdu_int16>(
          (((base_x + x) * 131 + y * 977) & 0xFFF) - 2048);
    }
  };

  for (int y = 0; y < ll_dims.size.y - 1; ++y) {
    fill_noisy(line0.line, 0, y);
    fill_noisy(line1.line, 1024, y);
    frag0.ifc.push(line0.line, &env.env);
    frag1.ifc.push(line1.line, &env.env);
  }
  // Push frag1 first on the final row so queue==nullptr enters Waiters
  // Protocol on the T-BLOCK entry while encoder backlog still holds NUM > 0.
  fill_noisy(line1.line, 1024, ll_dims.size.y - 1);
  frag1.ifc.push(line1.line, &env.env);
  fill_noisy(line0.line, 0, ll_dims.size.y - 1);
  frag0.ifc.push(line0.line, &env.env);

  while (!frag1.ifc.finish(&env.env)) {
  }
  while (!frag0.ifc.finish(&env.env)) {
  }

  FinishWaiterQueue(wq, env.env, /*descendants_only=*/true);
}

TEST_F(FusionTest, RequestTerminationClearsStartTBlock) {
  // With remaining_height=6 (> 1), scaled num_entries=8 (>= 6), Cblk={4,64}
  // (2 block stripes), and max_block_stripes=1 so kd_encoder registers MAX=1
  // during construction, start() installs T-BLOCK at lines 732-736.
  // Calling terminate(&wq, true) immediately propagates termination to wq's
  // descendant kd_fusion_core, exercising request_termination() clearing
  // B_FLAG and detaching kd_encoder cleanly without hitting the upstream
  // sync.set(B_FLAG) bug at vendor/coresys/transform/fusion.cpp:735 that
  // zeroes MAX on subsequent pushes.
  constexpr int kWidth = 128;
  constexpr int kHeight = 12;
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 8, false, true, 1, "Cblk={4,64}"));
  cs.codestream.access_siz()->finalize_all();

  ScopedThreadEnv env;
  ASSERT_NO_FATAL_FAILURE(env.Start(1, &cs.codestream));
  ScopedTile tile;
  tile.env = &env.env;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
  ASSERT_TRUE(tile.tile.exists());

  kdu_node ll_node =
      tile.tile.access_component(0).access_resolution(0).access_node();
  ASSERT_TRUE(ll_node.exists());

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  params.set_spatial_xform_dbuf_rows(16);
  params.set_max_block_stripes(1, 0);

  WaiterQueue wq;
  ASSERT_TRUE(env.env.attach_queue(&wq, nullptr, KDU_TRANSFORM_THREAD_DOMAIN));

  ScopedPushIfc frag0, frag1;
  frag0.ifc =
      kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_SHORTS,
                          1.0F, 0, 32, 0, 0, nullptr, nullptr, &env.env, &wq);
  frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                  KDU_LINE_BUF_SHORTS, 1.0F, -32, 32, 0, 0,
                                  nullptr, nullptr, &env.env, nullptr);

  allocator.finalize(cs.codestream);
  frag0.ifc.start(&env.env);
  frag1.ifc.start(&env.env);

  EXPECT_TRUE(env.env.terminate(&wq, true));
  FinishWaiterQueue(wq, env.env, /*descendants_only=*/false);
}

TEST_F(FusionTest, AdvanceStopsAtQueuedEntryWhenNewDependenciesAdded) {
  // With a worker thread paused on KDU_CODING_THREAD_DOMAIN until rows 0..8 are
  // deposited into an 8-entry buffer (dbuf_rows=16 scaled by 10/20 -> 8),
  // row 3 sets NUM=1, rows 4..8 queue up cleanly, and when the worker resumes
  // and runs do_advance(4..7), row 7 adds acc_new_dependencies=1 and finds
  // row 8 already complete in the buffer, executing fusion.cpp:330-340.
  constexpr int kWidth = 4096;
  constexpr int kHeight = 20;  // LL0 = 2048x10 (stripes: 0..3, 4..7, 8..9)
  std::vector<kdu_byte> encoded;
  VectorTarget target(encoded);
  ScopedCodestream cs;
  ASSERT_NO_FATAL_FAILURE(CreateCodestreamForCompressor(
      cs, target, kWidth, kHeight, 1, 12, true, false, 1, "Cblk={4,64}"));
  cs.codestream.access_siz()->finalize_all();

  ScopedThreadEnv env;
  ASSERT_NO_FATAL_FAILURE(env.Start(1, &cs.codestream));
  ScopedTile tile;
  tile.env = &env.env;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &env.env);
  ASSERT_TRUE(tile.tile.exists());

  kdu_node ll_node =
      tile.tile.access_component(0).access_resolution(0).access_node();
  ASSERT_TRUE(ll_node.exists());
  kdu_dims ll_dims;
  ll_node.get_dims(ll_dims);
  ASSERT_EQ(ll_dims.size.y, 10);

  kdu_sample_allocator allocator;
  kdu_push_pull_params params;
  params.set_spatial_xform_dbuf_rows(16);
  params.set_max_block_stripes(1, 0);

  ScopedLineBuf line0, line1;
  line0.line.pre_create(&allocator, 1024,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);
  line1.line.pre_create(&allocator, 1024,
                        KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0, 0);

  WaiterQueue wq;
  WaiterQueue blocker_q;
  ASSERT_TRUE(env.env.attach_queue(&wq, nullptr, KDU_TRANSFORM_THREAD_DOMAIN));
  ASSERT_TRUE(
      env.env.attach_queue(&blocker_q, nullptr, KDU_CODING_THREAD_DOMAIN));

  struct BlockerState {
    absl::Mutex mu;
    bool started = false;
    bool release = false;
    WaiterQueue* q = nullptr;
  } blocker_state;
  blocker_state.q = &blocker_q;

  struct BlockerJob : public kdu_thread_job {
    BlockerState* state = nullptr;
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      BlockerState* s = static_cast<BlockerJob*>(base)->state;
      {
        absl::MutexLock lock(s->mu);
        s->started = true;
      }
      {
        absl::MutexLock lock(
            s->mu, absl::Condition(
                       +[](bool* release) { return *release; }, &s->release));
      }
      s->q->FinishJob(caller);
    }
  };
  BlockerJob blocker_job;
  blocker_job.state = &blocker_state;
  blocker_job.set_job_func(&BlockerJob::Run);
  blocker_q.Launch(&blocker_job, &env.env);
  {
    absl::MutexLock lock(
        blocker_state.mu,
        absl::Condition(
            +[](bool* started) { return *started; }, &blocker_state.started));
  }

  ScopedPushIfc frag0, frag1;
  frag0.ifc =
      kdu_analysis_fusion(ll_node, &allocator, params, KDU_LINE_BUF_SHORTS,
                          1.0F, 0, 1024, 0, 0, nullptr, nullptr, &env.env, &wq);
  frag1.ifc = kdu_analysis_fusion(ll_node, &allocator, params,
                                  KDU_LINE_BUF_SHORTS, 1.0F, -1024, 1024, 0, 0,
                                  nullptr, nullptr, &env.env, nullptr);

  allocator.finalize(cs.codestream);
  line0.line.create();
  line1.line.create();
  frag0.ifc.start(&env.env);
  frag1.ifc.start(&env.env);

  auto fill_noisy = [](kdu_line_buf& line, int base_x, int y) {
    kdu_sample16* p = line.get_buf16();
    for (int x = 0; x < 1024; ++x) {
      p[x].ival = static_cast<kdu_int16>(
          (((base_x + x) * 131 + y * 977) & 0xFFF) - 2048);
    }
  };

  for (int y = 0; y < ll_dims.size.y - 1; ++y) {
    fill_noisy(line0.line, 0, y);
    fill_noisy(line1.line, 1024, y);
    frag0.ifc.push(line0.line, &env.env);
    frag1.ifc.push(line1.line, &env.env);
  }

  {
    absl::MutexLock lock(blocker_state.mu);
    blocker_state.release = true;
  }
  env.env.join(&blocker_q, false);

  fill_noisy(line0.line, 0, 9);
  fill_noisy(line1.line, 1024, 9);
  frag0.ifc.push(line0.line, &env.env);
  frag1.ifc.push(line1.line, &env.env);

  while (!frag1.ifc.finish(&env.env)) {
  }
  while (!frag0.ifc.finish(&env.env)) {
  }

  FinishWaiterQueue(wq, env.env, /*descendants_only=*/true);
}

}  // namespace
}  // namespace kdu_core
