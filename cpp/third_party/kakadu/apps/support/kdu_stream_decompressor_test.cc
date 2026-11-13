// Unit tests for
// third_party/kakadu/vendor/apps/support/kdu_stream_decompressor.cpp.

#include "third_party/kakadu/vendor/apps/support/kdu_stream_decompressor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/synchronization/mutex.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace kdu_supp {
namespace {

class RecordingMessageHandler : public kdu_core::kdu_thread_safe_message {
 public:
  explicit RecordingMessageHandler(bool throw_on_end)
      : throw_on_end_(throw_on_end) {}
  ~RecordingMessageHandler() noexcept override = default;

  void put_text(const char* string) override {
    if (string == nullptr) return;
    absl::MutexLock lock(&mu_);
    current_ += string;
  }

  void flush(bool end_of_message = false) override {
    if (!end_of_message) return;
    {
      absl::MutexLock lock(&mu_);
      history_.push_back(std::move(current_));
      current_.clear();
    }
    kdu_core::kdu_thread_safe_message::flush(true);
    if (throw_on_end_) {
      throw KDU_ERROR_EXCEPTION;
    }
  }

  std::vector<std::string> history() const {
    absl::MutexLock lock(&mu_);
    return history_;
  }

  void clear() {
    absl::MutexLock lock(&mu_);
    current_.clear();
    history_.clear();
  }

 private:
  bool throw_on_end_;
  mutable absl::Mutex mu_;
  std::string current_;
  std::vector<std::string> history_;
};

class MemoryTarget : public kdu_core::kdu_compressed_target {
 public:
  int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; }
  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes < 0) return false;
    data.insert(data.end(), buf, buf + num_bytes);
    return true;
  }
  std::vector<kdu_core::kdu_byte> data;
};

std::vector<kdu_core::kdu_byte> EncodeTestCodestream(int width, int height,
                                                     int comps,
                                                     bool reversible = true) {
  kdu_core::siz_params siz;
  siz.set(Scomponents, 0, 0, comps);
  for (int c = 0; c < comps; ++c) {
    siz.set(Sdims, c, 0, height);
    siz.set(Sdims, c, 1, width);
    siz.set(Sprecision, c, 0, 8);
    siz.set(Ssigned, c, 0, false);
  }
  siz.finalize_all();

  MemoryTarget tgt;
  kdu_core::kdu_codestream cs;
  cs.create(&siz, &tgt);
  auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->parse_string("Cblk={16,16}");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor compressor;
  compressor.start(cs);
  std::vector<kdu_core::kdu_byte> pixels(
      static_cast<size_t>(width) * height * comps, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < comps; ++c) {
        pixels[(static_cast<size_t>(y) * width + x) * comps + c] =
            static_cast<kdu_core::kdu_byte>((x * 7 + y * 13 + c * 41) & 0xFF);
      }
    }
  }
  std::vector<int> heights(comps, height);
  std::vector<int> sample_gaps(comps, comps);
  std::vector<int> row_gaps(comps, width * comps);
  std::vector<const kdu_core::kdu_byte*> ptrs(comps);
  for (int c = 0; c < comps; ++c) {
    ptrs[c] = pixels.data() + c;
  }
  // Use sequential multi-plane buffer layout for simplicity & determinism.
  std::vector<std::vector<kdu_core::kdu_byte>> planes(
      comps,
      std::vector<kdu_core::kdu_byte>(static_cast<size_t>(width) * height));
  for (int c = 0; c < comps; ++c) {
    for (size_t i = 0; i < planes[c].size(); ++i) {
      planes[c][i] = pixels[i * comps + c];
    }
    ptrs[c] = planes[c].data();
  }
  compressor.push_stripe(const_cast<kdu_core::kdu_byte**>(ptrs.data()),
                         heights.data());
  compressor.finish();
  return std::move(tgt.data);
}

class MemoryMjcFetchState : public kdu_mjc_stream_fetch_state {
 public:
  explicit MemoryMjcFetchState(std::vector<kdu_core::kdu_byte> stream,
                               size_t chunk_limit = 65536)
      : stream_(std::move(stream)), chunk_limit_(chunk_limit) {}

  int read_bytes(kdu_core::kdu_byte* buf, int num) override {
    if (num <= 0 || pos_ >= stream_.size()) return 0;
    const size_t avail = stream_.size() - pos_;
    const size_t want =
        std::min({avail, static_cast<size_t>(num), chunk_limit_});
    std::memcpy(buf, stream_.data() + pos_, want);
    pos_ += want;
    return static_cast<int>(want);
  }

 private:
  std::vector<kdu_core::kdu_byte> stream_;
  size_t pos_ = 0;
  size_t chunk_limit_;
};

void AppendBe32(std::vector<kdu_core::kdu_byte>& out,
                kdu_core::kdu_uint32 val) {
  out.push_back(static_cast<kdu_core::kdu_byte>((val >> 24) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>((val >> 16) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>((val >> 8) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>(val & 0xFF));
}

class RecordingStreamFilter : public kdu_ovlp_stream_filter {
 public:
  explicit RecordingStreamFilter(int reject_first_n = 0)
      : reject_first_n_(reject_first_n) {}

  bool filter(const kdu_core::kdu_byte* descriptor, size_t descriptor_length,
              kdu_core::kdu_uint64& interpretation,
              double& timestamp) override {
    ++calls_;
    if (calls_ <= reject_first_n_) {
      return false;
    }
    timestamp += 1.5;
    interpretation |= 0x10;
    return true;
  }

  int calls() const { return calls_; }

 private:
  int reject_first_n_;
  int calls_ = 0;
};

class RecordingServiceObserver : public kdu_ovlp_service_observer {
 public:
  void observe(bool success, int pre_discard_count, int post_discard_count,
               size_t num_fetched_bytes,
               kdu_core::kdu_int32 term_code) override {
    ++count_;
    last_buffered_ = num_fetched_bytes;
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
  size_t last_buffered_ = 0;
};

class ConfigurableStreamTransform : public kdu_ovlp_stream_transform {
 public:
  bool transform(kdu_core::kdu_codestream codestream,
                 const kdu_core::kdu_byte* descriptor, size_t descriptor_length,
                 kdu_core::kdu_uint64 interpretation,
                 kdu_core::kdu_thread_env* env) override {
    ++calls_;
    if (restrict_on_call_idx_ == calls_) {
      codestream.apply_input_restrictions(0, 0, 1, 1, nullptr,
                                          kdu_core::KDU_WANT_OUTPUT_COMPONENTS);
      codestream.change_appearance(true, false, true);
    }
    if (calls_ == reject_call_idx_ || reject_all_) {
      return false;
    }
    return true;
  }

  int reject_call_idx_ = -1;
  int restrict_on_call_idx_ = -1;
  bool reject_all_ = false;
  int calls_ = 0;
};

struct BufferedPacket {
  std::vector<kdu_core::kdu_byte> data;
  kdu_core::kdu_uint32 seq_id = 0;
  kdu_core::kdu_uint32 tstamp = 0;
  bool is_main_first = false;
};

class MemoryRtpFetchState : public kdu_rtp_stream_fetch_state {
 public:
  explicit MemoryRtpFetchState(
      std::vector<std::vector<kdu_core::kdu_byte>> pkts)
      : pkts_(std::move(pkts)) {}

  const kdu_core::kdu_byte* get_packet(kdu_core::kdu_uint16& length) override {
    if (stopped_ || idx_ >= pkts_.size()) {
      length = 0;
      return nullptr;
    }
    current_ = pkts_[idx_++];
    length = static_cast<kdu_core::kdu_uint16>(current_.size());
    return current_.data();
  }

  int push_to_reorder_buf(const kdu_core::kdu_byte* pkt,
                          kdu_core::kdu_uint16 length,
                          kdu_core::kdu_uint32 seq_id,
                          kdu_core::kdu_uint32 seq_mask,
                          kdu_core::kdu_uint32 tstamp,
                          bool is_main_first) override {
    ++push_calls_;
    if (reorder_.size() >= capacity_) {
      // Simulate displacing head packet to pending slot.
      displaced_ = std::move(reorder_.begin()->second);
      reorder_.erase(reorder_.begin());
      has_displaced_ = true;
    }
    BufferedPacket bp;
    bp.data.assign(pkt, pkt + length);
    bp.seq_id = seq_id;
    bp.tstamp = tstamp;
    bp.is_main_first = is_main_first;
    reorder_[seq_id] = std::move(bp);
    return has_displaced_ ? -1 : 1;
  }

  const kdu_core::kdu_byte* pull_from_reorder_buf(
      kdu_core::kdu_uint16& length, kdu_core::kdu_uint32& expected_seq_id,
      bool restrict_expected_seq_id_updates, bool main_first_only) override {
    if (has_displaced_) {
      has_displaced_ = false;
      if (!main_first_only || displaced_.is_main_first) {
        if (!restrict_expected_seq_id_updates || displaced_.is_main_first) {
          expected_seq_id = displaced_.seq_id;
        }
        pulled_ = std::move(displaced_.data);
        length = static_cast<kdu_core::kdu_uint16>(pulled_.size());
        return pulled_.data();
      }
    }
    auto it = reorder_.find(expected_seq_id);
    if (it != reorder_.end()) {
      if (!main_first_only || it->second.is_main_first) {
        pulled_ = std::move(it->second.data);
        reorder_.erase(it);
        length = static_cast<kdu_core::kdu_uint16>(pulled_.size());
        return pulled_.data();
      }
    }
    return nullptr;
  }

  void clean_reorder_buf(kdu_core::kdu_uint32 main_first_seq_id,
                         kdu_core::kdu_uint32 tstamp) override {
    ++clean_calls_;
    for (auto it = reorder_.begin(); it != reorder_.end();) {
      if (it->second.tstamp != tstamp) {
        it = reorder_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void shutdown() override { stopped_ = true; }

  void handle_intercepted_packet(const kdu_core::kdu_byte* data,
                                 kdu_core::kdu_uint16 len,
                                 bool first_in_codestream) override {
    intercepted_.emplace_back(data, data + len);
    intercepted_first_flags_.push_back(first_in_codestream);
  }

  int push_calls() const { return push_calls_; }
  int clean_calls() const { return clean_calls_; }
  const std::vector<std::vector<kdu_core::kdu_byte>>& intercepted() const {
    return intercepted_;
  }
  bool stopped() const { return stopped_; }

 private:
  std::vector<std::vector<kdu_core::kdu_byte>> pkts_;
  size_t idx_ = 0;
  std::vector<kdu_core::kdu_byte> current_;
  std::vector<kdu_core::kdu_byte> pulled_;
  std::map<kdu_core::kdu_uint32, BufferedPacket> reorder_;
  BufferedPacket displaced_;
  bool has_displaced_ = false;
  size_t capacity_ = 4;
  bool stopped_ = false;
  int push_calls_ = 0;
  int clean_calls_ = 0;
  std::vector<std::vector<kdu_core::kdu_byte>> intercepted_;
  std::vector<bool> intercepted_first_flags_;
};

std::vector<kdu_core::kdu_byte> BuildRtpPacket(
    kdu_core::kdu_uint16 preamb, kdu_core::kdu_uint16 seq16,
    kdu_core::kdu_uint32 tstamp, kdu_core::kdu_uint32 ssid,
    const std::array<kdu_core::kdu_byte, 8>& payload_hdr,
    const kdu_core::kdu_byte* payload, size_t payload_len, int csrc_words = 0,
    int ext_words = 0, int extra_payload_hdr_words = 0) {
  std::vector<kdu_core::kdu_byte> pkt;
  pkt.push_back(static_cast<kdu_core::kdu_byte>((preamb >> 8) & 0xFF));
  pkt.push_back(static_cast<kdu_core::kdu_byte>(preamb & 0xFF));
  pkt.push_back(static_cast<kdu_core::kdu_byte>((seq16 >> 8) & 0xFF));
  pkt.push_back(static_cast<kdu_core::kdu_byte>(seq16 & 0xFF));
  AppendBe32(pkt, tstamp);
  AppendBe32(pkt, ssid);
  for (int i = 0; i < csrc_words; ++i) {
    AppendBe32(pkt, 0x11111111U + i);
  }
  if ((preamb & 0x1000) != 0) {
    // Wait: RFC3550 ext header is 4 bytes (profile + length_in_words) +
    // 4*ext_words. Note parse_rtp_header reads
    // read_ptr[14+4*csrc_count..15+4*csrc_count] and adds 4*ext_count to
    // msg_header_length (starting at 12+4*csrc).
    pkt.push_back(0xBE);
    pkt.push_back(0xDE);
    pkt.push_back(static_cast<kdu_core::kdu_byte>((ext_words >> 8) & 0xFF));
    pkt.push_back(static_cast<kdu_core::kdu_byte>(ext_words & 0xFF));
    for (int i = 1; i < ext_words; ++i) {
      AppendBe32(pkt, 0x22222222U + i);
    }
  }
  pkt.insert(pkt.end(), payload_hdr.begin(), payload_hdr.end());
  for (int i = 0; i < extra_payload_hdr_words; ++i) {
    AppendBe32(pkt, 0x33333333U + i);
  }
  if (payload_len > 0) {
    pkt.insert(pkt.end(), payload, payload + payload_len);
  }
  return pkt;
}

class KduStreamDecompressorTest : public ::testing::Test {
 protected:
  KduStreamDecompressorTest() : errors_(true), warnings_(false) {}

  void SetUp() override {
    kdu_core::kdu_customize_errors(&errors_);
    kdu_core::kdu_customize_warnings(&warnings_);
  }

  void TearDown() override {
    kdu_core::kdu_customize_errors(nullptr);
    kdu_core::kdu_customize_warnings(nullptr);
  }

  RecordingMessageHandler errors_;
  RecordingMessageHandler warnings_;
};

TEST_F(KduStreamDecompressorTest, MjcThreeNodeRingAndConsumerOptions) {
  const std::vector<kdu_core::kdu_byte> small_cs =
      EncodeTestCodestream(16, 16, 1);
  const std::vector<kdu_core::kdu_byte> large_cs =
      EncodeTestCodestream(128, 128, 3, false);
  const std::vector<kdu_core::kdu_byte> bad_cs = {0xFF, 0x4F, 0xFF, 0x90,
                                                  0x00, 0x04, 0x00, 0x00};
  ASSERT_LE(small_cs.size(), 512U);
  ASSERT_GT(large_cs.size(), 512U);

  // Variable-length stream: [0: small_cs (filtered out)], [1: bad_cs],
  // [2: large_cs], [3: large_cs (restricted)]. Fits cleanly into the 3 nodes
  // (node_a, node_b, node_c) before consumer_open begins.
  std::vector<kdu_core::kdu_byte> mjc_stream;
  for (const auto* cs : {&small_cs, &bad_cs, &large_cs, &large_cs}) {
    AppendBe32(mjc_stream, static_cast<kdu_core::kdu_uint32>(cs->size()));
    mjc_stream.insert(mjc_stream.end(), cs->begin(), cs->end());
  }

  MemoryMjcFetchState fetch_state(mjc_stream);
  fetch_state.init(0, 1.0 / 30.0, true);

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  kdu_mjc_stream_decompressor node_a, node_b, node_c;
  node_a.configure(&fetch_state);
  node_b.configure(&fetch_state);
  node_c.configure(&fetch_state);

  // Build 3-node closed service and consumer rings so donate_equipment() runs.
  node_a.set_service_successor(&node_b);
  node_b.set_service_successor(&node_c);
  node_c.set_service_successor(&node_a);
  node_a.set_consumer_successor(&node_b);
  node_b.set_consumer_successor(&node_c);
  node_c.set_consumer_successor(&node_a);

  RecordingStreamFilter filter(/*reject_first_n=*/1);
  RecordingServiceObserver observer;
  ConfigurableStreamTransform transform;
  transform.reject_all_ = true;
  transform.restrict_on_call_idx_ = 2;

  kdu_core::kdu_membroker broker(1LL << 26);
  kdu_core::kdu_push_pull_params extra;
  node_a.set_resilience_opts(true, false, false, false, true);
  node_a.set_resilience_opts(false, true, true, true, true);
  node_a.set_proc_opts(16, 32, true, false, -1, &extra, true);
  node_a.set_mem_opts(&broker, KDU_SAMPLE_ALLOCATOR_DEF_FRAG_BITS, true);
  node_a.set_live_mode(true, true);
  node_a.set_post_discard_mode(false, true);
  node_a.set_live_mode(false, true);
  node_a.install_stream_filter(&filter, true);
  node_a.install_ovlp_service_observer(&observer, true);
  node_a.install_stream_transform(&transform, true);

  // Empty accessor defaults before consumer_open.
  EXPECT_EQ(node_a.consumer_get_max_stripe_heights(), nullptr);
  EXPECT_EQ(node_a.consumer_get_next_stripe_heights(), nullptr);
  EXPECT_EQ(node_a.consumer_get_num_components(), 0);
  EXPECT_TRUE(node_a.consumer_get_dims(0).is_empty());
  EXPECT_TRUE(node_a.consumer_get_full_dims(0, true).is_empty());
  EXPECT_EQ(node_a.consumer_get_subsampling(0).x, 0);
  EXPECT_EQ(node_a.consumer_get_min_subsampling(0).x, 0);
  EXPECT_EQ(node_a.consumer_get_registration(0, kdu_core::kdu_coords(1, 1)).x,
            0);
  int prec = 99;
  bool is_signed = true;
  EXPECT_FALSE(node_a.consumer_get_precision(0, prec, is_signed));
  EXPECT_EQ(prec, 0);
  EXPECT_FALSE(is_signed);
  int disc = 9, ml = 9;
  kdu_core::kdu_component_access_mode mode =
      kdu_core::KDU_WANT_OUTPUT_COMPONENTS;
  EXPECT_EQ(node_a.consumer_get_input_restrictions(disc, ml, mode), 0);
  bool tr = false, vf = false, hf = false;
  EXPECT_FALSE(node_a.consumer_get_appearance(tr, vf, hf));

  // Service all 3 queued frames synchronously before consuming to avoid
  // concurrent ovlp_open/ovlp_service event UB in unpatched kdu_threads.cpp.
  EXPECT_FALSE(node_a.enter_service(true));
  node_a.service_finished(true);

  kdu_ovlp_stream_decompressor* cur = &node_a;
  int opened_valid = 0;
  int opened_nonfatal_null = 0;
  for (int iter = 0; iter < 6; ++iter) {
    const kdu_core::kdu_byte* desc = nullptr;
    size_t desc_len = 0;
    kdu_core::kdu_uint64 interp = 0;
    double ts = -1.0;
    kdu_stripe_decompressor* eng =
        cur->consumer_open(desc, desc_len, interp, ts, &env);
    if (eng == nullptr) {
      if (desc == nullptr) {
        break;
      }
      // Bad codestream failed open_result==0 cleanly with non-null descriptor.
      ++opened_nonfatal_null;
      EXPECT_EQ(interp, 1ULL | 0x10ULL);
      EXPECT_GE(ts, 1.5);
    } else {
      ++opened_valid;
      EXPECT_FALSE(eng->is_active());
      // Check double consumer_open error guard.
      EXPECT_THROW(cur->consumer_open(desc, desc_len, interp, ts, &env),
                   kdu_core::kdu_exception);
      errors_.clear();

      const int nc = cur->consumer_get_num_components();
      EXPECT_GT(nc, 0);
      EXPECT_NE(cur->consumer_get_max_stripe_heights(), nullptr);
      for (int c = 0; c < nc; ++c) {
        EXPECT_FALSE(cur->consumer_get_dims(c).is_empty());
        EXPECT_FALSE(cur->consumer_get_full_dims(c, true).is_empty());
        EXPECT_GT(cur->consumer_get_subsampling(c).x, 0);
        EXPECT_GT(cur->consumer_get_min_subsampling(c).x, 0);
        (void)cur->consumer_get_registration(c, kdu_core::kdu_coords(1, 1));
        int p = 0;
        bool s = false;
        EXPECT_TRUE(cur->consumer_get_precision(c, p, s));
      }
      (void)cur->consumer_get_input_restrictions(disc, ml, mode);
      EXPECT_TRUE(cur->consumer_get_appearance(tr, vf, hf));
    }
    kdu_core::kdu_int32 term_code = -1;
    size_t comp_bytes = 0;
    kdu_core::kdu_long corrupt_pos = -1;
    bool dec_err = false;
    cur =
        cur->consumer_close(term_code, comp_bytes, corrupt_pos, dec_err, &env);
    ASSERT_NE(cur, nullptr);
  }

  EXPECT_EQ(opened_nonfatal_null, 1);
  EXPECT_EQ(opened_valid, 2);
  EXPECT_GE(filter.calls(), 4);
  EXPECT_GE(observer.count(), 3);

  kdu_core::kdu_int32 tc = 0;
  size_t cb = 0;
  kdu_core::kdu_long cp = 0;
  bool de = false;
  EXPECT_EQ(node_a.consumer_close(tc, cb, cp, de, &env), nullptr);

  node_a.consumer_shutdown(&env, true);
  // Subsequent consumer_open after shutdown returns NULL.
  const kdu_core::kdu_byte* desc = nullptr;
  size_t desc_len = 0;
  kdu_core::kdu_uint64 interp = 0;
  double ts = 0.0;
  EXPECT_EQ(node_b.consumer_open(desc, desc_len, interp, ts, &env), nullptr);
}

TEST_F(KduStreamDecompressorTest, MjcFixedLengthAndSingleStepService) {
  std::vector<kdu_core::kdu_byte> cs1 = EncodeTestCodestream(32, 32, 1);
  const size_t fixed_len = 600;
  ASSERT_LT(cs1.size(), fixed_len);
  cs1.resize(fixed_len, 0);

  std::vector<kdu_core::kdu_byte> mjc_stream = cs1;
  mjc_stream.insert(mjc_stream.end(), cs1.begin(), cs1.end());

  MemoryMjcFetchState fetch_state(mjc_stream);
  fetch_state.init(fixed_len, -1.0, false);

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  kdu_mjc_stream_decompressor a, b;
  ConfigurableStreamTransform transform;
  transform.reject_all_ = true;
  a.configure(&fetch_state);
  b.configure(&fetch_state);
  a.set_consumer_successor(&b);
  b.set_consumer_successor(&a);
  a.set_resilience_opts(false, false, false, false, true);
  a.install_stream_transform(&transform, true);

  // Step service with service_propagate=false (no service ring). Service both
  // nodes upfront, then consume each frame sequentially.
  ASSERT_TRUE(a.enter_service(false));
  ASSERT_TRUE(b.enter_service(false));
  a.service_finished(false);
  b.service_finished(false);
  for (int i = 0; i < 2; ++i) {
    kdu_mjc_stream_decompressor& node = (i == 0) ? a : b;
    const kdu_core::kdu_byte* desc = nullptr;
    size_t desc_len = 0;
    kdu_core::kdu_uint64 interp = 99;
    double ts = 99.0;
    kdu_stripe_decompressor* eng =
        node.consumer_open(desc, desc_len, interp, ts, &env);
    ASSERT_NE(eng, nullptr);
    EXPECT_LT(ts, 0.0);
    EXPECT_EQ(interp, 0ULL);
    EXPECT_FALSE(eng->is_active());
    EXPECT_FALSE(node.consumer_get_dims(0).is_empty());

    kdu_core::kdu_int32 tc = -1;
    size_t cb = 0;
    kdu_core::kdu_long cp = -1;
    bool de = true;
    EXPECT_NE(node.consumer_close(tc, cb, cp, de, &env), nullptr);
    EXPECT_GE(cb, fixed_len);
  }
  a.consumer_shutdown(&env, true);
}

TEST_F(KduStreamDecompressorTest, RtpRfc9828AutoDetectReorderDiscardIntercept) {
  const std::vector<kdu_core::kdu_byte> cs1 = EncodeTestCodestream(64, 64, 1);
  ASSERT_GT(cs1.size(), 120U);

  // Split codestream into Main packet (with XTRAC=1 & P=1 & S=1, CSRC=1, X=1)
  // and two Body packets sent out of order so reorder buffer is exercised.
  const size_t split1 = 60;
  const size_t split2 = cs1.size() - 40;
  const std::array<kdu_core::kdu_byte, 8> rfc9828_main_hdr = {
      0xC0, 0x90, 0x56, 0x00,  // MH=3(0b11), TP=0, P=1, XTRAC=1, PTSTAMP=0x056
      0x41, 0x02, 0x03, 0x04   // S=1, PRIMS=2, TRANS=3, MAT=4
  };
  const std::array<kdu_core::kdu_byte, 8> rfc9828_body_hdr = {
      0x00, 0x00, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00};

  // Helper lambdas build identical frame streams at arbitrary base seq/ts.
  auto make_frame_pkts = [&](kdu_core::kdu_uint16 seq_base,
                             kdu_core::kdu_uint32 ts_val,
                             kdu_core::kdu_uint32 ssrc, bool reorder) {
    std::vector<std::vector<kdu_core::kdu_byte>> out;
    auto p0 = BuildRtpPacket(0x9160, seq_base, ts_val, ssrc, rfc9828_main_hdr,
                             cs1.data(), split1, /*csrc=*/1, /*ext=*/1,
                             /*xtrac=*/1);
    auto p1 =
        BuildRtpPacket(0x8060, seq_base + 1, ts_val, ssrc, rfc9828_body_hdr,
                       cs1.data() + split1, split2 - split1);
    auto p2 =
        BuildRtpPacket(0x80E0, seq_base + 2, ts_val, ssrc, rfc9828_body_hdr,
                       cs1.data() + split2, cs1.size() - split2);
    out.push_back(std::move(p0));
    if (reorder) {
      out.push_back(std::move(p2));
      out.push_back(std::move(p1));
    } else {
      out.push_back(std::move(p1));
      out.push_back(std::move(p2));
    }
    return out;
  };

  std::vector<std::vector<kdu_core::kdu_byte>> pkts;
  // Noise packets skipped before format lock-in:
  // 1) <20 bytes packet
  pkts.push_back(std::vector<kdu_core::kdu_byte>(10, 0x80));
  // 2) wrong RTP version
  pkts.push_back(std::vector<kdu_core::kdu_byte>(24, 0x00));
  // 3) invalid CSRC truncation (rtp_error == 2)
  pkts.push_back(BuildRtpPacket(0x8F60, 1, 1000, 0xAABBCCDD, rfc9828_main_hdr,
                                nullptr, 0));
  // 4) unrecognized hi_nibble for auto-detect
  pkts.push_back(BuildRtpPacket(0x8060, 2, 1000, 0xAABBCCDD,
                                {0x50, 0, 0, 0, 0, 0, 0, 0}, cs1.data(), 10));
  // 5) RFC9828 hi_nibble candidate without SOC marker (0xFF 0x4F)
  const std::array<kdu_core::kdu_byte, 4> non_soc_bytes = {0, 0, 0, 0};
  pkts.push_back(BuildRtpPacket(0x8060, 3, 1000, 0xAABBCCDD,
                                {0xC0, 0x00, 0, 0, 0, 0, 0, 0},
                                non_soc_bytes.data(), non_soc_bytes.size()));

  // Frame 0 (will be discarded by initial_discard_count=1).
  for (auto& p : make_frame_pkts(10, 0x1056, 0x12345678U, false)) {
    pkts.push_back(std::move(p));
  }
  // Frame 1 (will be intercepted by initial_intercept_count=1).
  for (auto& p : make_frame_pkts(20, 0x2056, 0x12345678U, false)) {
    pkts.push_back(std::move(p));
  }
  // Wrong SSRC packet + stale sequence packet + Frame 2 (reordered body pkts).
  pkts.push_back(BuildRtpPacket(0x80E0, 25, 0x3056, 0xDEADBEEFU,
                                rfc9828_main_hdr, cs1.data(), split1));
  // Frame 2: starts at seq 30 (pushed/reordered cleanly).
  for (auto& p : make_frame_pkts(30, 0x3056, 0x12345678U, true)) {
    pkts.push_back(std::move(p));
  }
  // Frame 3: discontinuous backwards sequence jump (seq=5 < 33) with new ts.
  for (auto& p : make_frame_pkts(5, 0x4056, 0x12345678U, false)) {
    pkts.push_back(std::move(p));
  }

  MemoryRtpFetchState fetch_state(std::move(pkts));
  fetch_state.init(0, false, 0);  // auto-detect format + SSRC
  fetch_state.configure_codestream_handling(1, 1, false, 0.0);

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  kdu_rtp_stream_decompressor rtp_a, rtp_b;
  ConfigurableStreamTransform transform;
  transform.reject_all_ = true;
  rtp_a.configure(&fetch_state);
  rtp_b.configure(&fetch_state);
  rtp_a.set_service_successor(&rtp_b);
  rtp_b.set_service_successor(&rtp_a);
  rtp_a.set_consumer_successor(&rtp_b);
  rtp_b.set_consumer_successor(&rtp_a);
  rtp_a.install_stream_transform(&transform, true);

  EXPECT_FALSE(rtp_a.enter_service(true));
  rtp_a.service_finished(true);

  kdu_ovlp_stream_decompressor* cur = &rtp_a;
  int decoded_frames = 0;
  for (int i = 0; i < 4; ++i) {
    const kdu_core::kdu_byte* desc = nullptr;
    size_t desc_len = 0;
    kdu_core::kdu_uint64 interp = 0;
    double ts = -1.0;
    kdu_stripe_decompressor* eng =
        cur->consumer_open(desc, desc_len, interp, ts, &env);
    if (eng == nullptr) break;
    EXPECT_GT(desc_len, 20U);
    EXPECT_FALSE(cur->consumer_get_dims(0).is_empty());
    ++decoded_frames;
    kdu_core::kdu_int32 tc = -1;
    size_t cb = 0;
    kdu_core::kdu_long cp = -1;
    bool de = true;
    cur = cur->consumer_close(tc, cb, cp, de, &env);
  }

  EXPECT_EQ(decoded_frames, 2);
  EXPECT_EQ(fetch_state.intercepted().size(), 3U);
  EXPECT_GE(fetch_state.push_calls(), 1);
  EXPECT_GE(fetch_state.clean_calls(), 2);

  rtp_a.stop_all_fetching(true);
  EXPECT_TRUE(fetch_state.stopped());
  rtp_a.consumer_shutdown(&env, true);
}

TEST_F(KduStreamDecompressorTest, RtpLegacyRfc5371FormatAndErrorPaths) {
  const std::vector<kdu_core::kdu_byte> cs1 = EncodeTestCodestream(48, 48, 1);
  ASSERT_GT(cs1.size(), 90U);

  // Old RFC5371/2 format: high nibble 0011 (TP=0, MH=3) = 0x30.
  const std::array<kdu_core::kdu_byte, 8> old_main_hdr = {
      0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const std::array<kdu_core::kdu_byte, 8> old_body_hdr = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  const size_t split = 50;
  std::vector<std::vector<kdu_core::kdu_byte>> pkts;
  // Auto-detect candidate with old hi_nibble=3 but missing SOC marker.
  const std::array<kdu_core::kdu_byte, 4> non_soc = {0x00, 0x00, 0x00, 0x00};
  pkts.push_back(BuildRtpPacket(0x8060, 99, 8000, 0x01020304U, old_main_hdr,
                                non_soc.data(), non_soc.size()));
  // Intercepted packets with strip_intercepted_headers=true & seq gap
  // (100->102).
  pkts.push_back(BuildRtpPacket(0x8060, 100, 9000, 0x01020304U, old_main_hdr,
                                cs1.data(), split));
  pkts.push_back(BuildRtpPacket(0x80E0, 102, 9000, 0x01020304U, old_main_hdr,
                                cs1.data(), split));
  // Valid old-format codestream using auto-detect, plus out-of-order body
  // packet and duplicated stale packet.
  pkts.push_back(BuildRtpPacket(0x8060, 200, 18000, 0x01020304U, old_main_hdr,
                                cs1.data(), split));
  pkts.push_back(BuildRtpPacket(0x8060, 199, 18000, 0x01020304U, old_main_hdr,
                                cs1.data(), split));
  pkts.push_back(BuildRtpPacket(0x80E0, 201, 18000, 0x01020304U, old_body_hdr,
                                cs1.data() + split, cs1.size() - split));
  // Second frame where second packet has malformed RTP header (error code 2).
  pkts.push_back(BuildRtpPacket(0x8060, 202, 27000, 0x01020304U, old_main_hdr,
                                cs1.data(), split));
  pkts.push_back(BuildRtpPacket(0x8F60, 203, 27000, 0x01020304U, old_body_hdr,
                                nullptr, 0));

  MemoryRtpFetchState fetch_state(std::move(pkts));
  fetch_state.init(0x01020304U, true, 0);
  fetch_state.configure_codestream_handling(0, 1, true, 0.0);

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  kdu_rtp_stream_decompressor rtp_a, rtp_b, rtp_c;
  ConfigurableStreamTransform transform;
  transform.reject_all_ = true;
  rtp_a.configure(&fetch_state);
  rtp_b.configure(&fetch_state);
  rtp_c.configure(&fetch_state);
  rtp_a.set_service_successor(&rtp_b);
  rtp_b.set_service_successor(&rtp_c);
  rtp_c.set_service_successor(&rtp_a);
  rtp_a.set_consumer_successor(&rtp_b);
  rtp_b.set_consumer_successor(&rtp_c);
  rtp_c.set_consumer_successor(&rtp_a);
  rtp_a.install_stream_transform(&transform, true);

  EXPECT_FALSE(rtp_a.enter_service(true));
  rtp_a.service_finished(true);

  kdu_ovlp_stream_decompressor* cur = &rtp_a;
  int opened = 0;
  for (int i = 0; i < 6; ++i) {
    const kdu_core::kdu_byte* desc = nullptr;
    size_t desc_len = 0;
    kdu_core::kdu_uint64 interp = 0;
    double ts = -1.0;
    kdu_stripe_decompressor* eng =
        cur->consumer_open(desc, desc_len, interp, ts, &env);
    if (eng == nullptr) {
      if (desc == nullptr) break;
    } else {
      ++opened;
      EXPECT_FALSE(cur->consumer_get_dims(0).is_empty());
    }
    kdu_core::kdu_int32 tc = -1;
    size_t cb = 0;
    kdu_core::kdu_long cp = -1;
    bool de = false;
    cur = cur->consumer_close(tc, cb, cp, de, &env);
  }

  EXPECT_GE(opened, 1);
  EXPECT_EQ(fetch_state.intercepted().size(), 1U);
  rtp_a.consumer_shutdown(&env, true);
}

TEST_F(KduStreamDecompressorTest, MjcRestartComponentResizeFussyAndSkip) {
  const std::vector<kdu_core::kdu_byte> skip_cs =
      EncodeTestCodestream(128, 128, 3, false);
  const std::vector<kdu_core::kdu_byte> cs_1comp =
      EncodeTestCodestream(32, 32, 1);
  const std::vector<kdu_core::kdu_byte> cs_3comp =
      EncodeTestCodestream(128, 128, 3, false);
  ASSERT_GT(skip_cs.size(), 512U);

  std::vector<kdu_core::kdu_byte> mjc_stream;
  for (const auto* cs : {&skip_cs, &cs_1comp, &cs_3comp}) {
    AppendBe32(mjc_stream, static_cast<kdu_core::kdu_uint32>(cs->size()));
    mjc_stream.insert(mjc_stream.end(), cs->begin(), cs->end());
  }

  MemoryMjcFetchState fetch_state(mjc_stream);
  fetch_state.init(0, 1.0 / 30.0, false);

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  kdu_mjc_stream_decompressor node;
  RecordingStreamFilter filter(/*reject_first_n=*/1);
  ConfigurableStreamTransform transform;
  transform.reject_all_ = true;
  node.configure(&fetch_state);
  node.set_resilience_opts(true, false, false, false, false);
  node.set_proc_opts(16, 32, false, false, -1, nullptr, false);
  node.install_stream_filter(&filter, false);
  node.install_stream_transform(&transform, false);

  // Frame 0 (skip_cs > 512 bytes) is rejected by filter so raw_fetch_to_next
  // exercises skipping remaining_bytes > 0; Frame 1 (1-comp) opens fussy.
  ASSERT_TRUE(node.enter_service(false));
  const kdu_core::kdu_byte* desc = nullptr;
  size_t desc_len = 0;
  kdu_core::kdu_uint64 interp = 0;
  double ts = -1.0;
  kdu_stripe_decompressor* eng =
      node.consumer_open(desc, desc_len, interp, ts, &env);
  ASSERT_NE(eng, nullptr);
  EXPECT_EQ(node.consumer_get_num_components(), 1);
  kdu_core::kdu_int32 tc = 0;
  size_t cb = 0;
  kdu_core::kdu_long cp = 0;
  bool de = false;
  EXPECT_EQ(node.consumer_close(tc, cb, cp, de, &env), nullptr);

  // Reopen the same node with Frame 2 (3-comp) -> codestream.restart() and
  // reallocation of max_stripe_heights/working_stripe_heights.
  ASSERT_TRUE(node.enter_service(false));
  node.service_finished(false);
  eng = node.consumer_open(desc, desc_len, interp, ts, &env);
  ASSERT_NE(eng, nullptr);
  EXPECT_EQ(node.consumer_get_num_components(), 3);
  EXPECT_EQ(node.consumer_close(tc, cb, cp, de, &env), nullptr);
  // Intentionally omit consumer_shutdown so ~kdu_ovlp_stream_decompressor()
  // destroys the live codestream.
}

TEST_F(KduStreamDecompressorTest,
       RtpMultiPacketMainHeaderAndMidFrameEdgeCases) {
  const std::vector<kdu_core::kdu_byte> cs1 = EncodeTestCodestream(64, 64, 1);
  ASSERT_GT(cs1.size(), 160U);
  const size_t split1 = 60;
  const size_t split2 = 120;

  // Part 1: RFC9828 multi-packet MH, mid-stream noise/error, unexpected MH,
  // backwards discontinuous jump, type mismatch, and trailing reorder EOF.
  const std::array<kdu_core::kdu_byte, 8> new_mh1_p = {
      0x40, 0x90, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=1, P=1, XTRAC=1
  const std::array<kdu_core::kdu_byte, 8> new_mh2_p = {
      0x80, 0x90, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=2, P=1, XTRAC=1
  const std::array<kdu_core::kdu_byte, 8> new_mh3_p = {
      0xC0, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=3, P=1, XTRAC=1
  const std::array<kdu_core::kdu_byte, 8> new_mh3_tp1 = {
      0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=3, TP=1
  const std::array<kdu_core::kdu_byte, 8> new_body_nop = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const std::array<kdu_core::kdu_byte, 8> new_body_tp1 = {
      0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // body, TP=1

  kdu_core::kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  auto env_cleanup = absl::MakeCleanup([&]() {
    env.terminate(nullptr);
    env.destroy();
  });

  auto drain_rtp = [&](MemoryRtpFetchState& fetch_state) {
    kdu_rtp_stream_decompressor node;
    ConfigurableStreamTransform transform;
    transform.reject_all_ = true;
    node.configure(&fetch_state);
    node.install_stream_transform(&transform, false);
    for (int i = 0; i < 8 && node.enter_service(false); ++i) {
      const kdu_core::kdu_byte* desc = nullptr;
      size_t desc_len = 0;
      kdu_core::kdu_uint64 interp = 0;
      double ts = -1.0;
      (void)node.consumer_open(desc, desc_len, interp, ts, &env);
      kdu_core::kdu_int32 tc = 0;
      size_t cb = 0;
      kdu_core::kdu_long cp = 0;
      bool de = false;
      (void)node.consumer_close(tc, cb, cp, de, &env);
    }
    node.service_finished(false);
    node.consumer_shutdown(&env, false);
  };

  {
    std::vector<std::vector<kdu_core::kdu_byte>> pkts;
    // Frame 0 (ts=0x1005, P=1 PTSTAMP=0x005): MH=1 -> noise -> MH=2 -> spurious
    // MH=3 mid-body (!in_main_header).
    pkts.push_back(BuildRtpPacket(0x8060, 10, 0x1005, 0x12345678U, new_mh1_p,
                                  cs1.data(), split1, /*csrc=*/0, /*ext=*/0,
                                  /*xtrac=*/1));
    pkts.push_back(std::vector<kdu_core::kdu_byte>(10, 0x80));  // <20 bytes
    pkts.push_back(std::vector<kdu_core::kdu_byte>(24, 0x00));  // rtp_error==1
    pkts.push_back(BuildRtpPacket(0x8060, 11, 0x1005, 0x12345678U, new_mh2_p,
                                  cs1.data() + split1, split2 - split1,
                                  /*csrc=*/0, /*ext=*/0, /*xtrac=*/1));
    pkts.push_back(BuildRtpPacket(0x8060, 12, 0x1005, 0x12345678U, new_mh3_p,
                                  cs1.data(), 16, /*csrc=*/0, /*ext=*/0,
                                  /*xtrac=*/1));

    // Frame 1 (seq=20, ts=0x2000): stale seq=19 same ts -> discontinuous jump
    // backwards seq=5, ts=0x3000 with main_first_packet.
    pkts.push_back(BuildRtpPacket(0x8060, 20, 0x2000, 0x12345678U, new_mh3_tp1,
                                  cs1.data(), split1, /*csrc=*/0, /*ext=*/0,
                                  /*xtrac=*/1));
    pkts.push_back(BuildRtpPacket(0x8060, 19, 0x2000, 0x12345678U, new_body_tp1,
                                  cs1.data() + split1, 10));
    pkts.push_back(BuildRtpPacket(0x8060, 5, 0x3000, 0x12345678U, new_mh3_p,
                                  cs1.data(), split1, /*csrc=*/0, /*ext=*/0,
                                  /*xtrac=*/1));

    // Frame 2 (seq=5, ts=0x3000) receives TP=1 mismatch -> starts Frame 3
    // with body packet having new timestamp (!main_first_packet && tstamp_mm).
    pkts.push_back(BuildRtpPacket(0x8060, 6, 0x3000, 0x12345678U, new_body_tp1,
                                  cs1.data() + split1, 10));
    pkts.push_back(BuildRtpPacket(0x8060, 30, 0x4000, 0x12345678U, new_mh3_p,
                                  cs1.data(), split1, /*csrc=*/0, /*ext=*/0,
                                  /*xtrac=*/1));
    pkts.push_back(BuildRtpPacket(0x8060, 31, 0x5000, 0x12345678U, new_body_nop,
                                  cs1.data() + split1, 10));

    MemoryRtpFetchState fetch_state(std::move(pkts));
    fetch_state.init(0x12345678U, true, 1);  // new_format = true
    drain_rtp(fetch_state);
  }

  // Part 2: Old RFC5371 multi-packet MH, !in_main_header, stale seq, reorder
  // push in fetch_data, and backwards discontinuous jump.
  const std::array<kdu_core::kdu_byte, 8> old_mh1 = {
      0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=1
  const std::array<kdu_core::kdu_byte, 8> old_mh2 = {
      0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=2 (MH_LAST)
  const std::array<kdu_core::kdu_byte, 8> old_mh3 = {
      0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // MH=3
  const std::array<kdu_core::kdu_byte, 8> old_body = {0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x00, 0x00};

  std::vector<std::vector<kdu_core::kdu_byte>> old_pkts;
  old_pkts.push_back(BuildRtpPacket(0x8060, 10, 1000, 0x12345678U, old_mh1,
                                    cs1.data(), split1));
  old_pkts.push_back(BuildRtpPacket(0x8060, 11, 1000, 0x12345678U, old_mh2,
                                    cs1.data() + split1, split2 - split1));
  old_pkts.push_back(
      BuildRtpPacket(0x8060, 12, 1000, 0x12345678U, old_mh3, cs1.data(), 16));
  // Frame 1 (seq=20, ts=2000): stale seq=19, future seq=22 -> reorder buffer,
  // then discontinuous backwards jump seq=5 with ts=3000 & MH=3.
  old_pkts.push_back(BuildRtpPacket(0x8060, 20, 2000, 0x12345678U, old_mh3,
                                    cs1.data(), split1));
  old_pkts.push_back(BuildRtpPacket(0x8060, 19, 2000, 0x12345678U, old_body,
                                    cs1.data() + split1, 10));
  old_pkts.push_back(BuildRtpPacket(0x80E0, 22, 2000, 0x12345678U, old_body,
                                    cs1.data() + split1, 10));
  old_pkts.push_back(BuildRtpPacket(0x8060, 5, 3000, 0x12345678U, old_mh3,
                                    cs1.data(), split1));

  MemoryRtpFetchState old_fetch(std::move(old_pkts));
  old_fetch.init(0x12345678U, true, -1);  // old_format = true
  drain_rtp(old_fetch);
}

}  // namespace
}  // namespace kdu_supp
