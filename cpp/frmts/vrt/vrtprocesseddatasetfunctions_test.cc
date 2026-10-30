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
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class VrtProcessedDatasetFunctionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GDALVRTRegisterDefaultProcessedDatasetFuncs();
  }

  void TearDown() override {
    for (const auto& file : created_files_) {
      VSIUnlink(file.c_str());
    }
    created_files_.clear();
  }

  void TrackFile(const std::string& path) { created_files_.push_back(path); }

  void CreateTiff(const std::string& filename, int xsize, int ysize, int bands,
                  GDALDataType datatype, const double* padfGeoTransform,
                  const double* padfNoData,
                  const std::vector<std::vector<double>>& data) {
    TrackFile(filename);
    auto* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);
    auto poDS = std::unique_ptr<GDALDataset>(poDriver->Create(
        filename.c_str(), xsize, ysize, bands, datatype, nullptr));
    ASSERT_NE(poDS, nullptr);
    if (padfGeoTransform) {
      EXPECT_EQ(poDS->SetGeoTransform(const_cast<double*>(padfGeoTransform)),
                CE_None);
    }
    for (int b = 1; b <= bands; ++b) {
      auto* poBand = poDS->GetRasterBand(b);
      if (padfNoData) {
        EXPECT_EQ(poBand->SetNoDataValue(padfNoData[b - 1]), CE_None);
      }
      if (b - 1 < static_cast<int>(data.size()) && !data[b - 1].empty()) {
        EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                   const_cast<double*>(data[b - 1].data()),
                                   xsize, ysize, GDT_Float64, 0, 0),
                  CE_None);
      }
    }
  }

  std::vector<std::string> created_files_;
};

// =========================================================================
// BandAffineCombination Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, BandAffineCombinationBasic) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const std::vector<std::vector<double>> input_data = {
      {1.0, 2.0, 3.0, 4.0},     // Band 1
      {10.0, 20.0, 30.0, 40.0}  // Band 2
  };
  CreateTiff("/vsimem/affine_in.tif", 2, 2, 2, GDT_Float64, gt, nullptr,
             input_data);

  // Band 1: 10 + 1 * b1 + 2 * b2 -> for (1, 10): 10 + 1 + 20 = 31
  // Band 2: 5 + 2 * b1 - 0.5 * b2 -> for (1, 10): 5 + 2 - 5 = 2
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/affine_in.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='affine'>\n"
      "      <Algorithm>BandAffineCombination</Algorithm>\n"
      "      <Argument name='coefficients_1'>10,1,2</Argument>\n"
      "      <Argument name='coefficients_2'>5,2,-0.5</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);
  EXPECT_EQ(poVRT->GetRasterCount(), 2);

  std::vector<double> out_b1(4);
  std::vector<double> out_b2(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b1.data(), 2, 2, GDT_Float64, 0, 0),
            CE_None);
  EXPECT_EQ(poVRT->GetRasterBand(2)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b2.data(), 2, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_DOUBLE_EQ(out_b1[0], 31.0);  // 10 + 1 + 20
  EXPECT_DOUBLE_EQ(out_b1[1], 52.0);  // 10 + 2 + 40
  EXPECT_DOUBLE_EQ(out_b1[2], 73.0);  // 10 + 3 + 60
  EXPECT_DOUBLE_EQ(out_b1[3], 94.0);  // 10 + 4 + 80

  EXPECT_DOUBLE_EQ(out_b2[0], 2.0);   // 5 + 2 - 5
  EXPECT_DOUBLE_EQ(out_b2[1], -1.0);  // 5 + 4 - 10
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       BandAffineCombinationClampingAndNoData) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const double nodata[1] = {-999.0};
  const std::vector<std::vector<double>> input_data = {
      {-999.0, 5.0, 100.0, -50.0}};
  CreateTiff("/vsimem/affine_clamp.tif", 2, 2, 1, GDT_Float64, gt, nodata,
             input_data);

  // min=10, max=50, dst_nodata=-1, replacement_nodata=0
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/affine_clamp.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='affine'>\n"
      "      <Algorithm>BandAffineCombination</Algorithm>\n"
      "      <Argument name='src_nodata'>-999</Argument>\n"
      "      <Argument name='dst_nodata'>-1</Argument>\n"
      "      <Argument name='replacement_nodata'>0</Argument>\n"
      "      <Argument name='min'>10</Argument>\n"
      "      <Argument name='max'>50</Argument>\n"
      "      <Argument name='coefficients_1'>0,1</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_DOUBLE_EQ(out[0], -1.0);  // Nodata preserved
  EXPECT_DOUBLE_EQ(out[1], 10.0);  // 5 clamped up to min=10
  EXPECT_DOUBLE_EQ(out[2], 50.0);  // 100 clamped down to max=50
  EXPECT_DOUBLE_EQ(out[3], 10.0);  // -50 clamped up to min=10
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       BandAffineCombinationGetDstValueBranches) {
  // Test GetDstValue branches for Byte, Float16, Float32, Float64
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const std::vector<std::vector<double>> input_data = {{0.0, 42.0, 10.0, 20.0}};
  CreateTiff("/vsimem/affine_dt.tif", 2, 2, 1, GDT_Float64, gt, nullptr,
             input_data);

  // 1. Integer DT (Byte) with rounding match: dfVal = 0.1 -> round(0.1) == 0 ->
  // replacement_nodata=99
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_dt.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='dst_intended_datatype'>Byte</Argument>\n"
        "      <Argument name='dst_nodata'>0</Argument>\n"
        "      <Argument name='replacement_nodata'>99</Argument>\n"
        "      <Argument name='coefficients_1'>0.1,1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_None);
    EXPECT_DOUBLE_EQ(out[0], 99.0);  // round(0.1 + 0) == 0 -> replaced with 99
  }

  // 2. Float16 match
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_dt.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='dst_intended_datatype'>Float16</Argument>\n"
        "      <Argument name='dst_nodata'>42</Argument>\n"
        "      <Argument name='replacement_nodata'>123</Argument>\n"
        "      <Argument name='coefficients_1'>0,1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_None);
    EXPECT_DOUBLE_EQ(out[1], 123.0);  // 42.0 matches dst_nodata -> 123
  }

  // 3. Float32 match
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_dt.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='dst_intended_datatype'>Float32</Argument>\n"
        "      <Argument name='dst_nodata'>42</Argument>\n"
        "      <Argument name='replacement_nodata'>77</Argument>\n"
        "      <Argument name='coefficients_1'>0,1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_None);
    EXPECT_DOUBLE_EQ(out[1], 77.0);  // 42.0 matches dst_nodata -> 77
  }

  // 4. Float64 match with default replacement_nodata
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_dt.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='dst_intended_datatype'>Float64</Argument>\n"
        "      <Argument name='dst_nodata'>42</Argument>\n"
        "      <Argument name='coefficients_1'>0,1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr) << CPLGetLastErrorMsg();
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_None);
    // 42 matches dst_nodata, default replacement for 42 in Float64 is nextafter
    EXPECT_NE(out[1], 42.0);
  }
}

TEST_F(VrtProcessedDatasetFunctionsTest, BandAffineCombinationErrors) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  CreateTiff("/vsimem/affine_err.tif", 2, 2, 2, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Invalid band index <= 0
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_0'>10,1,2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Invalid band index > 65536
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_70000'>10,1,2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 3. Token count mismatch
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_1'>10,1</Argument>\n"  // Expects 3
                                                                   // tokens
                                                                   // (constant
                                                                   // + 2 bands)
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 4. Missing band in sequence (coefficients_2 provided, coefficients_1
  // missing)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_2'>10,1,2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 5. Final step band count mismatch
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <VRTRasterBand dataType='Float64' band='1'/>\n"
        "  <VRTRasterBand dataType='Float64' band='2'/>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_1'>10,1,2</Argument>\n"  // 2 bands
                                                                     // expected,
                                                                     // only 1
                                                                     // provided
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 6. Unrecognized argument
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='unknown_arg'>foo</Argument>\n"
        "      <Argument name='coefficients_1'>10,1,2</Argument>\n"
        "      <Argument name='coefficients_2'>20,3,4</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_NE(poVRT, nullptr);
  }

  // 7. Missing band in sequence when total count matches (coefficients_1 and
  // coefficients_3 for 2 bands)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/affine_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <OutputBands count='2'/>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='affine'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_1'>10,1,2</Argument>\n"
        "      <Argument name='coefficients_3'>20,3,4</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// LUT Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, LUTBasicAndLookupValueBranches) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const double nodata[1] = {-999.0};
  // Test values: -5 (< lower bound 0), 0 (exact lower), 5 (interpolated),
  // 10 (exact middle), 15 (interpolated), 20 (exact upper), 25 (> upper bound),
  // -999 (nodata)
  const std::vector<std::vector<double>> input_data = {
      {-5.0, 0.0, 5.0, 10.0, 15.0, 20.0, 25.0, -999.0}};
  CreateTiff("/vsimem/lut_in.tif", 4, 2, 1, GDT_Float64, gt, nodata,
             input_data);

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='4' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/lut_in.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='lut'>\n"
      "      <Algorithm>LUT</Algorithm>\n"
      "      <Argument name='src_nodata'>-999</Argument>\n"
      "      <Argument name='dst_nodata'>-1</Argument>\n"
      "      <Argument name='lut_1'>0:0,10:100,20:200</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out(8);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 2, out.data(),
                                              4, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_DOUBLE_EQ(out[0], 0.0);    // -5 < 0 -> clamped to output[0] = 0
  EXPECT_DOUBLE_EQ(out[1], 0.0);    // 0 == 0 -> exact match = 0
  EXPECT_DOUBLE_EQ(out[2], 50.0);   // 5 interpolated between 0 and 100 -> 50
  EXPECT_DOUBLE_EQ(out[3], 100.0);  // 10 == 10 -> exact match = 100
  EXPECT_DOUBLE_EQ(out[4],
                   150.0);  // 15 interpolated between 100 and 200 -> 150
  EXPECT_DOUBLE_EQ(out[5], 200.0);  // 20 == 20 -> exact match = 200
  EXPECT_DOUBLE_EQ(out[6], 200.0);  // 25 > 20 -> clamped to output.back() = 200
  EXPECT_DOUBLE_EQ(out[7], -1.0);   // -999 nodata -> dst_nodata = -1
}

TEST_F(VrtProcessedDatasetFunctionsTest, LUTErrors) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  CreateTiff("/vsimem/lut_err.tif", 2, 2, 2, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Invalid band index <= 0
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_0'>0:0,10:10</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Band index > nInBands
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_5'>0:0,10:10</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 3. Invalid token (not 2 values separated by colon)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_1'>0:0:0</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 4. Missing lut_1 when lut_2 provided
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_2'>0:0,10:10</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 5. Final step missing lut elements (2 bands expected, only lut_1 provided)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <OutputBands count='2'/>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_1'>0:0,10:10</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 6. Unrecognized argument warning
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lut_err.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lut'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='unknown_arg'>foo</Argument>\n"
        "      <Argument name='lut_1'>0:0,10:10</Argument>\n"
        "      <Argument name='lut_2'>0:0,10:10</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_NE(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// LocalScaleOffset Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetBasicAndNodata) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double src_nodata[1] = {-999.0};
  const std::vector<std::vector<double>> src_data = {
      {10.0, 20.0, -999.0, 50.0}};
  CreateTiff("/vsimem/lso_src.tif", 2, 2, 1, GDT_Float64, gt, src_nodata,
             src_data);

  // Gain dataset: 2x2, nodata = -888
  const double gain_nodata[1] = {-888.0};
  const std::vector<std::vector<double>> gain_data = {{2.0, 1.5, 2.0, -888.0}};
  CreateTiff("/vsimem/lso_gain.tif", 2, 2, 1, GDT_Float32, gt, gain_nodata,
             gain_data);

  // Offset dataset: 2x2, nodata = -777
  const double offset_nodata[1] = {-777.0};
  const std::vector<std::vector<double>> offset_data = {
      {5.0, 10.0, -777.0, 0.0}};
  CreateTiff("/vsimem/lso_offset.tif", 2, 2, 1, GDT_Float32, gt, offset_nodata,
             offset_data);

  // formula: src * gain - offset, then clamped to [min, max]
  // pixel 0: 10 * 2 - 5 = 15, clamped up to min=18
  // pixel 1: 20 * 1.5 - 10 = 20, clamped down to max=19
  // pixel 2: src is nodata -> -999
  // pixel 3: gain is nodata -> -999
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/lso_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='lso'>\n"
      "      <Algorithm>LocalScaleOffset</Algorithm>\n"
      "      <Argument "
      "name='gain_dataset_filename_1'>/vsimem/lso_gain.tif</Argument>\n"
      "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
      "      <Argument "
      "name='offset_dataset_filename_1'>/vsimem/lso_offset.tif</Argument>\n"
      "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
      "      <Argument name='gain_nodata'>-888</Argument>\n"
      "      <Argument name='offset_nodata'>-777</Argument>\n"
      "      <Argument name='nodata'>-999</Argument>\n"
      "      <Argument name='min'>18</Argument>\n"
      "      <Argument name='max'>19</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_DOUBLE_EQ(out[0], 18.0);
  EXPECT_DOUBLE_EQ(out[1], 19.0);
  EXPECT_DOUBLE_EQ(out[2], -999.0);
  EXPECT_DOUBLE_EQ(out[3], -999.0);
}

TEST_F(VrtProcessedDatasetFunctionsTest,
       LocalScaleOffsetRelativeToVRTAndAutoNoData) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double nodata[1] = {-1.0};
  CreateTiff("/vsimem/lso_rel_src.tif", 2, 2, 1, GDT_Float64, gt, nodata,
             {{10, 20, 30, 40}});
  CreateTiff("/vsimem/lso_rel_gain.tif", 2, 2, 1, GDT_Float32, gt, nodata,
             {{1, 1, 1, 1}});
  CreateTiff("/vsimem/lso_rel_offset.tif", 2, 2, 1, GDT_Float32, gt, nodata,
             {{0, 0, 0, 0}});

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename relativeToVRT='1'>lso_rel_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='lso'>\n"
      "      <Algorithm>LocalScaleOffset</Algorithm>\n"
      "      <Argument name='relativeToVRT'>true</Argument>\n"
      "      <Argument "
      "name='gain_dataset_filename_1'>lso_rel_gain.tif</Argument>\n"
      "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
      "      <Argument "
      "name='offset_dataset_filename_1'>lso_rel_offset.tif</Argument>\n"
      "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  TrackFile("/vsimem/test_rel.vrt");
  auto* poVSI = VSIFOpenL("/vsimem/test_rel.vrt", "wb");
  ASSERT_NE(poVSI, nullptr);
  VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), poVSI);
  VSIFCloseL(poVSI);

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      "/vsimem/test_rel.vrt", GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);
  EXPECT_DOUBLE_EQ(out[0], 10.0);
  EXPECT_DOUBLE_EQ(out[1], 20.0);
}

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetErrors) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/lso_err_src.tif", 2, 2, 1, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4}});
  CreateTiff("/vsimem/lso_err_aux.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             {{1, 1, 1, 1}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Missing gain_dataset_filename_1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Missing gain_dataset_band_1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 3. Missing offset_dataset_filename_1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 4. Missing offset_dataset_band_1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 5. Invalid band indices in argument names (band 0)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_0'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_0'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_0'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_0'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 6. 2-band dataset with one element missing in sequence
  CreateTiff("/vsimem/lso_err_src2.tif", 2, 2, 2, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}});
  // Missing gain_dataset_filename_2
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src2.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument name='gain_dataset_band_2'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "      <Argument name='offset_dataset_band_2'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // Missing gain_dataset_band_2
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src2.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "      <Argument name='offset_dataset_band_2'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // Missing offset_dataset_filename_2
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src2.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument name='gain_dataset_band_2'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "      <Argument name='offset_dataset_band_2'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // Missing offset_dataset_band_2
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src2.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument name='gain_dataset_band_2'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // CheckAllBands gap: gain_dataset_filename_2 without gain_dataset_filename_1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src2.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_2'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 6. Dataset open failure (nonexistent file)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/nonexistent.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 7. Aux dataset lacks geotransform
  {
    CreateTiff("/vsimem/lso_nogt.tif", 2, 2, 1, GDT_Float32, nullptr, nullptr,
               {{1, 1, 1, 1}});
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_nogt.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 8. Invalid band in aux dataset (band 99)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>99</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 9. Unrecognized argument
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument name='unknown_arg'>foo</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_err_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_NE(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

TEST_F(VrtProcessedDatasetFunctionsTest, LocalScaleOffsetLoadAuxDataErrors) {
  // Test LoadAuxData error branches:
  // 1. Inverted coordinates: aux has reversed X/Y geotransform
  const double gt_src[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/lso_load_src.tif", 2, 2, 1, GDT_Float64, gt_src, nullptr,
             {{1, 2, 3, 4}});

  const std::string reversed_vrt =
      "<VRTDataset rasterXSize='2' rasterYSize='2'>\n"
      "  <GeoTransform>100, -1, 0, 1000, 0, 1</GeoTransform>\n"
      "  <VRTRasterBand dataType='Float32' band='1'/>\n"
      "</VRTDataset>\n";
  TrackFile("/vsimem/lso_reversed.vrt");
  auto* poVSI = VSIFOpenL("/vsimem/lso_reversed.vrt", "wb");
  ASSERT_NE(poVSI, nullptr);
  VSIFWriteL(reversed_vrt.data(), 1, reversed_vrt.size(), poVSI);
  VSIFCloseL(poVSI);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_load_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_reversed.vrt</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_reversed.vrt</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 2, 2, out.data(), 2, 2,
                              GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
              CE_Failure);
  }

  // 2. Aux dataset out of bounds upper left (dfULPixel < -1)
  const double gt_shift_ul[6] = {200.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/lso_shift_ul.tif", 2, 2, 1, GDT_Float32, gt_shift_ul,
             nullptr, {{1, 1, 1, 1}});

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_load_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_shift_ul.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_shift_ul.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_Failure);
  }

  // 3. Aux dataset out of bounds lower right (dfLRPixel > XSize + 1)
  const double gt_small[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/lso_small.tif", 1, 1, 1, GDT_Float32, gt_small, nullptr,
             {{1}});
  CreateTiff("/vsimem/lso_load_src4.tif", 4, 4, 1, GDT_Float64, gt_src, nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});

  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='4' rasterYSize='4' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/lso_load_src4.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='lso'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/lso_small.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/lso_small.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<double> out(16);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, out.data(), 4, 4,
                              GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
              CE_Failure);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// Trimming Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingBasicRGBAndRGBFirst) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  // 4 bands: 1=Red, 2=Green, 3=Blue, 4=NIR (bRGBBandsAreFirst is true)
  const std::vector<std::vector<double>> src_data = {
      {100.0, 200.0, 50.0, -999.0},  // Red
      {150.0, 180.0, 70.0, 50.0},    // Green
      {120.0, 190.0, 60.0, 50.0},    // Blue
      {300.0, 400.0, -999.0, 50.0}
      // NIR (pixel 2 has NIR nodata while RGB is valid)
  };
  const double nodata[4] = {-999.0, -999.0, -999.0, -999.0};
  CreateTiff("/vsimem/trim_src4.tif", 2, 2, 4, GDT_Float64, gt, nodata,
             src_data);

  // Trimming dataset: 1 band, local max RGB = 250
  const std::vector<std::vector<double>> trim_data = {
      {250.0, 250.0, 250.0, 250.0}};
  CreateTiff("/vsimem/trim_aux.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             trim_data);

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/trim_src4.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='trimming'>\n"
      "      <Algorithm>Trimming</Algorithm>\n"
      "      <Argument "
      "name='trimming_dataset_filename'>/vsimem/trim_aux.tif</Argument>\n"
      "      <Argument name='red_band'>1</Argument>\n"
      "      <Argument name='green_band'>2</Argument>\n"
      "      <Argument name='blue_band'>3</Argument>\n"
      "      <Argument name='top_rgb'>255</Argument>\n"
      "      <Argument name='tone_ceil'>240</Argument>\n"
      "      <Argument name='top_margin'>0.05</Argument>\n"
      "      <Argument name='nodata'>-999</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);
  EXPECT_EQ(poVRT->GetRasterCount(), 4);

  std::vector<double> out_r(4);
  std::vector<double> out_nir(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out_r.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);
  EXPECT_EQ(poVRT->GetRasterBand(4)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_nir.data(), 2, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_GT(out_r[0], 0.0);
  EXPECT_GT(out_nir[0], 0.0);
  // Pixel 2 had NIR = -999 nodata -> all output bands get nodata
  EXPECT_DOUBLE_EQ(out_r[2], -999.0);
  EXPECT_DOUBLE_EQ(out_nir[2], -999.0);
  // Pixel 3 had Red = -999 nodata -> all output bands get nodata
  EXPECT_DOUBLE_EQ(out_r[3], -999.0);
  EXPECT_DOUBLE_EQ(out_nir[3], -999.0);
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingRGBNotFirst) {
  // Test bRGBBandsAreFirst is false (e.g. bands 2=Red, 3=Green, 4=Blue,
  // 1=Other)
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const std::vector<std::vector<double>> src_data = {
      {50.0, -999.0, 50.0, 50.0},  // Band 1: Other (pixel 1 has nodata)
      {100.0, 200.0, 50.0, 50.0},  // Band 2: Red
      {150.0, 180.0, 70.0, 50.0},  // Band 3: Green
      {120.0, 190.0, 60.0, 50.0}   // Band 4: Blue
  };
  const double nodata[4] = {-999.0, -999.0, -999.0, -999.0};
  CreateTiff("/vsimem/trim_src_notfirst.tif", 2, 2, 4, GDT_Float64, gt, nodata,
             src_data);

  const std::vector<std::vector<double>> trim_data = {
      {250.0, 250.0, -888.0, 250.0}  // Pixel 2 has trimming nodata
  };
  const double trim_nodata[1] = {-888.0};
  CreateTiff("/vsimem/trim_aux_notfirst.tif", 2, 2, 1, GDT_Float32, gt,
             trim_nodata, trim_data);

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/trim_src_notfirst.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='trimming'>\n"
      "      <Algorithm>Trimming</Algorithm>\n"
      "      <Argument "
      "name='trimming_dataset_filename'>/vsimem/trim_aux_notfirst.tif</"
      "Argument>\n"
      "      <Argument name='red_band'>2</Argument>\n"
      "      <Argument name='green_band'>3</Argument>\n"
      "      <Argument name='blue_band'>4</Argument>\n"
      "      <Argument name='top_rgb'>255</Argument>\n"
      "      <Argument name='tone_ceil'>240</Argument>\n"
      "      <Argument name='top_margin'>0.05</Argument>\n"
      "      <Argument name='nodata'>-999</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out_b1(4);
  std::vector<double> out_r(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 2, 2, out_b1.data(), 2, 2, GDT_Float64, 0, 0),
            CE_None);
  EXPECT_EQ(poVRT->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 2, 2, out_r.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);

  EXPECT_GT(out_b1[0], 0.0);
  EXPECT_GT(out_r[0], 0.0);
  // Pixel 1 had Band 1 = nodata -> all bands nodata
  EXPECT_DOUBLE_EQ(out_b1[1], -999.0);
  EXPECT_DOUBLE_EQ(out_r[1], -999.0);
  // Pixel 2 had Trimming = nodata -> all bands nodata
  EXPECT_DOUBLE_EQ(out_b1[2], -999.0);
  EXPECT_DOUBLE_EQ(out_r[2], -999.0);
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingErrors) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/trim_err_src.tif", 2, 2, 3, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}});
  CreateTiff("/vsimem/trim_err_aux.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             {{1, 1, 1, 1}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Invalid red band
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_err_aux.tif</Argument>\n"
        "      <Argument name='red_band'>0</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Duplicate bands (red == green)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_err_aux.tif</Argument>\n"
        "      <Argument name='red_band'>1</Argument>\n"
        "      <Argument name='green_band'>1</Argument>\n"
        "      <Argument name='blue_band'>3</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 3. Trimming dataset has 2 bands (must be 1)
  {
    CreateTiff("/vsimem/trim_2bands.tif", 2, 2, 2, GDT_Float32, gt, nullptr,
               {{1, 1, 1, 1}, {2, 2, 2, 2}});
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_2bands.tif</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 4. Trimming dataset open failure
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/nonexistent.tif</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 5. Trimming dataset lacks geotransform
  {
    CreateTiff("/vsimem/trim_nogt.tif", 2, 2, 1, GDT_Float32, nullptr, nullptr,
               {{1, 1, 1, 1}});
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_nogt.tif</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 6. Invalid green band (0)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_err_aux.tif</Argument>\n"
        "      <Argument name='red_band'>1</Argument>\n"
        "      <Argument name='green_band'>0</Argument>\n"
        "      <Argument name='blue_band'>3</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 7. Invalid blue band (0)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_err_aux.tif</Argument>\n"
        "      <Argument name='red_band'>1</Argument>\n"
        "      <Argument name='green_band'>2</Argument>\n"
        "      <Argument name='blue_band'>0</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 8. Unrecognized argument
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/trim_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='trimming'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument name='unknown_arg'>foo</Argument>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/trim_err_aux.tif</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_NE(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingRelativeToVRTAndNoData) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/trim_rel_src.tif", 2, 2, 3, GDT_Float64, gt, nullptr,
             {{100, 200, 50, 50}, {150, 180, 70, 50}, {120, 190, 60, 50}});
  CreateTiff("/vsimem/trim_rel_aux.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             {{250, 250, 250, 250}});

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='1'>trim_rel_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='trimming'>\n"
      "      <Algorithm>Trimming</Algorithm>\n"
      "      <Argument name='relativeToVRT'>true</Argument>\n"
      "      <Argument "
      "name='trimming_dataset_filename'>trim_rel_aux.tif</Argument>\n"
      "      <Argument name='trimming_nodata'>-777</Argument>\n"
      "      <Argument name='top_rgb'>255</Argument>\n"
      "      <Argument name='tone_ceil'>240</Argument>\n"
      "      <Argument name='top_margin'>0.05</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  TrackFile("/vsimem/test_trim_rel.vrt");
  auto* poVSI = VSIFOpenL("/vsimem/test_trim_rel.vrt", "wb");
  ASSERT_NE(poVSI, nullptr);
  VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), poVSI);
  VSIFCloseL(poVSI);

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      "/vsimem/test_trim_rel.vrt", GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);
}

TEST_F(VrtProcessedDatasetFunctionsTest, TrimmingLoadAuxDataError) {
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/trim_err_src4.tif", 4, 4, 3, GDT_Float64, gt, nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});
  CreateTiff("/vsimem/trim_small_aux.tif", 1, 1, 1, GDT_Float32, gt, nullptr,
             {{1}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='4' rasterYSize='4' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/trim_err_src4.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='trimming'>\n"
      "      <Algorithm>Trimming</Algorithm>\n"
      "      <Argument "
      "name='trimming_dataset_filename'>/vsimem/trim_small_aux.tif</Argument>\n"
      "      <Argument name='top_rgb'>255</Argument>\n"
      "      <Argument name='tone_ceil'>240</Argument>\n"
      "      <Argument name='top_margin'>0.05</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT, nullptr);
  std::vector<double> out(4 * 4 * 3);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, out.data(), 4, 4, GDT_Float64,
                            3, nullptr, 0, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();
}

// =========================================================================
// Expression Tests
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, ExpressionDialectsAndErrors) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  CreateTiff("/vsimem/expr_err_src.tif", 2, 2, 2, GDT_Float64, gt, nullptr,
             {{10.0, 20.0, 30.0, 40.0}, {1.0, 2.0, 3.0, 4.0}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. batch_size < 1
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/expr_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='expr'>\n"
        "      <Algorithm>Expression</Algorithm>\n"
        "      <Argument name='batch_size'>0</Argument>\n"
        "      <Argument name='expression'>B1</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Default dialect (muparser) when not compiled in
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/expr_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='expr'>\n"
        "      <Algorithm>Expression</Algorithm>\n"
        "      <Argument name='expression'>B1 * 2 + B2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 3. Dialect exprtk when not compiled in
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/expr_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='expr'>\n"
        "      <Algorithm>Expression</Algorithm>\n"
        "      <Argument name='dialect'>exprtk</Argument>\n"
        "      <Argument name='expression'>B1 * 2 + B2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 4. Unknown dialect
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/expr_err_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='expr'>\n"
        "      <Algorithm>Expression</Algorithm>\n"
        "      <Argument name='dialect'>unknown_dialect</Argument>\n"
        "      <Argument name='expression'>B1 * 2 + B2</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// Multi-step chaining and SetOutputValuesForInNoDataAndOutNoData coverage
// =========================================================================

TEST_F(VrtProcessedDatasetFunctionsTest, MultiStepChaining) {
  const double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const double nodata[1] = {-999.0};
  CreateTiff("/vsimem/chain_src.tif", 2, 2, 1, GDT_Float64, gt, nodata,
             {{10.0, 20.0, -999.0, 30.0}});

  // Step 1: LUT (non-final step, bIsFinalStep = false, allocates output nodata)
  // Step 2: BandAffineCombination (final step, bIsFinalStep = true)
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/chain_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='step1'>\n"
      "      <Algorithm>LUT</Algorithm>\n"
      "      <Argument name='lut_1'>0:0,10:100,20:200,30:300</Argument>\n"
      "    </Step>\n"
      "    <Step name='step2'>\n"
      "      <Algorithm>BandAffineCombination</Algorithm>\n"
      "      <Argument name='coefficients_1'>5,0.5</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> out(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);

  // 10 -> LUT -> 100 -> Affine 5 + 0.5*100 = 55
  EXPECT_DOUBLE_EQ(out[0], 55.0);
  // 20 -> LUT -> 200 -> Affine 5 + 0.5*200 = 105
  EXPECT_DOUBLE_EQ(out[1], 105.0);
  // -999 -> nodata
  EXPECT_DOUBLE_EQ(out[2], -999.0);
  // 30 -> LUT -> 300 -> Affine 5 + 0.5*300 = 155
  EXPECT_DOUBLE_EQ(out[3], 155.0);
}

TEST_F(VrtProcessedDatasetFunctionsTest, NonFinalStepsCoverage) {
  // Tests BandAffineCombination, LocalScaleOffset, and Trimming as non-final
  // steps (!bIsFinalStep)
  const double gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  CreateTiff("/vsimem/nonfinal_src.tif", 2, 2, 3, GDT_Float64, gt, nullptr,
             {{100, 200, 50, 50}, {150, 180, 70, 50}, {120, 190, 60, 50}});
  CreateTiff("/vsimem/nonfinal_aux.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             {{250, 250, 250, 250}});

  // 1. BandAffineCombination as non-final step followed by LUT
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/nonfinal_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='step1'>\n"
        "      <Algorithm>BandAffineCombination</Algorithm>\n"
        "      <Argument name='coefficients_1'>0,1,0,0</Argument>\n"
        "      <Argument name='coefficients_2'>0,0,1,0</Argument>\n"
        "      <Argument name='coefficients_3'>0,0,0,1</Argument>\n"
        "    </Step>\n"
        "    <Step name='step2'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_1'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_2'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_3'>0:0,100:1000</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_EQ(poVRT->GetRasterCount(), 3);
  }

  // 2. LocalScaleOffset as non-final step followed by LUT
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/nonfinal_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='step1'>\n"
        "      <Algorithm>LocalScaleOffset</Algorithm>\n"
        "      <Argument "
        "name='gain_dataset_filename_1'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_2'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_2'>1</Argument>\n"
        "      <Argument "
        "name='gain_dataset_filename_3'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='gain_dataset_band_3'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_1'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_1'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_2'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_2'>1</Argument>\n"
        "      <Argument "
        "name='offset_dataset_filename_3'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='offset_dataset_band_3'>1</Argument>\n"
        "    </Step>\n"
        "    <Step name='step2'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_1'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_2'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_3'>0:0,100:1000</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_EQ(poVRT->GetRasterCount(), 3);
  }

  // 3. Trimming as non-final step followed by LUT
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/nonfinal_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='step1'>\n"
        "      <Algorithm>Trimming</Algorithm>\n"
        "      <Argument "
        "name='trimming_dataset_filename'>/vsimem/nonfinal_aux.tif</Argument>\n"
        "      <Argument name='red_band'>1</Argument>\n"
        "      <Argument name='green_band'>2</Argument>\n"
        "      <Argument name='blue_band'>3</Argument>\n"
        "      <Argument name='top_rgb'>255</Argument>\n"
        "      <Argument name='tone_ceil'>240</Argument>\n"
        "      <Argument name='top_margin'>0.05</Argument>\n"
        "    </Step>\n"
        "    <Step name='step2'>\n"
        "      <Algorithm>LUT</Algorithm>\n"
        "      <Argument name='lut_1'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_2'>0:0,100:1000</Argument>\n"
        "      <Argument name='lut_3'>0:0,100:1000</Argument>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_EQ(poVRT->GetRasterCount(), 3);
  }
}

}  // namespace
}  // namespace autotest2
