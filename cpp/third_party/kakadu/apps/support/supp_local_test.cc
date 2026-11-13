// Unit tests for third_party/kakadu/vendor/apps/support/supp_local.cpp.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/support/kdu_region_decompressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
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

  void put_text(const char* string) override { current_ += string; }
  void flush(bool end_of_message = false) override {
    if (!end_of_message) return;
    history_.push_back(std::move(current_));
    current_.clear();
    kdu_core::kdu_thread_safe_message::flush(true);
    if (throw_on_end_) {
      throw KDU_ERROR_EXCEPTION;
    }
  }

  void clear() {
    current_.clear();
    history_.clear();
  }

  const std::vector<std::string>& history() const { return history_; }

 private:
  bool throw_on_end_;
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

class MemorySource : public kdu_core::kdu_compressed_source {
 public:
  explicit MemorySource(std::vector<kdu_core::kdu_byte> bytes)
      : bytes_(std::move(bytes)) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }
  kdu_core::kdu_long get_pos() override {
    return static_cast<kdu_core::kdu_long>(pos_);
  }
  bool seek(kdu_core::kdu_long offset) override {
    if (offset < 0) return false;
    pos_ = std::min(static_cast<size_t>(offset), bytes_.size());
    return true;
  }
  int read(kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= bytes_.size()) return 0;
    const size_t avail = bytes_.size() - pos_;
    const size_t n = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, bytes_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

 private:
  std::vector<kdu_core::kdu_byte> bytes_;
  size_t pos_ = 0;
};

std::vector<kdu_core::kdu_byte> EncodeTinyCodestream(int width = 32,
                                                     int height = 32) {
  kdu_core::siz_params siz;
  siz.set(Scomponents, 0, 0, 1);
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Sprecision, 0, 0, 8);
  siz.set(Ssigned, 0, 0, false);
  siz.finalize_all();

  MemoryTarget tgt;
  kdu_core::kdu_codestream cs;
  cs.create(&siz, &tgt);
  auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->parse_string("Cblk={16,16}");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor compressor;
  compressor.start(cs);
  std::vector<kdu_core::kdu_byte> pixels(static_cast<size_t>(width) * height,
                                         128);
  int heights[1] = {height};
  compressor.push_stripe(pixels.data(), heights);
  compressor.finish();
  return std::move(tgt.data);
}

class PolicyMembroker : public kdu_core::kdu_membroker {
 public:
  enum class Mode {
    kAllowAll,
    kDenyPartialOnExact,
    kDenyAll,
    kCappedPref,
  };

  explicit PolicyMembroker(Mode mode, kdu_core::kdu_int64 cap = 0)
      : kdu_membroker(-1), mode_(mode), cap_(cap) {}

  void set_mode(Mode mode, kdu_core::kdu_int64 cap = 0) {
    mode_ = mode;
    cap_ = cap;
  }

  kdu_core::kdu_int64 request(kdu_core::kdu_int64 min_bytes,
                              kdu_core::kdu_int64 pref_bytes,
                              kdu_core::kdu_int64 generator_limit) override {
    if (mode_ == Mode::kDenyAll) {
      return 0;
    }
    if (mode_ == Mode::kDenyPartialOnExact) {
      if (min_bytes == pref_bytes) {
        return 0;
      }
      const kdu_core::kdu_int64 grant = std::min(min_bytes, pref_bytes - 1);
      granted_ += grant;
      return grant;
    }
    if (mode_ == Mode::kCappedPref) {
      const kdu_core::kdu_int64 grant = std::max(min_bytes, cap_);
      const kdu_core::kdu_int64 clamped = std::min(grant, pref_bytes);
      granted_ += clamped;
      return clamped;
    }
    granted_ += pref_bytes;
    return pref_bytes;
  }

  void release(kdu_core::kdu_int64 returned_bytes) override {
    released_ += returned_bytes;
  }

  void note_allocation_failure(kdu_core::kdu_int64 deficit_bytes,
                               kdu_core::kdu_int64 allocated_bytes,
                               bool alloc_failed) override {
    ++failure_notifications_;
    last_deficit_ = deficit_bytes;
    last_alloc_failed_ = alloc_failed;
    kdu_membroker::note_allocation_failure(deficit_bytes, allocated_bytes,
                                           alloc_failed);
  }

  kdu_core::kdu_int64 granted() const { return granted_; }
  kdu_core::kdu_int64 released() const { return released_; }
  int failure_notifications() const { return failure_notifications_; }
  bool last_alloc_failed() const { return last_alloc_failed_; }

 private:
  Mode mode_;
  kdu_core::kdu_int64 cap_;
  kdu_core::kdu_int64 granted_ = 0;
  kdu_core::kdu_int64 released_ = 0;
  int failure_notifications_ = 0;
  kdu_core::kdu_int64 last_deficit_ = 0;
  bool last_alloc_failed_ = false;
};

class SuppLocalTest : public ::testing::Test {
 protected:
  SuppLocalTest() : errors_(true), warnings_(false) {}

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

TEST_F(SuppLocalTest, MembrokerAttachDetachAndOverlimitGrantAndDeny) {
  const std::vector<kdu_core::kdu_byte> bytes = EncodeTinyCodestream(32, 32);

  // 1. Exercise attach/detach/reattach/partial-reject via
  // kdu_region_decompressor.
  PolicyMembroker broker1(PolicyMembroker::Mode::kAllowAll);
  PolicyMembroker broker2(PolicyMembroker::Mode::kDenyPartialOnExact);
  {
    kdu_region_decompressor decomp;
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    auto cs_cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, nullptr, 0, 0, kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        decomp.start(cs, nullptr, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
    kdu_core::kdu_byte buf[32 * 32] = {};
    kdu_core::kdu_byte* ch[1] = {buf};
    kdu_core::kdu_dims incomplete = region;
    kdu_core::kdu_dims new_region;
    while (!incomplete.is_empty()) {
      ASSERT_TRUE(decomp.process(ch, false, 1, region.pos, region.size.x, 1024,
                                 1024, incomplete, new_region));
    }
    EXPECT_TRUE(decomp.finish());

    // After finish(), suppmem holds allocated tile_banks (> 0 bytes).
    // Attach broker1, attach broker1 again (same broker no-op), attempt
    // partial-grant attach of broker2 (fails & releases granted), attach
    // nullptr (detach), attach nullptr again (detach when already null), and
    // re-attach broker1 so ~kd_suppmem() detaches cleanly on destruction.
    decomp.mem_configure(&broker1, 0);
    EXPECT_GT(broker1.granted(), 0);
    decomp.mem_configure(&broker1, 0);
    decomp.mem_configure(&broker2, 0);
    EXPECT_EQ(broker2.granted(), broker2.released());
    decomp.mem_configure(nullptr, 0);
    decomp.mem_configure(nullptr, 0);
    decomp.mem_configure(&broker1, 0);
  }
  EXPECT_EQ(broker1.granted(), broker1.released());

  // 2. Attached from the start with capped grants -> handle_overlimit_alloc
  //    requests and succeeds when min_bytes <= granted.
  {
    PolicyMembroker capped(PolicyMembroker::Mode::kCappedPref, 16);
    kdu_region_decompressor decomp;
    decomp.mem_configure(&capped, 0);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    auto cs_cleanup = absl::MakeCleanup([&]() { cs.destroy(); });
    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, nullptr, 0, 0, kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        decomp.start(cs, nullptr, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
    EXPECT_TRUE(decomp.finish());
    decomp.mem_configure(nullptr, 0);
    EXPECT_GT(capped.granted(), 0);
    EXPECT_EQ(capped.granted(), capped.released());
  }

  // 3. Attached from the start with DenyAll -> handle_overlimit_alloc invokes
  //    note_allocation_failure() and raises KDU_ERROR(0x28061702).
  {
    PolicyMembroker deny(PolicyMembroker::Mode::kDenyAll);
    kdu_region_decompressor decomp;
    decomp.mem_configure(&deny, 0);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    auto cs_cleanup = absl::MakeCleanup([&]() { cs.destroy(); });
    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, nullptr, 0, 0, kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(1, 1));
    EXPECT_THROW(
        decomp.start(cs, nullptr, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)),
        kdu_core::kdu_exception);
    EXPECT_GE(deny.failure_notifications(), 1);
    EXPECT_FALSE(deny.last_alloc_failed());
    EXPECT_FALSE(errors_.history().empty());
    errors_.clear();
  }
}

}  // namespace
}  // namespace kdu_supp
