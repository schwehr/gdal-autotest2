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
// Tests built-in VRTProcessedDataset processing steps (BandAffineCombination,
// LUT, LocalScaleOffset, Trimming, and Expression) in
// vrtprocesseddatasetfunctions.cpp. Note that //third_party/gdal is compiled
// without muparser or exprtk, so Expression dialect tests pin the stub error
// paths.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtprocesseddatasetfunctions.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/vrtprocesseddatasetfunctions_test";

GDALDatasetUniquePtr OpenRasterVerbose(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR, nullptr,
                 nullptr, nullptr)));
}

GDALDatasetUniquePtr OpenRasterQuiet(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(GDALOpenEx(
      filename, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
}

// TODO(schwehr): Extract ScopedWarningCapturer to util/error_handler.h.
class ScopedWarningCapturer {
 public:
  ScopedWarningCapturer() {
    CPLPushErrorHandlerEx(
        [](CPLErr err_class, int, const char* err_msg) {
          auto* self =
              static_cast<ScopedWarningCapturer*>(CPLGetErrorHandlerUserData());
          self->last_class_ = err_class;
          self->last_message_ = err_msg ? err_msg : "";
        },
        this);
  }
  ~ScopedWarningCapturer() { CPLPopErrorHandler(); }

  ScopedWarningCapturer(const ScopedWarningCapturer&) = delete;
  ScopedWarningCapturer& operator=(const ScopedWarningCapturer&) = delete;

  CPLErr last_class() const { return last_class_; }
  const std::string& last_message() const { return last_message_; }

 private:
  CPLErr last_class_ = CE_None;
  std::string last_message_;
};

class VrtProcessedDatasetFunctionsTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_VRT();
    GDALRegister_GTiff();
    GDALVRTRegisterDefaultProcessedDatasetFuncs();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateTiff(const char* filename, int xsize, int ysize, int bands,
                  GDALDataType datatype, const double* geotransform,
                  const double* nodata,
                  const std::vector<std::vector<double>>& data) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver);
    GDALDatasetUniquePtr ds(
        driver->Create(filename, xsize, ysize, bands, datatype, nullptr));
    ASSERT_NE(nullptr, ds);
    if (geotransform != nullptr) {
      EXPECT_EQ(CE_None, ds->SetGeoTransform(GDALGeoTransform(geotransform)));
    }
    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      ASSERT_NE(nullptr, band);
      if (nodata != nullptr) {
        EXPECT_EQ(CE_None, band->SetNoDataValue(nodata[b - 1]));
      }
      if (b - 1 < static_cast<int>(data.size()) && !data[b - 1].empty()) {
        std::vector<double> band_data = data[b - 1];
        EXPECT_EQ(CE_None,
                  band->RasterIO(GF_Write, 0, 0, xsize, ysize, band_data.data(),
                                 xsize, ysize, GDT_Float64, 0, 0));
      }
    }
    ds.reset();
  }
};

// =========================================================================
// BandAffineCombination Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, BandAffineCombinationBasic) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const std::vector<std::vector<double>> input_data = {
      {1.0, 2.0, 3.0, 4.0},      // Band 1
      {10.0, 20.0, 30.0, 40.0},  // Band 2
  };
  constexpr char kInPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/affine_in.tif";
  CreateTiff(kInPath, 2, 2, 2, GDT_Float64, kGt, nullptr, input_data);

  // Band 1: 10 + 1 * b1 + 2 * b2 -> for (1, 10): 10 + 1 + 20 = 31
  // Band 2: 5 + 2 * b1 - 0.5 * b2 -> for (1, 10): 5 + 2 - 5 = 2
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_in.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">10,1,2</Argument>
      <Argument name="coefficients_2">5,2,-0.5</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(2, vrt_ds->GetRasterCount());

  std::vector<double> out_b1(4);
  std::vector<double> out_b2(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b1.data(), 2, 2, GDT_Float64, 0, 0));
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(2)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b2.data(), 2, 2, GDT_Float64, 0, 0));

  EXPECT_DOUBLE_EQ(31.0, out_b1[0]);  // 10 + 1 + 20
  EXPECT_DOUBLE_EQ(52.0, out_b1[1]);  // 10 + 2 + 40
  EXPECT_DOUBLE_EQ(73.0, out_b1[2]);  // 10 + 3 + 60
  EXPECT_DOUBLE_EQ(94.0, out_b1[3]);  // 10 + 4 + 80

  EXPECT_DOUBLE_EQ(2.0, out_b2[0]);   // 5 + 2 - 5
  EXPECT_DOUBLE_EQ(-1.0, out_b2[1]);  // 5 + 4 - 10
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       BandAffineCombinationClampingAndNoData) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  constexpr double kNoData[1] = {-999.0};
  const std::vector<std::vector<double>> input_data = {
      {-999.0, 5.0, 100.0, -50.0}};
  constexpr char kInPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/affine_clamp.tif";
  CreateTiff(kInPath, 2, 2, 1, GDT_Float64, kGt, kNoData, input_data);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_clamp.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="src_nodata">-999</Argument>
      <Argument name="dst_nodata">-1</Argument>
      <Argument name="replacement_nodata">0</Argument>
      <Argument name="min">10</Argument>
      <Argument name="max">50</Argument>
      <Argument name="coefficients_1">0,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                               2, 2, GDT_Float64, 0, 0));

  EXPECT_DOUBLE_EQ(-1.0, out[0]);  // Nodata preserved
  EXPECT_DOUBLE_EQ(10.0, out[1]);  // 5 clamped up to min=10
  EXPECT_DOUBLE_EQ(50.0, out[2]);  // 100 clamped down to max=50
  EXPECT_DOUBLE_EQ(10.0, out[3]);  // -50 clamped up to min=10
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       BandAffineCombinationGetDstValueBranches) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const std::vector<std::vector<double>> input_data = {{0.0, 42.0, 10.0, 20.0}};
  constexpr char kInPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/affine_dt.tif";
  CreateTiff(kInPath, 2, 2, 1, GDT_Float64, kGt, nullptr, input_data);

  // 1. Integer DT (Byte) with rounding match: round(0.1 + 0) == 0 -> 99
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_dt.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="dst_intended_datatype">Byte</Argument>
      <Argument name="dst_nodata">0</Argument>
      <Argument name="replacement_nodata">99</Argument>
      <Argument name="coefficients_1">0.1,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(CE_None,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, out.data(), 2, 2, GDT_Float64, 0, 0));
    EXPECT_DOUBLE_EQ(99.0, out[0]);
  }

  // 2. Float16 match
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_dt.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="dst_intended_datatype">Float16</Argument>
      <Argument name="dst_nodata">42</Argument>
      <Argument name="replacement_nodata">123</Argument>
      <Argument name="coefficients_1">0,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(CE_None,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, out.data(), 2, 2, GDT_Float64, 0, 0));
    EXPECT_DOUBLE_EQ(123.0, out[1]);
  }

  // 3. Float32 match
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_dt.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="dst_intended_datatype">Float32</Argument>
      <Argument name="dst_nodata">42</Argument>
      <Argument name="replacement_nodata">77</Argument>
      <Argument name="coefficients_1">0,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(CE_None,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, out.data(), 2, 2, GDT_Float64, 0, 0));
    EXPECT_DOUBLE_EQ(77.0, out[1]);
  }

  // 4. Float64 match with default replacement_nodata
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_dt.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="dst_intended_datatype">Float64</Argument>
      <Argument name="dst_nodata">42</Argument>
      <Argument name="coefficients_1">0,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(CE_None,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, out.data(), 2, 2, GDT_Float64, 0, 0));
    EXPECT_NE(42.0, out[1]);
  }
}

TEST_F(VrtProcessedDatasetFunctionsTest, BandAffineCombinationErrors) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  constexpr char kErrPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif";
  CreateTiff(kErrPath, 2, 2, 2, GDT_Float64, kGt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}});

  WithQuietHandler quiet;

  // 1. Invalid band index <= 0
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_0">10,1,2</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Invalid band index > 65536
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_70000">10,1,2</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Token count mismatch (expects 3 tokens: constant + 2 bands)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">10,1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Missing band in sequence (coefficients_2 provided without
  // coefficients_1)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_2">10,1,2</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. Final step band count mismatch (2 bands declared, only 1 provided)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <VRTRasterBand dataType="Float64" band="1"/>
  <VRTRasterBand dataType="Float64" band="2"/>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">10,1,2</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Unrecognized argument emits a warning and opens successfully
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="unknown_arg">foo</Argument>
      <Argument name="coefficients_1">10,1,2</Argument>
      <Argument name="coefficients_2">20,3,4</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_NE(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // 7. Missing band in sequence when total count matches (1 and 3 for 2 bands)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/affine_err.tif</SourceFilename>
  </Input>
  <OutputBands count="2"/>
  <ProcessingSteps>
    <Step name="affine">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">10,1,2</Argument>
      <Argument name="coefficients_3">20,3,4</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  CPLErrorReset();
}

// =========================================================================
// LUT Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, LUTBasicAndLookupValueBranches) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  constexpr double kNoData[1] = {-999.0};
  const std::vector<std::vector<double>> input_data = {
      {-5.0, 0.0, 5.0, 10.0, 15.0, 20.0, 25.0, -999.0}};
  constexpr char kInPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/lut_in.tif";
  CreateTiff(kInPath, 4, 2, 1, GDT_Float64, kGt, kNoData, input_data);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_in.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="src_nodata">-999</Argument>
      <Argument name="dst_nodata">-1</Argument>
      <Argument name="lut_1">0:0,10:100,20:200</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out(8);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 2, out.data(),
                                               4, 2, GDT_Float64, 0, 0));

  EXPECT_DOUBLE_EQ(0.0, out[0]);    // -5 < 0 -> clamped to output[0] = 0
  EXPECT_DOUBLE_EQ(0.0, out[1]);    // 0 == 0 -> exact match = 0
  EXPECT_DOUBLE_EQ(50.0, out[2]);   // 5 interpolated between 0 and 100 -> 50
  EXPECT_DOUBLE_EQ(100.0, out[3]);  // 10 == 10 -> exact match = 100
  EXPECT_DOUBLE_EQ(150.0,
                   out[4]);  // 15 interpolated between 100 and 200 -> 150
  EXPECT_DOUBLE_EQ(200.0, out[5]);  // 20 == 20 -> exact match = 200
  EXPECT_DOUBLE_EQ(200.0, out[6]);  // 25 > 20 -> clamped to output.back() = 200
  EXPECT_DOUBLE_EQ(-1.0, out[7]);   // -999 nodata -> dst_nodata = -1
}

TEST_F(VrtProcessedDatasetFunctionsTest, LUTErrors) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  constexpr char kErrPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif";
  CreateTiff(kErrPath, 2, 2, 2, GDT_Float64, kGt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}});

  WithQuietHandler quiet;

  // 1. Invalid band index <= 0
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_0">0:0,10:10</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Band index > nInBands
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_5">0:0,10:10</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Invalid token (not 2 values separated by colon)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0:0</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Missing lut_1 when lut_2 provided
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_2">0:0,10:10</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. Final step missing lut elements (2 bands expected, only lut_1 provided)
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <OutputBands count="2"/>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0,10:10</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Unrecognized argument emits a warning and opens successfully
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lut_err.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lut">
      <Algorithm>LUT</Algorithm>
      <Argument name="unknown_arg">foo</Argument>
      <Argument name="lut_1">0:0,10:10</Argument>
      <Argument name="lut_2">0:0,10:10</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    // Capture the unrecognized-argument warning before subsequent dataset
    // open calls internally invoke CPLErrorReset().
    ScopedWarningCapturer capturer;
    EXPECT_NE(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Warning, capturer.last_class());
    EXPECT_THAT(capturer.last_message(),
                ::testing::HasSubstr("Unrecognized argument name unknown_arg"));
  }
}

// =========================================================================
// LocalScaleOffset Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetBasicAndNodata) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSrcNoData[1] = {-999.0};
  const std::vector<std::vector<double>> src_data = {
      {10.0, 20.0, -999.0, 50.0}};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_src.tif", 2, 2, 1,
             GDT_Float64, kGt, kSrcNoData, src_data);

  constexpr double kGainNoData[1] = {-888.0};
  const std::vector<std::vector<double>> gain_data = {{2.0, 1.5, 2.0, -888.0}};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_gain.tif", 2, 2, 1,
             GDT_Float32, kGt, kGainNoData, gain_data);

  constexpr double kOffsetNoData[1] = {-777.0};
  const std::vector<std::vector<double>> offset_data = {
      {5.0, 10.0, -777.0, 0.0}};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_offset.tif", 2, 2,
             1, GDT_Float32, kGt, kOffsetNoData, offset_data);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_gain.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_offset.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
      <Argument name="gain_nodata">-888</Argument>
      <Argument name="offset_nodata">-777</Argument>
      <Argument name="nodata">-999</Argument>
      <Argument name="min">18</Argument>
      <Argument name="max">19</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                               2, 2, GDT_Float64, 0, 0));

  EXPECT_DOUBLE_EQ(18.0, out[0]);    // 10 * 2 - 5 = 15 -> clamped to 18
  EXPECT_DOUBLE_EQ(19.0, out[1]);    // 20 * 1.5 - 10 = 20 -> clamped to 19
  EXPECT_DOUBLE_EQ(-999.0, out[2]);  // src is nodata -> -999
  EXPECT_DOUBLE_EQ(-999.0, out[3]);  // gain is nodata -> -999
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       LocalScaleOffsetRelativeToVRTAndAutoNoData) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kNoData[1] = {-1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_rel_src.tif", 2, 2,
             1, GDT_Float64, kGt, kNoData, {{10, 20, 30, 40}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_rel_gain.tif", 2, 2,
             1, GDT_Float32, kGt, kNoData, {{1, 1, 1, 1}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_rel_offset.tif", 2,
             2, 1, GDT_Float32, kGt, kNoData, {{0, 0, 0, 0}});

  constexpr std::string_view kVrtXml = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="1">lso_rel_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="relativeToVRT">true</Argument>
      <Argument name="gain_dataset_filename_1">lso_rel_gain.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">lso_rel_offset.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  constexpr char kVrtPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/test_rel.vrt";
  VsiMemTempWrapper vrt_file(kVrtPath, std::string(kVrtXml));

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtPath);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                               2, 2, GDT_Float64, 0, 0));
  EXPECT_DOUBLE_EQ(10.0, out[0]);
  EXPECT_DOUBLE_EQ(20.0, out[1]);
}

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetErrors) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif", 2, 2,
             1, GDT_Float64, kGt, nullptr, {{1, 2, 3, 4}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif", 2, 2,
             2, GDT_Float64, kGt, nullptr, {{1, 2, 3, 4}, {5, 6, 7, 8}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif", 2, 2,
             1, GDT_Float32, kGt, nullptr, {{1, 1, 1, 1}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_nogt.tif", 2, 2, 1,
             GDT_Float32, nullptr, nullptr, {{1, 1, 1, 1}});

  WithQuietHandler quiet;

  // 1. Missing required arguments and invalid 0-based band indices
  constexpr std::string_view
      kFailingVrtXmls
          [] =
              {
                  // Missing gain_dataset_filename_1
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Missing gain_dataset_band_1
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Missing offset_dataset_filename_1
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Missing offset_dataset_band_1
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Band index 0 in gain_dataset_filename_0
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Band index 0 in gain_dataset_band_0
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_0">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Band index 0 in offset_dataset_filename_0
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Band index 0 in offset_dataset_band_0
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_0">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // 2-band dataset missing gain_dataset_filename_2
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="gain_dataset_band_2">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument><Argument name="offset_dataset_band_2">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // 2-band dataset missing gain_dataset_band_2
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument><Argument name="offset_dataset_band_2">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // 2-band dataset missing offset_dataset_filename_2
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="gain_dataset_band_2">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument><Argument name="offset_dataset_band_2">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // 2-band dataset missing offset_dataset_band_2
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="gain_dataset_band_2">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // CheckAllBands gap: gain_dataset_filename_2 without _1
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src2.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Dataset open failure (nonexistent file)
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/nonexistent.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Aux dataset lacks geotransform
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_nogt.tif</Argument><Argument name="gain_dataset_band_1">1</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
                  // Invalid band in aux dataset (band 99)
                  R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="lso"><Algorithm>LocalScaleOffset</Algorithm><Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="gain_dataset_band_1">99</Argument><Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument><Argument name="offset_dataset_band_1">1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
              };

  for (size_t i = 0; i < std::size(kFailingVrtXmls); ++i) {
    SCOPED_TRACE(i);
    CPLErrorReset();
    std::string xml(kFailingVrtXmls[i]);
    EXPECT_EQ(nullptr, OpenRasterQuiet(xml.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Unrecognized argument emits warning and opens successfully
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="unknown_arg">foo</Argument>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_err_aux.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    // Capture the unrecognized-argument warning before subsequent dataset
    // open calls internally invoke CPLErrorReset().
    ScopedWarningCapturer capturer;
    EXPECT_NE(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Warning, capturer.last_class());
    EXPECT_THAT(capturer.last_message(),
                ::testing::HasSubstr("Unrecognized argument name unknown_arg"));
  }
}

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetLoadAuxDataErrors) {
  constexpr double kGtSrc[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_load_src.tif", 2, 2,
             1, GDT_Float64, kGtSrc, nullptr, {{1, 2, 3, 4}});

  // 1. Inverted coordinates: aux has reversed X/Y geotransform
  constexpr std::string_view kReversedVrt = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <GeoTransform>100, -1, 0, 1000, 0, 1</GeoTransform>
  <VRTRasterBand dataType="Float32" band="1"/>
</VRTDataset>)vrt";
  VsiMemTempWrapper reversed_file(
      "/vsimem/vrtprocesseddatasetfunctions_test/lso_reversed.vrt",
      std::string(kReversedVrt));

  WithQuietHandler quiet;

  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_load_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_reversed.vrt</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_reversed.vrt</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterQuiet(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    std::vector<double> out(4);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              vrt_ds->RasterIO(GF_Read, 0, 0, 2, 2, out.data(), 2, 2,
                               GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Aux dataset out of bounds upper left
  constexpr double kGtShiftUl[6] = {200.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_shift_ul.tif", 2, 2,
             1, GDT_Float32, kGtShiftUl, nullptr, {{1, 1, 1, 1}});

  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_load_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_shift_ul.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_shift_ul.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterQuiet(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    std::vector<double> out(4);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 2, 2, out.data(), 2, 2, GDT_Float64, 0, 0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Aux dataset out of bounds lower right
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_small.tif", 1, 1, 1,
             GDT_Float32, kGtSrc, nullptr, {{1}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/lso_load_src4.tif", 4,
             4, 1, GDT_Float64, kGtSrc, nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});

  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/lso_load_src4.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="lso">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_small.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/lso_small.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterQuiet(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    std::vector<double> out(16);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              vrt_ds->RasterIO(GF_Read, 0, 0, 4, 4, out.data(), 4, 4,
                               GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  CPLErrorReset();
}

// =========================================================================
// Trimming Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingBasicRGBAndRGBFirst) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const std::vector<std::vector<double>> src_data = {
      {100.0, 200.0, 50.0, -999.0},
      {150.0, 180.0, 70.0, 50.0},
      {120.0, 190.0, 60.0, 50.0},
      {300.0, 400.0, -999.0, 50.0},
  };
  constexpr double kNoData[4] = {-999.0, -999.0, -999.0, -999.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_src4.tif", 2, 2, 4,
             GDT_Float64, kGt, kNoData, src_data);

  const std::vector<std::vector<double>> trim_data = {
      {250.0, 250.0, 250.0, 250.0}};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_aux.tif", 2, 2, 1,
             GDT_Float32, kGt, nullptr, trim_data);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_src4.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="trimming">
      <Algorithm>Trimming</Algorithm>
      <Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_aux.tif</Argument>
      <Argument name="red_band">1</Argument>
      <Argument name="green_band">2</Argument>
      <Argument name="blue_band">3</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
      <Argument name="nodata">-999</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(4, vrt_ds->GetRasterCount());

  std::vector<double> out_r(4);
  std::vector<double> out_nir(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_r.data(), 2, 2, GDT_Float64, 0, 0));
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(4)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_nir.data(), 2, 2, GDT_Float64, 0, 0));

  EXPECT_LT(0.0, out_r[0]);
  EXPECT_LT(0.0, out_nir[0]);
  EXPECT_DOUBLE_EQ(-999.0, out_r[2]);
  EXPECT_DOUBLE_EQ(-999.0, out_nir[2]);
  EXPECT_DOUBLE_EQ(-999.0, out_r[3]);
  EXPECT_DOUBLE_EQ(-999.0, out_nir[3]);
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingRGBNotFirst) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const std::vector<std::vector<double>> src_data = {
      {50.0, -999.0, 50.0, 50.0},
      {100.0, 200.0, 50.0, 50.0},
      {150.0, 180.0, 70.0, 50.0},
      {120.0, 190.0, 60.0, 50.0},
  };
  constexpr double kNoData[4] = {-999.0, -999.0, -999.0, -999.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_src_notfirst.tif",
             2, 2, 4, GDT_Float64, kGt, kNoData, src_data);

  const std::vector<std::vector<double>> trim_data = {
      {250.0, 250.0, -888.0, 250.0}};
  constexpr double kTrimNoData[1] = {-888.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_aux_notfirst.tif",
             2, 2, 1, GDT_Float32, kGt, kTrimNoData, trim_data);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_src_notfirst.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="trimming">
      <Algorithm>Trimming</Algorithm>
      <Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_aux_notfirst.tif</Argument>
      <Argument name="red_band">2</Argument>
      <Argument name="green_band">3</Argument>
      <Argument name="blue_band">4</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
      <Argument name="nodata">-999</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out_b1(4);
  std::vector<double> out_r(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b1.data(), 2, 2, GDT_Float64, 0, 0));
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(2)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_r.data(), 2, 2, GDT_Float64, 0, 0));

  EXPECT_LT(0.0, out_b1[0]);
  EXPECT_LT(0.0, out_r[0]);
  EXPECT_DOUBLE_EQ(-999.0, out_b1[1]);
  EXPECT_DOUBLE_EQ(-999.0, out_r[1]);
  EXPECT_DOUBLE_EQ(-999.0, out_b1[2]);
  EXPECT_DOUBLE_EQ(-999.0, out_r[2]);
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingErrors) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif", 2, 2,
             3, GDT_Float64, kGt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif", 2, 2,
             1, GDT_Float32, kGt, nullptr, {{1, 1, 1, 1}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_2bands.tif", 2, 2,
             2, GDT_Float32, kGt, nullptr, {{1, 1, 1, 1}, {2, 2, 2, 2}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_nogt.tif", 2, 2, 1,
             GDT_Float32, nullptr, nullptr, {{1, 1, 1, 1}});

  WithQuietHandler quiet;

  constexpr std::string_view kFailingVrtXmls[] = {
      // 1. Invalid red band
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif</Argument><Argument name="red_band">0</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 2. Duplicate bands (red == green)
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif</Argument><Argument name="red_band">1</Argument><Argument name="green_band">1</Argument><Argument name="blue_band">3</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 3. Trimming dataset has 2 bands (must be 1)
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_2bands.tif</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 4. Trimming dataset open failure
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/nonexistent.tif</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 5. Trimming dataset lacks geotransform
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_nogt.tif</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 6. Invalid green band (0)
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif</Argument><Argument name="red_band">1</Argument><Argument name="green_band">0</Argument><Argument name="blue_band">3</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 7. Invalid blue band (0)
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="trimming"><Algorithm>Trimming</Algorithm><Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif</Argument><Argument name="red_band">1</Argument><Argument name="green_band">2</Argument><Argument name="blue_band">0</Argument><Argument name="top_rgb">255</Argument><Argument name="tone_ceil">240</Argument><Argument name="top_margin">0.05</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
  };

  for (size_t i = 0; i < std::size(kFailingVrtXmls); ++i) {
    SCOPED_TRACE(i);
    CPLErrorReset();
    std::string xml(kFailingVrtXmls[i]);
    EXPECT_EQ(nullptr, OpenRasterQuiet(xml.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 8. Unrecognized argument emits warning and opens successfully
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="trimming">
      <Algorithm>Trimming</Algorithm>
      <Argument name="unknown_arg">foo</Argument>
      <Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_aux.tif</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    // Capture the unrecognized-argument warning before subsequent dataset
    // open calls internally invoke CPLErrorReset().
    ScopedWarningCapturer capturer;
    EXPECT_NE(nullptr, OpenRasterQuiet(kVrtXml));
    EXPECT_EQ(CE_Warning, capturer.last_class());
    EXPECT_THAT(capturer.last_message(),
                ::testing::HasSubstr("Unrecognized argument name unknown_arg"));
  }
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingRelativeToVRTAndNoData) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_rel_src.tif", 2, 2,
             3, GDT_Float64, kGt, nullptr,
             {{100, 200, 50, 50}, {150, 180, 70, 50}, {120, 190, 60, 50}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_rel_aux.tif", 2, 2,
             1, GDT_Float32, kGt, nullptr, {{250, 250, 250, 250}});

  constexpr std::string_view kVrtXml = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="1">trim_rel_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="trimming">
      <Algorithm>Trimming</Algorithm>
      <Argument name="relativeToVRT">true</Argument>
      <Argument name="trimming_dataset_filename">trim_rel_aux.tif</Argument>
      <Argument name="trimming_nodata">-777</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  constexpr char kVrtPath[] =
      "/vsimem/vrtprocesseddatasetfunctions_test/test_trim_rel.vrt";
  VsiMemTempWrapper vrt_file(kVrtPath, std::string(kVrtXml));

  EXPECT_NE(nullptr, OpenRasterVerbose(kVrtPath));
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingLoadAuxDataError) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src4.tif", 4,
             4, 3, GDT_Float64, kGt, nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/trim_small_aux.tif", 1,
             1, 1, GDT_Float32, kGt, nullptr, {{1}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/trim_err_src4.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="trimming">
      <Algorithm>Trimming</Algorithm>
      <Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/trim_small_aux.tif</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  WithQuietHandler quiet;
  GDALDatasetUniquePtr vrt_ds = OpenRasterQuiet(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  std::vector<double> out(4 * 4 * 3);
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            vrt_ds->RasterIO(GF_Read, 0, 0, 4, 4, out.data(), 4, 4, GDT_Float64,
                             3, nullptr, 0, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// =========================================================================
// Expression Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, ExpressionDialectsAndErrors) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/expr_err_src.tif", 2, 2,
             2, GDT_Float64, kGt, nullptr,
             {{10.0, 20.0, 30.0, 40.0}, {1.0, 2.0, 3.0, 4.0}});

  WithQuietHandler quiet;

  constexpr std::string_view kFailingVrtXmls[] = {
      // 1. batch_size < 1
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/expr_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="expr"><Algorithm>Expression</Algorithm><Argument name="batch_size">0</Argument><Argument name="expression">B1</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 2. Default dialect (muparser) when not compiled in
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/expr_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="expr"><Algorithm>Expression</Algorithm><Argument name="expression">B1 * 2 + B2</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 3. Dialect exprtk when not compiled in
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/expr_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="expr"><Algorithm>Expression</Algorithm><Argument name="dialect">exprtk</Argument><Argument name="expression">B1 * 2 + B2</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
      // 4. Unknown dialect
      R"vrt(<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset"><Input><SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/expr_err_src.tif</SourceFilename></Input><ProcessingSteps><Step name="expr"><Algorithm>Expression</Algorithm><Argument name="dialect">unknown_dialect</Argument><Argument name="expression">B1 * 2 + B2</Argument></Step></ProcessingSteps></VRTDataset>)vrt",
  };

  for (size_t i = 0; i < std::size(kFailingVrtXmls); ++i) {
    SCOPED_TRACE(i);
    CPLErrorReset();
    std::string xml(kFailingVrtXmls[i]);
    EXPECT_EQ(nullptr, OpenRasterQuiet(xml.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  CPLErrorReset();
}

// =========================================================================
// Multi-step chaining and SetOutputValuesForInNoDataAndOutNoData coverage
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, MultiStepChaining) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  constexpr double kNoData[1] = {-999.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/chain_src.tif", 2, 2, 1,
             GDT_Float64, kGt, kNoData, {{10.0, 20.0, -999.0, 30.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/chain_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="step1">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0,10:100,20:200,30:300</Argument>
    </Step>
    <Step name="step2">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">5,0.5</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> out(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                               2, 2, GDT_Float64, 0, 0));

  EXPECT_DOUBLE_EQ(55.0, out[0]);
  EXPECT_DOUBLE_EQ(105.0, out[1]);
  EXPECT_DOUBLE_EQ(-999.0, out[2]);
  EXPECT_DOUBLE_EQ(155.0, out[3]);
}

TEST_F(VrtProcessedDatasetFunctionsTest, NonFinalStepsCoverage) {
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_src.tif", 2, 2,
             3, GDT_Float64, kGt, nullptr,
             {{100, 200, 50, 50}, {150, 180, 70, 50}, {120, 190, 60, 50}});
  CreateTiff("/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif", 2, 2,
             1, GDT_Float32, kGt, nullptr, {{250, 250, 250, 250}});

  // 1. BandAffineCombination as non-final step followed by LUT
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="step1">
      <Algorithm>BandAffineCombination</Algorithm>
      <Argument name="coefficients_1">0,1,0,0</Argument>
      <Argument name="coefficients_2">0,0,1,0</Argument>
      <Argument name="coefficients_3">0,0,0,1</Argument>
    </Step>
    <Step name="step2">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0,100:1000</Argument>
      <Argument name="lut_2">0:0,100:1000</Argument>
      <Argument name="lut_3">0:0,100:1000</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(3, vrt_ds->GetRasterCount());
  }

  // 2. LocalScaleOffset as non-final step followed by LUT
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="step1">
      <Algorithm>LocalScaleOffset</Algorithm>
      <Argument name="gain_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="gain_dataset_band_1">1</Argument>
      <Argument name="gain_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="gain_dataset_band_2">1</Argument>
      <Argument name="gain_dataset_filename_3">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="gain_dataset_band_3">1</Argument>
      <Argument name="offset_dataset_filename_1">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="offset_dataset_band_1">1</Argument>
      <Argument name="offset_dataset_filename_2">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="offset_dataset_band_2">1</Argument>
      <Argument name="offset_dataset_filename_3">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="offset_dataset_band_3">1</Argument>
    </Step>
    <Step name="step2">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0,100:1000</Argument>
      <Argument name="lut_2">0:0,100:1000</Argument>
      <Argument name="lut_3">0:0,100:1000</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(3, vrt_ds->GetRasterCount());
  }

  // 3. Trimming as non-final step followed by LUT
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2" subClass="VRTProcessedDataset">
  <Input>
    <SourceFilename relativeToVRT="0">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name="step1">
      <Algorithm>Trimming</Algorithm>
      <Argument name="trimming_dataset_filename">/vsimem/vrtprocesseddatasetfunctions_test/nonfinal_aux.tif</Argument>
      <Argument name="red_band">1</Argument>
      <Argument name="green_band">2</Argument>
      <Argument name="blue_band">3</Argument>
      <Argument name="top_rgb">255</Argument>
      <Argument name="tone_ceil">240</Argument>
      <Argument name="top_margin">0.05</Argument>
    </Step>
    <Step name="step2">
      <Algorithm>LUT</Algorithm>
      <Argument name="lut_1">0:0,100:1000</Argument>
      <Argument name="lut_2">0:0,100:1000</Argument>
      <Argument name="lut_3">0:0,100:1000</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(3, vrt_ds->GetRasterCount());
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover muparser and exprtk expression evaluation dialects in
// vrtprocesseddatasetfunctions.cpp if enabled in //third_party/gdal/BUILD.
