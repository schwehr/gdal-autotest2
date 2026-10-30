// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class PixelFunctionsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    GDALRegisterDefaultPixelFunc();
  }

  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void TearDown() override {
    for (const auto& filename : to_unlink_) {
      VSIUnlink(filename.c_str());
    }
    to_unlink_.clear();
  }

  void TrackFile(const std::string& filename) {
    to_unlink_.push_back(filename);
  }

 private:
  std::vector<std::string> to_unlink_;
};

VRTDerivedRasterBand::PixelFunc GetFunc(const char* name) {
  const auto* pair = VRTDerivedRasterBand::GetPixelFunction(name);
  if (pair == nullptr) return nullptr;
  return pair->first;
}

// ----------------------------------------------------------------------------
// 1. Registration and Lookup
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, RegistrationAndLookup) {
  EXPECT_EQ(GDALRegisterDefaultPixelFunc(), CE_None);

  const char* expected_names[] = {"real",
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

  for (const char* name : expected_names) {
    const auto* pair = VRTDerivedRasterBand::GetPixelFunction(name);
    ASSERT_NE(pair, nullptr) << "Failed to find pixel function: " << name;
    EXPECT_TRUE(static_cast<bool>(pair->first));
  }

  EXPECT_EQ(VRTDerivedRasterBand::GetPixelFunction("non_existent_func"),
            nullptr);

  auto names = VRTDerivedRasterBand::GetPixelFunctionNames();
  EXPECT_GE(names.size(), sizeof(expected_names) / sizeof(expected_names[0]));
}

// ----------------------------------------------------------------------------
// 2. Real and Imag
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, RealAndImag) {
  auto pfnReal = GetFunc("real");
  auto pfnImag = GetFunc("imag");
  ASSERT_TRUE(static_cast<bool>(pfnReal));
  ASSERT_TRUE(static_cast<bool>(pfnImag));

  // Failure: nSources != 1
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(
        pfnReal(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    EXPECT_EQ(
        pfnReal(nullptr, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    EXPECT_EQ(
        pfnImag(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
  }

  // Complex to real: CFloat64, CFloat32, CInt32, CInt16
  {
    std::complex<double> c64[2] = {{10.5, -20.5}, {30.25, 40.75}};
    void* sources[] = {c64};
    double r64[2] = {0, 0};
    double i64[2] = {0, 0};
    EXPECT_EQ(pfnReal(sources, 1, r64, 2, 1, GDT_CFloat64, GDT_Float64, 8, 16,
                      nullptr),
              CE_None);
    EXPECT_EQ(pfnImag(sources, 1, i64, 2, 1, GDT_CFloat64, GDT_Float64, 8, 16,
                      nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(r64[0], 10.5);
    EXPECT_DOUBLE_EQ(r64[1], 30.25);
    EXPECT_DOUBLE_EQ(i64[0], -20.5);
    EXPECT_DOUBLE_EQ(i64[1], 40.75);

    std::complex<float> c32[2] = {{1.5f, 2.5f}, {-3.5f, -4.5f}};
    sources[0] = c32;
    float r32[2] = {0, 0};
    float i32[2] = {0, 0};
    EXPECT_EQ(pfnReal(sources, 1, r32, 2, 1, GDT_CFloat32, GDT_Float32, 4, 8,
                      nullptr),
              CE_None);
    EXPECT_EQ(pfnImag(sources, 1, i32, 2, 1, GDT_CFloat32, GDT_Float32, 4, 8,
                      nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(r32[0], 1.5f);
    EXPECT_FLOAT_EQ(i32[0], 2.5f);

    int16_t ci16[4] = {100, 200, -300, -400};
    sources[0] = ci16;
    int16_t ri16[2] = {0, 0};
    int16_t ii16[2] = {0, 0};
    EXPECT_EQ(
        pfnReal(sources, 1, ri16, 2, 1, GDT_CInt16, GDT_Int16, 2, 4, nullptr),
        CE_None);
    EXPECT_EQ(
        pfnImag(sources, 1, ii16, 2, 1, GDT_CInt16, GDT_Int16, 2, 4, nullptr),
        CE_None);
    EXPECT_EQ(ri16[0], 100);
    EXPECT_EQ(ii16[0], 200);

    int32_t ci32[4] = {1000, 2000, -3000, -4000};
    sources[0] = ci32;
    int32_t ri32[2] = {0, 0};
    int32_t ii32[2] = {0, 0};
    EXPECT_EQ(
        pfnReal(sources, 1, ri32, 2, 1, GDT_CInt32, GDT_Int32, 4, 8, nullptr),
        CE_None);
    EXPECT_EQ(
        pfnImag(sources, 1, ii32, 2, 1, GDT_CInt32, GDT_Int32, 4, 8, nullptr),
        CE_None);
    EXPECT_EQ(ri32[0], 1000);
    EXPECT_EQ(ii32[0], 2000);
  }

  // Non-complex sources
  {
    float f32[2] = {42.0f, 99.0f};
    void* sources[] = {f32};
    float rOut[2] = {0, 0};
    float iOut[2] = {999.0f, 999.0f};
    EXPECT_EQ(pfnReal(sources, 1, rOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      nullptr),
              CE_None);
    EXPECT_EQ(pfnImag(sources, 1, iOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(rOut[0], 42.0f);
    EXPECT_FLOAT_EQ(iOut[0], 0.0f);
    EXPECT_FLOAT_EQ(iOut[1], 0.0f);
  }
}

// ----------------------------------------------------------------------------
// 3. Complex and Polar
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, ComplexAndPolar) {
  auto pfnComplex = GetFunc("complex");
  auto pfnPolar = GetFunc("polar");
  ASSERT_TRUE(static_cast<bool>(pfnComplex));
  ASSERT_TRUE(static_cast<bool>(pfnPolar));

  // ComplexPixelFunc failures
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    std::complex<float> out[2];
    EXPECT_EQ(pfnComplex(nullptr, 1, out, 2, 1, GDT_Float32, GDT_CFloat32, 8,
                         16, nullptr),
              CE_Failure);
    EXPECT_EQ(pfnComplex(nullptr, 3, out, 2, 1, GDT_Float32, GDT_CFloat32, 8,
                         16, nullptr),
              CE_Failure);
  }

  // ComplexPixelFunc 2 sources
  {
    float realPart[2] = {1.5f, 3.5f};
    float imagPart[2] = {2.5f, 4.5f};
    void* sources[] = {realPart, imagPart};
    std::complex<float> cOut[2];
    EXPECT_EQ(pfnComplex(sources, 2, cOut, 2, 1, GDT_Float32, GDT_CFloat32, 8,
                         16, nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(cOut[0].real(), 1.5f);
    EXPECT_FLOAT_EQ(cOut[0].imag(), 2.5f);
  }

  // PolarPixelFunc failures
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    std::complex<double> out[2];
    EXPECT_EQ(pfnPolar(nullptr, 1, out, 2, 1, GDT_Float64, GDT_CFloat64, 16, 32,
                       nullptr),
              CE_Failure);
  }

  // PolarPixelFunc AMPLITUDE (default), INTENSITY, dB
  {
    double amp[4] = {2.0, 4.0, 0.0, -1.0};
    double phase[4] = {0.0, M_PI / 2.0, 0.0, 0.0};
    void* sources[] = {amp, phase};
    std::complex<double> cOut[4];

    // Default AMPLITUDE
    EXPECT_EQ(pfnPolar(sources, 2, cOut, 4, 1, GDT_Float64, GDT_CFloat64, 16,
                       64, nullptr),
              CE_None);
    EXPECT_NEAR(cOut[0].real(), 2.0, 1e-6);
    EXPECT_NEAR(cOut[0].imag(), 0.0, 1e-6);
    EXPECT_NEAR(cOut[1].real(), 0.0, 1e-6);
    EXPECT_NEAR(cOut[1].imag(), 4.0, 1e-6);

    // INTENSITY: sqrt(max(0, amp))
    CPLStringList aosArgs;
    aosArgs.AddNameValue("amplitude_type", "INTENSITY");
    EXPECT_EQ(pfnPolar(sources, 2, cOut, 4, 1, GDT_Float64, GDT_CFloat64, 16,
                       64, aosArgs.List()),
              CE_None);
    EXPECT_NEAR(cOut[1].imag(), 2.0, 1e-6);  // sqrt(4.0) = 2.0
    EXPECT_NEAR(cOut[3].real(), 0.0, 1e-6);  // -1 clamped to 0

    // dB: pow(10, amp/20) or -inf
    aosArgs.Clear();
    aosArgs.AddNameValue("amplitude_type", "dB");
    EXPECT_EQ(pfnPolar(sources, 2, cOut, 4, 1, GDT_Float64, GDT_CFloat64, 16,
                       64, aosArgs.List()),
              CE_None);
    EXPECT_NEAR(cOut[0].real(), std::pow(10.0, 2.0 / 20.0), 1e-6);

    // Invalid amplitude_type
    aosArgs.Clear();
    aosArgs.AddNameValue("amplitude_type", "INVALID");
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(pfnPolar(sources, 2, cOut, 4, 1, GDT_Float64, GDT_CFloat64, 16,
                       64, aosArgs.List()),
              CE_Failure);
  }
}

// ----------------------------------------------------------------------------
// 4. Module, Phase, Conj
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, ModulePhaseConj) {
  auto pfnMod = GetFunc("mod");
  auto pfnPhase = GetFunc("phase");
  auto pfnConj = GetFunc("conj");
  ASSERT_TRUE(static_cast<bool>(pfnMod));
  ASSERT_TRUE(static_cast<bool>(pfnPhase));
  ASSERT_TRUE(static_cast<bool>(pfnConj));

  // Module
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    double out[2];
    EXPECT_EQ(
        pfnMod(nullptr, 0, out, 2, 1, GDT_Float64, GDT_Float64, 8, 16, nullptr),
        CE_Failure);

    // Complex source
    std::complex<double> c64[2] = {{3.0, 4.0}, {-5.0, 12.0}};
    void* sources[] = {c64};
    EXPECT_EQ(pfnMod(sources, 1, out, 2, 1, GDT_CFloat64, GDT_Float64, 8, 16,
                     nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(out[0], 5.0);
    EXPECT_DOUBLE_EQ(out[1], 13.0);

    // Real source (fabs)
    double r64[2] = {-42.5, 10.0};
    sources[0] = r64;
    EXPECT_EQ(
        pfnMod(sources, 1, out, 2, 1, GDT_Float64, GDT_Float64, 8, 16, nullptr),
        CE_None);
    EXPECT_DOUBLE_EQ(out[0], 42.5);
    EXPECT_DOUBLE_EQ(out[1], 10.0);
  }

  // Phase
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    double out[2];
    EXPECT_EQ(pfnPhase(nullptr, 2, out, 2, 1, GDT_Float64, GDT_Float64, 8, 16,
                       nullptr),
              CE_Failure);

    // Complex source
    std::complex<double> c64[2] = {{1.0, 1.0}, {-1.0, 0.0}};
    void* sources[] = {c64};
    EXPECT_EQ(pfnPhase(sources, 1, out, 2, 1, GDT_CFloat64, GDT_Float64, 8, 16,
                       nullptr),
              CE_None);
    EXPECT_NEAR(out[0], M_PI / 4.0, 1e-6);
    EXPECT_NEAR(out[1], M_PI, 1e-6);

    // Unsigned integer: returns 0
    uint16_t u16[2] = {10, 20};
    sources[0] = u16;
    EXPECT_EQ(pfnPhase(sources, 1, out, 2, 1, GDT_UInt16, GDT_Float64, 8, 16,
                       nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(out[0], 0.0);

    // Signed real: pi if < 0, 0 if >= 0
    double r64[2] = {-5.0, 10.0};
    sources[0] = r64;
    EXPECT_EQ(pfnPhase(sources, 1, out, 2, 1, GDT_Float64, GDT_Float64, 8, 16,
                       nullptr),
              CE_None);
    EXPECT_NEAR(out[0], M_PI, 1e-6);
    EXPECT_DOUBLE_EQ(out[1], 0.0);
  }

  // Conj
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    std::complex<double> cOut[2];
    EXPECT_EQ(pfnConj(nullptr, 0, cOut, 2, 1, GDT_CFloat64, GDT_CFloat64, 16,
                      32, nullptr),
              CE_Failure);

    // Complex to complex
    std::complex<double> c64[2] = {{3.0, 4.0}, {5.0, -12.0}};
    void* sources[] = {c64};
    EXPECT_EQ(pfnConj(sources, 1, cOut, 2, 1, GDT_CFloat64, GDT_CFloat64, 16,
                      32, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(cOut[0].real(), 3.0);
    EXPECT_DOUBLE_EQ(cOut[0].imag(), -4.0);
    EXPECT_DOUBLE_EQ(cOut[1].real(), 5.0);
    EXPECT_DOUBLE_EQ(cOut[1].imag(), 12.0);

    // Real to real (fallback to real)
    double rIn[2] = {10.0, 20.0};
    double rOut[2] = {0, 0};
    sources[0] = rIn;
    EXPECT_EQ(pfnConj(sources, 1, rOut, 2, 1, GDT_Float64, GDT_Float64, 8, 16,
                      nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(rOut[0], 10.0);
  }
}

// ----------------------------------------------------------------------------
// 5. Sum and Diff
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, SumAndDiff) {
  auto pfnSum = GetFunc("sum");
  auto pfnDiff = GetFunc("diff");
  ASSERT_TRUE(static_cast<bool>(pfnSum));
  ASSERT_TRUE(static_cast<bool>(pfnDiff));

  // Failures
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(
        pfnSum(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    CPLStringList aosBadK;
    aosBadK.AddNameValue("k", "invalid_number");
    void* dummySources[] = {out};
    EXPECT_EQ(pfnSum(dummySources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                     aosBadK.List()),
              CE_Failure);

    EXPECT_EQ(
        pfnDiff(nullptr, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    EXPECT_EQ(
        pfnDiff(nullptr, 3, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
  }

  constexpr int N =
      35;  // Test vectorization (16/32 elements) + remainder (3 elements)

  // Sum with complex types
  {
    std::vector<std::complex<float>> cSrc0(N, {1.0f, 2.0f});
    std::vector<std::complex<float>> cSrc1(N, {3.0f, 4.0f});
    std::vector<std::complex<float>> cOut(N);
    void* sources[] = {cSrc0.data(), cSrc1.data()};
    EXPECT_EQ(pfnSum(sources, 2, cOut.data(), N, 1, GDT_CFloat32, GDT_CFloat32,
                     sizeof(std::complex<float>),
                     N * sizeof(std::complex<float>), nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(cOut[0].real(), 4.0f);
    EXPECT_FLOAT_EQ(cOut[0].imag(), 6.0f);
  }

  // Sum same types (SSE2 paths): GDT_Byte, GDT_Int8, GDT_UInt16, GDT_Int16,
  // GDT_UInt32, GDT_Int32, GDT_Float32, GDT_Float64
  {
    // Byte
    std::vector<uint8_t> b0(N, 10), b1(N, 20), bOut(N);
    void* sourcesB[] = {b0.data(), b1.data()};
    EXPECT_EQ(pfnSum(sourcesB, 2, bOut.data(), N, 1, GDT_Byte, GDT_Byte, 1, N,
                     nullptr),
              CE_None);
    EXPECT_EQ(bOut[0], 30);
    EXPECT_EQ(bOut[N - 1], 30);

    // Int8
    std::vector<int8_t> s8_0(N, 5), s8_1(N, -2), s8Out(N);
    void* sourcesS8[] = {s8_0.data(), s8_1.data()};
    EXPECT_EQ(pfnSum(sourcesS8, 2, s8Out.data(), N, 1, GDT_Int8, GDT_Int8, 1, N,
                     nullptr),
              CE_None);
    EXPECT_EQ(s8Out[0], 3);

    // UInt16
    std::vector<uint16_t> u16_0(N, 100), u16_1(N, 200), u16Out(N);
    void* sourcesU16[] = {u16_0.data(), u16_1.data()};
    EXPECT_EQ(pfnSum(sourcesU16, 2, u16Out.data(), N, 1, GDT_UInt16, GDT_UInt16,
                     2, N * 2, nullptr),
              CE_None);
    EXPECT_EQ(u16Out[0], 300);

    // Int16
    std::vector<int16_t> i16_0(N, -100), i16_1(N, 300), i16Out(N);
    void* sourcesI16[] = {i16_0.data(), i16_1.data()};
    EXPECT_EQ(pfnSum(sourcesI16, 2, i16Out.data(), N, 1, GDT_Int16, GDT_Int16,
                     2, N * 2, nullptr),
              CE_None);
    EXPECT_EQ(i16Out[0], 200);

    // UInt32
    std::vector<uint32_t> u32_0(N, 1000), u32_1(N, 2000), u32Out(N);
    void* sourcesU32[] = {u32_0.data(), u32_1.data()};
    EXPECT_EQ(pfnSum(sourcesU32, 2, u32Out.data(), N, 1, GDT_UInt32, GDT_UInt32,
                     4, N * 4, nullptr),
              CE_None);
    EXPECT_EQ(u32Out[0], 3000);

    // Int32
    std::vector<int32_t> i32_0(N, -1000), i32_1(N, 5000), i32Out(N);
    void* sourcesI32[] = {i32_0.data(), i32_1.data()};
    EXPECT_EQ(pfnSum(sourcesI32, 2, i32Out.data(), N, 1, GDT_Int32, GDT_Int32,
                     4, N * 4, nullptr),
              CE_None);
    EXPECT_EQ(i32Out[0], 4000);

    // Float32
    std::vector<float> f32_0(N, 1.25f), f32_1(N, 2.75f), f32Out(N);
    void* sourcesF32[] = {f32_0.data(), f32_1.data()};
    EXPECT_EQ(pfnSum(sourcesF32, 2, f32Out.data(), N, 1, GDT_Float32,
                     GDT_Float32, 4, N * 4, nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(f32Out[0], 4.0f);

    // Float64
    std::vector<double> f64_0(N, 1.125), f64_1(N, 2.875), f64Out(N);
    void* sourcesF64[] = {f64_0.data(), f64_1.data()};
    EXPECT_EQ(pfnSum(sourcesF64, 2, f64Out.data(), N, 1, GDT_Float64,
                     GDT_Float64, 8, N * 8, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(f64Out[0], 4.0);

    // UInt64 & Int64
    std::vector<uint64_t> u64_0(N, 100), u64_1(N, 200), u64Out(N);
    void* sourcesU64[] = {u64_0.data(), u64_1.data()};
    EXPECT_EQ(pfnSum(sourcesU64, 2, u64Out.data(), N, 1, GDT_UInt64, GDT_UInt64,
                     8, N * 8, nullptr),
              CE_None);
    EXPECT_EQ(u64Out[0], 300);

    std::vector<int64_t> i64_0(N, -50), i64_1(N, 150), i64Out(N);
    void* sourcesI64[] = {i64_0.data(), i64_1.data()};
    EXPECT_EQ(pfnSum(sourcesI64, 2, i64Out.data(), N, 1, GDT_Int64, GDT_Int64,
                     8, N * 8, nullptr),
              CE_None);
    EXPECT_EQ(i64Out[0], 100);
  }

  // Sum with k, NoData, propagateNoData, and mixed types
  {
    CPLStringList aosArgs;
    aosArgs.AddNameValue("k", "10.0");
    aosArgs.AddNameValue("NoData", "99");
    aosArgs.AddNameValue("propagateNoData", "true");

    float f0[4] = {1.0f, 99.0f, 3.0f, 4.0f};
    float f1[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    void* sources[] = {f0, f1};
    float fOut[4] = {0};
    EXPECT_EQ(pfnSum(sources, 2, fOut, 4, 1, GDT_Float32, GDT_Float32, 4, 16,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 16.0f);  // 1 + 5 + 10
    EXPECT_FLOAT_EQ(fOut[1], 99.0f);  // NoData propagated

    // propagateNoData = false (skip NoData)
    aosArgs.SetNameValue("propagateNoData", "false");
    EXPECT_EQ(pfnSum(sources, 2, fOut, 4, 1, GDT_Float32, GDT_Float32, 4, 16,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[1], 16.0f);  // 6 + 10 (99 was skipped)

    // Mixed types (source Byte to dest Float64)
    uint8_t u8[2] = {10, 20};
    void* srcMixed[] = {u8};
    double dOut[2] = {0};
    EXPECT_EQ(
        pfnSum(srcMixed, 1, dOut, 2, 1, GDT_Byte, GDT_Float64, 8, 16, nullptr),
        CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 10.0);
  }

  // Diff
  {
    // Real
    float f0[2] = {10.0f, 25.0f};
    float f1[2] = {3.0f, 30.0f};
    void* sources[] = {f0, f1};
    float fOut[2];
    EXPECT_EQ(pfnDiff(sources, 2, fOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 7.0f);
    EXPECT_FLOAT_EQ(fOut[1], -5.0f);

    // Complex
    std::complex<double> c0[2] = {{10.0, 20.0}, {5.0, 6.0}};
    std::complex<double> c1[2] = {{3.0, 5.0}, {1.0, 2.0}};
    void* cSources[] = {c0, c1};
    std::complex<double> cOut[2];
    EXPECT_EQ(pfnDiff(cSources, 2, cOut, 2, 1, GDT_CFloat64, GDT_CFloat64, 16,
                      32, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(cOut[0].real(), 7.0);
    EXPECT_DOUBLE_EQ(cOut[0].imag(), 15.0);

    // Diff with NoData
    CPLStringList aosArgs;
    aosArgs.AddNameValue("NoData", "-999");
    f0[1] = -999.0f;
    EXPECT_EQ(pfnDiff(sources, 2, fOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[1], -999.0f);
  }
}

// ----------------------------------------------------------------------------
// 6. Mul, Div, CMul
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MulDivCMul) {
  auto pfnMul = GetFunc("mul");
  auto pfnDiv = GetFunc("div");
  auto pfnCMul = GetFunc("cmul");
  ASSERT_TRUE(static_cast<bool>(pfnMul));
  ASSERT_TRUE(static_cast<bool>(pfnDiv));
  ASSERT_TRUE(static_cast<bool>(pfnCMul));

  // Failures
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(
        pfnMul(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    EXPECT_EQ(
        pfnDiv(nullptr, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);
    EXPECT_EQ(pfnCMul(nullptr, 1, out, 2, 1, GDT_CFloat32, GDT_CFloat32, 8, 16,
                      nullptr),
              CE_Failure);
  }

  // Mul
  {
    float f0[2] = {2.0f, 5.0f};
    float f1[2] = {3.0f, 4.0f};
    void* sources[] = {f0, f1};
    float fOut[2];

    CPLStringList aosArgs;
    aosArgs.AddNameValue("k", "2.0");
    EXPECT_EQ(pfnMul(sources, 2, fOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 12.0f);  // 2 * 3 * 2

    // Complex mul
    std::complex<double> c0[1] = {{1.0, 2.0}};
    std::complex<double> c1[1] = {{3.0, 4.0}};
    void* cSources[] = {c0, c1};
    std::complex<double> cOut[1];
    EXPECT_EQ(pfnMul(cSources, 2, cOut, 1, 1, GDT_CFloat64, GDT_CFloat64, 16,
                     16, nullptr),
              CE_None);
    // (1+2i)*(3+4i) = 3 - 8 + i(4 + 6) = -5 + 10i
    EXPECT_DOUBLE_EQ(cOut[0].real(), -5.0);
    EXPECT_DOUBLE_EQ(cOut[0].imag(), 10.0);
  }

  // Div
  {
    float f0[2] = {10.0f, 5.0f};
    float f1[2] = {2.0f, 0.0f};  // Div by 0
    void* sources[] = {f0, f1};
    float fOut[2];
    EXPECT_EQ(
        pfnDiv(sources, 2, fOut, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 5.0f);
    EXPECT_TRUE(std::isinf(fOut[1]));

    // Complex div
    std::complex<double> c0[2] = {{-5.0, 10.0}, {1.0, 2.0}};
    std::complex<double> c1[2] = {{1.0, 2.0}, {0.0, 0.0}};  // Div by 0
    void* cSources[] = {c0, c1};
    std::complex<double> cOut[2];
    EXPECT_EQ(pfnDiv(cSources, 2, cOut, 2, 1, GDT_CFloat64, GDT_CFloat64, 16,
                     32, nullptr),
              CE_None);
    EXPECT_NEAR(cOut[0].real(), 3.0, 1e-6);
    EXPECT_NEAR(cOut[0].imag(), 4.0, 1e-6);
    EXPECT_TRUE(std::isinf(cOut[1].real()));
  }

  // CMul
  {
    std::complex<float> c0[1] = {{2.0f, 3.0f}};
    std::complex<float> c1[1] = {{4.0f, -5.0f}};
    void* sources[] = {c0, c1};
    std::complex<float> cOut[1];
    EXPECT_EQ(pfnCMul(sources, 2, cOut, 1, 1, GDT_CFloat32, GDT_CFloat32, 8, 8,
                      nullptr),
              CE_None);
    // (2+3i) * conj(4-5i) = (2*4 + 3*-5) + i(4*3 - 2*-5) = -7 + 22i
    EXPECT_FLOAT_EQ(cOut[0].real(), -7.0f);
    EXPECT_FLOAT_EQ(cOut[0].imag(), 22.0f);

    // Non-complex inputs
    float r0[1] = {3.0f};
    float r1[1] = {4.0f};
    void* rSources[] = {r0, r1};
    std::complex<double> crOut[1];
    EXPECT_EQ(pfnCMul(rSources, 2, crOut, 1, 1, GDT_Float32, GDT_CFloat64, 16,
                      16, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(crOut[0].real(), 12.0);
    EXPECT_DOUBLE_EQ(crOut[0].imag(), 0.0);
  }
}

// ----------------------------------------------------------------------------
// 7. Inv, Intensity, Sqrt, Log10, DB, Exp, Pow
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, InvIntensitySqrtLog10DBExpPow) {
  auto pfnInv = GetFunc("inv");
  auto pfnIntensity = GetFunc("intensity");
  auto pfnSqrt = GetFunc("sqrt");
  auto pfnLog10 = GetFunc("log10");
  auto pfnDB = GetFunc("dB");
  auto pfnExp = GetFunc("exp");
  auto pfndB2amp = GetFunc("dB2amp");
  auto pfndB2pow = GetFunc("dB2pow");
  auto pfnPow = GetFunc("pow");

  // Inv
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(
        pfnInv(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);

    float fIn[2] = {4.0f, 0.0f};
    void* sources[] = {fIn};
    CPLStringList aosArgs;
    aosArgs.AddNameValue("k", "2.0");
    EXPECT_EQ(pfnInv(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 0.5f);  // 2 / 4
    EXPECT_TRUE(std::isinf(out[1]));

    std::complex<double> cIn[1] = {{1.0, 1.0}};
    void* cSources[] = {cIn};
    std::complex<double> cOut[1];
    EXPECT_EQ(pfnInv(cSources, 1, cOut, 1, 1, GDT_CFloat64, GDT_CFloat64, 16,
                     16, nullptr),
              CE_None);
    EXPECT_NEAR(cOut[0].real(), 0.5, 1e-6);
    EXPECT_NEAR(cOut[0].imag(), -0.5, 1e-6);
  }

  // Intensity
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    double out[1];
    EXPECT_EQ(pfnIntensity(nullptr, 0, out, 1, 1, GDT_CFloat64, GDT_Float64, 8,
                           8, nullptr),
              CE_Failure);

    std::complex<double> cIn[1] = {{3.0, 4.0}};
    void* sources[] = {cIn};
    EXPECT_EQ(pfnIntensity(sources, 1, out, 1, 1, GDT_CFloat64, GDT_Float64, 8,
                           8, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(out[0], 25.0);

    // Real input computes x * x
    double rIn[1] = {5.0};
    sources[0] = rIn;
    EXPECT_EQ(pfnIntensity(sources, 1, out, 1, 1, GDT_Float64, GDT_Float64, 8,
                           8, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(out[0], 25.0);
  }

  // Sqrt
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(
        pfnSqrt(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);

    float fIn[2] = {9.0f, -4.0f};
    void* sources[] = {fIn};
    EXPECT_EQ(
        pfnSqrt(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_None);
    EXPECT_FLOAT_EQ(out[0], 3.0f);
    EXPECT_TRUE(std::isnan(out[1]));
  }

  // Log10 & dB
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(pfnLog10(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                       nullptr),
              CE_Failure);

    float fIn[2] = {100.0f, 0.0f};
    void* sources[] = {fIn};
    EXPECT_EQ(pfnLog10(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                       nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 2.0f);
    EXPECT_TRUE(std::isinf(out[1]));

    // Complex Log10
    std::complex<double> cIn[1] = {{10.0, 0.0}};
    void* cSources[] = {cIn};
    double dOut[1];
    EXPECT_EQ(pfnLog10(cSources, 1, dOut, 1, 1, GDT_CFloat64, GDT_Float64, 8, 8,
                       nullptr),
              CE_None);
    EXPECT_NEAR(dOut[0], 1.0, 1e-6);

    // dB
    CPLStringList aosArgs;
    aosArgs.AddNameValue("fact", "10.0");
    EXPECT_EQ(pfnDB(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                    aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 20.0f);  // 10 * log10(100) = 20
  }

  // Exp, dB2amp, dB2pow
  {
    float fIn[2] = {2.0f, 0.0f};
    void* sources[] = {fIn};
    float out[2];

    CPLStringList aosArgs;
    aosArgs.AddNameValue("base", "2.0");
    aosArgs.AddNameValue("fact", "3.0");
    EXPECT_EQ(pfnExp(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 64.0f);  // 2^(2 * 3) = 64

    float dbIn[1] = {20.0f};
    sources[0] = dbIn;
    EXPECT_EQ(pfndB2amp(sources, 1, out, 1, 1, GDT_Float32, GDT_Float32, 4, 4,
                        nullptr),
              CE_None);
    EXPECT_NEAR(out[0], 10.0f, 1e-4);

    EXPECT_EQ(pfndB2pow(sources, 1, out, 1, 1, GDT_Float32, GDT_Float32, 4, 4,
                        nullptr),
              CE_None);
    EXPECT_NEAR(out[0], 100.0f, 1e-4);
  }

  // Pow
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[1];
    EXPECT_EQ(
        pfnPow(nullptr, 0, out, 1, 1, GDT_Float32, GDT_Float32, 4, 4, nullptr),
        CE_Failure);
    // Missing power arg
    float fIn[1] = {3.0f};
    void* sources[] = {fIn};
    EXPECT_EQ(
        pfnPow(sources, 1, out, 1, 1, GDT_Float32, GDT_Float32, 4, 4, nullptr),
        CE_Failure);

    CPLStringList aosArgs;
    aosArgs.AddNameValue("power", "3.0");
    EXPECT_EQ(pfnPow(sources, 1, out, 1, 1, GDT_Float32, GDT_Float32, 4, 4,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 27.0f);
  }
}

// ----------------------------------------------------------------------------
// 8. Interpolate, ReplaceNoData, Scale, NormDiff
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, InterpolateReplaceNoDataScaleNormDiff) {
  auto pfnInterpLin = GetFunc("interpolate_linear");
  auto pfnInterpExp = GetFunc("interpolate_exp");
  auto pfnReplaceND = GetFunc("replace_nodata");
  auto pfnScale = GetFunc("scale");
  auto pfnNormDiff = GetFunc("norm_diff");

  // Interpolate
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[1];
    EXPECT_EQ(pfnInterpLin(nullptr, 1, out, 1, 1, GDT_Float32, GDT_Float32, 4,
                           4, nullptr),
              CE_Failure);

    CPLStringList aosArgs;
    aosArgs.AddNameValue("t0", "0.0");
    aosArgs.AddNameValue("dt", "10.0");
    aosArgs.AddNameValue("t", "5.0");

    float src0[1] = {10.0f};
    float src1[1] = {20.0f};
    void* sources[] = {src0, src1};
    EXPECT_EQ(pfnInterpLin(sources, 2, out, 1, 1, GDT_Float32, GDT_Float32, 4,
                           4, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 15.0f);

    EXPECT_EQ(pfnInterpExp(sources, 2, out, 1, 1, GDT_Float32, GDT_Float32, 4,
                           4, aosArgs.List()),
              CE_None);
    EXPECT_GT(out[0], 0.0f);

    // Boundary t == t0 (using non-zero t0 and t)
    aosArgs.SetNameValue("t0", "5.0");
    aosArgs.SetNameValue("dt", "10.0");
    aosArgs.SetNameValue("t", "5.0");
    EXPECT_EQ(pfnInterpLin(sources, 2, out, 1, 1, GDT_Float32, GDT_Float32, 4,
                           4, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 10.0f);

    // Boundary t == t1
    aosArgs.SetNameValue("t", "15.0");
    EXPECT_EQ(pfnInterpLin(sources, 2, out, 1, 1, GDT_Float32, GDT_Float32, 4,
                           4, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 20.0f);
  }

  // ReplaceNoData
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(pfnReplaceND(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, nullptr),
              CE_Failure);

    float fIn[2] = {0.0f, 42.0f};
    void* sources[] = {fIn};
    // Missing NoData
    EXPECT_EQ(pfnReplaceND(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, nullptr),
              CE_Failure);

    CPLStringList aosArgs;
    aosArgs.AddNameValue("NoData", "0.0");
    aosArgs.AddNameValue("to", "99.0");
    EXPECT_EQ(pfnReplaceND(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 99.0f);
    EXPECT_FLOAT_EQ(out[1], 42.0f);

    // Using nan on non-floating output fails
    uint8_t bOut[2];
    aosArgs.SetNameValue("to", "nan");
    EXPECT_EQ(pfnReplaceND(sources, 1, bOut, 2, 1, GDT_Float32, GDT_Byte, 1, 2,
                           aosArgs.List()),
              CE_Failure);
  }

  // Scale
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(pfnScale(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                       nullptr),
              CE_Failure);

    float fIn[2] = {10.0f, -999.0f};
    void* sources[] = {fIn};

    CPLStringList aosArgs;
    aosArgs.AddNameValue("scale", "2.0");
    aosArgs.AddNameValue("offset", "5.0");
    aosArgs.AddNameValue("NoData", "-999.0");
    EXPECT_EQ(pfnScale(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                       aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 25.0f);  // 10 * 2 + 5
    EXPECT_FLOAT_EQ(out[1], -999.0f);
  }

  // NormDiff
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    EXPECT_EQ(pfnNormDiff(nullptr, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                          nullptr),
              CE_Failure);

    float f0[2] = {5.0f, 0.0f};
    float f1[2] = {3.0f, 0.0f};  // denom 0
    void* sources[] = {f0, f1};
    EXPECT_EQ(pfnNormDiff(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                          nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 0.25f);  // (5-3)/(5+3) = 2/8 = 0.25
    EXPECT_TRUE(std::isinf(out[1]));
  }
}

// ----------------------------------------------------------------------------
// 9. Min, Max, ArgMin, ArgMax
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MinMaxArgMinArgMax) {
  auto pfnMin = GetFunc("min");
  auto pfnArgMin = GetFunc("argmin");
  auto pfnMax = GetFunc("max");
  auto pfnArgMax = GetFunc("argmax");

  constexpr int N = 35;  // Test SSE2 + scalar loop

  // Min with Byte, Int16, UInt16, Float32
  {
    std::vector<uint8_t> b0(N, 10), b1(N, 5), b2(N, 20), bOut(N);
    void* sources[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(pfnMin(sources, 3, bOut.data(), N, 1, GDT_Byte, GDT_Byte, 1, N,
                     nullptr),
              CE_None);
    EXPECT_EQ(bOut[0], 5);
    EXPECT_EQ(bOut[N - 1], 5);

    std::vector<int16_t> i0(N, -10), i1(N, -50), iOut(N);
    void* sourcesI[] = {i0.data(), i1.data()};
    EXPECT_EQ(pfnMin(sourcesI, 2, iOut.data(), N, 1, GDT_Int16, GDT_Int16, 2,
                     N * 2, nullptr),
              CE_None);
    EXPECT_EQ(iOut[0], -50);

    std::vector<uint16_t> u0(N, 100), u1(N, 50), uOut(N);
    void* sourcesU[] = {u0.data(), u1.data()};
    EXPECT_EQ(pfnMin(sourcesU, 2, uOut.data(), N, 1, GDT_UInt16, GDT_UInt16, 2,
                     N * 2, nullptr),
              CE_None);
    EXPECT_EQ(uOut[0], 50);

    std::vector<float> f0(N, 3.5f), f1(N, 1.5f), fOut(N);
    void* sourcesF[] = {f0.data(), f1.data()};
    EXPECT_EQ(pfnMin(sourcesF, 2, fOut.data(), N, 1, GDT_Float32, GDT_Float32,
                     4, N * 4, nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 1.5f);
  }

  // Max with Byte, Int16, UInt16, Float32
  {
    std::vector<uint8_t> b0(N, 10), b1(N, 5), b2(N, 20), bOut(N);
    void* sources[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(pfnMax(sources, 3, bOut.data(), N, 1, GDT_Byte, GDT_Byte, 1, N,
                     nullptr),
              CE_None);
    EXPECT_EQ(bOut[0], 20);

    std::vector<int16_t> i0(N, -10), i1(N, -50), iOut(N);
    void* sourcesI[] = {i0.data(), i1.data()};
    EXPECT_EQ(pfnMax(sourcesI, 2, iOut.data(), N, 1, GDT_Int16, GDT_Int16, 2,
                     N * 2, nullptr),
              CE_None);
    EXPECT_EQ(iOut[0], -10);

    std::vector<uint16_t> u0(N, 100), u1(N, 50), uOut(N);
    void* sourcesU[] = {u0.data(), u1.data()};
    EXPECT_EQ(pfnMax(sourcesU, 2, uOut.data(), N, 1, GDT_UInt16, GDT_UInt16, 2,
                     N * 2, nullptr),
              CE_None);
    EXPECT_EQ(uOut[0], 100);

    std::vector<float> f0(N, 3.5f), f1(N, 1.5f), fOut(N);
    void* sourcesF[] = {f0.data(), f1.data()};
    EXPECT_EQ(pfnMax(sourcesF, 2, fOut.data(), N, 1, GDT_Float32, GDT_Float32,
                     4, N * 4, nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 3.5f);
  }

  // ArgMin and ArgMax (1-based index)
  {
    float f0[3] = {10.0f, 20.0f, 30.0f};
    float f1[3] = {20.0f, 10.0f, 40.0f};
    float f2[3] = {30.0f, 40.0f, 10.0f};
    void* sources[] = {f0, f1, f2};
    int32_t idxOut[3];

    EXPECT_EQ(pfnArgMin(sources, 3, idxOut, 3, 1, GDT_Float32, GDT_Int32, 4, 12,
                        nullptr),
              CE_None);
    EXPECT_EQ(idxOut[0], 1);
    EXPECT_EQ(idxOut[1], 2);
    EXPECT_EQ(idxOut[2], 3);

    EXPECT_EQ(pfnArgMax(sources, 3, idxOut, 3, 1, GDT_Float32, GDT_Int32, 4, 12,
                        nullptr),
              CE_None);
    EXPECT_EQ(idxOut[0], 3);
    EXPECT_EQ(idxOut[1], 3);
    EXPECT_EQ(idxOut[2], 2);
  }

  // Min with k and NoData
  {
    float f0[2] = {10.0f, 99.0f};
    float f1[2] = {20.0f, 99.0f};
    void* sources[] = {f0, f1};
    float out[2];

    CPLStringList aosArgs;
    aosArgs.AddNameValue("k", "5.0");
    aosArgs.AddNameValue("NoData", "99.0");
    EXPECT_EQ(pfnMin(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                     aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 5.0f);  // min(10, 20, k=5) = 5
    EXPECT_FLOAT_EQ(out[1], 5.0f);  // all NoData -> k=5
  }
}

// ----------------------------------------------------------------------------
// 10. Expression
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Expression) {
  auto pfnExpr = GetFunc("expression");
  ASSERT_TRUE(static_cast<bool>(pfnExpr));

  // Complex not supported
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    void* dummySources[] = {out};
    EXPECT_EQ(pfnExpr(dummySources, 1, out, 2, 1, GDT_CFloat32, GDT_Float32, 4,
                      8, nullptr),
              CE_Failure);
  }

  // Missing expression or bad source names
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float in[2] = {1, 2};
    void* sources[] = {in};
    float out[2];
    EXPECT_EQ(
        pfnExpr(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8, nullptr),
        CE_Failure);

    CPLStringList aosArgs;
    aosArgs.AddNameValue("expression", "b1 + 1");
    aosArgs.AddNameValue("source_names", "b1|b2");  // 2 names for 1 source
    EXPECT_EQ(pfnExpr(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      aosArgs.List()),
              CE_Failure);
  }

  // Expression when parser is not enabled returns CE_Failure
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float b1[2] = {10.0f, 20.0f};
    float b2[2] = {3.0f, 4.0f};
    void* sources[] = {b1, b2};
    float out[2];

    CPLStringList aosArgs;
    aosArgs.AddNameValue("expression", "b1 + 2 * b2");
    aosArgs.AddNameValue("source_names", "b1|b2");
    EXPECT_EQ(pfnExpr(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                      aosArgs.List()),
              CE_Failure);
  }
}

// ----------------------------------------------------------------------------
// 11. Reclassify
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Reclassify) {
  auto pfnReclassify = GetFunc("reclassify");
  ASSERT_TRUE(static_cast<bool>(pfnReclassify));

  // Failures
  {
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    float out[2];
    void* dummySources[] = {out};
    EXPECT_EQ(pfnReclassify(nullptr, 0, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                            8, nullptr),
              CE_Failure);
    EXPECT_EQ(pfnReclassify(dummySources, 1, out, 2, 1, GDT_CFloat32,
                            GDT_Float32, 4, 8, nullptr),
              CE_Failure);

    float in[2] = {1, 2};
    void* sources[] = {in};
    // Missing mapping
    EXPECT_EQ(pfnReclassify(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                            8, nullptr),
              CE_Failure);
  }

  // Valid reclassify mappings
  {
    float in[4] = {1.0f, 2.0f, 3.0f, 99.0f};
    void* sources[] = {in};
    float out[4];

    CPLStringList aosArgs;
    aosArgs.AddNameValue("mapping", "1=10; 2=20; default=0");
    EXPECT_EQ(pfnReclassify(sources, 1, out, 4, 1, GDT_Float32, GDT_Float32, 4,
                            16, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 10.0f);
    EXPECT_FLOAT_EQ(out[1], 20.0f);
    EXPECT_FLOAT_EQ(out[2], 0.0f);  // default
    EXPECT_FLOAT_EQ(out[3], 0.0f);

    // With NoData
    aosArgs.Clear();
    aosArgs.AddNameValue("mapping", "1=10; 2=20; NO_DATA=NO_DATA; default=0");
    aosArgs.AddNameValue("NoData", "99.0");
    EXPECT_EQ(pfnReclassify(sources, 1, out, 4, 1, GDT_Float32, GDT_Float32, 4,
                            16, aosArgs.List()),
              CE_None);
    EXPECT_FLOAT_EQ(out[3], 99.0f);  // NoData preserved
  }
}

// ----------------------------------------------------------------------------
// 12. Basic Functions: Mean, GeometricMean, HarmonicMean, Median, Mode
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, BasicPixelFuncs) {
  auto pfnMean = GetFunc("mean");
  auto pfnGeoMean = GetFunc("geometric_mean");
  auto pfnHarmonicMean = GetFunc("harmonic_mean");
  auto pfnMedian = GetFunc("median");
  auto pfnMode = GetFunc("mode");

  ASSERT_TRUE(static_cast<bool>(pfnMean));
  ASSERT_TRUE(static_cast<bool>(pfnGeoMean));
  ASSERT_TRUE(static_cast<bool>(pfnHarmonicMean));
  ASSERT_TRUE(static_cast<bool>(pfnMedian));
  ASSERT_TRUE(static_cast<bool>(pfnMode));

  constexpr int N = 35;  // Test vectorization (16 elements) + remainder

  // Mean SSE2 paths:
  // - Byte with 2 sources (_mm_avg_epu8)
  // - Byte with 3 sources (fast_d)
  // - Int16 with 2 & 3 sources
  // - UInt16 with 2 & 3 sources
  // - Float32, Float64
  {
    std::vector<uint8_t> b0(N, 10), b1(N, 20), b2(N, 30), bOut(N);
    void* sourcesB2[] = {b0.data(), b1.data()};
    EXPECT_EQ(pfnMean(sourcesB2, 2, bOut.data(), N, 1, GDT_Byte, GDT_Byte, 1, N,
                      nullptr),
              CE_None);
    EXPECT_EQ(bOut[0], 15);
    EXPECT_EQ(bOut[N - 1], 15);

    void* sourcesB3[] = {b0.data(), b1.data(), b2.data()};
    EXPECT_EQ(pfnMean(sourcesB3, 3, bOut.data(), N, 1, GDT_Byte, GDT_Byte, 1, N,
                      nullptr),
              CE_None);
    EXPECT_EQ(bOut[0], 20);

    // Int16
    std::vector<int16_t> i0(N, 100), i1(N, 200), i2(N, 300), iOut(N);
    void* sourcesI2[] = {i0.data(), i1.data()};
    EXPECT_EQ(pfnMean(sourcesI2, 2, iOut.data(), N, 1, GDT_Int16, GDT_Int16, 2,
                      N * 2, nullptr),
              CE_None);
    EXPECT_EQ(iOut[0], 150);

    void* sourcesI3[] = {i0.data(), i1.data(), i2.data()};
    EXPECT_EQ(pfnMean(sourcesI3, 3, iOut.data(), N, 1, GDT_Int16, GDT_Int16, 2,
                      N * 2, nullptr),
              CE_None);
    EXPECT_EQ(iOut[0], 200);

    // UInt16
    std::vector<uint16_t> u0(N, 100), u1(N, 200), u2(N, 300), uOut(N);
    void* sourcesU2[] = {u0.data(), u1.data()};
    EXPECT_EQ(pfnMean(sourcesU2, 2, uOut.data(), N, 1, GDT_UInt16, GDT_UInt16,
                      2, N * 2, nullptr),
              CE_None);
    EXPECT_EQ(uOut[0], 150);

    void* sourcesU3[] = {u0.data(), u1.data(), u2.data()};
    EXPECT_EQ(pfnMean(sourcesU3, 3, uOut.data(), N, 1, GDT_UInt16, GDT_UInt16,
                      2, N * 2, nullptr),
              CE_None);
    EXPECT_EQ(uOut[0], 200);

    // Float32 & Float64
    std::vector<float> f0(N, 1.5f), f1(N, 4.5f), fOut(N);
    void* sourcesF[] = {f0.data(), f1.data()};
    EXPECT_EQ(pfnMean(sourcesF, 2, fOut.data(), N, 1, GDT_Float32, GDT_Float32,
                      4, N * 4, nullptr),
              CE_None);
    EXPECT_FLOAT_EQ(fOut[0], 3.0f);

    std::vector<double> d0(N, 10.0), d1(N, 20.0), dOut(N);
    void* sourcesD[] = {d0.data(), d1.data()};
    EXPECT_EQ(pfnMean(sourcesD, 2, dOut.data(), N, 1, GDT_Float64, GDT_Float64,
                      8, N * 8, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 15.0);
  }

  // Mean with infinities, NoData, propagateNoData
  {
    double d0[3] = {std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(), 10.0};
    double d1[3] = {std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity(), 20.0};
    void* sources[] = {d0, d1};
    double dOut[3];
    EXPECT_EQ(pfnMean(sources, 2, dOut, 3, 1, GDT_Float64, GDT_Float64, 8, 24,
                      nullptr),
              CE_None);
    EXPECT_TRUE(std::isinf(dOut[0]));
    EXPECT_TRUE(std::isnan(dOut[1]));  // -inf + inf -> NaN
    EXPECT_DOUBLE_EQ(dOut[2], 15.0);

    CPLStringList aosArgs;
    aosArgs.AddNameValue("NoData", "99.0");
    aosArgs.AddNameValue("propagateNoData", "true");
    d0[2] = 99.0;
    EXPECT_EQ(pfnMean(sources, 2, dOut, 3, 1, GDT_Float64, GDT_Float64, 8, 24,
                      aosArgs.List()),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[2], 99.0);
  }

  // Geometric Mean
  {
    double d0[2] = {4.0, 2.0};
    double d1[2] = {9.0, 8.0};
    void* sources[] = {d0, d1};
    double dOut[2];
    EXPECT_EQ(pfnGeoMean(sources, 2, dOut, 2, 1, GDT_Float64, GDT_Float64, 8,
                         16, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 6.0);  // sqrt(4 * 9) = 6
    EXPECT_DOUBLE_EQ(dOut[1], 4.0);  // sqrt(2 * 8) = 4
  }

  // Harmonic Mean
  {
    double d0[2] = {2.0, 0.0};
    double d1[2] = {6.0, 10.0};
    void* sources[] = {d0, d1};
    double dOut[2];
    EXPECT_EQ(pfnHarmonicMean(sources, 2, dOut, 2, 1, GDT_Float64, GDT_Float64,
                              8, 16, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 3.0);  // 2 / (1/2 + 1/6) = 2 / (4/6) = 3.0

    // propagateZero = 1
    CPLStringList aosArgs;
    aosArgs.AddNameValue("propagateZero", "1");
    EXPECT_EQ(pfnHarmonicMean(sources, 2, dOut, 2, 1, GDT_Float64, GDT_Float64,
                              8, 16, aosArgs.List()),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[1], 0.0);
  }

  // Median (odd and even source counts)
  {
    double d0[1] = {10.0};
    double d1[1] = {50.0};
    double d2[1] = {20.0};
    void* sources3[] = {d0, d1, d2};
    double dOut[1];
    EXPECT_EQ(pfnMedian(sources3, 3, dOut, 1, 1, GDT_Float64, GDT_Float64, 8, 8,
                        nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 20.0);  // sorted: 10, 20, 50 -> 20

    double d3[1] = {30.0};
    void* sources4[] = {d0, d1, d2, d3};
    EXPECT_EQ(pfnMedian(sources4, 4, dOut, 1, 1, GDT_Float64, GDT_Float64, 8, 8,
                        nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0],
                     25.0);  // sorted: 10, 20, 30, 50 -> (20+30)/2 = 25
  }

  // Mode
  {
    double d0[1] = {5.0};
    double d1[1] = {10.0};
    double d2[1] = {10.0};
    double d3[1] = {20.0};
    void* sources[] = {d0, d1, d2, d3};
    double dOut[1];
    EXPECT_EQ(pfnMode(sources, 4, dOut, 1, 1, GDT_Float64, GDT_Float64, 8, 8,
                      nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(dOut[0], 10.0);
  }
}

// ----------------------------------------------------------------------------
// 13. VRTDerivedRasterBand Integration
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, VRTDerivedRasterBandIntegration) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/pf_src1.tif");
  TrackFile("/vsimem/pf_src2.tif");
  auto poDS1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/pf_src1.tif", 2, 2, 1, GDT_Float32, nullptr));
  auto poDS2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/pf_src2.tif", 2, 2, 1, GDT_Float32, nullptr));
  ASSERT_NE(poDS1, nullptr);
  ASSERT_NE(poDS2, nullptr);

  float data1[4] = {10, 20, 30, 40};
  float data2[4] = {1, 2, 3, 4};
  EXPECT_EQ(poDS1->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, data1, 2, 2,
                                              GDT_Float32, 0, 0),
            CE_None);
  EXPECT_EQ(poDS2->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 2, 2, data2, 2, 2,
                                              GDT_Float32, 0, 0),
            CE_None);
  poDS1.reset();
  poDS2.reset();

  // Derived band with "sum"
  const char* pszVRTXML =
      "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">"
      "  <VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "    <PixelFunctionType>sum</PixelFunctionType>"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/pf_src1.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "    </SimpleSource>"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/pf_src2.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "    </SimpleSource>"
      "  </VRTRasterBand>"
      "</VRTDataset>";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(pszVRTXML, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  float outSum[4] = {0};
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, outSum, 2, 2,
                                              GDT_Float32, 0, 0),
            CE_None);
  EXPECT_FLOAT_EQ(outSum[0], 11.0f);
  EXPECT_FLOAT_EQ(outSum[1], 22.0f);
  EXPECT_FLOAT_EQ(outSum[2], 33.0f);
  EXPECT_FLOAT_EQ(outSum[3], 44.0f);

  // Derived band with "expression" (without parser, returns CE_Failure)
  const char* pszVRTExprXML =
      "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">"
      "  <VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "    <PixelFunctionType>expression</PixelFunctionType>"
      "    <PixelFunctionArguments expression=\"b1 * 2 + b2\" />"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/pf_src1.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "    </SimpleSource>"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/pf_src2.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"2\" ySize=\"2\"/>"
      "    </SimpleSource>"
      "  </VRTRasterBand>"
      "</VRTDataset>";

  auto poVRTExpr = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(pszVRTExprXML, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRTExpr, nullptr);

  CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
  float outExpr[4] = {0};
  EXPECT_EQ(poVRTExpr->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, outExpr,
                                                  2, 2, GDT_Float32, 0, 0),
            CE_Failure);
}

// ----------------------------------------------------------------------------
// 14. Additional Coverage: Mean 129 Sources & Delta Inf
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, Mean129SourcesAndDeltaInf) {
  auto pfnMean = GetFunc("mean");
  ASSERT_TRUE(static_cast<bool>(pfnMean));

  // 129 Byte sources with Byte output and no nodata: takes the SSE2 fast path
  // that accumulates in int32 and divides with libdivide.
  constexpr int N = 35;
  std::vector<std::vector<uint8_t>> srcs(129, std::vector<uint8_t>(N, 10));
  std::vector<void*> papoSources(129);
  for (int i = 0; i < 129; ++i) {
    papoSources[i] = srcs[i].data();
  }
  std::vector<uint8_t> bOut(N, 0);
  EXPECT_EQ(pfnMean(papoSources.data(), 129, bOut.data(), N, 1, GDT_Byte,
                    GDT_Byte, 1, N, nullptr),
            CE_None);
  EXPECT_EQ(bOut[0], 10);
  EXPECT_EQ(bOut[N - 1], 10);

  // Mean of DBL_MAX and -DBL_MAX: the running-mean delta overflows to
  // infinity, so the fallback that divides each operand separately is used.
  double d0[1] = {std::numeric_limits<double>::max()};
  double d1[1] = {-std::numeric_limits<double>::max()};
  void* sourcesDelta[] = {d0, d1};
  double dOut[1] = {0};
  EXPECT_EQ(pfnMean(sourcesDelta, 2, dOut, 1, 1, GDT_Float64, GDT_Float64, 8, 8,
                    nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(dOut[0], 0.0);
}

// ----------------------------------------------------------------------------
// 15. Additional Coverage: Sum Optimized Paths
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, SumOptimizedPaths) {
  auto pfnSum = GetFunc("sum");
  ASSERT_TRUE(static_cast<bool>(pfnSum));

  constexpr int N = 35;
  // GDT_Byte to GDT_Int32: uses OptimizedSumPackedOutput<uint8_t, int32_t>,
  // which accumulates packed Byte sources directly into the Int32 output.
  std::vector<uint8_t> b0(N, 10), b1(N, 20);
  void* srcsB[] = {b0.data(), b1.data()};
  std::vector<int32_t> i32Out(N, 0);
  EXPECT_EQ(pfnSum(srcsB, 2, i32Out.data(), N, 1, GDT_Byte, GDT_Int32, 4, N * 4,
                   nullptr),
            CE_None);
  EXPECT_EQ(i32Out[0], 30);
  EXPECT_EQ(i32Out[N - 1], 30);

  // Float32 output from UInt16, Int16, Int32 and Float64 sources: each source
  // type dispatches to OptimizedSumPackedOutput<Tsrc, float>.
  std::vector<uint16_t> u16(N, 100);
  void* srcsU16[] = {u16.data()};
  std::vector<float> fOut(N, 0);
  EXPECT_EQ(pfnSum(srcsU16, 1, fOut.data(), N, 1, GDT_UInt16, GDT_Float32, 4,
                   N * 4, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(fOut[0], 100.0f);

  std::vector<int16_t> i16(N, -50);
  void* srcsI16[] = {i16.data()};
  EXPECT_EQ(pfnSum(srcsI16, 1, fOut.data(), N, 1, GDT_Int16, GDT_Float32, 4,
                   N * 4, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(fOut[0], -50.0f);

  std::vector<int32_t> i32(N, 500);
  void* srcsI32[] = {i32.data()};
  EXPECT_EQ(pfnSum(srcsI32, 1, fOut.data(), N, 1, GDT_Int32, GDT_Float32, 4,
                   N * 4, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(fOut[0], 500.0f);

  std::vector<double> d64(N, 12.5);
  void* srcsD64[] = {d64.data()};
  EXPECT_EQ(pfnSum(srcsD64, 1, fOut.data(), N, 1, GDT_Float64, GDT_Float32, 4,
                   N * 4, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(fOut[0], 12.5f);

  // OptimizedSumThroughLargerType: integer/float source and output type pairs
  // that are summed in a wider intermediate type and then narrowed.
  // Byte -> UInt16
  std::vector<uint16_t> u16Out(N, 0);
  EXPECT_EQ(pfnSum(srcsB, 2, u16Out.data(), N, 1, GDT_Byte, GDT_UInt16, 2,
                   N * 2, nullptr),
            CE_None);
  EXPECT_EQ(u16Out[0], 30);

  // UInt16 -> Int16
  std::vector<int16_t> i16Out(N, 0);
  EXPECT_EQ(pfnSum(srcsU16, 1, i16Out.data(), N, 1, GDT_UInt16, GDT_Int16, 2,
                   N * 2, nullptr),
            CE_None);
  EXPECT_EQ(i16Out[0], 100);

  // Int16 -> Int32
  EXPECT_EQ(pfnSum(srcsI16, 1, i32Out.data(), N, 1, GDT_Int16, GDT_Int32, 4,
                   N * 4, nullptr),
            CE_None);
  EXPECT_EQ(i32Out[0], -50);

  // Int32 -> Byte
  std::vector<uint8_t> bOut(N, 0);
  std::vector<int32_t> i32Small(N, 25);
  void* srcsI32Small[] = {i32Small.data()};
  EXPECT_EQ(pfnSum(srcsI32Small, 1, bOut.data(), N, 1, GDT_Int32, GDT_Byte, 1,
                   N, nullptr),
            CE_None);
  EXPECT_EQ(bOut[0], 25);

  // Float32 -> Int16
  std::vector<float> f32(N, 40.0f);
  void* srcsF32[] = {f32.data()};
  EXPECT_EQ(pfnSum(srcsF32, 1, i16Out.data(), N, 1, GDT_Float32, GDT_Int16, 2,
                   N * 2, nullptr),
            CE_None);
  EXPECT_EQ(i16Out[0], 40);

  // Float64 -> Int16
  EXPECT_EQ(pfnSum(srcsD64, 1, i16Out.data(), N, 1, GDT_Float64, GDT_Int16, 2,
                   N * 2, nullptr),
            CE_None);
  EXPECT_EQ(i16Out[0], 13);
}

// ----------------------------------------------------------------------------
// 16. Additional Coverage: Min and Max Float64 SSE2
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, MinMaxFloat64SSE2) {
  auto pfnMin = GetFunc("min");
  auto pfnMax = GetFunc("max");
  ASSERT_TRUE(static_cast<bool>(pfnMin));
  ASSERT_TRUE(static_cast<bool>(pfnMax));

  constexpr int N = 35;
  std::vector<double> d0(N), d1(N), dMin(N), dMax(N);
  for (int i = 0; i < N; ++i) {
    d0[i] = (i % 2 == 0) ? 10.0 : 20.0;
    d1[i] = (i % 2 == 0) ? 20.0 : 10.0;
  }
  void* sources[] = {d0.data(), d1.data()};
  EXPECT_EQ(pfnMin(sources, 2, dMin.data(), N, 1, GDT_Float64, GDT_Float64, 8,
                   N * 8, nullptr),
            CE_None);
  EXPECT_EQ(pfnMax(sources, 2, dMax.data(), N, 1, GDT_Float64, GDT_Float64, 8,
                   N * 8, nullptr),
            CE_None);
  for (int i = 0; i < N; ++i) {
    EXPECT_DOUBLE_EQ(dMin[i], 10.0);
    EXPECT_DOUBLE_EQ(dMax[i], 20.0);
  }
}

// ----------------------------------------------------------------------------
// 17. Additional Coverage: GetSrcVal on all types
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, GetSrcValAllDataTypes) {
  auto pfnDiff = GetFunc("diff");
  ASSERT_TRUE(static_cast<bool>(pfnDiff));

  // GDT_Byte
  uint8_t b[2] = {10, 5};
  void* srcB[] = {&b[0], &b[1]};
  double out[1];
  EXPECT_EQ(pfnDiff(srcB, 2, out, 1, 1, GDT_Byte, GDT_Float64, 8, 8, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(out[0], 5.0);

  // GDT_UInt16
  uint16_t u16[2] = {100, 20};
  void* srcU16[] = {&u16[0], &u16[1]};
  EXPECT_EQ(
      pfnDiff(srcU16, 2, out, 1, 1, GDT_UInt16, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 80.0);

  // GDT_Int16
  int16_t i16[2] = {50, 100};
  void* srcI16[] = {&i16[0], &i16[1]};
  EXPECT_EQ(
      pfnDiff(srcI16, 2, out, 1, 1, GDT_Int16, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], -50.0);

  // GDT_Int32
  int32_t i32[2] = {1000, 400};
  void* srcI32[] = {&i32[0], &i32[1]};
  EXPECT_EQ(
      pfnDiff(srcI32, 2, out, 1, 1, GDT_Int32, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 600.0);

  // GDT_Float16
  GFloat16 f16[2] = {GFloat16(10.0f), GFloat16(4.0f)};
  void* srcF16[] = {&f16[0], &f16[1]};
  EXPECT_EQ(
      pfnDiff(srcF16, 2, out, 1, 1, GDT_Float16, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 6.0);

  // GDT_CInt16
  int16_t ci16_0[2] = {10, 0};
  int16_t ci16_1[2] = {3, 0};
  void* srcCI16[] = {ci16_0, ci16_1};
  EXPECT_EQ(
      pfnDiff(srcCI16, 2, out, 1, 1, GDT_CInt16, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 7.0);

  // GDT_CInt32
  int32_t ci32_0[2] = {20, 0};
  int32_t ci32_1[2] = {5, 0};
  void* srcCI32[] = {ci32_0, ci32_1};
  EXPECT_EQ(
      pfnDiff(srcCI32, 2, out, 1, 1, GDT_CInt32, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 15.0);

  // GDT_CFloat16
  GFloat16 cf16_0[2] = {GFloat16(30.0f), GFloat16(0.0f)};
  GFloat16 cf16_1[2] = {GFloat16(12.0f), GFloat16(0.0f)};
  void* srcCF16[] = {cf16_0, cf16_1};
  EXPECT_EQ(
      pfnDiff(srcCF16, 2, out, 1, 1, GDT_CFloat16, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 18.0);

  // GDT_Unknown
  double unk[2] = {0, 0};
  void* srcUnk[] = {&unk[0], &unk[1]};
  EXPECT_EQ(
      pfnDiff(srcUnk, 2, out, 1, 1, GDT_Unknown, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 0.0);

  // GDT_TypeCount
  EXPECT_EQ(
      pfnDiff(srcUnk, 2, out, 1, 1, GDT_TypeCount, GDT_Float64, 8, 8, nullptr),
      CE_None);
  EXPECT_DOUBLE_EQ(out[0], 0.0);
}

// ----------------------------------------------------------------------------
// 18. Additional Coverage: Bad Arguments and Edge Cases
// ----------------------------------------------------------------------------
TEST_F(PixelFunctionsTest, BadArgumentsAndEdgeCases) {
  CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
  float val[2] = {1.0f, 2.0f};
  void* sources[] = {val, val};
  float out[2];

  CPLStringList badArgs;
  badArgs.AddNameValue("NoData", "not_a_number");

  // Sum with bad NoData
  EXPECT_EQ(GetFunc("sum")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);

  // Diff with bad NoData
  EXPECT_EQ(GetFunc("diff")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                            8, badArgs.List()),
            CE_Failure);

  // Mul with bad k & bad NoData
  CPLStringList badK;
  badK.AddNameValue("k", "not_a_number");
  EXPECT_EQ(GetFunc("mul")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badK.List()),
            CE_Failure);
  EXPECT_EQ(GetFunc("mul")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);

  // Div with bad NoData
  EXPECT_EQ(GetFunc("div")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);

  // Inv with bad k & bad NoData
  EXPECT_EQ(GetFunc("inv")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badK.List()),
            CE_Failure);
  EXPECT_EQ(GetFunc("inv")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);

  // Sqrt with bad NoData
  EXPECT_EQ(GetFunc("sqrt")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                            8, badArgs.List()),
            CE_Failure);

  // Log10 with bad NoData
  EXPECT_EQ(GetFunc("log10")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                             8, badArgs.List()),
            CE_Failure);

  // dB with bad fact
  CPLStringList badFact;
  badFact.AddNameValue("fact", "not_a_number");
  EXPECT_EQ(GetFunc("dB")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4, 8,
                          badFact.List()),
            CE_Failure);

  // Pow with bad power & bad NoData
  CPLStringList badPow;
  badPow.AddNameValue("power", "not_a_number");
  EXPECT_EQ(GetFunc("pow")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badPow.List()),
            CE_Failure);
  EXPECT_EQ(GetFunc("pow")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);

  // InterpolateLinear with bad args
  CPLStringList badInterp;
  badInterp.AddNameValue("t", "bad");
  EXPECT_EQ(GetFunc("interpolate_linear")(sources, 2, out, 2, 1, GDT_Float32,
                                          GDT_Float32, 4, 8, badInterp.List()),
            CE_Failure);
  badInterp.SetNameValue("t", "5.0");
  badInterp.AddNameValue("t0", "bad");
  EXPECT_EQ(GetFunc("interpolate_linear")(sources, 2, out, 2, 1, GDT_Float32,
                                          GDT_Float32, 4, 8, badInterp.List()),
            CE_Failure);
  badInterp.SetNameValue("t0", "0.0");
  badInterp.AddNameValue("dt", "bad");
  EXPECT_EQ(GetFunc("interpolate_linear")(sources, 2, out, 2, 1, GDT_Float32,
                                          GDT_Float32, 4, 8, badInterp.List()),
            CE_Failure);
  badInterp.SetNameValue("dt", "1.0");
  // "t" must be finite and non-zero.
  badInterp.SetNameValue("t", "0.0");
  EXPECT_EQ(GetFunc("interpolate_linear")(sources, 2, out, 2, 1, GDT_Float32,
                                          GDT_Float32, 4, 8, badInterp.List()),
            CE_Failure);

  // InterpolateExp with bad args
  EXPECT_EQ(GetFunc("interpolate_exp")(sources, 2, out, 2, 1, GDT_Float32,
                                       GDT_Float32, 4, 8, badInterp.List()),
            CE_Failure);

  // ReplaceNoData with bad args
  CPLStringList badReplace;
  badReplace.AddNameValue("to", "bad");
  EXPECT_EQ(GetFunc("replace_nodata")(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, badReplace.List()),
            CE_Failure);
  badReplace.SetNameValue("to", "10.0");
  badReplace.AddNameValue("NoData", "bad");
  EXPECT_EQ(GetFunc("replace_nodata")(sources, 1, out, 2, 1, GDT_Float32,
                                      GDT_Float32, 4, 8, badReplace.List()),
            CE_Failure);

  // Scale with bad args
  CPLStringList badScale;
  badScale.AddNameValue("scale", "bad");
  EXPECT_EQ(GetFunc("scale")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                             8, badScale.List()),
            CE_Failure);
  badScale.SetNameValue("scale", "2.0");
  badScale.AddNameValue("NoData", "bad");
  EXPECT_EQ(GetFunc("scale")(sources, 1, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                             8, badScale.List()),
            CE_Failure);

  // NormDiff with complex or bad NoData
  EXPECT_EQ(GetFunc("norm_diff")(sources, 2, out, 2, 1, GDT_Float32,
                                 GDT_Float32, 4, 8, badArgs.List()),
            CE_Failure);
  EXPECT_EQ(GetFunc("norm_diff")(sources, 2, out, 2, 1, GDT_CFloat32,
                                 GDT_Float32, 4, 8, nullptr),
            CE_Failure);

  // BasicPixelFunc (mean, harmonic_mean, min) with complex or bad NoData
  EXPECT_EQ(GetFunc("mean")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                            8, badArgs.List()),
            CE_Failure);
  EXPECT_EQ(GetFunc("mean")(sources, 2, out, 2, 1, GDT_CFloat32, GDT_Float32, 4,
                            8, nullptr),
            CE_Failure);
  EXPECT_EQ(GetFunc("harmonic_mean")(sources, 2, out, 2, 1, GDT_CFloat32,
                                     GDT_Float32, 4, 8, nullptr),
            CE_Failure);
  EXPECT_EQ(GetFunc("min")(sources, 2, out, 2, 1, GDT_CFloat32, GDT_Float32, 4,
                           8, nullptr),
            CE_Failure);
  EXPECT_EQ(GetFunc("min")(sources, 2, out, 2, 1, GDT_Float32, GDT_Float32, 4,
                           8, badArgs.List()),
            CE_Failure);
}

}  // namespace
