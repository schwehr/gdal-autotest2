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

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class VRTPansharpenedTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }

  void TearDown() override {
    for (const auto& filename : to_unlink_) {
      VSIUnlink(filename.c_str());
      VSIUnlink((filename + ".ovr").c_str());
    }
    to_unlink_.clear();
  }

  void TrackFile(const std::string& filename) {
    to_unlink_.push_back(filename);
  }

  void CreateTiff(const std::string& filename, int xsize, int ysize, int bands,
                  GDALDataType dt, const double* gt, const char* srs_wkt,
                  const double* nodata,
                  const std::vector<std::vector<double>>& band_data,
                  int nbits = 0, const std::vector<int>& overviews = {}) {
    TrackFile(filename);
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);

    char** papszOptions = nullptr;
    if (nbits > 0 && nbits != 8 && nbits != 16 && nbits != 32) {
      papszOptions =
          CSLSetNameValue(papszOptions, "NBITS", CPLSPrintf("%d", nbits));
    }

    auto poDS = std::unique_ptr<GDALDataset>(poDriver->Create(
        filename.c_str(), xsize, ysize, bands, dt, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(poDS, nullptr);

    if (gt != nullptr) {
      EXPECT_EQ(poDS->SetGeoTransform(const_cast<double*>(gt)), CE_None);
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      srs.importFromWkt(srs_wkt);
      EXPECT_EQ(poDS->SetSpatialRef(&srs), CE_None);
    }

    for (int b = 1; b <= bands; ++b) {
      auto* poBand = poDS->GetRasterBand(b);
      if (nodata != nullptr) {
        EXPECT_EQ(poBand->SetNoDataValue(nodata[b - 1]), CE_None);
      }
      if (nbits > 0) {
        poBand->SetMetadataItem("NBITS", CPLSPrintf("%d", nbits),
                                "IMAGE_STRUCTURE");
      }
      if (b - 1 < static_cast<int>(band_data.size())) {
        const auto& data = band_data[b - 1];
        if (!data.empty()) {
          const size_t total_pixels = static_cast<size_t>(xsize) * ysize;
          const double* write_ptr = data.data();
          std::vector<double> padded_data;
          if (data.size() < total_pixels) {
            padded_data.assign(total_pixels, data[0]);
            std::copy(data.begin(), data.end(), padded_data.begin());
            write_ptr = padded_data.data();
          }
          EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                     const_cast<double*>(write_ptr), xsize,
                                     ysize, GDT_Float64, 0, 0),
                    CE_None);
        }
      }
    }

    if (!overviews.empty()) {
      EXPECT_EQ(
          poDS->BuildOverviews("NEAREST", static_cast<int>(overviews.size()),
                               const_cast<int*>(overviews.data()), 0, nullptr,
                               nullptr, nullptr),
          CE_None);
    }
  }

 private:
  std::vector<std::string> to_unlink_;
};

// =========================================================================
// C API GDALCreatePansharpenedVRT tests
// =========================================================================

TEST_F(VRTPansharpenedTest, GDALCreatePansharpenedVRTValidation) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // NULL pszXML
  EXPECT_EQ(
      GDALCreatePansharpenedVRT(nullptr, reinterpret_cast<GDALRasterBandH>(1),
                                1, reinterpret_cast<GDALRasterBandH*>(1)),
      nullptr);

  // NULL hPanchroBand
  EXPECT_EQ(GDALCreatePansharpenedVRT("<VRTDataset/>", nullptr, 1,
                                      reinterpret_cast<GDALRasterBandH*>(1)),
            nullptr);

  // NULL pahInputSpectralBands
  EXPECT_EQ(
      GDALCreatePansharpenedVRT(
          "<VRTDataset/>", reinterpret_cast<GDALRasterBandH>(1), 1, nullptr),
      nullptr);

  // Invalid XML
  EXPECT_EQ(GDALCreatePansharpenedVRT("<InvalidXML",
                                      reinterpret_cast<GDALRasterBandH>(1), 1,
                                      reinterpret_cast<GDALRasterBandH*>(1)),
            nullptr);

  CPLPopErrorHandler();
}

TEST_F(VRTPansharpenedTest, GDALCreatePansharpenedVRTSuccessAndOptions) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  // Pan: 8x8, 1 band
  std::vector<double> pan_data(64);
  for (int i = 0; i < 64; ++i) pan_data[i] = 100.0 + i;
  CreateTiff("/vsimem/api_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {pan_data});

  // Spectral: 4x4, 2 bands
  std::vector<double> spec_b1(16), spec_b2(16);
  for (int i = 0; i < 16; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff("/vsimem/api_spec.tif", 4, 4, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {spec_b1, spec_b2});

  auto poPanDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/api_pan.tif", GDAL_OF_RASTER));
  ASSERT_NE(poPanDS, nullptr);

  auto poSpecDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/api_spec.tif", GDAL_OF_RASTER));
  ASSERT_NE(poSpecDS, nullptr);

  GDALRasterBandH hPanBand = poPanDS->GetRasterBand(1);
  GDALRasterBandH ahSpecBands[2] = {poSpecDS->GetRasterBand(1),
                                    poSpecDS->GetRasterBand(2)};

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <SpectralBand dstBand='1'/>\n"
      "    <SpectralBand dstBand='2'/>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  GDALDatasetH hVRT =
      GDALCreatePansharpenedVRT(vrt_xml.c_str(), hPanBand, 2, ahSpecBands);
  ASSERT_NE(hVRT, nullptr);

  GDALDataset* poVRT = GDALDataset::FromHandle(hVRT);
  EXPECT_EQ(poVRT->GetRasterXSize(), 8);
  EXPECT_EQ(poVRT->GetRasterYSize(), 8);
  EXPECT_EQ(poVRT->GetRasterCount(), 2);

  std::vector<float> buf(64);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(),
                                              8, 8, GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(buf[0], 0.0f);

  GDALClose(hVRT);

  // Error case: More SpectralBand in XML than in source array
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const std::string bad_xml1 =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <SpectralBand dstBand='1'/>\n"
      "    <SpectralBand dstBand='2'/>\n"
      "    <SpectralBand dstBand='3'/>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";
  EXPECT_EQ(
      GDALCreatePansharpenedVRT(bad_xml1.c_str(), hPanBand, 2, ahSpecBands),
      nullptr);

  // Error case: Less SpectralBand in XML than in source array
  const std::string bad_xml2 =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <SpectralBand dstBand='1'/>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";
  EXPECT_EQ(
      GDALCreatePansharpenedVRT(bad_xml2.c_str(), hPanBand, 2, ahSpecBands),
      nullptr);
  CPLPopErrorHandler();
}

// =========================================================================
// Basic Pansharpening XML Tests
// =========================================================================

TEST_F(VRTPansharpenedTest, BasicPansharpening) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  // Pan: 8x8, 1 band
  std::vector<double> pan_data(64);
  for (int i = 0; i < 64; ++i) pan_data[i] = 100.0 + i;
  CreateTiff("/vsimem/basic_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {pan_data});

  // Spectral: 4x4, 3 bands (R, G, B)
  std::vector<double> spec_r(16), spec_g(16), spec_b(16);
  for (int i = 0; i < 16; ++i) {
    spec_r[i] = 10.0 + i;
    spec_g[i] = 20.0 + i;
    spec_b[i] = 30.0 + i;
  }
  CreateTiff("/vsimem/basic_spec.tif", 4, 4, 3, GDT_Float32, spec_gt, nullptr,
             nullptr, {spec_r, spec_g, spec_b});

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='8' rasterYSize='8' "
      "subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/basic_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/basic_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/basic_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='3'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/basic_spec.tif</SourceFilename>\n"
      "      <SourceBand>3</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  EXPECT_EQ(poVRT->GetRasterXSize(), 8);
  EXPECT_EQ(poVRT->GetRasterYSize(), 8);
  EXPECT_EQ(poVRT->GetRasterCount(), 3);

  // GetBlockSize
  auto* poPanBand =
      dynamic_cast<VRTPansharpenedRasterBand*>(poVRT->GetRasterBand(1));
  ASSERT_NE(poPanBand, nullptr);
  int bx = 0, by = 0;
  static_cast<VRTPansharpenedDataset*>(poVRT.get())->GetBlockSize(&bx, &by);
  EXPECT_EQ(bx, 512);  // default block size
  EXPECT_EQ(by, 512);

  // Metadata
  EXPECT_STREQ(poVRT->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"),
               "PIXEL");

  // GetFileList
  char** papszFiles = poVRT->GetFileList();
  EXPECT_NE(papszFiles, nullptr);
  EXPECT_GE(CSLCount(papszFiles), 2);
  CSLDestroy(papszFiles);

  // AddBand is unsupported
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(poVRT->AddBand(GDT_Float32, nullptr), CE_Failure);
  CPLPopErrorHandler();

  // Read full dataset using direct IRasterIO (pixel-interleaved path)
  std::vector<float> all_data(8 * 8 * 3);
  int panBandMap[3] = {1, 2, 3};
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 8, 8, all_data.data(), 8, 8,
                            GDT_Float32, 3, panBandMap, 3 * sizeof(float),
                            8 * 3 * sizeof(float), sizeof(float), nullptr),
            CE_None);
  EXPECT_GT(all_data[0], 0.0f);

  // Read sub-window on dataset
  std::vector<float> sub_data(4 * 4 * 3);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 2, 2, 4, 4, sub_data.data(), 4, 4,
                            GDT_Float32, 3, panBandMap, 3 * sizeof(float),
                            4 * 3 * sizeof(float), sizeof(float), nullptr),
            CE_None);

  // Write on dataset fails
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(poVRT->RasterIO(GF_Write, 0, 0, 8, 8, all_data.data(), 8, 8,
                            GDT_Float32, 3, panBandMap, 0, 0, 0, nullptr),
            CE_Failure);

  // Write on raster band fails
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 8, 8, all_data.data(), 8, 8, GDT_Float32, 0, 0),
            CE_Failure);
  CPLPopErrorHandler();

  // Read individual band
  std::vector<float> band1(64);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, band1.data(),
                                              8, 8, GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(band1[0], 0.0f);
}

// =========================================================================
// Pansharpening Options and Customization
// =========================================================================

TEST_F(VRTPansharpenedTest, OptionsAndCustomization) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  CreateTiff("/vsimem/opt_pan.tif", 8, 8, 1, GDT_UInt16, pan_gt, nullptr,
             nullptr, {{100.0}});
  CreateTiff("/vsimem/opt_spec.tif", 4, 4, 3, GDT_UInt16, spec_gt, nullptr,
             nullptr, {{10.0}, {20.0}, {30.0}});

  // Test: BlockXSize/BlockYSize, Weights, Resampling, NumThreads, BitDepth,
  // relativeToVRT
  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <BlockXSize>16</BlockXSize>\n"
      "  <BlockYSize>16</BlockYSize>\n"
      "  <PansharpeningOptions>\n"
      "    <Algorithm>WeightedBrovey</Algorithm>\n"
      "    <AlgorithmOptions>\n"
      "      <Weights>0.4,0.3,0.3</Weights>\n"
      "    </AlgorithmOptions>\n"
      "    <Resampling>Bilinear</Resampling>\n"
      "    <NumThreads>ALL_CPUS</NumThreads>\n"
      "    <BitDepth>12</BitDepth>\n"
      "    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>\n"
      "    <NoData>None</NoData>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename relativeToVRT='1'>opt_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <OpenOptions>\n"
      "        <OOI key='NUM_THREADS'>1</OOI>\n"
      "      </OpenOptions>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <OpenOptions>\n"
      "        <OOI key='NUM_THREADS'>1</OOI>\n"
      "      </OpenOptions>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='3'>\n"
      "      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>\n"
      "      <SourceBand>3</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  TrackFile("/vsimem/test_opt.vrt");
  auto* poVSI = VSIFOpenL("/vsimem/test_opt.vrt", "wb");
  ASSERT_NE(poVSI, nullptr);
  VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), poVSI);
  VSIFCloseL(poVSI);

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      "/vsimem/test_opt.vrt", GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  int bx = 0, by = 0;
  static_cast<VRTPansharpenedDataset*>(poVRT.get())->GetBlockSize(&bx, &by);
  EXPECT_EQ(bx, 16);
  EXPECT_EQ(by, 16);

  // BitDepth 12 sets NBITS to "12"
  EXPECT_STREQ(
      poVRT->GetRasterBand(1)->GetMetadataItem("NBITS", "IMAGE_STRUCTURE"),
      "12");

  // Test Serialization to XML
  char** papszMD = poVRT->GetMetadata("xml:VRT");
  ASSERT_NE(papszMD, nullptr);
  ASSERT_NE(papszMD[0], nullptr);
  const std::string serialized = papszMD[0];
  EXPECT_NE(serialized.find("<Algorithm>WeightedBrovey</Algorithm>"),
            std::string::npos);
  EXPECT_NE(serialized.find("<NumThreads>ALL_CPUS</NumThreads>"),
            std::string::npos);
  EXPECT_NE(serialized.find("<BitDepth>12</BitDepth>"), std::string::npos);
  EXPECT_NE(serialized.find("<NoData>None</NoData>"), std::string::npos);
  EXPECT_NE(serialized.find("relativeToVRT=\"1\""), std::string::npos);

  // Re-open from serialized XML saved to /vsimem
  TrackFile("/vsimem/test_opt2.vrt");
  auto* poVSI2 = VSIFOpenL("/vsimem/test_opt2.vrt", "wb");
  ASSERT_NE(poVSI2, nullptr);
  VSIFWriteL(serialized.data(), 1, serialized.size(), poVSI2);
  VSIFCloseL(poVSI2);

  auto poVRT2 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/test_opt2.vrt", GDAL_OF_RASTER));
  ASSERT_NE(poVRT2, nullptr);
}

TEST_F(VRTPansharpenedTest, OptionsNumThreadsNumericAndBitDepthInherited) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  CreateTiff("/vsimem/threads_pan.tif", 4, 4, 1, GDT_UInt16, pan_gt, nullptr,
             nullptr, {{100.0}});
  // NBITS = 11 on spectral band with UInt16
  CreateTiff("/vsimem/threads_spec.tif", 2, 2, 2, GDT_UInt16, spec_gt, nullptr,
             nullptr, {{10.0}, {20.0}}, 11);

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <NumThreads>2</NumThreads>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/threads_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/threads_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/threads_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT, nullptr);

  // Inherited NBITS from spectral dataset
  EXPECT_STREQ(
      poVRT->GetRasterBand(1)->GetMetadataItem("NBITS", "IMAGE_STRUCTURE"),
      "11");

  char** papszMD = poVRT->GetMetadata("xml:VRT");
  ASSERT_NE(papszMD, nullptr);
  EXPECT_NE(std::string(papszMD[0]).find("<NumThreads>2</NumThreads>"),
            std::string::npos);
}

// =========================================================================
// NoData Handling Tests
// =========================================================================

TEST_F(VRTPansharpenedTest, NoDataHandling) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  // 1. Both pan and spectral have matching nodata value (-999)
  const double nodata_999[2] = {-999.0, -999.0};
  CreateTiff("/vsimem/nd_pan.tif", 4, 4, 1, GDT_Float32, pan_gt, nullptr,
             nodata_999, {{100.0}});
  CreateTiff("/vsimem/nd_spec.tif", 2, 2, 2, GDT_Float32, spec_gt, nullptr,
             nodata_999, {{10.0}, {20.0}});

  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);

    int bHasNoData = FALSE;
    double dfND = poVRT->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(dfND, -999.0);
  }

  // 2. Explicit NoData=None disables nodata
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <NoData>None</NoData>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);

    int bHasNoData = FALSE;
    poVRT->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_FALSE(bHasNoData);
  }

  // 3. Pan without nodata -> no output nodata
  CreateTiff("/vsimem/nd_pan_none.tif", 4, 4, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_pan_none.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/nd_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);

    int bHasNoData = FALSE;
    poVRT->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_FALSE(bHasNoData);
  }
}

// =========================================================================
// Spatial Extent Adjustment Tests (Union, Intersection, None,
// NoneWithoutWarning)
// =========================================================================

TEST_F(VRTPansharpenedTest, SpatialExtentAdjustmentUnion) {
  // Pan extent: [0, 8] x [0, 8], pixel size 1
  const double pan_gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  CreateTiff("/vsimem/adj_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});

  // Spectral extent: [4, 12] x [0, 8], pixel size 2 (shifted horizontally by 4
  // > pixel size 2)
  const double spec_gt[6] = {4.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/adj_spec.tif", 4, 4, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {{10.0}, {20.0}});

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  // Union expands extent to [0, 12] x [0, 8] -> 12 x 8
  EXPECT_EQ(poVRT->GetRasterXSize(), 12);
  EXPECT_EQ(poVRT->GetRasterYSize(), 8);

  std::vector<float> data(12 * 8);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 12, 8, data.data(),
                                              12, 8, GDT_Float32, 0, 0),
            CE_None);
}

TEST_F(VRTPansharpenedTest, SpatialExtentAdjustmentIntersection) {
  // Pan extent: [0, 8] x [0, 8]
  const double pan_gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  CreateTiff("/vsimem/adj_int_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});

  // Spectral extent: [4, 12] x [0, 8] (shifted horizontally by 4 > pixel size
  // 2)
  const double spec_gt[6] = {4.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/adj_int_spec.tif", 4, 4, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {{10.0}, {20.0}});

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <SpatialExtentAdjustment>Intersection</SpatialExtentAdjustment>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_int_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_int_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/adj_int_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  // Intersection clips extent to [4, 8] x [0, 8] -> 4 x 8
  EXPECT_EQ(poVRT->GetRasterXSize(), 4);
  EXPECT_EQ(poVRT->GetRasterYSize(), 8);

  std::vector<float> data(4 * 8);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 8, data.data(),
                                              4, 8, GDT_Float32, 0, 0),
            CE_None);
}

TEST_F(VRTPansharpenedTest, SpatialExtentAdjustmentNoneAndNoneWithoutWarning) {
  const double pan_gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  const double spec_gt[6] = {0.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/adj_none_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});
  CreateTiff("/vsimem/adj_none_spec.tif", 4, 4, 2, GDT_Float32, spec_gt,
             nullptr, nullptr, {{10.0}, {20.0}});

  // 1. None
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <SpatialExtentAdjustment>None</SpatialExtentAdjustment>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 2. NoneWithoutWarning
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    "
        "<SpatialExtentAdjustment>NoneWithoutWarning</"
        "SpatialExtentAdjustment>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 3. Unsupported adjustment name defaults to Union with warning
  {
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    "
        "<SpatialExtentAdjustment>InvalidAdjustment</SpatialExtentAdjustment>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_none_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    CPLPopErrorHandler();
  }
}

TEST_F(VRTPansharpenedTest, SpatialExtentAdjustmentFailures) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Rotating terms error with Union adjustment
  const double pan_gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  const double spec_rot_gt[6] = {2.0, 2.0, 0.5, 8.0, 0.5, -2.0};
  CreateTiff("/vsimem/adj_fail_pan.tif", 8, 8, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});
  CreateTiff("/vsimem/adj_fail_rot.tif", 4, 4, 2, GDT_Float32, spec_rot_gt,
             nullptr, nullptr, {{10.0}, {20.0}});

  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_rot.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_rot.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  // 2. Disjoint datasets error with Intersection adjustment
  const double spec_disjoint_gt[6] = {100.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/adj_fail_disjoint.tif", 4, 4, 2, GDT_Float32,
             spec_disjoint_gt, nullptr, nullptr, {{10.0}, {20.0}});

  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <SpatialExtentAdjustment>Intersection</SpatialExtentAdjustment>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_pan.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_disjoint.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/adj_fail_disjoint.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_EQ(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// Projection and SRS Handling
// =========================================================================

TEST_F(VRTPansharpenedTest, ProjectionHandling) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  OGRSpatialReference srs4326, srs3857;
  srs4326.SetWellKnownGeogCS("WGS84");
  srs3857.importFromEPSG(3857);
  char* pszWkt4326 = nullptr;
  char* pszWkt3857 = nullptr;
  srs4326.exportToWkt(&pszWkt4326);
  srs3857.exportToWkt(&pszWkt3857);

  // 1. Pan has 4326, spectral has 3857 (mismatched projection warning)
  CreateTiff("/vsimem/srs_pan_4326.tif", 4, 4, 1, GDT_Float32, pan_gt,
             pszWkt4326, nullptr, {{100.0}});
  CreateTiff("/vsimem/srs_spec_3857.tif", 2, 2, 2, GDT_Float32, spec_gt,
             pszWkt3857, nullptr, {{10.0}, {20.0}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_pan_4326.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_3857.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_3857.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 2. Pan has 4326, spectral has none (spectral lacks projection warning)
  CreateTiff("/vsimem/srs_spec_none.tif", 2, 2, 2, GDT_Float32, spec_gt,
             nullptr, nullptr, {{10.0}, {20.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_pan_4326.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_none.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_none.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 3. Pan has none, spectral has 4326 (pan lacks projection warning)
  CreateTiff("/vsimem/srs_pan_none.tif", 4, 4, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_pan_none.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_3857.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename "
        "relativeToVRT='0'>/vsimem/srs_spec_3857.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  CPLPopErrorHandler();
  CPLFree(pszWkt4326);
  CPLFree(pszWkt3857);
}

// =========================================================================
// Caching and Block I/O Tests
// =========================================================================

TEST_F(VRTPansharpenedTest, CachingAndBlockIO) {
  // Pan: 10x10 with BlockXSize 8, BlockYSize 8 -> block (1, 1) has nReqXSize =
  // 2 < 8, nReqYSize = 2 < 8
  const double pan_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  const double spec_gt[6] = {0.0, 2.0, 0.0, 10.0, 0.0, -2.0};

  std::vector<double> pan_data(100);
  for (int i = 0; i < 100; ++i) pan_data[i] = 100.0 + i;
  CreateTiff("/vsimem/block_pan.tif", 10, 10, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {pan_data});

  std::vector<double> spec_b1(25), spec_b2(25);
  for (int i = 0; i < 25; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff("/vsimem/block_spec.tif", 5, 5, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {spec_b1, spec_b2});

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <BlockXSize>8</BlockXSize>\n"
      "  <BlockYSize>8</BlockYSize>\n"
      "  <PansharpeningOptions>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/block_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/block_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/block_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  // 1. IReadBlock on corner block (1, 1) exercises padding: nReqXSize <
  // nBlockXSize and nReqYSize < nBlockYSize
  std::vector<float> block_buf(8 * 8);
  EXPECT_EQ(poVRT->GetRasterBand(1)->ReadBlock(1, 1, block_buf.data()),
            CE_None);
  EXPECT_GT(block_buf[0], 0.0f);

  // 2. Scanline caching: reading line 0 of band 1, then line 0 of band 2 hits
  // the cache!
  std::vector<float> line_b1(10);
  std::vector<float> line_b2(10);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 10, 1, line_b1.data(), 10, 1, GDT_Float32, 0, 0),
            CE_None);
  // Subsequent read on band 2 hits m_pabyLastBufferBandRasterIO cache
  EXPECT_EQ(poVRT->GetRasterBand(2)->RasterIO(
                GF_Read, 0, 0, 10, 1, line_b2.data(), 10, 1, GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(line_b1[0], 0.0f);
  EXPECT_GT(line_b2[0], 0.0f);

  // 3. Sub-window caching: read sub-window on band 1, then same sub-window on
  // band 2
  std::vector<float> sub_b1(4);
  std::vector<float> sub_b2(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(
                GF_Read, 2, 2, 2, 2, sub_b1.data(), 2, 2, GDT_Float32, 0, 0),
            CE_None);
  EXPECT_EQ(poVRT->GetRasterBand(2)->RasterIO(
                GF_Read, 2, 2, 2, 2, sub_b2.data(), 2, 2, GDT_Float32, 0, 0),
            CE_None);
}

// =========================================================================
// Overview Tests
// =========================================================================

TEST_F(VRTPansharpenedTest, Overviews) {
  const double pan_gt[6] = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
  const double spec_gt[6] = {0.0, 2.0, 0.0, 16.0, 0.0, -2.0};

  // Pan: 16x16 with overview factor 2 (8x8)
  std::vector<double> pan_data(256);
  for (int i = 0; i < 256; ++i) pan_data[i] = 100.0 + i;
  CreateTiff("/vsimem/ovr_pan.tif", 16, 16, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {pan_data}, 0, {2});

  // Spectral: 8x8 with overview factor 2 (4x4)
  std::vector<double> spec_b1(64), spec_b2(64);
  for (int i = 0; i < 64; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff("/vsimem/ovr_spec.tif", 8, 8, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {spec_b1, spec_b2}, 0, {2});

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/ovr_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/ovr_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "    <SpectralBand dstBand='2'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/ovr_spec.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  auto* poBand = poVRT->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  // Overview count should be 1
  EXPECT_EQ(poBand->GetOverviewCount(), 1);

  // GetOverview with out-of-range indices
  EXPECT_EQ(poBand->GetOverview(-1), nullptr);
  EXPECT_EQ(poBand->GetOverview(99), nullptr);

  // GetOverview(0)
  auto* poOvrBand = poBand->GetOverview(0);
  ASSERT_NE(poOvrBand, nullptr);
  EXPECT_EQ(poOvrBand->GetXSize(), 8);
  EXPECT_EQ(poOvrBand->GetYSize(), 8);

  // Read from overview band
  std::vector<float> ovr_data(64);
  EXPECT_EQ(poOvrBand->RasterIO(GF_Read, 0, 0, 8, 8, ovr_data.data(), 8, 8,
                                GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(ovr_data[0], 0.0f);

  // Downsampled read on dataset triggering TryOverviewRasterIO
  std::vector<float> ds_data(8 * 8 * 2);
  int panBandMap[2] = {1, 2};
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 16, 16, ds_data.data(), 8, 8,
                            GDT_Float32, 2, panBandMap, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_GT(ds_data[0], 0.0f);

  // Downsampled read on band triggering TryOverviewRasterIO
  std::vector<float> ds_band_data(8 * 8);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 16, 16, ds_band_data.data(), 8, 8,
                             GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(ds_band_data[0], 0.0f);
}

TEST_F(VRTPansharpenedTest, OverviewsWithNonPansharpenBand) {
  // When dataset contains a non-VRTPansharpenedRasterBand, GetOverviewCount()
  // returns 0
  const double pan_gt[6] = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
  const double spec_gt[6] = {0.0, 2.0, 0.0, 16.0, 0.0, -2.0};
  CreateTiff("/vsimem/ovr_np_pan.tif", 16, 16, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}}, 0, {2});
  CreateTiff("/vsimem/ovr_np_spec.tif", 8, 8, 1, GDT_Float32, spec_gt, nullptr,
             nullptr, {{10.0}}, 0, {2});

  // Band 2 is an ordinary VRTSourcedRasterBand, not a pansharpened band.
  const std::string vrt_xml =
      "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
      "  <PansharpeningOptions>\n"
      "    <PanchroBand>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/ovr_np_pan.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </PanchroBand>\n"
      "    <SpectralBand dstBand='1'>\n"
      "      <SourceFilename "
      "relativeToVRT='0'>/vsimem/ovr_np_spec.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "    </SpectralBand>\n"
      "  </PansharpeningOptions>\n"
      "  <VRTRasterBand dataType='Float32' band='1' "
      "subClass='VRTPansharpenedRasterBand'/>\n"
      "  <VRTRasterBand dataType='Float32' band='2'/>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT, nullptr);
  EXPECT_EQ(poVRT->GetRasterBand(1)->GetOverviewCount(), 0);
}

// =========================================================================
// Error Handling and Validation Branches
// =========================================================================

TEST_F(VRTPansharpenedTest, ErrorBranches) {
  const double pan_gt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  const double spec_gt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};
  CreateTiff("/vsimem/err_pan.tif", 4, 4, 1, GDT_Float32, pan_gt, nullptr,
             nullptr, {{100.0}});
  CreateTiff("/vsimem/err_spec.tif", 2, 2, 2, GDT_Float32, spec_gt, nullptr,
             nullptr, {{10.0}, {20.0}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Missing PansharpeningOptions
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'/>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 2. Deprecated MSShiftX / MSShiftY
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <MSShiftX>1.0</MSShiftX>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 3. Missing PanchroBand
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 4. Empty SourceFilename in PanchroBand
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand/>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 5. Panchro file does not exist
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/nonexistent.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 6. Invalid SourceBand in PanchroBand (band 99)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "      <SourceBand>99</SourceBand>\n"
        "    </PanchroBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 7. Unsupported Algorithm
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <Algorithm>UnsupportedAlg</Algorithm>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 8. Missing geotransform on panchro dataset
  CreateTiff("/vsimem/err_pan_nogt.tif", 4, 4, 1, GDT_Float32, nullptr, nullptr,
             nullptr, {{100.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan_nogt.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 9. Missing SourceFilename on SpectralBand
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'/>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 10. Nonexistent file on SpectralBand
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/nonexistent.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 11. Missing geotransform on SpectralBand
  CreateTiff("/vsimem/err_spec_nogt.tif", 2, 2, 2, GDT_Float32, nullptr,
             nullptr, nullptr, {{10.0}, {20.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec_nogt.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 12. Inverted vertical orientation between spectral and pan
  const double spec_inverted_gt[6] = {100.0, 2.0, 0.0, -1000.0, 0.0, 2.0};
  CreateTiff("/vsimem/err_spec_inverted.tif", 2, 2, 2, GDT_Float32,
             spec_inverted_gt, nullptr, nullptr, {{10.0}, {20.0}});
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec_inverted.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 13. Inconsistent declared dimensions with panchro dataset
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='8' rasterYSize='8' "
        "subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"  // size
                                                                        // 4x4
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 14. Invalid dstBand (dstBand <= 0)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='0'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 15. Invalid SourceBand on spectral dataset (band 99)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>99</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 16. Duplicate dstBand mapping
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 17. No spectral bands defined
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 18. Hole in dstBand numbering (dstBand 1 and dstBand 3 without dstBand 2)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='3'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 19. Mismatched weights count (1 weight for 2 bands)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <AlgorithmOptions>\n"
        "      <Weights>0.5</Weights>\n"
        "    </AlgorithmOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "    <SpectralBand dstBand='2'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>2</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 20. Explicit bands: VRTPansharpenedRasterBand without corresponding
  // SpectralBand
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "  <VRTRasterBand dataType='Float32' band='1' "
        "subClass='VRTPansharpenedRasterBand'/>\n"
        "  <VRTRasterBand dataType='Float32' band='2' "
        "subClass='VRTPansharpenedRasterBand'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 21. Explicit bands: dstBand > nBands
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='3'>\n"
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "  <VRTRasterBand dataType='Float32' band='1' "
        "subClass='VRTPansharpenedRasterBand'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 22. Spectral band dimensions larger than panchro band (triggers warning)
  CreateTiff("/vsimem/err_spec_large.tif", 8, 8, 2, GDT_Float32, spec_gt,
             nullptr, nullptr, {{10.0}, {20.0}});
  {
    // err_pan.tif is 4x4; err_spec_large.tif is 8x8.
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand dstBand='1'>\n"
        "      <SourceFilename>/vsimem/err_spec_large.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 23. No spectral band mapped to an output band (triggers warning)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTPansharpenedDataset'>\n"
        "  <PansharpeningOptions>\n"
        "    <PanchroBand>\n"
        "      <SourceFilename>/vsimem/err_pan.tif</SourceFilename>\n"
        "    </PanchroBand>\n"
        "    <SpectralBand>\n"  // No dstBand attribute
        "      <SourceFilename>/vsimem/err_spec.tif</SourceFilename>\n"
        "      <SourceBand>1</SourceBand>\n"
        "    </SpectralBand>\n"
        "  </PansharpeningOptions>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  CPLPopErrorHandler();
}

}  // namespace
