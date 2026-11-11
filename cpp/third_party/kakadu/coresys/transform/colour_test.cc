// Unit tests for third_party/kakadu/vendor/coresys/transform/colour.cpp and
// kdu_convert_rgb_to_ycc / kdu_convert_ycc_to_rgb in
// third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h.

#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
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

class ColourTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const handler = new ThrowingErrorHandler();
    kdu_customize_errors(handler);
  }
};

const ::testing::Environment* const kColourEnv =
    ::testing::AddGlobalTestEnvironment(new ColourTestEnvironment());

constexpr int kLineWidth = 19;
constexpr double kAlphaR = 0.299;
constexpr double kAlphaB = 0.114;
constexpr double kAlphaG = 1.0 - kAlphaR - kAlphaB;
constexpr double kCbFact = 1.0 / (2.0 * (1.0 - kAlphaB));
constexpr double kCrFact = 1.0 / (2.0 * (1.0 - kAlphaR));

TEST(ColourTest, StaticInitializationPopulatesAllFunctionPointers) {
  EXPECT_NE(kdu_convert_rgb_to_ycc_rev16, nullptr);
  EXPECT_NE(kdu_convert_rgb_to_ycc_irrev16, nullptr);
  EXPECT_NE(kdu_convert_rgb_to_ycc_rev32, nullptr);
  EXPECT_NE(kdu_convert_rgb_to_ycc_irrev32, nullptr);
  EXPECT_NE(kdu_convert_rgb_to_ycc_rev64, nullptr);

  EXPECT_NE(kdu_convert_ycc_to_rgb_rev16, nullptr);
  EXPECT_NE(kdu_convert_ycc_to_rgb_irrev16, nullptr);
  EXPECT_NE(kdu_convert_ycc_to_rgb_rev32, nullptr);
  EXPECT_NE(kdu_convert_ycc_to_rgb_irrev32, nullptr);
  EXPECT_NE(kdu_convert_ycc_to_rgb_rev64, nullptr);
}

TEST(ColourTest, NonPositiveLengthLeavesBuffersUntouched) {
  for (int n : {0, -1}) {
    kdu_int16 r16 = 123, g16 = -456, b16 = 789;
    kdu_convert_rgb_to_ycc_rev16(&r16, &g16, &b16, n);
    EXPECT_EQ(r16, 123);
    EXPECT_EQ(g16, -456);
    EXPECT_EQ(b16, 789);
    kdu_convert_ycc_to_rgb_rev16(&r16, &g16, &b16, n);
    EXPECT_EQ(r16, 123);
    EXPECT_EQ(g16, -456);
    EXPECT_EQ(b16, 789);

    kdu_convert_rgb_to_ycc_irrev16(&r16, &g16, &b16, n);
    EXPECT_EQ(r16, 123);
    EXPECT_EQ(g16, -456);
    EXPECT_EQ(b16, 789);
    kdu_convert_ycc_to_rgb_irrev16(&r16, &g16, &b16, n);
    EXPECT_EQ(r16, 123);
    EXPECT_EQ(g16, -456);
    EXPECT_EQ(b16, 789);

    kdu_int32 r32 = 100001, g32 = -200002, b32 = 300003;
    kdu_convert_rgb_to_ycc_rev32(&r32, &g32, &b32, n);
    EXPECT_EQ(r32, 100001);
    EXPECT_EQ(g32, -200002);
    EXPECT_EQ(b32, 300003);
    kdu_convert_ycc_to_rgb_rev32(&r32, &g32, &b32, n);
    EXPECT_EQ(r32, 100001);
    EXPECT_EQ(g32, -200002);
    EXPECT_EQ(b32, 300003);

    kdu_int64 r64 = (1LL << 40), g64 = -(1LL << 41), b64 = (3LL << 39);
    kdu_convert_rgb_to_ycc_rev64(&r64, &g64, &b64, n);
    EXPECT_EQ(r64, (1LL << 40));
    EXPECT_EQ(g64, -(1LL << 41));
    EXPECT_EQ(b64, (3LL << 39));
    kdu_convert_ycc_to_rgb_rev64(&r64, &g64, &b64, n);
    EXPECT_EQ(r64, (1LL << 40));
    EXPECT_EQ(g64, -(1LL << 41));
    EXPECT_EQ(b64, (3LL << 39));

    float rf = 0.25f, gf = -0.375f, bf = 0.5f;
    kdu_convert_rgb_to_ycc_irrev32(&rf, &gf, &bf, n);
    EXPECT_FLOAT_EQ(rf, 0.25f);
    EXPECT_FLOAT_EQ(gf, -0.375f);
    EXPECT_FLOAT_EQ(bf, 0.5f);
    kdu_convert_ycc_to_rgb_irrev32(&rf, &gf, &bf, n);
    EXPECT_FLOAT_EQ(rf, 0.25f);
    EXPECT_FLOAT_EQ(gf, -0.375f);
    EXPECT_FLOAT_EQ(bf, 0.5f);
  }
}

TEST(ColourTest, ReversibleRct16BitExactRoundTrip) {
  std::vector<kdu_int16> r(kLineWidth), g(kLineWidth), b(kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    r[i] = static_cast<kdu_int16>((i - 9) * 700 + (i % 3));
    g[i] = static_cast<kdu_int16>((5 - i) * 500 - (i % 5));
    b[i] = static_cast<kdu_int16>((i - 4) * 600 + (i % 7));
  }
  const std::vector<kdu_int16> orig_r = r;
  const std::vector<kdu_int16> orig_g = g;
  const std::vector<kdu_int16> orig_b = b;

  kdu_convert_rgb_to_ycc_rev16(r.data(), g.data(), b.data(), kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    const int expected_y = (orig_r[i] + 2 * orig_g[i] + orig_b[i]) >> 2;
    EXPECT_EQ(r[i], static_cast<kdu_int16>(expected_y)) << "i=" << i;
    EXPECT_EQ(g[i], static_cast<kdu_int16>(orig_b[i] - orig_g[i])) << "i=" << i;
    EXPECT_EQ(b[i], static_cast<kdu_int16>(orig_r[i] - orig_g[i])) << "i=" << i;
  }

  kdu_convert_ycc_to_rgb_rev16(r.data(), g.data(), b.data(), kLineWidth);
  EXPECT_EQ(r, orig_r);
  EXPECT_EQ(g, orig_g);
  EXPECT_EQ(b, orig_b);
}

TEST(ColourTest, ReversibleRct32BitExactRoundTripWithLargeMagnitudes) {
  std::vector<kdu_int32> r(kLineWidth), g(kLineWidth), b(kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    r[i] = ((i & 1) ? 1 : -1) * ((1 << 24) + i * 1234567);
    g[i] = ((i % 3 == 0) ? -1 : 1) * ((1 << 23) + i * 765432);
    b[i] = ((i % 2 == 0) ? 1 : -1) * ((1 << 25) - i * 999983);
  }
  const std::vector<kdu_int32> orig_r = r;
  const std::vector<kdu_int32> orig_g = g;
  const std::vector<kdu_int32> orig_b = b;

  kdu_convert_rgb_to_ycc_rev32(r.data(), g.data(), b.data(), kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    const kdu_int64 expected_y = (static_cast<kdu_int64>(orig_r[i]) +
                                  2 * static_cast<kdu_int64>(orig_g[i]) +
                                  static_cast<kdu_int64>(orig_b[i])) >>
                                 2;
    EXPECT_EQ(r[i], static_cast<kdu_int32>(expected_y)) << "i=" << i;
    EXPECT_EQ(g[i], orig_b[i] - orig_g[i]) << "i=" << i;
    EXPECT_EQ(b[i], orig_r[i] - orig_g[i]) << "i=" << i;
  }

  kdu_convert_ycc_to_rgb_rev32(r.data(), g.data(), b.data(), kLineWidth);
  EXPECT_EQ(r, orig_r);
  EXPECT_EQ(g, orig_g);
  EXPECT_EQ(b, orig_b);
}

TEST(ColourTest, ReversibleRct64BitExactRoundTripBeyondInt32) {
  std::vector<kdu_int64> r(kLineWidth), g(kLineWidth), b(kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    r[i] = ((i - 9) * (1LL << 38)) + (i * 123456789LL);
    g[i] = ((4 - i) * (1LL << 37)) - (i * 987654321LL);
    b[i] = ((i - 3) * (1LL << 39)) + (i * 555555555LL);
  }
  // Explicitly verify both +/- (1LL << 40) endpoints are covered.
  r[0] = (1LL << 40);
  g[0] = -(1LL << 40);
  b[0] = (1LL << 40) - 3;

  const std::vector<kdu_int64> orig_r = r;
  const std::vector<kdu_int64> orig_g = g;
  const std::vector<kdu_int64> orig_b = b;

  kdu_convert_rgb_to_ycc_rev64(r.data(), g.data(), b.data(), kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    const kdu_int64 expected_y =
        (orig_r[i] + orig_g[i] + orig_g[i] + orig_b[i]) >> 2;
    EXPECT_EQ(r[i], expected_y) << "i=" << i;
    EXPECT_EQ(g[i], orig_b[i] - orig_g[i]) << "i=" << i;
    EXPECT_EQ(b[i], orig_r[i] - orig_g[i]) << "i=" << i;
  }

  kdu_convert_ycc_to_rgb_rev64(r.data(), g.data(), b.data(), kLineWidth);
  EXPECT_EQ(r, orig_r);
  EXPECT_EQ(g, orig_g);
  EXPECT_EQ(b, orig_b);
}

TEST(ColourTest, IrreversibleIct16BitForwardAndRoundTrip) {
  std::vector<kdu_int16> r(kLineWidth), g(kLineWidth), b(kLineWidth);
  // First 4 entries cover gray, pure R, pure G, pure B in 13-bit normalized
  // fixed point (KDU_FIX_POINT == 13 -> 4096 corresponds to 0.5).
  r[0] = 2048;
  g[0] = 2048;
  b[0] = 2048;
  r[1] = 4096;
  g[1] = 0;
  b[1] = 0;
  r[2] = 0;
  g[2] = 4096;
  b[2] = 0;
  r[3] = 0;
  g[3] = 0;
  b[3] = 4096;
  for (int i = 4; i < kLineWidth; ++i) {
    r[i] = static_cast<kdu_int16>((i - 11) * 400);
    g[i] = static_cast<kdu_int16>((10 - i) * 350);
    b[i] = static_cast<kdu_int16>((i - 8) * 300);
  }
  const std::vector<kdu_int16> orig_r = r;
  const std::vector<kdu_int16> orig_g = g;
  const std::vector<kdu_int16> orig_b = b;

  kdu_convert_rgb_to_ycc_irrev16(r.data(), g.data(), b.data(), kLineWidth);

  // Neutral gray must yield zero chrominance and Y == v.
  EXPECT_EQ(r[0], 2048);
  EXPECT_EQ(g[0], 0);
  EXPECT_EQ(b[0], 0);

  for (int i = 0; i < kLineWidth; ++i) {
    const double ey =
        kAlphaR * orig_r[i] + kAlphaG * orig_g[i] + kAlphaB * orig_b[i];
    const double ecb = kCbFact * (orig_b[i] - ey);
    const double ecr = kCrFact * (orig_r[i] - ey);
    EXPECT_NEAR(r[i], ey, 1.5) << "i=" << i;
    EXPECT_NEAR(g[i], ecb, 1.5) << "i=" << i;
    EXPECT_NEAR(b[i], ecr, 1.5) << "i=" << i;
  }

  kdu_convert_ycc_to_rgb_irrev16(r.data(), g.data(), b.data(), kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    EXPECT_NEAR(r[i], orig_r[i], 3) << "i=" << i;
    EXPECT_NEAR(g[i], orig_g[i], 3) << "i=" << i;
    EXPECT_NEAR(b[i], orig_b[i], 3) << "i=" << i;
  }
}

TEST(ColourTest, IrreversibleIct32BitFloatForwardAndRoundTrip) {
  std::vector<float> r(kLineWidth), g(kLineWidth), b(kLineWidth);
  r[0] = -0.25f;
  g[0] = -0.25f;
  b[0] = -0.25f;
  r[1] = 1.0f;
  g[1] = 0.0f;
  b[1] = 0.0f;
  r[2] = 0.0f;
  g[2] = 1.0f;
  b[2] = 0.0f;
  r[3] = 0.0f;
  g[3] = 0.0f;
  b[3] = 1.0f;
  for (int i = 4; i < kLineWidth; ++i) {
    r[i] = -0.5f + static_cast<float>(i) * 0.06f;
    g[i] = 0.5f - static_cast<float>(i) * 0.05f;
    b[i] = -0.4f + static_cast<float>(i) * 0.045f;
  }
  const std::vector<float> orig_r = r;
  const std::vector<float> orig_g = g;
  const std::vector<float> orig_b = b;

  kdu_convert_rgb_to_ycc_irrev32(r.data(), g.data(), b.data(), kLineWidth);

  EXPECT_NEAR(r[0], -0.25f, 1e-6f);
  EXPECT_NEAR(g[0], 0.0f, 1e-6f);
  EXPECT_NEAR(b[0], 0.0f, 1e-6f);

  EXPECT_NEAR(r[1], 0.299f, 1e-6f);
  EXPECT_NEAR(b[1], 0.5f, 1e-6f);  // (1 - ALPHA_R) / (2 * (1 - ALPHA_R))
  EXPECT_NEAR(r[2], 0.587f, 1e-6f);
  EXPECT_NEAR(r[3], 0.114f, 1e-6f);
  EXPECT_NEAR(g[3], 0.5f, 1e-6f);  // (1 - ALPHA_B) / (2 * (1 - ALPHA_B))

  for (int i = 0; i < kLineWidth; ++i) {
    const double ey =
        kAlphaR * orig_r[i] + kAlphaG * orig_g[i] + kAlphaB * orig_b[i];
    EXPECT_NEAR(r[i], ey, 1e-6) << "i=" << i;
    EXPECT_NEAR(g[i], kCbFact * (orig_b[i] - ey), 1e-6) << "i=" << i;
    EXPECT_NEAR(b[i], kCrFact * (orig_r[i] - ey), 1e-6) << "i=" << i;
  }

  kdu_convert_ycc_to_rgb_irrev32(r.data(), g.data(), b.data(), kLineWidth);
  for (int i = 0; i < kLineWidth; ++i) {
    EXPECT_NEAR(r[i], orig_r[i], 1e-5f) << "i=" << i;
    EXPECT_NEAR(g[i], orig_g[i], 1e-5f) << "i=" << i;
    EXPECT_NEAR(b[i], orig_b[i], 1e-5f) << "i=" << i;
  }
}

TEST(ColourTest, LineBufWrappersExerciseAllFiveModesAndPartialWidth) {
  constexpr kdu_byte kModes[5] = {
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,    // rev16
      KDU_LINE_BUF_SHORTS,                            // irrev16
      0,                                              // irrev32
      KDU_LINE_BUF_ABSOLUTE,                          // rev32
      KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_VPRECISE,  // rev64
  };

  kdu_sample_allocator allocator;
  kdu_line_buf c1[5], c2[5], c3[5];
  for (int m = 0; m < 5; ++m) {
    c1[m].pre_create(&allocator, kLineWidth, kModes[m], 0, 0);
    c2[m].pre_create(&allocator, kLineWidth, kModes[m], 0, 0);
    c3[m].pre_create(&allocator, kLineWidth, kModes[m], 0, 0);
  }
  allocator.finalize(kdu_codestream());
  for (int m = 0; m < 5; ++m) {
    c1[m].create();
    c2[m].create();
    c3[m].create();
  }

  constexpr int kPartialWidth = 7;

  // Mode 0: rev16
  {
    std::vector<kdu_int16> er(kLineWidth), eg(kLineWidth), eb(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      er[i] = static_cast<kdu_int16>((i - 8) * 300);
      eg[i] = static_cast<kdu_int16>((6 - i) * 200);
      eb[i] = static_cast<kdu_int16>((i - 5) * 250);
      c1[0].get_buf16()[i].ival = er[i];
      c2[0].get_buf16()[i].ival = eg[i];
      c3[0].get_buf16()[i].ival = eb[i];
    }
    kdu_convert_rgb_to_ycc(c1[0], c2[0], c3[0]);
    std::vector<kdu_int16> ycc_r(kLineWidth), ycc_g(kLineWidth),
        ycc_b(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      ycc_r[i] = c1[0].get_buf16()[i].ival;
      ycc_g[i] = c2[0].get_buf16()[i].ival;
      ycc_b[i] = c3[0].get_buf16()[i].ival;
      EXPECT_EQ(ycc_g[i], static_cast<kdu_int16>(eb[i] - eg[i]));
    }
    kdu_convert_ycc_to_rgb(c1[0], c2[0], c3[0], kPartialWidth);
    for (int i = 0; i < kPartialWidth; ++i) {
      EXPECT_EQ(c1[0].get_buf16()[i].ival, er[i]);
      EXPECT_EQ(c2[0].get_buf16()[i].ival, eg[i]);
      EXPECT_EQ(c3[0].get_buf16()[i].ival, eb[i]);
    }
    for (int i = kPartialWidth; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[0].get_buf16()[i].ival, ycc_r[i]);
      EXPECT_EQ(c2[0].get_buf16()[i].ival, ycc_g[i]);
      EXPECT_EQ(c3[0].get_buf16()[i].ival, ycc_b[i]);
      // Restore transformed samples for the full-line default width test.
      c1[0].get_buf16()[i].ival = ycc_r[i];
      c2[0].get_buf16()[i].ival = ycc_g[i];
      c3[0].get_buf16()[i].ival = ycc_b[i];
    }
    for (int i = 0; i < kPartialWidth; ++i) {
      c1[0].get_buf16()[i].ival = ycc_r[i];
      c2[0].get_buf16()[i].ival = ycc_g[i];
      c3[0].get_buf16()[i].ival = ycc_b[i];
    }
    kdu_convert_ycc_to_rgb(c1[0], c2[0], c3[0]);
    for (int i = 0; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[0].get_buf16()[i].ival, er[i]);
      EXPECT_EQ(c2[0].get_buf16()[i].ival, eg[i]);
      EXPECT_EQ(c3[0].get_buf16()[i].ival, eb[i]);
    }
  }

  // Mode 1: irrev16
  {
    std::vector<kdu_int16> er(kLineWidth), eg(kLineWidth), eb(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      er[i] = static_cast<kdu_int16>((i - 9) * 350);
      eg[i] = static_cast<kdu_int16>((7 - i) * 300);
      eb[i] = static_cast<kdu_int16>((i - 4) * 280);
      c1[1].get_buf16()[i].ival = er[i];
      c2[1].get_buf16()[i].ival = eg[i];
      c3[1].get_buf16()[i].ival = eb[i];
    }
    kdu_convert_rgb_to_ycc(c1[1], c2[1], c3[1]);
    std::vector<kdu_int16> ycc_r(kLineWidth), ycc_g(kLineWidth),
        ycc_b(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      ycc_r[i] = c1[1].get_buf16()[i].ival;
      ycc_g[i] = c2[1].get_buf16()[i].ival;
      ycc_b[i] = c3[1].get_buf16()[i].ival;
    }
    kdu_convert_ycc_to_rgb(c1[1], c2[1], c3[1], kPartialWidth);
    for (int i = 0; i < kPartialWidth; ++i) {
      EXPECT_NEAR(c1[1].get_buf16()[i].ival, er[i], 3);
      EXPECT_NEAR(c2[1].get_buf16()[i].ival, eg[i], 3);
      EXPECT_NEAR(c3[1].get_buf16()[i].ival, eb[i], 3);
    }
    for (int i = kPartialWidth; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[1].get_buf16()[i].ival, ycc_r[i]);
      EXPECT_EQ(c2[1].get_buf16()[i].ival, ycc_g[i]);
      EXPECT_EQ(c3[1].get_buf16()[i].ival, ycc_b[i]);
    }
    for (int i = 0; i < kLineWidth; ++i) {
      c1[1].get_buf16()[i].ival = ycc_r[i];
      c2[1].get_buf16()[i].ival = ycc_g[i];
      c3[1].get_buf16()[i].ival = ycc_b[i];
    }
    kdu_convert_ycc_to_rgb(c1[1], c2[1], c3[1]);
    for (int i = 0; i < kLineWidth; ++i) {
      EXPECT_NEAR(c1[1].get_buf16()[i].ival, er[i], 3);
      EXPECT_NEAR(c2[1].get_buf16()[i].ival, eg[i], 3);
      EXPECT_NEAR(c3[1].get_buf16()[i].ival, eb[i], 3);
    }
  }

  // Mode 2: irrev32
  {
    std::vector<float> er(kLineWidth), eg(kLineWidth), eb(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      er[i] = -0.45f + 0.05f * static_cast<float>(i);
      eg[i] = 0.45f - 0.04f * static_cast<float>(i);
      eb[i] = -0.35f + 0.045f * static_cast<float>(i);
      c1[2].get_buf32()[i].fval = er[i];
      c2[2].get_buf32()[i].fval = eg[i];
      c3[2].get_buf32()[i].fval = eb[i];
    }
    kdu_convert_rgb_to_ycc(c1[2], c2[2], c3[2]);
    std::vector<float> ycc_r(kLineWidth), ycc_g(kLineWidth), ycc_b(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      ycc_r[i] = c1[2].get_buf32()[i].fval;
      ycc_g[i] = c2[2].get_buf32()[i].fval;
      ycc_b[i] = c3[2].get_buf32()[i].fval;
    }
    kdu_convert_ycc_to_rgb(c1[2], c2[2], c3[2], kPartialWidth);
    for (int i = 0; i < kPartialWidth; ++i) {
      EXPECT_NEAR(c1[2].get_buf32()[i].fval, er[i], 1e-5f);
      EXPECT_NEAR(c2[2].get_buf32()[i].fval, eg[i], 1e-5f);
      EXPECT_NEAR(c3[2].get_buf32()[i].fval, eb[i], 1e-5f);
    }
    for (int i = kPartialWidth; i < kLineWidth; ++i) {
      EXPECT_FLOAT_EQ(c1[2].get_buf32()[i].fval, ycc_r[i]);
      EXPECT_FLOAT_EQ(c2[2].get_buf32()[i].fval, ycc_g[i]);
      EXPECT_FLOAT_EQ(c3[2].get_buf32()[i].fval, ycc_b[i]);
    }
    for (int i = 0; i < kLineWidth; ++i) {
      c1[2].get_buf32()[i].fval = ycc_r[i];
      c2[2].get_buf32()[i].fval = ycc_g[i];
      c3[2].get_buf32()[i].fval = ycc_b[i];
    }
    kdu_convert_ycc_to_rgb(c1[2], c2[2], c3[2]);
    for (int i = 0; i < kLineWidth; ++i) {
      EXPECT_NEAR(c1[2].get_buf32()[i].fval, er[i], 1e-5f);
      EXPECT_NEAR(c2[2].get_buf32()[i].fval, eg[i], 1e-5f);
      EXPECT_NEAR(c3[2].get_buf32()[i].fval, eb[i], 1e-5f);
    }
  }

  // Mode 3: rev32
  {
    std::vector<kdu_int32> er(kLineWidth), eg(kLineWidth), eb(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      er[i] = (i - 8) * 100000;
      eg[i] = (6 - i) * 90000;
      eb[i] = (i - 3) * 110000;
      c1[3].get_buf32()[i].ival = er[i];
      c2[3].get_buf32()[i].ival = eg[i];
      c3[3].get_buf32()[i].ival = eb[i];
    }
    kdu_convert_rgb_to_ycc(c1[3], c2[3], c3[3]);
    std::vector<kdu_int32> ycc_r(kLineWidth), ycc_g(kLineWidth),
        ycc_b(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      ycc_r[i] = c1[3].get_buf32()[i].ival;
      ycc_g[i] = c2[3].get_buf32()[i].ival;
      ycc_b[i] = c3[3].get_buf32()[i].ival;
    }
    kdu_convert_ycc_to_rgb(c1[3], c2[3], c3[3], kPartialWidth);
    for (int i = 0; i < kPartialWidth; ++i) {
      EXPECT_EQ(c1[3].get_buf32()[i].ival, er[i]);
      EXPECT_EQ(c2[3].get_buf32()[i].ival, eg[i]);
      EXPECT_EQ(c3[3].get_buf32()[i].ival, eb[i]);
    }
    for (int i = kPartialWidth; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[3].get_buf32()[i].ival, ycc_r[i]);
      EXPECT_EQ(c2[3].get_buf32()[i].ival, ycc_g[i]);
      EXPECT_EQ(c3[3].get_buf32()[i].ival, ycc_b[i]);
    }
    for (int i = 0; i < kLineWidth; ++i) {
      c1[3].get_buf32()[i].ival = ycc_r[i];
      c2[3].get_buf32()[i].ival = ycc_g[i];
      c3[3].get_buf32()[i].ival = ycc_b[i];
    }
    kdu_convert_ycc_to_rgb(c1[3], c2[3], c3[3]);
    for (int i = 0; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[3].get_buf32()[i].ival, er[i]);
      EXPECT_EQ(c2[3].get_buf32()[i].ival, eg[i]);
      EXPECT_EQ(c3[3].get_buf32()[i].ival, eb[i]);
    }
  }

  // Mode 4: rev64
  {
    std::vector<kdu_int64> er(kLineWidth), eg(kLineWidth), eb(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      er[i] = static_cast<kdu_int64>(i - 7) * (1LL << 36);
      eg[i] = static_cast<kdu_int64>(5 - i) * (1LL << 35);
      eb[i] = static_cast<kdu_int64>(i - 4) * (1LL << 37);
      c1[4].get_buf64()[i].ival = er[i];
      c2[4].get_buf64()[i].ival = eg[i];
      c3[4].get_buf64()[i].ival = eb[i];
    }
    kdu_convert_rgb_to_ycc(c1[4], c2[4], c3[4]);
    std::vector<kdu_int64> ycc_r(kLineWidth), ycc_g(kLineWidth),
        ycc_b(kLineWidth);
    for (int i = 0; i < kLineWidth; ++i) {
      ycc_r[i] = c1[4].get_buf64()[i].ival;
      ycc_g[i] = c2[4].get_buf64()[i].ival;
      ycc_b[i] = c3[4].get_buf64()[i].ival;
    }
    kdu_convert_ycc_to_rgb(c1[4], c2[4], c3[4], kPartialWidth);
    for (int i = 0; i < kPartialWidth; ++i) {
      EXPECT_EQ(c1[4].get_buf64()[i].ival, er[i]);
      EXPECT_EQ(c2[4].get_buf64()[i].ival, eg[i]);
      EXPECT_EQ(c3[4].get_buf64()[i].ival, eb[i]);
    }
    for (int i = kPartialWidth; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[4].get_buf64()[i].ival, ycc_r[i]);
      EXPECT_EQ(c2[4].get_buf64()[i].ival, ycc_g[i]);
      EXPECT_EQ(c3[4].get_buf64()[i].ival, ycc_b[i]);
    }
    for (int i = 0; i < kLineWidth; ++i) {
      c1[4].get_buf64()[i].ival = ycc_r[i];
      c2[4].get_buf64()[i].ival = ycc_g[i];
      c3[4].get_buf64()[i].ival = ycc_b[i];
    }
    kdu_convert_ycc_to_rgb(c1[4], c2[4], c3[4]);
    for (int i = 0; i < kLineWidth; ++i) {
      EXPECT_EQ(c1[4].get_buf64()[i].ival, er[i]);
      EXPECT_EQ(c2[4].get_buf64()[i].ival, eg[i]);
      EXPECT_EQ(c3[4].get_buf64()[i].ival, eb[i]);
    }
  }
}

}  // namespace
}  // namespace kdu_core
