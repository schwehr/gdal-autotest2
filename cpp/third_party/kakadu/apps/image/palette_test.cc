// Unit tests for third_party/kakadu/vendor/apps/image/palette.cpp
// and kdu_rgb8_palette in third_party/kakadu/vendor/apps/image/kdu_image.h.

#include <array>
#include <cstdlib>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/image/kdu_image.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

class ThrowingErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      kdu_core::kdu_thread_safe_message::flush(true);
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_core::kdu_thread_safe_message::flush(false);
  }
};

class SilentWarningHandler final : public kdu_core::kdu_thread_safe_message {};

class CapturingWarningHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* text) override { current_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      all_warnings_ += current_;
      current_.clear();
    }
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
  }

  const std::string& all_warnings() const { return all_warnings_; }

 private:
  std::string current_;
  std::string all_warnings_;
};

kdu_core::kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

kdu_core::kdu_message* GlobalWarningHandler() {
  static auto* const handler = new SilentWarningHandler();
  return handler;
}

class PaletteTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(GlobalErrorHandler());
    kdu_core::kdu_customize_warnings(GlobalWarningHandler());
  }
};

const ::testing::Environment* const kPaletteEnv =
    ::testing::AddGlobalTestEnvironment(new PaletteTestEnvironment());

struct RgbEntry {
  kdu_core::kdu_int32 r;
  kdu_core::kdu_int32 g;
  kdu_core::kdu_int32 b;
};

int PathL1Cost(const kdu_rgb8_palette& pal, int count) {
  int total = 0;
  for (int i = 0; i + 1 < count; ++i) {
    total += std::abs(pal.red[i] - pal.red[i + 1]);
    total += std::abs(pal.green[i] - pal.green[i + 1]);
    total += std::abs(pal.blue[i] - pal.blue[i + 1]);
  }
  return total;
}

TEST(PaletteTest, ExistsNegationAndIsMonochrome) {
  kdu_rgb8_palette pal;
  EXPECT_EQ(0, pal.input_bits);
  EXPECT_EQ(0, pal.output_bits);
  EXPECT_EQ(0, pal.source_component);
  EXPECT_FALSE(pal.exists());
  EXPECT_TRUE(!pal);

  pal.input_bits = 2;
  EXPECT_FALSE(pal.exists());
  EXPECT_TRUE(!pal);

  pal.input_bits = 0;
  pal.output_bits = 8;
  EXPECT_FALSE(pal.exists());
  EXPECT_TRUE(!pal);

  pal.input_bits = 2;
  pal.output_bits = 8;
  EXPECT_TRUE(pal.exists());
  EXPECT_FALSE(!pal);

  for (int i = 0; i < 4; ++i) {
    pal.red[i] = i * 50;
    pal.green[i] = i * 50;
    pal.blue[i] = i * 50;
  }
  EXPECT_TRUE(pal.is_monochrome());

  pal.green[1] += 1;
  EXPECT_FALSE(pal.is_monochrome());

  pal.green[1] = pal.red[1];
  pal.blue[2] += 1;
  EXPECT_FALSE(pal.is_monochrome());
}

TEST(PaletteTest, RearrangePreservesAlreadyOrderedPaletteAndWarns) {
  CapturingWarningHandler warning_capture;
  absl::Cleanup restore_warnings = [] {
    kdu_core::kdu_customize_warnings(GlobalWarningHandler());
  };
  kdu_core::kdu_customize_warnings(&warning_capture);

  kdu_rgb8_palette pal;
  pal.input_bits = 2;
  pal.output_bits = 8;
  constexpr std::array<RgbEntry, 4> kOrdered = {{
      {0, 0, 0},
      {40, 40, 40},
      {80, 80, 80},
      {120, 120, 120},
  }};
  for (int i = 0; i < 4; ++i) {
    pal.red[i] = kOrdered[i].r;
    pal.green[i] = kOrdered[i].g;
    pal.blue[i] = kOrdered[i].b;
  }

  std::array<kdu_core::kdu_byte, 4> map = {};
  pal.rearrange(map.data());

  EXPECT_NE(std::string::npos,
            warning_capture.all_warnings().find("Optimizing palette"));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(i, map[i]);
    EXPECT_EQ(kOrdered[i].r, pal.red[i]);
    EXPECT_EQ(kOrdered[i].g, pal.green[i]);
    EXPECT_EQ(kOrdered[i].b, pal.blue[i]);
  }
}

TEST(PaletteTest, RearrangeOptimizesScrambledPaletteToValidPermutation) {
  kdu_rgb8_palette pal;
  pal.input_bits = 3;
  pal.output_bits = 8;
  constexpr std::array<RgbEntry, 8> kScrambled = {{
      {250, 250, 250},
      {0, 0, 0},
      {240, 240, 240},
      {10, 10, 10},
      {200, 0, 0},
      {20, 20, 20},
      {210, 0, 0},
      {245, 245, 245},
  }};
  for (int i = 0; i < 8; ++i) {
    pal.red[i] = kScrambled[i].r;
    pal.green[i] = kScrambled[i].g;
    pal.blue[i] = kScrambled[i].b;
  }

  const int initial_cost = PathL1Cost(pal, 8);
  std::array<kdu_core::kdu_byte, 8> map = {};
  pal.rearrange(map.data());
  const int rearranged_cost = PathL1Cost(pal, 8);

  EXPECT_LT(rearranged_cost, initial_cost);

  std::array<bool, 8> seen = {};
  for (int orig = 0; orig < 8; ++orig) {
    const kdu_core::kdu_byte new_idx = map[orig];
    ASSERT_LT(new_idx, 8);
    EXPECT_FALSE(seen[new_idx]);
    seen[new_idx] = true;
    EXPECT_EQ(kScrambled[orig].r, pal.red[new_idx]);
    EXPECT_EQ(kScrambled[orig].g, pal.green[new_idx]);
    EXPECT_EQ(kScrambled[orig].b, pal.blue[new_idx]);
  }
}

}  // namespace
}  // namespace kdu_supp
