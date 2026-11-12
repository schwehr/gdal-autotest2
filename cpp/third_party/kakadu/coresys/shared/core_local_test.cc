// Copyright 2026 Google LLC
// Tests for third_party/kakadu/vendor/coresys/shared/core_local.cpp via
// Kakadu's public kdu_membroker, kdu_codestream, kdu_pdescriptor_set,
// kdu_multi_analysis, and kdu_multi_synthesis APIs.

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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

  void put_text(const char* text) override { pending_ += text; }

  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      last_message_ = std::move(pending_);
      pending_.clear();
      kdu_thread_safe_message::flush(end_of_message);
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
    kdu_thread_safe_message::flush(end_of_message);
  }

  const std::string& last_message() const { return last_message_; }
  void Clear() {
    pending_.clear();
    last_message_.clear();
  }

 private:
  std::string pending_;
  std::string last_message_;
};

class SilentWarningHandler final : public kdu_thread_safe_message {
 public:
  ~SilentWarningHandler() noexcept override = default;
  void put_text(const char* /*text*/) override {}
};

ThrowingErrorHandler& GlobalErrorHandler() {
  static auto* handler = new ThrowingErrorHandler();
  return *handler;
}

SilentWarningHandler& GlobalWarningHandler() {
  static auto* handler = new SilentWarningHandler();
  return *handler;
}

class CoreLocalTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_customize_errors(&GlobalErrorHandler());
    kdu_customize_warnings(&GlobalWarningHandler());
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new CoreLocalTestEnvironment());

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
    if (pos_ >= data_.size() || num_bytes <= 0) return 0;
    const size_t avail = data_.size() - pos_;
    const size_t n = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) return false;
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
};

class TrackingMemBroker final : public kdu_membroker {
 public:
  static constexpr kdu_exception kCustomBrokerException = 0x544d424b;

  explicit TrackingMemBroker(kdu_int64 limit_bytes = -1,
                             bool throw_on_failure = false)
      : kdu_membroker(limit_bytes), throw_on_failure_(throw_on_failure) {}

  void note_allocation_failure(kdu_int64 deficit_bytes,
                               kdu_int64 allocated_bytes,
                               bool alloc_failed) override {
    ++failure_count_;
    last_deficit_bytes_ = deficit_bytes;
    last_allocated_bytes_ = allocated_bytes;
    last_alloc_failed_ = alloc_failed;
    if (throw_on_failure_) {
      throw kCustomBrokerException;
    }
    kdu_membroker::note_allocation_failure(deficit_bytes, allocated_bytes,
                                           alloc_failed);
  }

  int failure_count() const { return failure_count_; }
  kdu_int64 last_deficit_bytes() const { return last_deficit_bytes_; }
  kdu_int64 last_allocated_bytes() const { return last_allocated_bytes_; }
  bool last_alloc_failed() const { return last_alloc_failed_; }

 private:
  bool throw_on_failure_;
  int failure_count_ = 0;
  kdu_int64 last_deficit_bytes_ = -1;
  kdu_int64 last_allocated_bytes_ = -1;
  bool last_alloc_failed_ = true;
};

void ConfigureSmallImageParams(siz_params& siz, int width, int height) {
  ASSERT_TRUE(siz.parse_string("Scomponents=1"));
  const std::string sdims =
      "Sdims={" + std::to_string(height) + "," + std::to_string(width) + "}";
  ASSERT_TRUE(siz.parse_string(sdims.c_str()));
  ASSERT_TRUE(siz.parse_string("Sprecision=8"));
  ASSERT_TRUE(siz.parse_string("Ssigned=no"));
  static_cast<kdu_params*>(&siz)->finalize();
}

void WriteLineSample(kdu_line_buf* line, int x, kdu_int16 value) {
  if (auto* buf16 = line->get_buf16(); buf16 != nullptr) {
    buf16[x].ival = value;
  } else {
    auto* buf32 = line->get_buf32();
    ASSERT_NE(buf32, nullptr);
    buf32[x].ival = value;
  }
}

kdu_int16 ReadLineSample(kdu_line_buf* line, int x) {
  if (const auto* buf16 = line->get_buf16(); buf16 != nullptr) {
    return buf16[x].ival;
  }
  const auto* buf32 = line->get_buf32();
  EXPECT_NE(buf32, nullptr);
  return static_cast<kdu_int16>(buf32[x].ival);
}

class CoreLocalTest : public ::testing::Test {
 protected:
  void SetUp() override { GlobalErrorHandler().Clear(); }
};

TEST_F(CoreLocalTest, CodestreamAndMultiTransformBrokerLifecycle) {
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  std::vector<kdu_byte> bitstream;

  // 1. Encode a 16x16 reversible codestream with a memory broker.
  {
    kdu_membroker broker(1LL << 20);
    VectorTarget target(bitstream);
    siz_params siz;
    ConfigureSmallImageParams(siz, kWidth, kHeight);

    kdu_codestream codestream;
    absl::Cleanup destroy_cs = [&codestream] {
      if (codestream.exists()) codestream.destroy();
    };
    codestream.create(&siz, &target, /*fragment_region=*/nullptr,
                      /*fragment_tiles_generated=*/0,
                      /*fragment_tile_bytes_generated=*/0,
                      /*env=*/nullptr, &broker);
    siz_params* cs_siz = codestream.access_siz();
    ASSERT_TRUE(cs_siz->parse_string("Creversible=yes"));
    ASSERT_TRUE(cs_siz->parse_string("Clevels=1"));
    cs_siz->finalize_all();

    EXPECT_GT(broker.get_allocated_bytes(/*local_only=*/false), 0);

    kdu_tile tile = codestream.open_tile(kdu_coords(0, 0), nullptr);
    ASSERT_TRUE(tile.exists());
    absl::Cleanup close_tile = [&tile] {
      if (tile.exists()) tile.close();
    };

    kdu_multi_analysis engine;
    absl::Cleanup destroy_engine = [&engine] {
      if (engine.exists()) engine.destroy();
    };
    engine.create(codestream, tile, /*env=*/nullptr, /*env_queue=*/nullptr,
                  KDU_MULTI_XFORM_DEFAULT_FLAGS, /*roi=*/nullptr,
                  /*buffer_rows=*/1, /*external_sample_allocator=*/nullptr,
                  /*extra_params=*/nullptr, &broker);

    for (int y = 0; y < kHeight; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr);
      ASSERT_NE(line, nullptr);
      for (int x = 0; x < kWidth; ++x) {
        WriteLineSample(line, x, static_cast<kdu_int16>((y * kWidth + x) % 64));
      }
      engine.exchange_line(0, line);
    }

    std::move(destroy_engine).Invoke();
    std::move(close_tile).Invoke();
    kdu_long max_bytes = 1LL << 20;
    codestream.flush(&max_bytes, 1);
    std::move(destroy_cs).Invoke();

    EXPECT_EQ(0, broker.get_allocated_bytes(/*local_only=*/false));
    EXPECT_GT(broker.get_max_allocated_bytes(/*local_only=*/false), 0);
  }
  ASSERT_FALSE(bitstream.empty());

  // 2. Decode with multi-threaded kdu_thread_env so kd_cs_thread_context
  // attaches and detaches its kd_coremem cleanly.
  {
    kdu_membroker broker(1LL << 20);
    kdu_thread_env env;
    env.create();
    absl::Cleanup destroy_env = [&env] {
      if (env.exists()) env.destroy();
    };
    ASSERT_TRUE(env.add_thread());

    VectorSource source(bitstream);
    kdu_codestream codestream;
    absl::Cleanup destroy_cs = [&codestream, &env] {
      if (env.exists()) env.cs_terminate(codestream);
      if (codestream.exists()) codestream.destroy();
    };
    codestream.create(&source, &env, &broker);

    kdu_tile tile = codestream.open_tile(kdu_coords(0, 0), &env);
    ASSERT_TRUE(tile.exists());
    absl::Cleanup close_tile = [&tile, &env] {
      if (tile.exists()) tile.close(&env);
    };

    kdu_multi_synthesis engine;
    absl::Cleanup destroy_engine = [&engine, &env] {
      if (engine.exists()) engine.destroy(&env);
    };
    engine.create(codestream, tile, &env, /*env_queue=*/nullptr,
                  KDU_MULTI_XFORM_DEFAULT_FLAGS, /*buffer_rows=*/1,
                  /*external_sample_allocator=*/nullptr,
                  /*extra_params=*/nullptr, &broker);

    for (int y = 0; y < kHeight; ++y) {
      kdu_line_buf* line = engine.get_line(0, &env);
      ASSERT_NE(line, nullptr);
      for (int x = 0; x < kWidth; ++x) {
        EXPECT_EQ(static_cast<kdu_int16>((y * kWidth + x) % 64),
                  ReadLineSample(line, x));
      }
    }

    std::move(destroy_engine).Invoke();
    std::move(close_tile).Invoke();
    std::move(destroy_cs).Invoke();
    std::move(destroy_env).Invoke();
    EXPECT_EQ(0, broker.get_allocated_bytes(/*local_only=*/false));
  }

  // 3. Interchange codestream creation and cleanup with a broker.
  {
    kdu_membroker broker(1LL << 20);
    siz_params siz;
    ConfigureSmallImageParams(siz, kWidth, kHeight);
    kdu_codestream codestream;
    absl::Cleanup destroy_cs = [&codestream] {
      if (codestream.exists()) codestream.destroy();
    };
    codestream.create(&siz, /*env=*/nullptr, &broker);
    EXPECT_GT(broker.get_allocated_bytes(/*local_only=*/false), 0);
    std::move(destroy_cs).Invoke();
    EXPECT_EQ(0, broker.get_allocated_bytes(/*local_only=*/false));
  }
}

TEST_F(CoreLocalTest, PdescriptorSetBrokerGrowthAndRelease) {
  kdu_membroker broker(1LL << 20);
  {
    kdu_pdescriptor_set pset(&broker);
    ASSERT_TRUE(pset.init(kdu_coords(0, 0), /*packet_bytes_entries=*/4));
    for (int i = 0; i < 20; ++i) {
      const int idx = pset.add_pdescriptor(0, 0, kdu_coords(i, 0));
      EXPECT_EQ(i, idx);
    }
    EXPECT_GT(broker.get_allocated_bytes(/*local_only=*/false), 0);
    pset.reset(/*release_memory=*/true);
  }
  EXPECT_EQ(0, broker.get_allocated_bytes(/*local_only=*/false));
}

TEST_F(CoreLocalTest, BrokerLimitExceededNotifiesFailureAndThrows) {
  // 1. Default note_allocation_failure leads to KDU_ERROR 0x28061702.
  {
    TrackingMemBroker broker(/*limit_bytes=*/64, /*throw_on_failure=*/false);
    kdu_pdescriptor_set pset(&broker);
    ASSERT_TRUE(pset.init(kdu_coords(0, 0), /*packet_bytes_entries=*/4));
    EXPECT_THROW(pset.add_pdescriptor(0, 0, kdu_coords(0, 0)), kdu_exception);
    EXPECT_EQ(1, broker.failure_count());
    EXPECT_GT(broker.last_deficit_bytes(), 0);
    EXPECT_FALSE(broker.last_alloc_failed());
    EXPECT_NE(GlobalErrorHandler().last_message().find(
                  "application-imposed memory limit"),
              std::string::npos);
  }

  // 2. Custom note_allocation_failure exception unwinds before KDU_ERROR.
  GlobalErrorHandler().Clear();
  {
    TrackingMemBroker broker(/*limit_bytes=*/64, /*throw_on_failure=*/true);
    kdu_pdescriptor_set pset(&broker);
    ASSERT_TRUE(pset.init(kdu_coords(0, 0), /*packet_bytes_entries=*/4));
    try {
      pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
      FAIL() << "Expected TrackingMemBroker custom exception";
    } catch (kdu_exception exc) {
      EXPECT_EQ(TrackingMemBroker::kCustomBrokerException, exc);
    }
    EXPECT_EQ(1, broker.failure_count());
    EXPECT_FALSE(broker.last_alloc_failed());
    EXPECT_TRUE(GlobalErrorHandler().last_message().empty());
  }
}

}  // namespace
}  // namespace kdu_core
