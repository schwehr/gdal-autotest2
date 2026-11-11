// Unit tests for third_party/kakadu/vendor/coresys/compressed/blocks.cpp and
// kdu_block in third_party/kakadu/vendor/coresys/common/kdu_compressed.h.

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  void put_text(const char* /*string*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

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

// Opens an output codestream and a single LL code-block so tests can exercise
// kdu_block (whose constructor/destructor are private), closing and destroying
// cleanly on both normal exit and kdu_exception unwinding.
class ScopedOutputBlock {
 public:
  ScopedOutputBlock() : target_(compressed_bytes_) {
    siz_params siz;
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, 16);
    siz.set(Sdims, 0, 1, 16);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params&>(siz).finalize();

    codestream_.create(&siz, &target_);
    codestream_.access_siz()->parse_string("Clevels=0");
    codestream_.access_siz()->finalize_all();

    tile_ = codestream_.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile_.access_component(0);
    kdu_resolution res = tc.access_resolution(0);
    subband_ = res.access_subband(LL_BAND);
    block_ = subband_.open_block(kdu_coords(0, 0));
    if (block_ != nullptr) {
      // Output codestreams leave num_passes and missing_msbs uninitialized
      // until the encoder populates them; initialize before close_block().
      orig_modes_ = block_->modes;
      orig_size_ = block_->size;
      orig_region_ = block_->region;
      orig_inset_ = block_->inset;
      orig_transpose_ = block_->transpose;
      orig_vflip_ = block_->vflip;
      orig_hflip_ = block_->hflip;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
    }
  }

  ~ScopedOutputBlock() {
    if (block_ != nullptr) {
      block_->modes = orig_modes_;
      block_->size = orig_size_;
      block_->region = orig_region_;
      block_->inset = orig_inset_;
      block_->transpose = orig_transpose_;
      block_->vflip = orig_vflip_;
      block_->hflip = orig_hflip_;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
      subband_.close_block(block_);
      block_ = nullptr;
    }
    if (tile_.exists()) {
      tile_.close();
    }
    if (codestream_.exists()) {
      codestream_.destroy();
    }
  }

  ScopedOutputBlock(const ScopedOutputBlock&) = delete;
  ScopedOutputBlock& operator=(const ScopedOutputBlock&) = delete;

  kdu_block* block() const { return block_; }

 private:
  std::vector<kdu_byte> compressed_bytes_;
  VectorTarget target_;
  kdu_codestream codestream_;
  kdu_tile tile_;
  kdu_subband subband_;
  kdu_block* block_ = nullptr;
  int orig_modes_ = 0;
  kdu_coords orig_size_;
  kdu_dims orig_region_;
  kdu_coords orig_inset_;
  bool orig_transpose_ = false;
  bool orig_vflip_ = false;
  bool orig_hflip_ = false;
};

class BlocksTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { kdu_customize_errors(GlobalErrorHandler()); }
};

TEST_F(BlocksTest, SetMaxPassesCopyGrowReallocateAndRejectOverflow) {
  ScopedOutputBlock ctx;
  kdu_block* block = ctx.block();
  ASSERT_NE(block, nullptr);

  // 1. Default capacity and no-op call below current capacity.
  EXPECT_EQ(block->max_passes, 96);
  EXPECT_EQ(block->get_max_passes(), 96);
  block->set_max_passes(96);
  block->set_max_passes(32);
  EXPECT_EQ(block->max_passes, 96);

  // 2. Populate existing arrays before growing with copy_existing=true.
  for (int i = 0; i < 96; ++i) {
    block->pass_lengths[i] = i + 1;
    block->pass_slopes[i] = static_cast<kdu_uint16>(0xFF00 - i);
  }
  block->set_max_passes(128, /*copy_existing=*/true);
  EXPECT_EQ(block->max_passes, 128);
  for (int i = 0; i < 96; ++i) {
    EXPECT_EQ(block->pass_lengths[i], i + 1);
    EXPECT_EQ(block->pass_slopes[i], static_cast<kdu_uint16>(0xFF00 - i));
  }

  // 3. Grow a second time with copy_existing=false to free prior heap handle.
  block->set_max_passes(192, /*copy_existing=*/false);
  EXPECT_EQ(block->max_passes, 192);
  block->pass_lengths[191] = 777;
  block->pass_slopes[191] = 1234;
  EXPECT_EQ(block->pass_lengths[191], 777);
  EXPECT_EQ(block->pass_slopes[191], 1234);

  // 4. Values >= 256 trigger a fatal KDU_ERROR_DEV.
  EXPECT_THROW(block->set_max_passes(256), kdu_exception);
}

TEST_F(BlocksTest, SetMaxBytesAlignsCopiesPreambleReallocatesAndRejectsHuge) {
  ScopedOutputBlock ctx;
  kdu_block* block = ctx.block();
  ASSERT_NE(block, nullptr);

  // 1. Initial internal byte_buffer guarantees [-32, max_bytes+63] and 32B
  // align.
  const int initial_max_bytes = 16384 - 128;
  EXPECT_EQ(block->max_bytes, initial_max_bytes);
  EXPECT_EQ(block->get_max_bytes(), initial_max_bytes);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block->byte_buffer) & 31, 0u);

  block->set_max_bytes(initial_max_bytes);
  EXPECT_EQ(block->max_bytes, initial_max_bytes);

  // 2. Initialize preamble [-32, -1] and body [0, initial_max_bytes - 1], then
  // grow with a size that is not a multiple of 64 to check rounding.
  for (int i = -32; i < initial_max_bytes; ++i) {
    block->byte_buffer[i] = static_cast<kdu_byte>((i * 13 + 7) & 0xFF);
  }
  block->set_max_bytes(20000, /*copy_existing=*/true);
  EXPECT_EQ(block->max_bytes, 20032);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block->byte_buffer) & 31, 0u);
  for (int i = -32; i < initial_max_bytes; ++i) {
    EXPECT_EQ(block->byte_buffer[i],
              static_cast<kdu_byte>((i * 13 + 7) & 0xFF));
  }
  for (int i = -32; i < block->max_bytes + 64; ++i) {
    block->byte_buffer[i] = 0x5A;
  }

  // 3. Reallocate with copy_existing=false to delete previous byte_handle.
  block->set_max_bytes(32768, /*copy_existing=*/false);
  EXPECT_EQ(block->max_bytes, 32768);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block->byte_buffer) & 31, 0u);

  // 4. Requests exceeding 1 MiB throw a KDU_ERROR_DEV.
  EXPECT_THROW(block->set_max_bytes((1 << 20) + 1), kdu_exception);
}

TEST_F(BlocksTest, AllocateSampleContextAndIctxtBuffersAcrossGrowthSteps) {
  ScopedOutputBlock ctx;
  kdu_block* block = ctx.block();
  ASSERT_NE(block, nullptr);

  // 1. 32-bit sample_buffer (blk_handles[2]), hide, restore, and grow again.
  block->set_max_samples(100);
  EXPECT_GE(block->max_samples, 100);
  ASSERT_NE(block->sample_buffer, nullptr);
  for (int i = -2 * KDU_ALIGN_SAMPLES32;
       i < block->max_samples + 4 * KDU_ALIGN_SAMPLES32; ++i) {
    block->sample_buffer[i] = i;
  }
  block->set_max_samples(50);
  block->hide_sample_buffer();
  EXPECT_EQ(block->sample_buffer, nullptr);
  EXPECT_EQ(block->max_samples, 0);
  block->set_max_samples(50);
  EXPECT_NE(block->sample_buffer, nullptr);
  EXPECT_GE(block->max_samples, 50);
  block->set_max_samples(4096);
  EXPECT_GE(block->max_samples, 4096);

  // 2. 64-bit sample_buffer64 (blk_handles[4]), no-op, and grow again.
  block->set_max_samples64(64);
  EXPECT_GE(block->max_samples64, 64);
  ASSERT_NE(block->sample_buffer64, nullptr);
  for (int i = -2 * KDU_ALIGN_SAMPLES64;
       i < block->max_samples64 + 4 * KDU_ALIGN_SAMPLES64; ++i) {
    block->sample_buffer64[i] = i;
  }
  block->set_max_samples64(64);
  block->set_max_samples64(1024);
  EXPECT_GE(block->max_samples64, 1024);

  // 3. Context buffer (blk_handles[1]) and ictxt buffer (blk_handles[3]).
  block->set_max_contexts(64);
  EXPECT_GE(block->max_contexts, 64);
  ASSERT_NE(block->context_buffer, nullptr);
  block->context_buffer[0] = 42;
  block->set_max_contexts(64);
  block->set_max_contexts(1024);
  EXPECT_GE(block->max_contexts, 1024);

  block->set_max_ictxts(64);
  EXPECT_GE(block->max_ictxts, 64);
  ASSERT_NE(block->ictxt_buffer, nullptr);
  block->ictxt_buffer[0] = 99;
  block->set_max_ictxts(64);
  block->set_max_ictxts(1024);
  EXPECT_GE(block->max_ictxts, 1024);

  // 4. Exceeding 1M ints in allocate_mem_blk throws KDU_ERROR_DEV.
  EXPECT_THROW(block->set_max_contexts((1 << 20) + 1), kdu_exception);
}

TEST_F(BlocksTest, MapStorageModesAccessorsAndTimingStats) {
  ScopedOutputBlock ctx;
  kdu_block* block = ctx.block();
  ASSERT_NE(block, nullptr);

  // 1. Contexts only: first allocation resets retained state to 0; second
  // preserves.
  EXPECT_EQ(block->map_storage(/*contexts=*/128, /*samples=*/0,
                               /*retained_state=*/11),
            0);
  EXPECT_EQ(block->max_contexts, 0);
  ASSERT_NE(block->context_buffer, nullptr);
  EXPECT_EQ(block->map_storage(/*contexts=*/128, /*samples=*/0,
                               /*retained_state=*/12),
            11);

  // 2. Samples only (bigger allocation triggers blk_handles[0] reallocation).
  EXPECT_EQ(block->map_storage(/*contexts=*/0, /*samples=*/256,
                               /*retained_state=*/22),
            0);
  EXPECT_EQ(block->max_samples, 0);
  ASSERT_NE(block->sample_buffer, nullptr);
  EXPECT_EQ(block->map_storage(/*contexts=*/0, /*samples=*/256,
                               /*retained_state=*/23),
            22);

  // 3. Both contexts and samples with pre_write_required=false and enough
  // context padding to pack sample_buffer right after context_buffer.
  EXPECT_EQ(block->map_storage(/*contexts=*/64, /*samples=*/64,
                               /*retained_state=*/33,
                               /*pre_write_required=*/false),
            0);
  EXPECT_EQ(block->map_storage(/*contexts=*/64, /*samples=*/64,
                               /*retained_state=*/34,
                               /*pre_write_required=*/false),
            33);

  // 4. Both contexts and samples with pre_write_required=true and growth.
  EXPECT_EQ(block->map_storage(/*contexts=*/64, /*samples=*/2048,
                               /*retained_state=*/44,
                               /*pre_write_required=*/true),
            0);
  EXPECT_EQ(block->max_contexts, 0);
  EXPECT_EQ(block->max_samples, 0);
  EXPECT_EQ(block->map_storage(/*contexts=*/64, /*samples=*/2048,
                               /*retained_state=*/45,
                               /*pre_write_required=*/true),
            44);

  // 5. Language-binding property accessors (saving/restoring before close).
  const kdu_coords orig_size = block->get_size();
  const kdu_dims orig_region = block->get_region();
  const kdu_coords orig_inset = block->get_inset();
  const bool orig_transpose = block->get_transpose();
  const bool orig_vflip = block->get_vflip();
  const bool orig_hflip = block->get_hflip();

  block->set_size(kdu_coords(8, 8));
  EXPECT_EQ(block->get_size(), kdu_coords(8, 8));
  kdu_dims custom_region;
  custom_region.pos = kdu_coords(1, 2);
  custom_region.size = kdu_coords(4, 5);
  block->set_region(custom_region);
  EXPECT_EQ(block->get_region().pos, kdu_coords(1, 2));
  EXPECT_EQ(block->get_region().size, kdu_coords(4, 5));
  block->set_inset(kdu_coords(1, 0));
  EXPECT_EQ(block->get_inset(), kdu_coords(1, 0));
  EXPECT_EQ(block->get_coding_origin(), kdu_coords(0, 0));
  block->set_transpose(true);
  block->set_vflip(true);
  block->set_hflip(true);
  EXPECT_TRUE(block->get_transpose());
  EXPECT_TRUE(block->get_vflip());
  EXPECT_TRUE(block->get_hflip());

  block->set_missing_msbs(3);
  EXPECT_EQ(block->get_missing_msbs(), 3);
  block->set_num_passes(2);
  EXPECT_EQ(block->get_num_passes(), 2);
  int set_lengths[2] = {15, 29};
  int get_lengths[2] = {0, 0};
  int set_slopes[2] = {500, 300};
  int get_slopes[2] = {0, 0};
  block->set_pass_lengths(set_lengths);
  block->get_pass_lengths(get_lengths);
  EXPECT_EQ(get_lengths[0], 15);
  EXPECT_EQ(get_lengths[1], 29);
  block->set_pass_slopes(set_slopes);
  block->get_pass_slopes(get_slopes);
  EXPECT_EQ(get_slopes[0], 500);
  EXPECT_EQ(get_slopes[1], 300);

  kdu_byte set_bytes[4] = {0x11, 0x22, 0x33, 0x44};
  kdu_byte get_bytes[4] = {};
  block->set_buffered_bytes(set_bytes, -1, 4);
  block->get_buffered_bytes(get_bytes, -1, 4);
  EXPECT_EQ(get_bytes[0], 0x11);
  EXPECT_EQ(get_bytes[1], 0x22);
  EXPECT_EQ(get_bytes[2], 0x33);
  EXPECT_EQ(get_bytes[3], 0x44);

  // Restore structural fields before timing & close.
  block->set_size(orig_size);
  block->set_region(orig_region);
  block->set_inset(orig_inset);
  block->set_transpose(orig_transpose);
  block->set_vflip(orig_vflip);
  block->set_hflip(orig_hflip);
  block->set_missing_msbs(0);
  block->set_num_passes(0);

  // 6. Timing helper methods.
  block->initialize_timing(0);
  EXPECT_EQ(block->start_timing(), 1);
  block->finish_timing();

  block->initialize_timing(2);
  EXPECT_EQ(block->start_timing(), 2);
  block->finish_timing();
  kdu_long unique_samples = 0;
  double time_wasted = -1.0;
  const double once_time = block->get_timing_stats(unique_samples, time_wasted);
  EXPECT_EQ(unique_samples, static_cast<kdu_long>(orig_size.x) * orig_size.y);
  EXPECT_GE(once_time, 0.0);
  EXPECT_GE(time_wasted, 0.0);
}

}  // namespace
}  // namespace kdu_core
