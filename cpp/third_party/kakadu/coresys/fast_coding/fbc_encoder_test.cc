#include <array>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_block_coding.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

// Entry points implemented in third_party/kakadu/vendor/coresys/fast_coding/
// fbc_encoder.cpp.
extern void fbc_get_block_encode_caps(int& caps);
extern void fbc_block_encode(kdu_core::kdu_block_encoder_base* base,
                             kdu_core::kdu_block* block, bool reversible,
                             const float* per_plane_wmse,
                             kdu_core::kdu_uint16 min_slope_threshold,
                             kdu_core::kdu_uint16 max_slope_threshold,
                             bool use_existing_slopes);
extern void fbc_block_encode_noopt(kdu_core::kdu_block_encoder_base* base,
                                   kdu_core::kdu_block* block, bool reversible,
                                   const float* per_plane_wmse,
                                   kdu_core::kdu_uint16 min_slope_threshold,
                                   kdu_core::kdu_uint16 max_slope_threshold,
                                   bool use_existing_slopes);
extern bool fbc_block_encode16(kdu_core::kdu_block_encoder_base* base,
                               kdu_core::kdu_block* block,
                               kdu_core::kdu_uint16* data, bool reversible,
                               const float* per_plane_wmse,
                               kdu_core::kdu_uint16 min_slope_threshold,
                               kdu_core::kdu_uint16 max_slope_threshold);
extern bool fbc_block_encode16_noopt(kdu_core::kdu_block_encoder_base* base,
                                     kdu_core::kdu_block* block,
                                     kdu_core::kdu_uint16* data,
                                     bool reversible,
                                     const float* per_plane_wmse,
                                     kdu_core::kdu_uint16 min_slope_threshold,
                                     kdu_core::kdu_uint16 max_slope_threshold);

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
  ScopedOutputBlock() : target_(compressed_bytes_) {
    kdu_core::siz_params siz;
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, 16);
    siz.set(Sdims, 0, 1, 16);
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

TEST(FbcEncoderTest, CapabilitiesReportNoHtSupport) {
  int caps = 0;
  fbc_get_block_encode_caps(caps);
  EXPECT_EQ(caps, 0);

  // Without FBC_ENABLED, fbc_get_block_encode_caps leaves existing flags
  // intact.
  caps = KDU_BLOCK_CODER_CAP_J2K1;
  fbc_get_block_encode_caps(caps);
  EXPECT_EQ(caps, KDU_BLOCK_CODER_CAP_J2K1);
}

TEST(FbcEncoderTest, StubEntryPointsZeroPassesAndReject16Bit) {
  ScopedMessageHandlers scoped_handlers;

  ScopedOutputBlock scoped_block;
  ASSERT_NE(scoped_block.block(), nullptr);

  kdu_core::kdu_block_encoder encoder;
  encoder.init(true);
  const float wmse[4] = {1.0F, 1.0F, 1.0F, 1.0F};
  std::array<kdu_core::kdu_uint16, 256> data16{};

  scoped_block.block()->num_passes = 3;
  fbc_block_encode(&encoder, scoped_block.block(), true, wmse, 0, 0xFFFF,
                   false);
  EXPECT_EQ(scoped_block.block()->num_passes, 0);

  scoped_block.block()->num_passes = 3;
  fbc_block_encode_noopt(&encoder, scoped_block.block(), false, wmse, 0, 0xFFFF,
                         false);
  EXPECT_EQ(scoped_block.block()->num_passes, 0);

  // Repeat calls ensure both first-call warning branch and subsequent calls
  // behave cleanly without re-issuing warnings or crashing.
  scoped_block.block()->num_passes = 2;
  fbc_block_encode(&encoder, scoped_block.block(), true, wmse, 0, 0xFFFF,
                   false);
  EXPECT_EQ(scoped_block.block()->num_passes, 0);

  scoped_block.block()->num_passes = 2;
  fbc_block_encode_noopt(&encoder, scoped_block.block(), false, wmse, 0, 0xFFFF,
                         false);
  EXPECT_EQ(scoped_block.block()->num_passes, 0);

  EXPECT_FALSE(fbc_block_encode16(&encoder, scoped_block.block(), data16.data(),
                                  true, wmse, 0, 0xFFFF));
  EXPECT_FALSE(fbc_block_encode16_noopt(&encoder, scoped_block.block(),
                                        data16.data(), false, wmse, 0, 0xFFFF));
}

TEST(FbcEncoderTest, KduBlockEncoderDispatchesToHtStubs) {
  ScopedMessageHandlers scoped_handlers;

  ScopedOutputBlock scoped_block;
  ASSERT_NE(scoped_block.block(), nullptr);
  const float wmse[4] = {1.0F, 1.0F, 1.0F, 1.0F};

  for (bool allow_opt : {true, false}) {
    kdu_core::kdu_block_encoder encoder;
    encoder.init(allow_opt);
    scoped_block.block()->modes |= Cmodes_HT;
    scoped_block.block()->num_passes = 3;
    encoder.encode(scoped_block.block(), true, wmse, 0, 0xFFFF, false);
    EXPECT_EQ(scoped_block.block()->num_passes, 0);
  }
}

}  // namespace
