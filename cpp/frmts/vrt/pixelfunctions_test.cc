// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests the built-in VRT derived raster band pixel functions
// (pixelfunctions.cpp). Expression evaluation ("expression" pixel function) is
// compiled without muparser/exprtk in //third_party/gdal and is pinned to
// return CE_Failure.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/pixelfunctions.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <cmath>
#include <complex>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/pixelfunctions_test";
constexpr char kSrc1Path[] = "/vsimem/pixelfunctions_test/pf_src1.tif";
constexpr char kSrc2Path[] = "/vsimem/pixelfunctions_test/pf_src2.tif";

class PixelFunctionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
    ASSERT_EQ(CE_None, GDALRegisterDefaultPixelFunc());
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

VRTDerivedRasterBand::PixelFunc GetFunc(const char* name) {
  const auto* pair = VRTDerivedRasterBand::GetPixelFunction(name);
  if (pair == nullptr) {
    return nullptr;
  }
  return pair->first;
}

// ----------------------------------------------------------------------------
// 1. Registration and Lookup
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, RegistrationAndLookup) {
  EXPECT_EQ(CE_None, GDALRegisterDefaultPixelFunc());

  constexpr const char* kExpectedNames[] = {"real",
                                            "imag",
                                            "complex",
                                            "polar",
                                            "mod",
                                            "phase",
                                            "conj",
                                            "sum",
                                            "diff",
                                            "mul",
                                            "div",
                                            "cmul",
                                            "inv",
                                            "intensity",
                                            "sqrt",
                                            "log10",
                                            "dB",
                                            "exp",
                                            "dB2amp",
                                            "dB2pow",
                                            "pow",
                                            "interpolate_linear",
                                            "interpolate_exp",
                                            "replace_nodata",
                                            "scale",
                                            "norm_diff",
                                            "min",
                                            "argmin",
                                            "max",
                                            "argmax",
                                            "expression",
                                            "reclassify",
                                            "mean",
                                            "geometric_mean",
                                            "harmonic_mean",
                                            "median",
                                            "mode"};

  for (const char* name : kExpectedNames) {
    const auto* pair = VRTDerivedRasterBand::GetPixelFunction(name);
    ASSERT_NE(nullptr, pair) << "Failed to find pixel function: " << name;
    EXPECT_TRUE(static_cast<bool>(pair->first));
  }

  EXPECT_EQ(nullptr,
            VRTDerivedRasterBand::GetPixelFunction("non_existent_func"));

  auto names = VRTDerivedRasterBand::GetPixelFunctionNames();
  EXPECT_GE(names.size(), std::size(kExpectedNames));
}

// ----------------------------------------------------------------------------
// 2. Real and Imag
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, RealAndImag) {
  auto pfn_real = GetFunc("real");
  auto pfn_imag = GetFunc("imag");
  ASSERT_TRUE(static_cast<bool>(pfn_real));
  ASSERT_TRUE(static_cast<bool>(pfn_imag));

  // Failure: nSources != 1
  {
    WithQuietHandler quiet;
    float out[2] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_real(nullptr, 0, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, pfn_real(nullptr, 2, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, pfn_imag(nullptr, 0, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    CPLErrorReset();
  }

  // Complex to real: CFloat64, CFloat32, CInt16, CInt32
  {
    std::complex<double> c64[2] = {{10.5, -20.5}, {30.25, 40.75}};
    void* sources[] = {c64};
    double r64[2] = {0.0, 0.0};
    double i64[2] = {0.0, 0.0};
    EXPECT_EQ(CE_None, pfn_real(sources, 1, r64, 2, 1, GDT_CFloat64,
                                GDT_Float64, 8, 16, nullptr));
    EXPECT_EQ(CE_None, pfn_imag(sources, 1, i64, 2, 1, GDT_CFloat64,
                                GDT_Float64, 8, 16, nullptr));
    EXPECT_DOUBLE_EQ(10.5, r64[0]);
    EXPECT_DOUBLE_EQ(30.25, r64[1]);
    EXPECT_DOUBLE_EQ(-20.5, i64[0]);
    EXPECT_DOUBLE_EQ(40.75, i64[1]);

    std::complex<float> c32[2] = {{1.5f, 2.5f}, {-3.5f, -4.5f}};
    sources[0] = c32;
    float r32[2] = {0.0f, 0.0f};
    float i32[2] = {0.0f, 0.0f};
    EXPECT_EQ(CE_None, pfn_real(sources, 1, r32, 2, 1, GDT_CFloat32,
                                GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_None, pfn_imag(sources, 1, i32, 2, 1, GDT_CFloat32,
                                GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(1.5f, r32[0]);
    EXPECT_FLOAT_EQ(2.5f, i32[0]);

    int16_t ci16[4] = {100, 200, -300, -400};
    sources[0] = ci16;
    int16_t ri16[2] = {0, 0};
    int16_t ii16[2] = {0, 0};
    EXPECT_EQ(CE_None, pfn_real(sources, 1, ri16, 2, 1, GDT_CInt16, GDT_Int16,
                                2, 4, nullptr));
    EXPECT_EQ(CE_None, pfn_imag(sources, 1, ii16, 2, 1, GDT_CInt16, GDT_Int16,
                                2, 4, nullptr));
    EXPECT_EQ(100, ri16[0]);
    EXPECT_EQ(200, ii16[0]);

    int32_t ci32[4] = {1000, 2000, -3000, -4000};
    sources[0] = ci32;
    int32_t ri32[2] = {0, 0};
    int32_t ii32[2] = {0, 0};
    EXPECT_EQ(CE_None, pfn_real(sources, 1, ri32, 2, 1, GDT_CInt32, GDT_Int32,
                                4, 8, nullptr));
    EXPECT_EQ(CE_None, pfn_imag(sources, 1, ii32, 2, 1, GDT_CInt32, GDT_Int32,
                                4, 8, nullptr));
    EXPECT_EQ(1000, ri32[0]);
    EXPECT_EQ(2000, ii32[0]);
  }

  // Non-complex sources
  {
    float f32[2] = {42.0f, 99.0f};
    void* sources[] = {f32};
    float r_out[2] = {0.0f, 0.0f};
    float i_out[2] = {999.0f, 999.0f};
    EXPECT_EQ(CE_None, pfn_real(sources, 1, r_out, 2, 1, GDT_Float32,
                                GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_None, pfn_imag(sources, 1, i_out, 2, 1, GDT_Float32,
                                GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(42.0f, r_out[0]);
    EXPECT_FLOAT_EQ(0.0f, i_out[0]);
    EXPECT_FLOAT_EQ(0.0f, i_out[1]);
  }
}

// ----------------------------------------------------------------------------
// 3. Complex and Polar
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, ComplexAndPolar) {
  auto pfn_complex = GetFunc("complex");
  auto pfn_polar = GetFunc("polar");
  ASSERT_TRUE(static_cast<bool>(pfn_complex));
  ASSERT_TRUE(static_cast<bool>(pfn_polar));

  // ComplexPixelFunc failures
  {
    WithQuietHandler quiet;
    std::complex<float> out[2];
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_complex(nullptr, 1, out, 2, 1, GDT_Float32,
                                      GDT_CFloat32, 8, 16, nullptr));
    EXPECT_EQ(CE_Failure, pfn_complex(nullptr, 3, out, 2, 1, GDT_Float32,
                                      GDT_CFloat32, 8, 16, nullptr));
    CPLErrorReset();
  }

  // ComplexPixelFunc 2 sources
  {
    float real_part[2] = {1.5f, 3.5f};
    float imag_part[2] = {2.5f, 4.5f};
    void* sources[] = {real_part, imag_part};
    std::complex<float> c_out[2];
    EXPECT_EQ(CE_None, pfn_complex(sources, 2, c_out, 2, 1, GDT_Float32,
                                   GDT_CFloat32, 8, 16, nullptr));
    EXPECT_FLOAT_EQ(1.5f, c_out[0].real());
    EXPECT_FLOAT_EQ(2.5f, c_out[0].imag());
  }

  // PolarPixelFunc failures
  {
    WithQuietHandler quiet;
    std::complex<double> out[2];
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_polar(nullptr, 1, out, 2, 1, GDT_Float64,
                                    GDT_CFloat64, 16, 32, nullptr));
    CPLErrorReset();
  }

  // PolarPixelFunc AMPLITUDE (default), INTENSITY, dB
  {
    double amp[4] = {2.0, 4.0, 0.0, -1.0};
    double phase[4] = {0.0, std::numbers::pi / 2.0, 0.0, 0.0};
    void* sources[] = {amp, phase};
    std::complex<double> c_out[4];

    // Default AMPLITUDE
    EXPECT_EQ(CE_None, pfn_polar(sources, 2, c_out, 4, 1, GDT_Float64,
                                 GDT_CFloat64, 16, 64, nullptr));
    EXPECT_NEAR(2.0, c_out[0].real(), 1e-6);
    EXPECT_NEAR(0.0, c_out[0].imag(), 1e-6);
    EXPECT_NEAR(0.0, c_out[1].real(), 1e-6);
    EXPECT_NEAR(4.0, c_out[1].imag(), 1e-6);

    // INTENSITY: sqrt(max(0, amp))
    CPLStringList args;
    args.AddNameValue("amplitude_type", "INTENSITY");
    EXPECT_EQ(CE_None, pfn_polar(sources, 2, c_out, 4, 1, GDT_Float64,
                                 GDT_CFloat64, 16, 64, args.List()));
    EXPECT_NEAR(2.0, c_out[1].imag(), 1e-6);  // sqrt(4.0) = 2.0
    EXPECT_NEAR(0.0, c_out[3].real(), 1e-6);  // -1 clamped to 0

    // dB: pow(10, amp/20) or -inf
    args.Clear();
    args.AddNameValue("amplitude_type", "dB");
    EXPECT_EQ(CE_None, pfn_polar(sources, 2, c_out, 4, 1, GDT_Float64,
                                 GDT_CFloat64, 16, 64, args.List()));
    EXPECT_NEAR(std::pow(10.0, 2.0 / 20.0), c_out[0].real(), 1e-6);

    // Invalid amplitude_type
    args.Clear();
    args.AddNameValue("amplitude_type", "INVALID");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_polar(sources, 2, c_out, 4, 1, GDT_Float64,
                                    GDT_CFloat64, 16, 64, args.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

// ----------------------------------------------------------------------------
// 4. Module, Phase, Conj
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, ModulePhaseConj) {
  auto pfn_mod = GetFunc("mod");
  auto pfn_phase = GetFunc("phase");
  auto pfn_conj = GetFunc("conj");
  ASSERT_TRUE(static_cast<bool>(pfn_mod));
  ASSERT_TRUE(static_cast<bool>(pfn_phase));
  ASSERT_TRUE(static_cast<bool>(pfn_conj));

  // Module
  {
    double out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_mod(nullptr, 0, out, 2, 1, GDT_Float64,
                                    GDT_Float64, 8, 16, nullptr));
      CPLErrorReset();
    }

    // Complex source
    std::complex<double> c64[2] = {{3.0, 4.0}, {-5.0, 12.0}};
    void* sources[] = {c64};
    EXPECT_EQ(CE_None, pfn_mod(sources, 1, out, 2, 1, GDT_CFloat64, GDT_Float64,
                               8, 16, nullptr));
    EXPECT_DOUBLE_EQ(5.0, out[0]);
    EXPECT_DOUBLE_EQ(13.0, out[1]);

    // Real source (fabs)
    double r64[2] = {-42.5, 10.0};
    sources[0] = r64;
    EXPECT_EQ(CE_None, pfn_mod(sources, 1, out, 2, 1, GDT_Float64, GDT_Float64,
                               8, 16, nullptr));
    EXPECT_DOUBLE_EQ(42.5, out[0]);
    EXPECT_DOUBLE_EQ(10.0, out[1]);
  }

  // Phase
  {
    double out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_phase(nullptr, 2, out, 2, 1, GDT_Float64,
                                      GDT_Float64, 8, 16, nullptr));
      CPLErrorReset();
    }

    // Complex source
    std::complex<double> c64[2] = {{1.0, 1.0}, {-1.0, 0.0}};
    void* sources[] = {c64};
    EXPECT_EQ(CE_None, pfn_phase(sources, 1, out, 2, 1, GDT_CFloat64,
                                 GDT_Float64, 8, 16, nullptr));
    EXPECT_NEAR(std::numbers::pi / 4.0, out[0], 1e-6);
    EXPECT_NEAR(std::numbers::pi, out[1], 1e-6);

    // Unsigned integer: returns 0
    uint16_t u16[2] = {10, 20};
    sources[0] = u16;
    EXPECT_EQ(CE_None, pfn_phase(sources, 1, out, 2, 1, GDT_UInt16, GDT_Float64,
                                 8, 16, nullptr));
    EXPECT_DOUBLE_EQ(0.0, out[0]);

    // Signed real: pi if < 0, 0 if >= 0
    double r64[2] = {-5.0, 10.0};
    sources[0] = r64;
    EXPECT_EQ(CE_None, pfn_phase(sources, 1, out, 2, 1, GDT_Float64,
                                 GDT_Float64, 8, 16, nullptr));
    EXPECT_NEAR(std::numbers::pi, out[0], 1e-6);
    EXPECT_DOUBLE_EQ(0.0, out[1]);
  }

  // Conj
  {
    std::complex<double> c_out[2];
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_conj(nullptr, 0, c_out, 2, 1, GDT_CFloat64,
                                     GDT_CFloat64, 16, 32, nullptr));
      CPLErrorReset();
    }

    // Complex to complex
    std::complex<double> c64[2] = {{3.0, 4.0}, {5.0, -12.0}};
    void* sources[] = {c64};
    EXPECT_EQ(CE_None, pfn_conj(sources, 1, c_out, 2, 1, GDT_CFloat64,
                                GDT_CFloat64, 16, 32, nullptr));
    EXPECT_DOUBLE_EQ(3.0, c_out[0].real());
    EXPECT_DOUBLE_EQ(-4.0, c_out[0].imag());
    EXPECT_DOUBLE_EQ(5.0, c_out[1].real());
    EXPECT_DOUBLE_EQ(12.0, c_out[1].imag());

    // Real to real (fallback to real)
    double r_in[2] = {10.0, 20.0};
    double r_out[2] = {0.0, 0.0};
    sources[0] = r_in;
    EXPECT_EQ(CE_None, pfn_conj(sources, 1, r_out, 2, 1, GDT_Float64,
                                GDT_Float64, 8, 16, nullptr));
    EXPECT_DOUBLE_EQ(10.0, r_out[0]);
  }
}

// ----------------------------------------------------------------------------
// 5. Sum and Diff
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, SumAndDiff) {
  auto pfn_sum = GetFunc("sum");
  auto pfn_diff = GetFunc("diff");
  ASSERT_TRUE(static_cast<bool>(pfn_sum));
  ASSERT_TRUE(static_cast<bool>(pfn_diff));

  // Failures
  {
    WithQuietHandler quiet;
    float out[2] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_sum(nullptr, 0, out, 2, 1, GDT_Float32,
                                  GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLStringList bad_k;
    bad_k.AddNameValue("k", "invalid_number");
    void* dummy_sources[] = {out};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_sum(dummy_sources, 1, out, 2, 1, GDT_Float32,
                                  GDT_Float32, 4, 8, bad_k.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_diff(nullptr, 1, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, pfn_diff(nullptr, 3, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    CPLErrorReset();
  }

  constexpr int kNumPixels =
      35;  // Test vectorization (16/32 elements) + remainder (3 elements)

  // Sum with complex types
  {
    std::vector<std::complex<float>> c_src0(kNumPixels, {1.0f, 2.0f});
    std::vector<std::complex<float>> c_src1(kNumPixels, {3.0f, 4.0f});
    std::vector<std::complex<float>> c_out(kNumPixels);
    void* sources[] = {c_src0.data(), c_src1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources, 2, c_out.data(), kNumPixels, 1, GDT_CFloat32,
                      GDT_CFloat32, sizeof(std::complex<float>),
                      kNumPixels * sizeof(std::complex<float>), nullptr));
    EXPECT_FLOAT_EQ(4.0f, c_out[0].real());
    EXPECT_FLOAT_EQ(6.0f, c_out[0].imag());
  }

  // Sum same types (SSE2 paths): GDT_Byte, GDT_Int8, GDT_UInt16, GDT_Int16,
  // GDT_UInt32, GDT_Int32, GDT_Float32, GDT_Float64, GDT_UInt64, GDT_Int64
  {
    std::vector<uint8_t> b0(kNumPixels, 10), b1(kNumPixels, 20),
        b_out(kNumPixels);
    void* sources_b[] = {b0.data(), b1.data()};
    EXPECT_EQ(CE_None, pfn_sum(sources_b, 2, b_out.data(), kNumPixels, 1,
                               GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
    EXPECT_EQ(30, b_out[0]);
    EXPECT_EQ(30, b_out[kNumPixels - 1]);

    std::vector<int8_t> s8_0(kNumPixels, 5), s8_1(kNumPixels, -2),
        s8_out(kNumPixels);
    void* sources_s8[] = {s8_0.data(), s8_1.data()};
    EXPECT_EQ(CE_None, pfn_sum(sources_s8, 2, s8_out.data(), kNumPixels, 1,
                               GDT_Int8, GDT_Int8, 1, kNumPixels, nullptr));
    EXPECT_EQ(3, s8_out[0]);

    std::vector<uint16_t> u16_0(kNumPixels, 100), u16_1(kNumPixels, 200),
        u16_out(kNumPixels);
    void* sources_u16[] = {u16_0.data(), u16_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_u16, 2, u16_out.data(), kNumPixels, 1, GDT_UInt16,
                      GDT_UInt16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(300, u16_out[0]);

    std::vector<int16_t> i16_0(kNumPixels, -100), i16_1(kNumPixels, 300),
        i16_out(kNumPixels);
    void* sources_i16[] = {i16_0.data(), i16_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_i16, 2, i16_out.data(), kNumPixels, 1, GDT_Int16,
                      GDT_Int16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(200, i16_out[0]);

    std::vector<uint32_t> u32_0(kNumPixels, 1000), u32_1(kNumPixels, 2000),
        u32_out(kNumPixels);
    void* sources_u32[] = {u32_0.data(), u32_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_u32, 2, u32_out.data(), kNumPixels, 1, GDT_UInt32,
                      GDT_UInt32, 4, kNumPixels * 4, nullptr));
    EXPECT_EQ(3000u, u32_out[0]);

    std::vector<int32_t> i32_0(kNumPixels, -1000), i32_1(kNumPixels, 5000),
        i32_out(kNumPixels);
    void* sources_i32[] = {i32_0.data(), i32_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_i32, 2, i32_out.data(), kNumPixels, 1, GDT_Int32,
                      GDT_Int32, 4, kNumPixels * 4, nullptr));
    EXPECT_EQ(4000, i32_out[0]);

    std::vector<float> f32_0(kNumPixels, 1.25f), f32_1(kNumPixels, 2.75f),
        f32_out(kNumPixels);
    void* sources_f32[] = {f32_0.data(), f32_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_f32, 2, f32_out.data(), kNumPixels, 1,
                      GDT_Float32, GDT_Float32, 4, kNumPixels * 4, nullptr));
    EXPECT_FLOAT_EQ(4.0f, f32_out[0]);

    std::vector<double> f64_0(kNumPixels, 1.125), f64_1(kNumPixels, 2.875),
        f64_out(kNumPixels);
    void* sources_f64[] = {f64_0.data(), f64_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_f64, 2, f64_out.data(), kNumPixels, 1,
                      GDT_Float64, GDT_Float64, 8, kNumPixels * 8, nullptr));
    EXPECT_DOUBLE_EQ(4.0, f64_out[0]);

    std::vector<uint64_t> u64_0(kNumPixels, 100), u64_1(kNumPixels, 200),
        u64_out(kNumPixels);
    void* sources_u64[] = {u64_0.data(), u64_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_u64, 2, u64_out.data(), kNumPixels, 1, GDT_UInt64,
                      GDT_UInt64, 8, kNumPixels * 8, nullptr));
    EXPECT_EQ(300u, u64_out[0]);

    std::vector<int64_t> i64_0(kNumPixels, -50), i64_1(kNumPixels, 150),
        i64_out(kNumPixels);
    void* sources_i64[] = {i64_0.data(), i64_1.data()};
    EXPECT_EQ(CE_None,
              pfn_sum(sources_i64, 2, i64_out.data(), kNumPixels, 1, GDT_Int64,
                      GDT_Int64, 8, kNumPixels * 8, nullptr));
    EXPECT_EQ(100, i64_out[0]);
  }

  // Sum with k, NoData, propagateNoData, and mixed types
  {
    CPLStringList args;
    args.AddNameValue("k", "10.0");
    args.AddNameValue("NoData", "99");
    args.AddNameValue("propagateNoData", "true");

    float f0[4] = {1.0f, 99.0f, 3.0f, 4.0f};
    float f1[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    void* sources[] = {f0, f1};
    float f_out[4] = {};
    EXPECT_EQ(CE_None, pfn_sum(sources, 2, f_out, 4, 1, GDT_Float32,
                               GDT_Float32, 4, 16, args.List()));
    EXPECT_FLOAT_EQ(16.0f, f_out[0]);  // 1 + 5 + 10
    EXPECT_FLOAT_EQ(99.0f, f_out[1]);  // NoData propagated

    // propagateNoData = false (skip NoData)
    args.SetNameValue("propagateNoData", "false");
    EXPECT_EQ(CE_None, pfn_sum(sources, 2, f_out, 4, 1, GDT_Float32,
                               GDT_Float32, 4, 16, args.List()));
    EXPECT_FLOAT_EQ(16.0f, f_out[1]);  // 6 + 10 (99 was skipped)

    // Mixed types (source Byte to dest Float64)
    uint8_t u8[2] = {10, 20};
    void* src_mixed[] = {u8};
    double d_out[2] = {};
    EXPECT_EQ(CE_None, pfn_sum(src_mixed, 1, d_out, 2, 1, GDT_Byte, GDT_Float64,
                               8, 16, nullptr));
    EXPECT_DOUBLE_EQ(10.0, d_out[0]);
  }

  // Diff
  {
    float f0[2] = {10.0f, 25.0f};
    float f1[2] = {3.0f, 30.0f};
    void* sources[] = {f0, f1};
    float f_out[2] = {};
    EXPECT_EQ(CE_None, pfn_diff(sources, 2, f_out, 2, 1, GDT_Float32,
                                GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(7.0f, f_out[0]);
    EXPECT_FLOAT_EQ(-5.0f, f_out[1]);

    std::complex<double> c0[2] = {{10.0, 20.0}, {5.0, 6.0}};
    std::complex<double> c1[2] = {{3.0, 5.0}, {1.0, 2.0}};
    void* c_sources[] = {c0, c1};
    std::complex<double> c_out[2];
    EXPECT_EQ(CE_None, pfn_diff(c_sources, 2, c_out, 2, 1, GDT_CFloat64,
                                GDT_CFloat64, 16, 32, nullptr));
    EXPECT_DOUBLE_EQ(7.0, c_out[0].real());
    EXPECT_DOUBLE_EQ(15.0, c_out[0].imag());

    CPLStringList args;
    args.AddNameValue("NoData", "-999");
    f0[1] = -999.0f;
    EXPECT_EQ(CE_None, pfn_diff(sources, 2, f_out, 2, 1, GDT_Float32,
                                GDT_Float32, 4, 8, args.List()));
    EXPECT_FLOAT_EQ(-999.0f, f_out[1]);
  }
}

// ----------------------------------------------------------------------------
// 6. Mul, Div, CMul
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MulDivCMul) {
  auto pfn_mul = GetFunc("mul");
  auto pfn_div = GetFunc("div");
  auto pfn_cmul = GetFunc("cmul");
  ASSERT_TRUE(static_cast<bool>(pfn_mul));
  ASSERT_TRUE(static_cast<bool>(pfn_div));
  ASSERT_TRUE(static_cast<bool>(pfn_cmul));

  // Failures
  {
    WithQuietHandler quiet;
    float out[2] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_mul(nullptr, 0, out, 2, 1, GDT_Float32,
                                  GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_div(nullptr, 1, out, 2, 1, GDT_Float32,
                                  GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, pfn_cmul(nullptr, 1, out, 2, 1, GDT_CFloat32,
                                   GDT_CFloat32, 8, 16, nullptr));
    CPLErrorReset();
  }

  // Mul
  {
    float f0[2] = {2.0f, 5.0f};
    float f1[2] = {3.0f, 4.0f};
    void* sources[] = {f0, f1};
    float f_out[2] = {};

    CPLStringList args;
    args.AddNameValue("k", "2.0");
    EXPECT_EQ(CE_None, pfn_mul(sources, 2, f_out, 2, 1, GDT_Float32,
                               GDT_Float32, 4, 8, args.List()));
    EXPECT_FLOAT_EQ(12.0f, f_out[0]);  // 2 * 3 * 2

    // Complex mul: (1+2i)*(3+4i) = -5 + 10i
    std::complex<double> c0[1] = {{1.0, 2.0}};
    std::complex<double> c1[1] = {{3.0, 4.0}};
    void* c_sources[] = {c0, c1};
    std::complex<double> c_out[1];
    EXPECT_EQ(CE_None, pfn_mul(c_sources, 2, c_out, 1, 1, GDT_CFloat64,
                               GDT_CFloat64, 16, 16, nullptr));
    EXPECT_DOUBLE_EQ(-5.0, c_out[0].real());
    EXPECT_DOUBLE_EQ(10.0, c_out[0].imag());
  }

  // Div
  {
    float f0[2] = {10.0f, 5.0f};
    float f1[2] = {2.0f, 0.0f};  // Div by 0
    void* sources[] = {f0, f1};
    float f_out[2] = {};
    EXPECT_EQ(CE_None, pfn_div(sources, 2, f_out, 2, 1, GDT_Float32,
                               GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(5.0f, f_out[0]);
    EXPECT_TRUE(std::isinf(f_out[1]));

    std::complex<double> c0[2] = {{-5.0, 10.0}, {1.0, 2.0}};
    std::complex<double> c1[2] = {{1.0, 2.0}, {0.0, 0.0}};  // Div by 0
    void* c_sources[] = {c0, c1};
    std::complex<double> c_out[2];
    EXPECT_EQ(CE_None, pfn_div(c_sources, 2, c_out, 2, 1, GDT_CFloat64,
                               GDT_CFloat64, 16, 32, nullptr));
    EXPECT_NEAR(3.0, c_out[0].real(), 1e-6);
    EXPECT_NEAR(4.0, c_out[0].imag(), 1e-6);
    EXPECT_TRUE(std::isinf(c_out[1].real()));
  }

  // CMul
  {
    std::complex<float> c0[1] = {{2.0f, 3.0f}};
    std::complex<float> c1[1] = {{4.0f, -5.0f}};
    void* sources[] = {c0, c1};
    std::complex<float> c_out[1];
    EXPECT_EQ(CE_None, pfn_cmul(sources, 2, c_out, 1, 1, GDT_CFloat32,
                                GDT_CFloat32, 8, 8, nullptr));
    // (2+3i) * conj(4-5i) = -7 + 22i
    EXPECT_FLOAT_EQ(-7.0f, c_out[0].real());
    EXPECT_FLOAT_EQ(22.0f, c_out[0].imag());

    // Non-complex inputs
    float r0[1] = {3.0f};
    float r1[1] = {4.0f};
    void* r_sources[] = {r0, r1};
    std::complex<double> cr_out[1];
    EXPECT_EQ(CE_None, pfn_cmul(r_sources, 2, cr_out, 1, 1, GDT_Float32,
                                GDT_CFloat64, 16, 16, nullptr));
    EXPECT_DOUBLE_EQ(12.0, cr_out[0].real());
    EXPECT_DOUBLE_EQ(0.0, cr_out[0].imag());
  }
}

// ----------------------------------------------------------------------------
// 7. Inv, Intensity, Sqrt, Log10, DB, Exp, Pow
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, InvIntensitySqrtLog10DBExpPow) {
  auto pfn_inv = GetFunc("inv");
  auto pfn_intensity = GetFunc("intensity");
  auto pfn_sqrt = GetFunc("sqrt");
  auto pfn_log10 = GetFunc("log10");
  auto pfn_db = GetFunc("dB");
  auto pfn_exp = GetFunc("exp");
  auto pfn_db2amp = GetFunc("dB2amp");
  auto pfn_db2pow = GetFunc("dB2pow");
  auto pfn_pow = GetFunc("pow");
  ASSERT_TRUE(static_cast<bool>(pfn_inv));
  ASSERT_TRUE(static_cast<bool>(pfn_intensity));
  ASSERT_TRUE(static_cast<bool>(pfn_sqrt));
  ASSERT_TRUE(static_cast<bool>(pfn_log10));
  ASSERT_TRUE(static_cast<bool>(pfn_db));
  ASSERT_TRUE(static_cast<bool>(pfn_exp));
  ASSERT_TRUE(static_cast<bool>(pfn_db2amp));
  ASSERT_TRUE(static_cast<bool>(pfn_db2pow));
  ASSERT_TRUE(static_cast<bool>(pfn_pow));

  // Inv
  {
    float out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_inv(nullptr, 0, out, 2, 1, GDT_Float32,
                                    GDT_Float32, 4, 8, nullptr));
      CPLErrorReset();
    }

    float f_in[2] = {4.0f, 0.0f};
    void* sources[] = {f_in};
    CPLStringList args;
    args.AddNameValue("k", "2.0");
    EXPECT_EQ(CE_None, pfn_inv(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32,
                               4, 8, args.List()));
    EXPECT_FLOAT_EQ(0.5f, out[0]);  // 2 / 4
    EXPECT_TRUE(std::isinf(out[1]));

    std::complex<double> c_in[1] = {{1.0, 1.0}};
    void* c_sources[] = {c_in};
    std::complex<double> c_out[1];
    EXPECT_EQ(CE_None, pfn_inv(c_sources, 1, c_out, 1, 1, GDT_CFloat64,
                               GDT_CFloat64, 16, 16, nullptr));
    EXPECT_NEAR(0.5, c_out[0].real(), 1e-6);
    EXPECT_NEAR(-0.5, c_out[0].imag(), 1e-6);
  }

  // Intensity
  {
    double out[1] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_intensity(nullptr, 0, out, 1, 1, GDT_CFloat64,
                                          GDT_Float64, 8, 8, nullptr));
      CPLErrorReset();
    }

    std::complex<double> c_in[1] = {{3.0, 4.0}};
    void* sources[] = {c_in};
    EXPECT_EQ(CE_None, pfn_intensity(sources, 1, out, 1, 1, GDT_CFloat64,
                                     GDT_Float64, 8, 8, nullptr));
    EXPECT_DOUBLE_EQ(25.0, out[0]);

    // Real input computes x * x
    double r_in[1] = {5.0};
    sources[0] = r_in;
    EXPECT_EQ(CE_None, pfn_intensity(sources, 1, out, 1, 1, GDT_Float64,
                                     GDT_Float64, 8, 8, nullptr));
    EXPECT_DOUBLE_EQ(25.0, out[0]);
  }

  // Sqrt
  {
    float out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_sqrt(nullptr, 0, out, 2, 1, GDT_Float32,
                                     GDT_Float32, 4, 8, nullptr));
      CPLErrorReset();
    }

    float f_in[2] = {9.0f, -4.0f};
    void* sources[] = {f_in};
    EXPECT_EQ(CE_None, pfn_sqrt(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32,
                                4, 8, nullptr));
    EXPECT_FLOAT_EQ(3.0f, out[0]);
    EXPECT_TRUE(std::isnan(out[1]));
  }

  // Log10 & dB
  {
    float out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_log10(nullptr, 0, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, nullptr));
      CPLErrorReset();
    }

    float f_in[2] = {100.0f, 0.0f};
    void* sources[] = {f_in};
    EXPECT_EQ(CE_None, pfn_log10(sources, 1, out, 2, 1, GDT_Float32,
                                 GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(2.0f, out[0]);
    EXPECT_TRUE(std::isinf(out[1]));

    // Complex Log10
    std::complex<double> c_in[1] = {{10.0, 0.0}};
    void* c_sources[] = {c_in};
    double d_out[1] = {};
    EXPECT_EQ(CE_None, pfn_log10(c_sources, 1, d_out, 1, 1, GDT_CFloat64,
                                 GDT_Float64, 8, 8, nullptr));
    EXPECT_NEAR(1.0, d_out[0], 1e-6);

    // dB
    CPLStringList args;
    args.AddNameValue("fact", "10.0");
    EXPECT_EQ(CE_None, pfn_db(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32,
                              4, 8, args.List()));
    EXPECT_FLOAT_EQ(20.0f, out[0]);  // 10 * log10(100) = 20
  }

  // Exp, dB2amp, dB2pow
  {
    float f_in[2] = {2.0f, 0.0f};
    void* sources[] = {f_in};
    float out[2] = {};

    CPLStringList args;
    args.AddNameValue("base", "2.0");
    args.AddNameValue("fact", "3.0");
    EXPECT_EQ(CE_None, pfn_exp(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32,
                               4, 8, args.List()));
    EXPECT_FLOAT_EQ(64.0f, out[0]);  // 2^(2 * 3) = 64

    float db_in[1] = {20.0f};
    sources[0] = db_in;
    EXPECT_EQ(CE_None, pfn_db2amp(sources, 1, out, 1, 1, GDT_Float32,
                                  GDT_Float32, 4, 4, nullptr));
    EXPECT_NEAR(10.0f, out[0], 1e-4);

    EXPECT_EQ(CE_None, pfn_db2pow(sources, 1, out, 1, 1, GDT_Float32,
                                  GDT_Float32, 4, 4, nullptr));
    EXPECT_NEAR(100.0f, out[0], 1e-4);
  }

  // Pow
  {
    float out[1] = {};
    float f_in[1] = {3.0f};
    void* sources[] = {f_in};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_pow(nullptr, 0, out, 1, 1, GDT_Float32,
                                    GDT_Float32, 4, 4, nullptr));

      // Missing power arg
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_pow(sources, 1, out, 1, 1, GDT_Float32,
                                    GDT_Float32, 4, 4, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    CPLStringList args;
    args.AddNameValue("power", "3.0");
    EXPECT_EQ(CE_None, pfn_pow(sources, 1, out, 1, 1, GDT_Float32, GDT_Float32,
                               4, 4, args.List()));
    EXPECT_FLOAT_EQ(27.0f, out[0]);
  }
}

// ----------------------------------------------------------------------------
// 8. Interpolate, ReplaceNoData, Scale, NormDiff
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, InterpolateReplaceNoDataScaleNormDiff) {
  auto pfn_interp_lin = GetFunc("interpolate_linear");
  auto pfn_interp_exp = GetFunc("interpolate_exp");
  auto pfn_replace_nd = GetFunc("replace_nodata");
  auto pfn_scale = GetFunc("scale");
  auto pfn_norm_diff = GetFunc("norm_diff");
  ASSERT_TRUE(static_cast<bool>(pfn_interp_lin));
  ASSERT_TRUE(static_cast<bool>(pfn_interp_exp));
  ASSERT_TRUE(static_cast<bool>(pfn_replace_nd));
  ASSERT_TRUE(static_cast<bool>(pfn_scale));
  ASSERT_TRUE(static_cast<bool>(pfn_norm_diff));

  // Interpolate
  {
    float out[1] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_interp_lin(nullptr, 1, out, 1, 1, GDT_Float32,
                                           GDT_Float32, 4, 4, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    CPLStringList args;
    args.AddNameValue("t0", "0.0");
    args.AddNameValue("dt", "10.0");
    args.AddNameValue("t", "5.0");

    float src0[1] = {10.0f};
    float src1[1] = {20.0f};
    void* sources[] = {src0, src1};
    EXPECT_EQ(CE_None, pfn_interp_lin(sources, 2, out, 1, 1, GDT_Float32,
                                      GDT_Float32, 4, 4, args.List()));
    EXPECT_FLOAT_EQ(15.0f, out[0]);

    EXPECT_EQ(CE_None, pfn_interp_exp(sources, 2, out, 1, 1, GDT_Float32,
                                      GDT_Float32, 4, 4, args.List()));
    EXPECT_LT(0.0f, out[0]);

    // Boundary t == t0 (using non-zero t0 and t)
    args.SetNameValue("t0", "5.0");
    args.SetNameValue("dt", "10.0");
    args.SetNameValue("t", "5.0");
    EXPECT_EQ(CE_None, pfn_interp_lin(sources, 2, out, 1, 1, GDT_Float32,
                                      GDT_Float32, 4, 4, args.List()));
    EXPECT_FLOAT_EQ(10.0f, out[0]);

    // Boundary t == t1
    args.SetNameValue("t", "15.0");
    EXPECT_EQ(CE_None, pfn_interp_lin(sources, 2, out, 1, 1, GDT_Float32,
                                      GDT_Float32, 4, 4, args.List()));
    EXPECT_FLOAT_EQ(20.0f, out[0]);
  }

  // ReplaceNoData
  {
    float out[2] = {};
    float f_in[2] = {0.0f, 42.0f};
    void* sources[] = {f_in};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_replace_nd(nullptr, 0, out, 2, 1, GDT_Float32,
                                           GDT_Float32, 4, 8, nullptr));

      // Missing NoData
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_replace_nd(sources, 1, out, 2, 1, GDT_Float32,
                                           GDT_Float32, 4, 8, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    CPLStringList args;
    args.AddNameValue("NoData", "0.0");
    args.AddNameValue("to", "99.0");
    EXPECT_EQ(CE_None, pfn_replace_nd(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, args.List()));
    EXPECT_FLOAT_EQ(99.0f, out[0]);
    EXPECT_FLOAT_EQ(42.0f, out[1]);

    // Using nan on non-floating output fails
    uint8_t b_out[2] = {};
    args.SetNameValue("to", "nan");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_replace_nd(sources, 1, b_out, 2, 1, GDT_Float32,
                                         GDT_Byte, 1, 2, args.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Scale
  {
    float out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_scale(nullptr, 0, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, nullptr));
      CPLErrorReset();
    }

    float f_in[2] = {10.0f, -999.0f};
    void* sources[] = {f_in};

    CPLStringList args;
    args.AddNameValue("scale", "2.0");
    args.AddNameValue("offset", "5.0");
    args.AddNameValue("NoData", "-999.0");
    EXPECT_EQ(CE_None, pfn_scale(sources, 1, out, 2, 1, GDT_Float32,
                                 GDT_Float32, 4, 8, args.List()));
    EXPECT_FLOAT_EQ(25.0f, out[0]);  // 10 * 2 + 5
    EXPECT_FLOAT_EQ(-999.0f, out[1]);
  }

  // NormDiff
  {
    float out[2] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, pfn_norm_diff(nullptr, 1, out, 2, 1, GDT_Float32,
                                          GDT_Float32, 4, 8, nullptr));
      CPLErrorReset();
    }

    float f0[2] = {5.0f, 0.0f};
    float f1[2] = {3.0f, 0.0f};  // denom 0
    void* sources[] = {f0, f1};
    EXPECT_EQ(CE_None, pfn_norm_diff(sources, 2, out, 2, 1, GDT_Float32,
                                     GDT_Float32, 4, 8, nullptr));
    EXPECT_FLOAT_EQ(0.25f, out[0]);  // (5-3)/(5+3) = 2/8 = 0.25
    EXPECT_TRUE(std::isinf(out[1]));
  }
}

// ----------------------------------------------------------------------------
// 9. Min, Max, ArgMin, ArgMax
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MinMaxArgMinArgMax) {
  auto pfn_min = GetFunc("min");
  auto pfn_arg_min = GetFunc("argmin");
  auto pfn_max = GetFunc("max");
  auto pfn_arg_max = GetFunc("argmax");
  ASSERT_TRUE(static_cast<bool>(pfn_min));
  ASSERT_TRUE(static_cast<bool>(pfn_arg_min));
  ASSERT_TRUE(static_cast<bool>(pfn_max));
  ASSERT_TRUE(static_cast<bool>(pfn_arg_max));

  constexpr int kNumPixels = 35;  // Test SSE2 + scalar loop

  // Min with Byte, Int16, UInt16, Float32
  {
    std::vector<uint8_t> b0(kNumPixels, 10), b1(kNumPixels, 5),
        b2(kNumPixels, 20), b_out(kNumPixels);
    void* sources[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(CE_None, pfn_min(sources, 3, b_out.data(), kNumPixels, 1,
                               GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
    EXPECT_EQ(5, b_out[0]);
    EXPECT_EQ(5, b_out[kNumPixels - 1]);

    std::vector<int16_t> i0(kNumPixels, -10), i1(kNumPixels, -50),
        i_out(kNumPixels);
    void* sources_i[] = {i0.data(), i1.data()};
    EXPECT_EQ(CE_None,
              pfn_min(sources_i, 2, i_out.data(), kNumPixels, 1, GDT_Int16,
                      GDT_Int16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(-50, i_out[0]);

    std::vector<uint16_t> u0(kNumPixels, 100), u1(kNumPixels, 50),
        u_out(kNumPixels);
    void* sources_u[] = {u0.data(), u1.data()};
    EXPECT_EQ(CE_None,
              pfn_min(sources_u, 2, u_out.data(), kNumPixels, 1, GDT_UInt16,
                      GDT_UInt16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(50, u_out[0]);

    std::vector<float> f0(kNumPixels, 3.5f), f1(kNumPixels, 1.5f),
        f_out(kNumPixels);
    void* sources_f[] = {f0.data(), f1.data()};
    EXPECT_EQ(CE_None,
              pfn_min(sources_f, 2, f_out.data(), kNumPixels, 1, GDT_Float32,
                      GDT_Float32, 4, kNumPixels * 4, nullptr));
    EXPECT_FLOAT_EQ(1.5f, f_out[0]);
  }

  // Max with Byte, Int16, UInt16, Float32
  {
    std::vector<uint8_t> b0(kNumPixels, 10), b1(kNumPixels, 5),
        b2(kNumPixels, 20), b_out(kNumPixels);
    void* sources[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(CE_None, pfn_max(sources, 3, b_out.data(), kNumPixels, 1,
                               GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
    EXPECT_EQ(20, b_out[0]);

    std::vector<int16_t> i0(kNumPixels, -10), i1(kNumPixels, -50),
        i_out(kNumPixels);
    void* sources_i[] = {i0.data(), i1.data()};
    EXPECT_EQ(CE_None,
              pfn_max(sources_i, 2, i_out.data(), kNumPixels, 1, GDT_Int16,
                      GDT_Int16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(-10, i_out[0]);

    std::vector<uint16_t> u0(kNumPixels, 100), u1(kNumPixels, 50),
        u_out(kNumPixels);
    void* sources_u[] = {u0.data(), u1.data()};
    EXPECT_EQ(CE_None,
              pfn_max(sources_u, 2, u_out.data(), kNumPixels, 1, GDT_UInt16,
                      GDT_UInt16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(100, u_out[0]);

    std::vector<float> f0(kNumPixels, 3.5f), f1(kNumPixels, 1.5f),
        f_out(kNumPixels);
    void* sources_f[] = {f0.data(), f1.data()};
    EXPECT_EQ(CE_None,
              pfn_max(sources_f, 2, f_out.data(), kNumPixels, 1, GDT_Float32,
                      GDT_Float32, 4, kNumPixels * 4, nullptr));
    EXPECT_FLOAT_EQ(3.5f, f_out[0]);
  }

  // ArgMin and ArgMax (1-based index)
  {
    float f0[3] = {10.0f, 20.0f, 30.0f};
    float f1[3] = {20.0f, 10.0f, 40.0f};
    float f2[3] = {30.0f, 40.0f, 10.0f};
    void* sources[] = {f0, f1, f2};
    int32_t idx_out[3] = {};

    EXPECT_EQ(CE_None, pfn_arg_min(sources, 3, idx_out, 3, 1, GDT_Float32,
                                   GDT_Int32, 4, 12, nullptr));
    EXPECT_EQ(1, idx_out[0]);
    EXPECT_EQ(2, idx_out[1]);
    EXPECT_EQ(3, idx_out[2]);

    EXPECT_EQ(CE_None, pfn_arg_max(sources, 3, idx_out, 3, 1, GDT_Float32,
                                   GDT_Int32, 4, 12, nullptr));
    EXPECT_EQ(3, idx_out[0]);
    EXPECT_EQ(3, idx_out[1]);
    EXPECT_EQ(2, idx_out[2]);
  }

  // Min with k and NoData
  {
    float f0[2] = {10.0f, 99.0f};
    float f1[2] = {20.0f, 99.0f};
    void* sources[] = {f0, f1};
    float out[2] = {};

    CPLStringList args;
    args.AddNameValue("k", "5.0");
    args.AddNameValue("NoData", "99.0");
    EXPECT_EQ(CE_None, pfn_min(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32,
                               4, 8, args.List()));
    EXPECT_FLOAT_EQ(5.0f, out[0]);  // min(10, 20, k=5) = 5
    EXPECT_FLOAT_EQ(5.0f, out[1]);  // all NoData -> k=5
  }
}

// ----------------------------------------------------------------------------
// 10. Expression
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Expression) {
  auto pfn_expr = GetFunc("expression");
  ASSERT_TRUE(static_cast<bool>(pfn_expr));

  WithQuietHandler quiet;

  // Complex not supported
  {
    float out[2] = {};
    void* dummy_sources[] = {out};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_expr(dummy_sources, 1, out, 2, 1, GDT_CFloat32,
                                   GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Missing expression or bad source names
  {
    float in[2] = {1.0f, 2.0f};
    void* sources[] = {in};
    float out[2] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_expr(sources, 1, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLStringList args;
    args.AddNameValue("expression", "b1 + 1");
    args.AddNameValue("source_names", "b1|b2");  // 2 names for 1 source
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_expr(sources, 1, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, args.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Expression parser is disabled in //third_party/gdal -> returns CE_Failure
  {
    float b1[2] = {10.0f, 20.0f};
    float b2[2] = {3.0f, 4.0f};
    void* sources[] = {b1, b2};
    float out[2] = {};

    CPLStringList args;
    args.AddNameValue("expression", "b1 + 2 * b2");
    args.AddNameValue("source_names", "b1|b2");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_expr(sources, 2, out, 2, 1, GDT_Float32,
                                   GDT_Float32, 4, 8, args.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("muparser"));
  }
  CPLErrorReset();
}

// ----------------------------------------------------------------------------
// 11. Reclassify
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Reclassify) {
  auto pfn_reclassify = GetFunc("reclassify");
  ASSERT_TRUE(static_cast<bool>(pfn_reclassify));

  // Failures
  {
    WithQuietHandler quiet;
    float out[2] = {};
    void* dummy_sources[] = {out};
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_reclassify(nullptr, 0, out, 2, 1, GDT_Float32,
                                         GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              pfn_reclassify(dummy_sources, 1, out, 2, 1, GDT_CFloat32,
                             GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    float in[2] = {1.0f, 2.0f};
    void* sources[] = {in};
    // Missing mapping
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, pfn_reclassify(sources, 1, out, 2, 1, GDT_Float32,
                                         GDT_Float32, 4, 8, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Valid reclassify mappings
  {
    float in[4] = {1.0f, 2.0f, 3.0f, 99.0f};
    void* sources[] = {in};
    float out[4] = {};

    CPLStringList args;
    args.AddNameValue("mapping", "1=10; 2=20; default=0");
    EXPECT_EQ(CE_None, pfn_reclassify(sources, 1, out, 4, 1, GDT_Float32,
                                      GDT_Float32, 4, 16, args.List()));
    EXPECT_FLOAT_EQ(10.0f, out[0]);
    EXPECT_FLOAT_EQ(20.0f, out[1]);
    EXPECT_FLOAT_EQ(0.0f, out[2]);  // default
    EXPECT_FLOAT_EQ(0.0f, out[3]);

    // With NoData
    args.Clear();
    args.AddNameValue("mapping", "1=10; 2=20; NO_DATA=NO_DATA; default=0");
    args.AddNameValue("NoData", "99.0");
    EXPECT_EQ(CE_None, pfn_reclassify(sources, 1, out, 4, 1, GDT_Float32,
                                      GDT_Float32, 4, 16, args.List()));
    EXPECT_FLOAT_EQ(99.0f, out[3]);  // NoData preserved
  }
}

// ----------------------------------------------------------------------------
// 12. Basic Functions: Mean, GeometricMean, HarmonicMean, Median, Mode
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, BasicPixelFuncs) {
  auto pfn_mean = GetFunc("mean");
  auto pfn_geo_mean = GetFunc("geometric_mean");
  auto pfn_harmonic_mean = GetFunc("harmonic_mean");
  auto pfn_median = GetFunc("median");
  auto pfn_mode = GetFunc("mode");

  ASSERT_TRUE(static_cast<bool>(pfn_mean));
  ASSERT_TRUE(static_cast<bool>(pfn_geo_mean));
  ASSERT_TRUE(static_cast<bool>(pfn_harmonic_mean));
  ASSERT_TRUE(static_cast<bool>(pfn_median));
  ASSERT_TRUE(static_cast<bool>(pfn_mode));

  constexpr int kNumPixels =
      35;  // Test vectorization (16 elements) + remainder

  // Mean SSE2 paths: Byte, Int16, UInt16 (2 & 3 sources), Float32, Float64
  {
    std::vector<uint8_t> b0(kNumPixels, 10), b1(kNumPixels, 20),
        b2(kNumPixels, 30), b_out(kNumPixels);
    void* sources_b2[] = {b0.data(), b1.data()};
    EXPECT_EQ(CE_None, pfn_mean(sources_b2, 2, b_out.data(), kNumPixels, 1,
                                GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
    EXPECT_EQ(15, b_out[0]);
    EXPECT_EQ(15, b_out[kNumPixels - 1]);

    void* sources_b3[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(CE_None, pfn_mean(sources_b3, 3, b_out.data(), kNumPixels, 1,
                                GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
    EXPECT_EQ(20, b_out[0]);

    // Int16
    std::vector<int16_t> i0(kNumPixels, 100), i1(kNumPixels, 200),
        i2(kNumPixels, 300), i_out(kNumPixels);
    void* sources_i2[] = {i0.data(), i1.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_i2, 2, i_out.data(), kNumPixels, 1, GDT_Int16,
                       GDT_Int16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(150, i_out[0]);

    void* sources_i3[] = {i0.data(), i1.data(), i2.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_i3, 3, i_out.data(), kNumPixels, 1, GDT_Int16,
                       GDT_Int16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(200, i_out[0]);

    // UInt16
    std::vector<uint16_t> u0(kNumPixels, 100), u1(kNumPixels, 200),
        u2(kNumPixels, 300), u_out(kNumPixels);
    void* sources_u2[] = {u0.data(), u1.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_u2, 2, u_out.data(), kNumPixels, 1, GDT_UInt16,
                       GDT_UInt16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(150, u_out[0]);

    void* sources_u3[] = {u0.data(), u1.data(), u2.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_u3, 3, u_out.data(), kNumPixels, 1, GDT_UInt16,
                       GDT_UInt16, 2, kNumPixels * 2, nullptr));
    EXPECT_EQ(200, u_out[0]);

    // Float32 & Float64
    std::vector<float> f0(kNumPixels, 1.5f), f1(kNumPixels, 4.5f),
        f_out(kNumPixels);
    void* sources_f[] = {f0.data(), f1.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_f, 2, f_out.data(), kNumPixels, 1, GDT_Float32,
                       GDT_Float32, 4, kNumPixels * 4, nullptr));
    EXPECT_FLOAT_EQ(3.0f, f_out[0]);

    std::vector<double> d0(kNumPixels, 10.0), d1(kNumPixels, 20.0),
        d_out(kNumPixels);
    void* sources_d[] = {d0.data(), d1.data()};
    EXPECT_EQ(CE_None,
              pfn_mean(sources_d, 2, d_out.data(), kNumPixels, 1, GDT_Float64,
                       GDT_Float64, 8, kNumPixels * 8, nullptr));
    EXPECT_DOUBLE_EQ(15.0, d_out[0]);
  }

  // Mean with infinities, NoData, propagateNoData
  {
    double d0[3] = {std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(), 10.0};
    double d1[3] = {std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(), 20.0};
    void* sources[] = {d0, d1};
    double d_out[3] = {};
    EXPECT_EQ(CE_None, pfn_mean(sources, 2, d_out, 3, 1, GDT_Float64,
                                GDT_Float64, 8, 24, nullptr));
    EXPECT_TRUE(std::isinf(d_out[0]));
    EXPECT_TRUE(std::isnan(d_out[1]));  // -inf + inf -> NaN
    EXPECT_DOUBLE_EQ(15.0, d_out[2]);

    CPLStringList args;
    args.AddNameValue("NoData", "99.0");
    args.AddNameValue("propagateNoData", "true");
    d0[2] = 99.0;
    EXPECT_EQ(CE_None, pfn_mean(sources, 2, d_out, 3, 1, GDT_Float64,
                                GDT_Float64, 8, 24, args.List()));
    EXPECT_DOUBLE_EQ(99.0, d_out[2]);
  }

  // Geometric Mean
  {
    double d0[2] = {4.0, 2.0};
    double d1[2] = {9.0, 8.0};
    void* sources[] = {d0, d1};
    double d_out[2] = {};
    EXPECT_EQ(CE_None, pfn_geo_mean(sources, 2, d_out, 2, 1, GDT_Float64,
                                    GDT_Float64, 8, 16, nullptr));
    EXPECT_DOUBLE_EQ(6.0, d_out[0]);  // sqrt(4 * 9) = 6
    EXPECT_DOUBLE_EQ(4.0, d_out[1]);  // sqrt(2 * 8) = 4
  }

  // Harmonic Mean
  {
    double d0[2] = {2.0, 0.0};
    double d1[2] = {6.0, 10.0};
    void* sources[] = {d0, d1};
    double d_out[2] = {};
    EXPECT_EQ(CE_None, pfn_harmonic_mean(sources, 2, d_out, 2, 1, GDT_Float64,
                                         GDT_Float64, 8, 16, nullptr));
    EXPECT_DOUBLE_EQ(3.0, d_out[0]);  // 2 / (1/2 + 1/6) = 3.0

    // propagateZero = 1
    CPLStringList args;
    args.AddNameValue("propagateZero", "1");
    EXPECT_EQ(CE_None, pfn_harmonic_mean(sources, 2, d_out, 2, 1, GDT_Float64,
                                         GDT_Float64, 8, 16, args.List()));
    EXPECT_DOUBLE_EQ(0.0, d_out[1]);
  }

  // Median (odd and even source counts)
  {
    double d0[1] = {10.0};
    double d1[1] = {50.0};
    double d2[1] = {20.0};
    void* sources3[] = {d0, d1, d2};
    double d_out[1] = {};
    EXPECT_EQ(CE_None, pfn_median(sources3, 3, d_out, 1, 1, GDT_Float64,
                                  GDT_Float64, 8, 8, nullptr));
    EXPECT_DOUBLE_EQ(20.0, d_out[0]);

    double d3[1] = {30.0};
    void* sources4[] = {d0, d1, d2, d3};
    EXPECT_EQ(CE_None, pfn_median(sources4, 4, d_out, 1, 1, GDT_Float64,
                                  GDT_Float64, 8, 8, nullptr));
    EXPECT_DOUBLE_EQ(25.0, d_out[0]);
  }

  // Mode
  {
    double d0[1] = {5.0};
    double d1[1] = {10.0};
    double d2[1] = {10.0};
    double d3[1] = {20.0};
    void* sources[] = {d0, d1, d2, d3};
    double d_out[1] = {};
    EXPECT_EQ(CE_None, pfn_mode(sources, 4, d_out, 1, 1, GDT_Float64,
                                GDT_Float64, 8, 8, nullptr));
    EXPECT_DOUBLE_EQ(10.0, d_out[0]);
  }
}

// ----------------------------------------------------------------------------
// 13. VRTDerivedRasterBand Integration
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, VRTDerivedRasterBandIntegration) {
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);

  GDALDatasetUniquePtr ds1(
      gtiff_drv->Create(kSrc1Path, 2, 2, 1, GDT_Float32, nullptr));
  GDALDatasetUniquePtr ds2(
      gtiff_drv->Create(kSrc2Path, 2, 2, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);

  float data1[4] = {10.0f, 20.0f, 30.0f, 40.0f};
  float data2[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  ASSERT_EQ(CE_None, ds1->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 2, 2, data1, 2, 2, GDT_Float32, 0, 0));
  ASSERT_EQ(CE_None, ds2->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 2, 2, data2, 2, 2, GDT_Float32, 0, 0));
  ds1.reset();
  ds2.reset();

  // Derived band with "sum"
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
    <PixelFunctionType>sum</PixelFunctionType>
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/pixelfunctions_test/pf_src1.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2"/>
      <DstRect xOff="0" yOff="0" xSize="2" ySize="2"/>
    </SimpleSource>
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/pixelfunctions_test/pf_src2.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2"/>
      <DstRect xOff="0" yOff="0" xSize="2" ySize="2"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds(GDALDataset::FromHandle(
      GDALOpenEx(kVrtXml, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, vrt_ds);

  float out_sum[4] = {};
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out_sum, 2,
                                               2, GDT_Float32, 0, 0));
  EXPECT_FLOAT_EQ(11.0f, out_sum[0]);
  EXPECT_FLOAT_EQ(22.0f, out_sum[1]);
  EXPECT_FLOAT_EQ(33.0f, out_sum[2]);
  EXPECT_FLOAT_EQ(44.0f, out_sum[3]);

  // Derived band with "expression" (without parser, returns CE_Failure)
  constexpr char kVrtExprXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
    <PixelFunctionType>expression</PixelFunctionType>
    <PixelFunctionArguments expression="b1 * 2 + b2" />
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/pixelfunctions_test/pf_src1.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2"/>
      <DstRect xOff="0" yOff="0" xSize="2" ySize="2"/>
    </SimpleSource>
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/pixelfunctions_test/pf_src2.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2"/>
      <DstRect xOff="0" yOff="0" xSize="2" ySize="2"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_expr_ds(GDALDataset::FromHandle(
      GDALOpenEx(kVrtExprXml, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, vrt_expr_ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  float out_expr[4] = {};
  EXPECT_EQ(CE_Failure,
            vrt_expr_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_expr, 2, 2, GDT_Float32, 0, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// ----------------------------------------------------------------------------
// 14. Additional Coverage: Mean 129 Sources & Delta Inf
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Mean129SourcesAndDeltaInf) {
  auto pfn_mean = GetFunc("mean");
  ASSERT_TRUE(static_cast<bool>(pfn_mean));

  // 129 Byte sources with Byte output and no nodata: takes the SSE2 fast path
  // that accumulates in int32 and divides with libdivide.
  constexpr int kNumPixels = 35;
  std::vector<std::vector<uint8_t>> srcs(129,
                                         std::vector<uint8_t>(kNumPixels, 10));
  std::vector<void*> sources_ptr(129);
  for (int i = 0; i < 129; ++i) {
    sources_ptr[i] = srcs[i].data();
  }
  std::vector<uint8_t> b_out(kNumPixels, 0);
  EXPECT_EQ(CE_None, pfn_mean(sources_ptr.data(), 129, b_out.data(), kNumPixels,
                              1, GDT_Byte, GDT_Byte, 1, kNumPixels, nullptr));
  EXPECT_EQ(10, b_out[0]);
  EXPECT_EQ(10, b_out[kNumPixels - 1]);

  // Mean of DBL_MAX and -DBL_MAX: the running-mean delta overflows to
  // infinity, so the fallback that divides each operand separately is used.
  double d0[1] = {std::numeric_limits<double>::max()};
  double d1[1] = {-std::numeric_limits<double>::max()};
  void* sources_delta[] = {d0, d1};
  double d_out[1] = {0.0};
  EXPECT_EQ(CE_None, pfn_mean(sources_delta, 2, d_out, 1, 1, GDT_Float64,
                              GDT_Float64, 8, 8, nullptr));
  EXPECT_DOUBLE_EQ(0.0, d_out[0]);
}

// ----------------------------------------------------------------------------
// 15. Additional Coverage: Sum Optimized Paths
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, SumOptimizedPaths) {
  auto pfn_sum = GetFunc("sum");
  ASSERT_TRUE(static_cast<bool>(pfn_sum));

  constexpr int kNumPixels = 35;
  // GDT_Byte to GDT_Int32: uses OptimizedSumPackedOutput<uint8_t, int32_t>.
  std::vector<uint8_t> b0(kNumPixels, 10), b1(kNumPixels, 20);
  void* srcs_b[] = {b0.data(), b1.data()};
  std::vector<int32_t> i32_out(kNumPixels, 0);
  EXPECT_EQ(CE_None, pfn_sum(srcs_b, 2, i32_out.data(), kNumPixels, 1, GDT_Byte,
                             GDT_Int32, 4, kNumPixels * 4, nullptr));
  EXPECT_EQ(30, i32_out[0]);
  EXPECT_EQ(30, i32_out[kNumPixels - 1]);

  // Float32 output from UInt16, Int16, Int32, and Float64 sources.
  std::vector<uint16_t> u16(kNumPixels, 100);
  void* srcs_u16[] = {u16.data()};
  std::vector<float> f_out(kNumPixels, 0.0f);
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_u16, 1, f_out.data(), kNumPixels, 1, GDT_UInt16,
                    GDT_Float32, 4, kNumPixels * 4, nullptr));
  EXPECT_FLOAT_EQ(100.0f, f_out[0]);

  std::vector<int16_t> i16(kNumPixels, -50);
  void* srcs_i16[] = {i16.data()};
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_i16, 1, f_out.data(), kNumPixels, 1, GDT_Int16,
                    GDT_Float32, 4, kNumPixels * 4, nullptr));
  EXPECT_FLOAT_EQ(-50.0f, f_out[0]);

  std::vector<int32_t> i32(kNumPixels, 500);
  void* srcs_i32[] = {i32.data()};
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_i32, 1, f_out.data(), kNumPixels, 1, GDT_Int32,
                    GDT_Float32, 4, kNumPixels * 4, nullptr));
  EXPECT_FLOAT_EQ(500.0f, f_out[0]);

  std::vector<double> d64(kNumPixels, 12.5);
  void* srcs_d64[] = {d64.data()};
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_d64, 1, f_out.data(), kNumPixels, 1, GDT_Float64,
                    GDT_Float32, 4, kNumPixels * 4, nullptr));
  EXPECT_FLOAT_EQ(12.5f, f_out[0]);

  // OptimizedSumThroughLargerType: Byte -> UInt16
  std::vector<uint16_t> u16_out(kNumPixels, 0);
  EXPECT_EQ(CE_None, pfn_sum(srcs_b, 2, u16_out.data(), kNumPixels, 1, GDT_Byte,
                             GDT_UInt16, 2, kNumPixels * 2, nullptr));
  EXPECT_EQ(30, u16_out[0]);

  // UInt16 -> Int16
  std::vector<int16_t> i16_out(kNumPixels, 0);
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_u16, 1, i16_out.data(), kNumPixels, 1, GDT_UInt16,
                    GDT_Int16, 2, kNumPixels * 2, nullptr));
  EXPECT_EQ(100, i16_out[0]);

  // Int16 -> Int32
  EXPECT_EQ(CE_None, pfn_sum(srcs_i16, 1, i32_out.data(), kNumPixels, 1,
                             GDT_Int16, GDT_Int32, 4, kNumPixels * 4, nullptr));
  EXPECT_EQ(-50, i32_out[0]);

  // Int32 -> Byte
  std::vector<uint8_t> b_out(kNumPixels, 0);
  std::vector<int32_t> i32_small(kNumPixels, 25);
  void* srcs_i32_small[] = {i32_small.data()};
  EXPECT_EQ(CE_None, pfn_sum(srcs_i32_small, 1, b_out.data(), kNumPixels, 1,
                             GDT_Int32, GDT_Byte, 1, kNumPixels, nullptr));
  EXPECT_EQ(25, b_out[0]);

  // Float32 -> Int16
  std::vector<float> f32(kNumPixels, 40.0f);
  void* srcs_f32[] = {f32.data()};
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_f32, 1, i16_out.data(), kNumPixels, 1, GDT_Float32,
                    GDT_Int16, 2, kNumPixels * 2, nullptr));
  EXPECT_EQ(40, i16_out[0]);

  // Float64 -> Int16
  EXPECT_EQ(CE_None,
            pfn_sum(srcs_d64, 1, i16_out.data(), kNumPixels, 1, GDT_Float64,
                    GDT_Int16, 2, kNumPixels * 2, nullptr));
  EXPECT_EQ(13, i16_out[0]);
}

// ----------------------------------------------------------------------------
// 16. Additional Coverage: Min and Max Float64 SSE2
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MinMaxFloat64SSE2) {
  auto pfn_min = GetFunc("min");
  auto pfn_max = GetFunc("max");
  ASSERT_TRUE(static_cast<bool>(pfn_min));
  ASSERT_TRUE(static_cast<bool>(pfn_max));

  constexpr int kNumPixels = 35;
  std::vector<double> d0(kNumPixels), d1(kNumPixels), d_min(kNumPixels),
      d_max(kNumPixels);
  for (int i = 0; i < kNumPixels; ++i) {
    d0[i] = (i % 2 == 0) ? 10.0 : 20.0;
    d1[i] = (i % 2 == 0) ? 20.0 : 10.0;
  }
  void* sources[] = {d0.data(), d1.data()};
  EXPECT_EQ(CE_None,
            pfn_min(sources, 2, d_min.data(), kNumPixels, 1, GDT_Float64,
                    GDT_Float64, 8, kNumPixels * 8, nullptr));
  EXPECT_EQ(CE_None,
            pfn_max(sources, 2, d_max.data(), kNumPixels, 1, GDT_Float64,
                    GDT_Float64, 8, kNumPixels * 8, nullptr));
  for (int i = 0; i < kNumPixels; ++i) {
    EXPECT_DOUBLE_EQ(10.0, d_min[i]);
    EXPECT_DOUBLE_EQ(20.0, d_max[i]);
  }
}

// ----------------------------------------------------------------------------
// 17. Additional Coverage: GetSrcVal on all types
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, GetSrcValAllDataTypes) {
  auto pfn_diff = GetFunc("diff");
  ASSERT_TRUE(static_cast<bool>(pfn_diff));

  // GDT_Byte
  uint8_t b[2] = {10, 5};
  void* src_b[] = {&b[0], &b[1]};
  double out[1] = {};
  EXPECT_EQ(CE_None, pfn_diff(src_b, 2, out, 1, 1, GDT_Byte, GDT_Float64, 8, 8,
                              nullptr));
  EXPECT_DOUBLE_EQ(5.0, out[0]);

  // GDT_UInt16
  uint16_t u16[2] = {100, 20};
  void* src_u16[] = {&u16[0], &u16[1]};
  EXPECT_EQ(CE_None, pfn_diff(src_u16, 2, out, 1, 1, GDT_UInt16, GDT_Float64, 8,
                              8, nullptr));
  EXPECT_DOUBLE_EQ(80.0, out[0]);

  // GDT_Int16
  int16_t i16[2] = {50, 100};
  void* src_i16[] = {&i16[0], &i16[1]};
  EXPECT_EQ(CE_None, pfn_diff(src_i16, 2, out, 1, 1, GDT_Int16, GDT_Float64, 8,
                              8, nullptr));
  EXPECT_DOUBLE_EQ(-50.0, out[0]);

  // GDT_Int32
  int32_t i32[2] = {1000, 400};
  void* src_i32[] = {&i32[0], &i32[1]};
  EXPECT_EQ(CE_None, pfn_diff(src_i32, 2, out, 1, 1, GDT_Int32, GDT_Float64, 8,
                              8, nullptr));
  EXPECT_DOUBLE_EQ(600.0, out[0]);

  // GDT_Float16
  GFloat16 f16[2] = {GFloat16(10.0f), GFloat16(4.0f)};
  void* src_f16[] = {&f16[0], &f16[1]};
  EXPECT_EQ(CE_None, pfn_diff(src_f16, 2, out, 1, 1, GDT_Float16, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(6.0, out[0]);

  // GDT_CInt16
  int16_t ci16_0[2] = {10, 0};
  int16_t ci16_1[2] = {3, 0};
  void* src_ci16[] = {ci16_0, ci16_1};
  EXPECT_EQ(CE_None, pfn_diff(src_ci16, 2, out, 1, 1, GDT_CInt16, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(7.0, out[0]);

  // GDT_CInt32
  int32_t ci32_0[2] = {20, 0};
  int32_t ci32_1[2] = {5, 0};
  void* src_ci32[] = {ci32_0, ci32_1};
  EXPECT_EQ(CE_None, pfn_diff(src_ci32, 2, out, 1, 1, GDT_CInt32, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(15.0, out[0]);

  // GDT_CFloat16
  GFloat16 cf16_0[2] = {GFloat16(30.0f), GFloat16(0.0f)};
  GFloat16 cf16_1[2] = {GFloat16(12.0f), GFloat16(0.0f)};
  void* src_cf16[] = {cf16_0, cf16_1};
  EXPECT_EQ(CE_None, pfn_diff(src_cf16, 2, out, 1, 1, GDT_CFloat16, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(18.0, out[0]);

  // GDT_Unknown
  double unk[2] = {0.0, 0.0};
  void* src_unk[] = {&unk[0], &unk[1]};
  EXPECT_EQ(CE_None, pfn_diff(src_unk, 2, out, 1, 1, GDT_Unknown, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(0.0, out[0]);

  // GDT_TypeCount
  EXPECT_EQ(CE_None, pfn_diff(src_unk, 2, out, 1, 1, GDT_TypeCount, GDT_Float64,
                              8, 8, nullptr));
  EXPECT_DOUBLE_EQ(0.0, out[0]);
}

// ----------------------------------------------------------------------------
// 18. Additional Coverage: Bad Arguments and Edge Cases
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, BadArgumentsAndEdgeCases) {
  WithQuietHandler quiet;
  float val[2] = {1.0f, 2.0f};
  void* sources[] = {val, val};
  float out[2] = {};

  CPLStringList bad_nodata;
  bad_nodata.AddNameValue("NoData", "not_a_number");

  CPLStringList bad_k;
  bad_k.AddNameValue("k", "not_a_number");

  // Sum & Diff with bad NoData
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("sum")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("diff")(sources, 2, out, 2, 1, GDT_Float32,
                                        GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // Mul & Div & Inv with bad k & bad NoData
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("mul")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_k.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("mul")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("div")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("inv")(sources, 1, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_k.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("inv")(sources, 1, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // Sqrt, Log10, dB, Pow with bad args
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("sqrt")(sources, 1, out, 2, 1, GDT_Float32,
                                        GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("log10")(sources, 1, out, 2, 1, GDT_Float32,
                                         GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLStringList bad_fact;
  bad_fact.AddNameValue("fact", "not_a_number");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("dB")(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, bad_fact.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLStringList bad_pow;
  bad_pow.AddNameValue("power", "not_a_number");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("pow")(sources, 1, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_pow.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("pow")(sources, 1, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // InterpolateLinear & InterpolateExp with bad args
  CPLStringList bad_interp;
  bad_interp.AddNameValue("t", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("interpolate_linear")(sources, 2, out, 2, 1,
                                                      GDT_Float32, GDT_Float32,
                                                      4, 8, bad_interp.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  bad_interp.SetNameValue("t", "5.0");
  bad_interp.AddNameValue("t0", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("interpolate_linear")(sources, 2, out, 2, 1,
                                                      GDT_Float32, GDT_Float32,
                                                      4, 8, bad_interp.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  bad_interp.SetNameValue("t0", "0.0");
  bad_interp.AddNameValue("dt", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("interpolate_linear")(sources, 2, out, 2, 1,
                                                      GDT_Float32, GDT_Float32,
                                                      4, 8, bad_interp.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // Upstream quirk in InterpolatePixelFunc: checks dfT == 0 rather than dfDt ==
  // 0 before emitting "dt must be finite and non-zero".
  bad_interp.SetNameValue("dt", "1.0");
  bad_interp.SetNameValue("t", "0.0");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("interpolate_linear")(sources, 2, out, 2, 1,
                                                      GDT_Float32, GDT_Float32,
                                                      4, 8, bad_interp.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("interpolate_exp")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_interp.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // ReplaceNoData with bad args
  CPLStringList bad_replace;
  bad_replace.AddNameValue("to", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("replace_nodata")(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, bad_replace.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  bad_replace.SetNameValue("to", "10.0");
  bad_replace.AddNameValue("NoData", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("replace_nodata")(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, bad_replace.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // Scale with bad args
  CPLStringList bad_scale;
  bad_scale.AddNameValue("scale", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("scale")(sources, 1, out, 2, 1, GDT_Float32,
                                         GDT_Float32, 4, 8, bad_scale.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  bad_scale.SetNameValue("scale", "2.0");
  bad_scale.AddNameValue("NoData", "bad");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("scale")(sources, 1, out, 2, 1, GDT_Float32,
                                         GDT_Float32, 4, 8, bad_scale.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // NormDiff with complex or bad NoData
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("norm_diff")(sources, 2, out, 2, 1, GDT_Float32,
                                 GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("norm_diff")(sources, 2, out, 2, 1, GDT_CFloat32,
                                 GDT_Float32, 4, 8, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // BasicPixelFunc (mean, harmonic_mean, min) with complex or bad NoData
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("mean")(sources, 2, out, 2, 1, GDT_Float32,
                                        GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("mean")(sources, 2, out, 2, 1, GDT_CFloat32,
                                        GDT_Float32, 4, 8, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GetFunc("harmonic_mean")(sources, 2, out, 2, 1, GDT_CFloat32,
                                     GDT_Float32, 4, 8, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("min")(sources, 2, out, 2, 1, GDT_CFloat32,
                                       GDT_Float32, 4, 8, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GetFunc("min")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, bad_nodata.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining template specialization edge cases in
// InterpolateLinear/InterpolateLog10 and complex pixel functions in
// pixelfunctions.cpp.
