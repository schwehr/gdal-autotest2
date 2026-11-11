#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_block_coding.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

// Entry points implemented in third_party/kakadu/vendor/coresys/fast_coding/
// fbc_decoder.cpp.
extern void fbc_get_block_decode_caps(int& cap_flags);
extern bool fbc_block_decode(kdu_core::kdu_block_decoder_base* base,
                             kdu_core::kdu_block* block, bool reversible);
extern bool fbc_block_decode_noopt(kdu_core::kdu_block_decoder_base* base,
                                   kdu_core::kdu_block* block, bool reversible);
extern kdu_core::kdu_int16* fbc_block_decode16(
    kdu_core::kdu_block_decoder_base* base, kdu_core::kdu_block* block,
    int& stride16_out);

namespace {

class ThrowingErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class IgnoringWarningHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
  }
};

class ScopedMessageHandlers {
 public:
  ScopedMessageHandlers() {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }
  ~ScopedMessageHandlers() {
    kdu_core::kdu_customize_warnings(nullptr);
    kdu_core::kdu_customize_errors(nullptr);
  }

 private:
  ThrowingErrorHandler error_handler_;
  IgnoringWarningHandler warning_handler_;
};

class VectorTarget final : public kdu_core::kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_core::kdu_byte>& out) : out_(out) {}

  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_core::kdu_byte>& out_;
};

class ScopedOutputBlock {
 public:
  ScopedOutputBlock(int width, int height) : target_(compressed_bytes_) {
    kdu_core::siz_params siz;
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, height);
    siz.set(Sdims, 0, 1, width);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_core::kdu_params&>(siz).finalize();

    codestream_.create(&siz, &target_);
    codestream_.access_siz()->parse_string("Clevels=0");
    codestream_.access_siz()->finalize_all();

    tile_ = codestream_.open_tile(kdu_core::kdu_coords(0, 0));
    kdu_core::kdu_tile_comp tc = tile_.access_component(0);
    kdu_core::kdu_resolution res = tc.access_resolution(0);
    subband_ = res.access_subband(LL_BAND);
    block_ = subband_.open_block(kdu_core::kdu_coords(0, 0));
    if (block_ != nullptr) {
      orig_modes_ = block_->modes;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
    }
  }

  ~ScopedOutputBlock() {
    if (block_ != nullptr) {
      block_->modes = orig_modes_;
      block_->num_passes = 0;
      block_->missing_msbs = 0;
      subband_.close_block(block_);
    }
    if (tile_.exists()) {
      tile_.close();
    }
    if (codestream_.exists()) {
      codestream_.destroy();
    }
  }

  kdu_core::kdu_block* block() { return block_; }

 private:
  std::vector<kdu_core::kdu_byte> compressed_bytes_;
  VectorTarget target_;
  kdu_core::kdu_codestream codestream_;
  kdu_core::kdu_tile tile_;
  kdu_core::kdu_subband subband_;
  kdu_core::kdu_block* block_ = nullptr;
  int orig_modes_ = 0;
};

TEST(FbcDecoderTest, CapabilitiesReportNoHtSupport) {
  int caps = 0;
  fbc_get_block_decode_caps(caps);
  EXPECT_EQ(caps, 0);

  caps = KDU_BLOCK_CODER_CAP_J2K1;
  fbc_get_block_decode_caps(caps);
  EXPECT_EQ(caps, KDU_BLOCK_CODER_CAP_J2K1);
}

TEST(FbcDecoderTest, StubDecodeAllocatesZerosSamplesAndDecode16ReturnsNull) {
  ScopedMessageHandlers scoped_handlers;

  // Test both <= 4096 samples (16x16, hitting the 4096 floor) and > 4096
  // samples (128x128), plus calling again when max_samples is already large
  // enough.
  for (int dim : {16, 128}) {
    ScopedOutputBlock scoped_block(dim, dim);
    kdu_core::kdu_block* block = scoped_block.block();
    ASSERT_NE(block, nullptr);
    const int num_samples = block->size.x * block->size.y;

    kdu_core::kdu_block_decoder decoder;
    decoder.init(true);

    EXPECT_TRUE(fbc_block_decode(&decoder, block, true));
    EXPECT_GE(block->max_samples, 4096);
    ASSERT_NE(block->sample_buffer, nullptr);
    for (int i = 0; i < num_samples; ++i) {
      EXPECT_EQ(block->sample_buffer[i], 0);
    }

    // Dirty the buffer and call again with sufficient max_samples already
    // allocated, as well as the _noopt variant.
    std::memset(block->sample_buffer, 0x5A,
                sizeof(kdu_core::kdu_int32) * static_cast<size_t>(num_samples));
    EXPECT_TRUE(fbc_block_decode(&decoder, block, false));
    for (int i = 0; i < num_samples; ++i) {
      EXPECT_EQ(block->sample_buffer[i], 0);
    }

    std::memset(block->sample_buffer, 0x3C,
                sizeof(kdu_core::kdu_int32) * static_cast<size_t>(num_samples));
    EXPECT_TRUE(fbc_block_decode_noopt(&decoder, block, true));
    for (int i = 0; i < num_samples; ++i) {
      EXPECT_EQ(block->sample_buffer[i], 0);
    }

    int stride16 = -7;
    EXPECT_EQ(fbc_block_decode16(&decoder, block, stride16), nullptr);
    EXPECT_EQ(stride16, -7);
  }

  // Exercise fbc_block_decode_noopt when block->max_samples starts at 0.
  ScopedOutputBlock noopt_block(32, 32);
  kdu_core::kdu_block* block = noopt_block.block();
  ASSERT_NE(block, nullptr);
  kdu_core::kdu_block_decoder decoder;
  decoder.init(false);
  EXPECT_TRUE(fbc_block_decode_noopt(&decoder, block, false));
  EXPECT_GE(block->max_samples, 4096);
}

TEST(FbcDecoderTest, KduBlockDecoderDispatchesToHtStubs) {
  ScopedMessageHandlers scoped_handlers;

  for (bool allow_opt : {true, false}) {
    ScopedOutputBlock scoped_block(16, 16);
    kdu_core::kdu_block* block = scoped_block.block();
    ASSERT_NE(block, nullptr);
    block->modes |= Cmodes_HT;

    kdu_core::kdu_block_decoder decoder;
    decoder.init(allow_opt);
    EXPECT_TRUE(decoder.decode(block, true));
    ASSERT_NE(block->sample_buffer, nullptr);
    for (int i = 0; i < 256; ++i) {
      EXPECT_EQ(block->sample_buffer[i], 0);
    }

    int stride16 = 0;
    EXPECT_EQ(decoder.decode16(block, stride16), nullptr);
  }
}

}  // namespace
