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
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

CPLErr MyImageReadFunc(void* /*hCBData*/, int /*nXOff*/, int /*nYOff*/,
                       int nXSize, int nYSize, void* pData) {
  GByte* pabyData = static_cast<GByte*>(pData);
  std::fill(pabyData, pabyData + nXSize * nYSize, static_cast<GByte>(42));
  return CE_None;
}

class VRTSourcedRasterBandTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }

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

// 1. Constructors and Accessors
TEST_F(VRTSourcedRasterBandTest, ConstructorsAndAccessors) {
  VRTDataset ds(20, 30);

  // Constructor 1: (ds, band)
  VRTSourcedRasterBand band1(&ds, 1);
  EXPECT_TRUE(band1.IsSourcedRasterBand());
  EXPECT_EQ(band1.GetXSize(), 20);
  EXPECT_EQ(band1.GetYSize(), 30);
  EXPECT_EQ(band1.GetBand(), 1);
  EXPECT_EQ(band1.GetDataset(), &ds);

  // Constructor 2: (type, xsize, ysize)
  VRTSourcedRasterBand band2(GDT_Float32, 15, 25);
  EXPECT_TRUE(band2.IsSourcedRasterBand());
  EXPECT_EQ(band2.GetRasterDataType(), GDT_Float32);
  EXPECT_EQ(band2.GetXSize(), 15);
  EXPECT_EQ(band2.GetYSize(), 25);

  // Constructor 3: (ds, band, type, xsize, ysize)
  VRTSourcedRasterBand band3(&ds, 2, GDT_Int16, 20, 30);
  EXPECT_TRUE(band3.IsSourcedRasterBand());
  EXPECT_EQ(band3.GetRasterDataType(), GDT_Int16);
  EXPECT_EQ(band3.GetBand(), 2);

  // Constructor 4: (ds, band, type, xsize, ysize, blockX, blockY)
  VRTSourcedRasterBand band4(&ds, 3, GDT_Byte, 20, 30, 10, 15);
  EXPECT_TRUE(band4.IsSourcedRasterBand());
  int nBX = 0;
  int nBY = 0;
  band4.GetBlockSize(&nBX, &nBY);
  EXPECT_EQ(nBX, 10);
  EXPECT_EQ(nBY, 15);
}

// 2. AddSourceVariantsAndCAPI
TEST_F(VRTSourcedRasterBandTest, AddSourceVariantsAndCAPI) {
  VRTDataset ds(20, 20);
  auto poBand =
      std::make_unique<VRTSourcedRasterBand>(&ds, 1, GDT_Byte, 20, 20);

  TrackFile("/vsimem/src1.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/src1.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  auto poSrcDSOpen = GDALDataset::Open("/vsimem/src1.tif", GA_ReadOnly);
  ASSERT_NE(poSrcDSOpen, nullptr);

  // AddSource raw pointer and unique_ptr
  auto poSS1 = new VRTSimpleSource();
  poSS1->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(poBand->AddSource(poSS1), CE_None);

  auto poSS2 = std::make_unique<VRTSimpleSource>();
  poSS2->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(poBand->AddSource(std::move(poSS2)), CE_None);

  // VRTAddSource C API
  auto poSS3 = new VRTSimpleSource();
  poSS3->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(VRTAddSource(reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
                         reinterpret_cast<VRTSourceH>(poSS3)),
            CE_None);

  // AddSource with NBITS metadata
  poBand->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
  auto poSS4 = std::make_unique<VRTSimpleSource>();
  poSS4->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(poBand->AddSource(std::move(poSS4)), CE_None);
  poBand->SetMetadataItem("NBITS", nullptr, "IMAGE_STRUCTURE");

  // AddSimpleSource with filename (nearest and average)
  EXPECT_EQ(poBand->AddSimpleSource("/vsimem/src1.tif", 1, 0, 0, 20, 20, 0, 0,
                                    20, 20, "nearest", 0.0),
            CE_None);
  EXPECT_EQ(poBand->AddSimpleSource("/vsimem/src1.tif", 1, 0, 0, 20, 20, 0, 0,
                                    20, 20, "average", 0.0),
            CE_None);

  // AddSimpleSource with GDALRasterBand*
  EXPECT_EQ(poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20, 20,
                                    0, 0, 20, 20, "nearest", 0.0),
            CE_None);
  EXPECT_EQ(poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20, 20,
                                    0, 0, 20, 20, "average", 0.0),
            CE_None);

  // VRTAddSimpleSource C API
  EXPECT_EQ(
      VRTAddSimpleSource(
          reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
          reinterpret_cast<GDALRasterBandH>(poSrcDSOpen->GetRasterBand(1)), 0,
          0, 20, 20, 0, 0, 20, 20, nullptr, VRT_NODATA_UNSET),
      CE_None);

  // AddMaskBandSource
  EXPECT_EQ(poBand->AddMaskBandSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20,
                                      20, 0, 0, 20, 20),
            CE_None);

  // AddComplexSource with filename and band
  EXPECT_EQ(poBand->AddComplexSource("/vsimem/src1.tif", 1, 0, 0, 20, 20, 0, 0,
                                     20, 20, 1.0, 2.0, 0.0, 0),
            CE_None);
  EXPECT_EQ(poBand->AddComplexSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20,
                                     20, 0, 0, 20, 20, 1.0, 2.0, 0.0, 0),
            CE_None);

  // VRTAddComplexSource C API
  EXPECT_EQ(
      VRTAddComplexSource(
          reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
          reinterpret_cast<GDALRasterBandH>(poSrcDSOpen->GetRasterBand(1)), 0,
          0, 20, 20, 0, 0, 20, 20, 1.0, 2.0, 0.0),
      CE_None);

  // AddFuncSource and VRTAddFuncSource C API
  EXPECT_EQ(poBand->AddFuncSource(MyImageReadFunc, nullptr, 0.0), CE_None);
  EXPECT_EQ(
      VRTAddFuncSource(reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
                       MyImageReadFunc, nullptr, 0.0),
      CE_None);

  poBand.reset();
  GDALClose(poSrcDSOpen);
}

// 3. XMLInit and SerializeToXML
TEST_F(VRTSourcedRasterBandTest, XMLInitAndSerializeToXML) {
  TrackFile("/vsimem/xml_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/xml_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  const std::string vrt_xml =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/xml_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "    </SimpleSource>\n"
      "    <ComplexSource>\n"
      "      <SourceFilename>/vsimem/xml_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <ScaleOffset>1.0</ScaleOffset>\n"
      "      <ScaleRatio>2.0</ScaleRatio>\n"
      "      <NODATA>0</NODATA>\n"
      "    </ComplexSource>\n"
      "    <AveragedSource>\n"
      "      <SourceFilename>/vsimem/xml_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "    </AveragedSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<VRTDataset>(
      VRTDataset::OpenXML(vrt_xml.c_str(), nullptr, GA_ReadOnly));
  ASSERT_NE(poDS, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(poDS->GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);
  EXPECT_EQ(poBand->m_papoSources.size(), 3);

  bool bWarned = false;
  size_t nAccRAM = 0;
  CPLXMLNode* psTree = poBand->SerializeToXML(nullptr, bWarned, nAccRAM);
  ASSERT_NE(psTree, nullptr);
  CPLDestroyXMLNode(psTree);

  // XMLInit with invalid source element
  const std::string bad_vrt =
      "<VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "  <UnknownSourceTag/>\n"
      "</VRTRasterBand>\n";
  CPLXMLNode* psBadTree = CPLParseXMLString(bad_vrt.c_str());
  ASSERT_NE(psBadTree, nullptr);
  VRTMapSharedResources mapRes;
  VRTSourcedRasterBand testBand(poDS.get(), 1);
  EXPECT_EQ(testBand.XMLInit(psBadTree, nullptr, mapRes), CE_None);
  CPLDestroyXMLNode(psBadTree);
}

// 4. SkipBufferInitializationAndBufferInit
TEST_F(VRTSourcedRasterBandTest, SkipBufferInitializationAndBufferInit) {
  TrackFile("/vsimem/buf_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/buf_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  std::vector<GByte> line(20, 100);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poSrcDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // Add 1 simple source covering the full extent
  poBand->AddSimpleSource("/vsimem/buf_src.tif", 1, 0, 0, 20, 20, 0, 0, 20, 20);

  // Test buffer initialization variations
  // 1. SkipBufferInitialization is true for 1 source covering full extent
  std::vector<GByte> outBuf(400, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 20, 20,
                             GDT_Byte, 1, 20, nullptr),
            CE_None);

  // 2. Zero nodata memset
  VRTDataset dsZero(20, 20);
  dsZero.AddBand(GDT_Byte, nullptr);
  auto poBandZero =
      cpl::down_cast<VRTSourcedRasterBand*>(dsZero.GetRasterBand(1));
  poBandZero->SetNoDataValue(0.0);
  EXPECT_EQ(poBandZero->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 20, 20,
                                 GDT_Byte, 1, 20, nullptr),
            CE_None);

  // 3. Int64 nodata
  VRTDataset dsInt64(20, 20);
  dsInt64.AddBand(GDT_Int64, nullptr);
  auto poBandInt64 =
      cpl::down_cast<VRTSourcedRasterBand*>(dsInt64.GetRasterBand(1));
  poBandInt64->SetNoDataValueAsInt64(static_cast<int64_t>(-999));
  std::vector<int64_t> outInt64(400, 0);
  EXPECT_EQ(poBandInt64->RasterIO(GF_Read, 0, 0, 20, 20, outInt64.data(), 20,
                                  20, GDT_Int64, sizeof(int64_t),
                                  20 * sizeof(int64_t), nullptr),
            CE_None);
  EXPECT_EQ(outInt64[0], -999);

  // 4. UInt64 nodata
  VRTDataset dsUInt64(20, 20);
  dsUInt64.AddBand(GDT_UInt64, nullptr);
  auto poBandUInt64 =
      cpl::down_cast<VRTSourcedRasterBand*>(dsUInt64.GetRasterBand(1));
  poBandUInt64->SetNoDataValueAsUInt64(static_cast<uint64_t>(9999999999ULL));
  std::vector<uint64_t> outUInt64(400, 0);
  EXPECT_EQ(poBandUInt64->RasterIO(GF_Read, 0, 0, 20, 20, outUInt64.data(), 20,
                                   20, GDT_UInt64, sizeof(uint64_t),
                                   20 * sizeof(uint64_t), nullptr),
            CE_None);
  EXPECT_EQ(outUInt64[0], 9999999999ULL);

  // 5. Float64 non-zero nodata
  VRTDataset dsFloat(20, 20);
  dsFloat.AddBand(GDT_Float64, nullptr);
  auto poBandFloat =
      cpl::down_cast<VRTSourcedRasterBand*>(dsFloat.GetRasterBand(1));
  poBandFloat->SetNoDataValue(-9999.0);
  std::vector<double> outFloat(400, 0.0);
  EXPECT_EQ(poBandFloat->RasterIO(GF_Read, 0, 0, 20, 20, outFloat.data(), 20,
                                  20, GDT_Float64, sizeof(double),
                                  20 * sizeof(double), nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(outFloat[0], -9999.0);

  // 6. Non-standard line spacing
  std::vector<GByte> outSpaced(20 * 40, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, outSpaced.data(), 20, 20,
                             GDT_Byte, 1, 40, nullptr),
            CE_None);

  // 7. IReadBlock
  std::vector<GByte> blockBuf(20 * 20, 0);
  EXPECT_EQ(poBand->IReadBlock(0, 0, blockBuf.data()), CE_None);
}

// 5. IRasterIOMultiThreaded
TEST_F(VRTSourcedRasterBandTest, IRasterIOMultiThreaded) {
  TrackFile("/vsimem/mt_tile1.tif");
  TrackFile("/vsimem/mt_tile2.tif");

  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poTile1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/mt_tile1.tif", 500, 1000, 1, GDT_Byte, nullptr));
  ASSERT_NE(poTile1, nullptr);
  poTile1.reset();

  auto poTile2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/mt_tile2.tif", 500, 1000, 1, GDT_Byte, nullptr));
  ASSERT_NE(poTile2, nullptr);
  poTile2.reset();

  VRTDataset ds(1000, 1000);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/mt_tile1.tif", 1, 0, 0, 500, 1000, 0, 0, 500,
                          1000);
  poBand->AddSimpleSource("/vsimem/mt_tile2.tif", 1, 0, 0, 500, 1000, 500, 0,
                          500, 1000);

  // CanMultiThreadRasterIO checks
  int nContrib = 0;
  EXPECT_TRUE(poBand->CanMultiThreadRasterIO(0, 0, 1000, 1000, nContrib));
  EXPECT_EQ(nContrib, 2);

  // Overlapping sources -> CanMultiThreadRasterIO returns false
  VRTDataset dsOverlap(1000, 1000);
  dsOverlap.AddBand(GDT_Byte, nullptr);
  auto poBandOverlap =
      cpl::down_cast<VRTSourcedRasterBand*>(dsOverlap.GetRasterBand(1));
  poBandOverlap->AddSimpleSource("/vsimem/mt_tile1.tif", 1, 0, 0, 500, 1000, 0,
                                 0, 500, 1000);
  poBandOverlap->AddSimpleSource("/vsimem/mt_tile2.tif", 1, 0, 0, 500, 1000,
                                 250, 0, 500, 1000);
  int nContribOverlap = 0;
  EXPECT_FALSE(
      poBandOverlap->CanMultiThreadRasterIO(0, 0, 1000, 1000, nContribOverlap));

  // Multi-threaded RasterIO execution
  CPLSetConfigOption("VRT_NUM_THREADS", "2");
  std::vector<GByte> outBuf(1000 * 1000, 0);
  GDALRasterIOExtraArg sMTArg;
  INIT_RASTERIO_EXTRA_ARG(sMTArg);
  sMTArg.bFloatingPointWindowValidity = TRUE;
  sMTArg.dfXOff = 0.0;
  sMTArg.dfYOff = 0.0;
  sMTArg.dfXSize = 1000.0;
  sMTArg.dfYSize = 1000.0;
  sMTArg.pfnProgress = GDALDummyProgress;

  EXPECT_EQ(poBand->IRasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(), 1000,
                              1000, GDT_Byte, 1, 1000, &sMTArg),
            CE_None);
  CPLSetConfigOption("VRT_NUM_THREADS", nullptr);
}

// 6. CanIRasterIOBeForwardedToEachSource
TEST_F(VRTSourcedRasterBandTest, CanIRasterIOBeForwardedToEachSource) {
  TrackFile("/vsimem/fwd_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/fwd_src.tif", 50, 50, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  VRTDataset ds(50, 50);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/fwd_src.tif", 1, 0, 0, 50, 50, 0, 0, 50, 50);

  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);

  // 1. Nearest neighbour
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 50, 50,
                                                          50, 50, &sExtraArg));

  // 2. Non-nearest neighbour (bilinear) downsampling
  sExtraArg.eResampleAlg = GRIORA_Bilinear;
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 50, 50,
                                                          25, 25, &sExtraArg));

  // 3. ComplexSource with custom resampling
  VRTDataset dsComplex(50, 50);
  dsComplex.AddBand(GDT_Byte, nullptr);
  auto poBandComplex =
      cpl::down_cast<VRTSourcedRasterBand*>(dsComplex.GetRasterBand(1));
  poBandComplex->AddComplexSource("/vsimem/fwd_src.tif", 1, 0, 0, 50, 50, 0, 0,
                                  50, 50, 0.0, 1.0, 0.0, 0);
  EXPECT_TRUE(poBandComplex->CanIRasterIOBeForwardedToEachSource(
      GF_Read, 0, 0, 50, 50, 25, 25, &sExtraArg));
}

// 7. GetDataCoverageStatus
TEST_F(VRTSourcedRasterBandTest, GetDataCoverageStatus) {
  TrackFile("/vsimem/cov_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cov_src.tif", 50, 50, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  VRTDataset ds(100, 100);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // Tile 1 at (0, 0, 50, 50)
  poBand->AddSimpleSource("/vsimem/cov_src.tif", 1, 0, 0, 50, 50, 0, 0, 50, 50);

  double dfPct = -1.0;
  // Inside source
  int nStatus = poBand->GetDataCoverageStatus(0, 0, 50, 50, 0, &dfPct);
  EXPECT_NE(nStatus & GDAL_DATA_COVERAGE_STATUS_DATA, 0);

  // Outside source
  dfPct = -1.0;
  nStatus = poBand->GetDataCoverageStatus(60, 60, 40, 40, 0, &dfPct);
  EXPECT_NE(nStatus & GDAL_DATA_COVERAGE_STATUS_EMPTY, 0);

  // Single simple source covering full band
  VRTDataset dsFull(50, 50);
  dsFull.AddBand(GDT_Byte, nullptr);
  auto poBandFull =
      cpl::down_cast<VRTSourcedRasterBand*>(dsFull.GetRasterBand(1));
  poBandFull->AddSimpleSource("/vsimem/cov_src.tif", 1, 0, 0, 50, 50, 0, 0, 50,
                              50);
  dfPct = -1.0;
  nStatus = poBandFull->GetDataCoverageStatus(0, 0, 50, 50, 0, &dfPct);
  EXPECT_NE(nStatus & GDAL_DATA_COVERAGE_STATUS_DATA, 0);
}

// 8. MinMaxAndStatistics
TEST_F(VRTSourcedRasterBandTest, MinMaxAndStatistics) {
  TrackFile("/vsimem/stats_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/stats_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  std::vector<GByte> line(20, 50);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poSrcDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  int anOvr[] = {2};
  EXPECT_EQ(poSrcDS->BuildOverviews("NEAREST", 1, anOvr, 0, nullptr, nullptr,
                                    nullptr),
            CE_None);
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/stats_src.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);

  int bSuccess = FALSE;
  double dfMin = poBand->GetMinimum(&bSuccess);
  (void)dfMin;

  bSuccess = FALSE;
  double dfMax = poBand->GetMaximum(&bSuccess);
  (void)dfMax;

  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(poBand->ComputeRasterMinMax(FALSE, adfMinMax), CE_None);
  EXPECT_DOUBLE_EQ(adfMinMax[0], 50.0);
  EXPECT_DOUBLE_EQ(adfMinMax[1], 50.0);

  double dfSMin = 0.0;
  double dfSMax = 0.0;
  double dfSMean = 0.0;
  double dfSStdDev = 0.0;
  EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfSMin, &dfSMax, &dfSMean,
                                      &dfSStdDev, GDALDummyProgress, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(dfSMin, 50.0);
  EXPECT_DOUBLE_EQ(dfSMax, 50.0);
  EXPECT_DOUBLE_EQ(dfSMean, 50.0);
  EXPECT_DOUBLE_EQ(dfSStdDev, 0.0);

  // ComputeStatistics with bApproxOK = TRUE
  EXPECT_EQ(poBand->ComputeStatistics(TRUE, &dfSMin, &dfSMax, &dfSMean,
                                      &dfSStdDev, nullptr, nullptr),
            CE_None);
}

// 9. GetHistogram
TEST_F(VRTSourcedRasterBandTest, GetHistogram) {
  TrackFile("/vsimem/hist_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/hist_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  std::vector<GByte> line(20, 10);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poSrcDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/hist_src.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);

  std::vector<GUIntBig> hist(256, 0);
  EXPECT_EQ(poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE, FALSE,
                                 GDALDummyProgress, nullptr),
            CE_None);
  EXPECT_EQ(hist[10], 400);

  // Multiple sources fallback path
  poBand->AddSimpleSource("/vsimem/hist_src.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);
  std::vector<GUIntBig> hist2(256, 0);
  EXPECT_EQ(poBand->GetHistogram(0.0, 255.0, 256, hist2.data(), FALSE, FALSE,
                                 nullptr, nullptr),
            CE_None);
}

// 10. MetadataAndLocationInfo
TEST_F(VRTSourcedRasterBandTest, MetadataAndLocationInfo) {
  TrackFile("/vsimem/loc_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/loc_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  double gtCoeffs[6] = {0.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  ds.SetGeoTransform(GDALGeoTransform(gtCoeffs));

  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/loc_src.tif", 1, 0, 0, 20, 20, 0, 0, 20, 20);
  static_cast<VRTSimpleSource*>(poBand->m_papoSources[0].get())
      ->SetSourceDatasetName("/vsimem/loc_src.tif", false);

  // GetMetadataDomainList
  char** papszDomains = poBand->GetMetadataDomainList();
  EXPECT_NE(papszDomains, nullptr);
  EXPECT_GE(CSLFindString(papszDomains, "LocationInfo"), 0);
  CSLDestroy(papszDomains);

  // LocationInfo queries
  const char* pszLocInfo = poBand->GetMetadataItem("Pixel_5_5", "LocationInfo");
  ASSERT_NE(pszLocInfo, nullptr);
  EXPECT_NE(strstr(pszLocInfo, "<File>/vsimem/loc_src.tif</File>"), nullptr);

  const char* pszGeoLocInfo =
      poBand->GetMetadataItem("GeoPixel_5.5_14.5", "LocationInfo");
  ASSERT_NE(pszGeoLocInfo, nullptr);
  EXPECT_NE(strstr(pszGeoLocInfo, "<File>/vsimem/loc_src.tif</File>"), nullptr);

  // LocationInfo edge cases
  EXPECT_EQ(poBand->GetMetadataItem("Pixel_bad", "LocationInfo"), nullptr);
  EXPECT_EQ(poBand->GetMetadataItem("Pixel_100_100", "LocationInfo"), nullptr);
  EXPECT_EQ(poBand->GetMetadataItem("GeoPixel_bad", "LocationInfo"), nullptr);

  // vrt_sources domain
  char** papszSources = poBand->GetMetadata("vrt_sources");
  ASSERT_NE(papszSources, nullptr);
  EXPECT_GE(CSLCount(papszSources), 1);

  // SetMetadataItem in new_vrt_sources
  const std::string new_src_xml =
      "<SimpleSource>\n"
      "  <SourceFilename>/vsimem/loc_src.tif</SourceFilename>\n"
      "  <SourceBand>1</SourceBand>\n"
      "</SimpleSource>\n";
  EXPECT_EQ(poBand->SetMetadataItem("new_source", new_src_xml.c_str(),
                                    "new_vrt_sources"),
            CE_None);

  // SetMetadataItem in vrt_sources (replace source_0)
  EXPECT_EQ(
      poBand->SetMetadataItem("source_0", new_src_xml.c_str(), "vrt_sources"),
      CE_None);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(
      poBand->SetMetadataItem("source_bad", new_src_xml.c_str(), "vrt_sources"),
      CE_Failure);
  CPLPopErrorHandler();

  // SetMetadata in vrt_sources and new_vrt_sources
  char** papszNewSources = nullptr;
  papszNewSources =
      CSLSetNameValue(papszNewSources, "source_0", new_src_xml.c_str());
  EXPECT_EQ(poBand->SetMetadata(papszNewSources, "vrt_sources"), CE_None);
  EXPECT_EQ(poBand->SetMetadata(papszNewSources, "new_vrt_sources"), CE_None);
  CSLDestroy(papszNewSources);
}

// 11. FileListAndClosing
TEST_F(VRTSourcedRasterBandTest, FileListAndClosing) {
  TrackFile("/vsimem/fl_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/fl_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/fl_src.tif", 1, 0, 0, 20, 20, 0, 0, 20, 20);

  char** papszFileList = nullptr;
  int nSize = 0;
  int nMaxSize = 0;
  CPLHashSet* hSetFiles =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);

  poBand->GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
  EXPECT_GE(nSize, 1);
  EXPECT_NE(CSLFindString(papszFileList, "/vsimem/fl_src.tif"), -1);

  CSLDestroy(papszFileList);
  CPLHashSetDestroy(hSetFiles);

  EXPECT_EQ(poBand->FlushCache(false), CE_None);
  EXPECT_EQ(poBand->FlushCache(true), CE_None);
  EXPECT_EQ(poBand->CloseDependentDatasets(), TRUE);
  EXPECT_EQ(poBand->CloseDependentDatasets(), FALSE);
}

// 12. RemoveCoveredSources
TEST_F(VRTSourcedRasterBandTest, RemoveCoveredSources) {
  TrackFile("/vsimem/cov1.tif");
  TrackFile("/vsimem/cov2.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poDS1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cov1.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS1, nullptr);
  poDS1.reset();
  auto poDS2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cov2.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS2, nullptr);
  poDS2.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // Source 1 is at (0, 0, 10, 10)
  poBand->AddSimpleSource("/vsimem/cov1.tif", 1, 0, 0, 10, 10, 0, 0, 10, 10);

  // Source 2 is at (0, 0, 20, 20), which completely covers source 1
  poBand->AddSimpleSource("/vsimem/cov2.tif", 1, 0, 0, 20, 20, 0, 0, 20, 20);
  EXPECT_EQ(poBand->m_papoSources.size(), 2);

  poBand->RemoveCoveredSources(nullptr);
  // Source 1 should have been removed
  EXPECT_EQ(poBand->m_papoSources.size(), 1);
}

// 13. MosaicStatisticsMinMaxHistogram
TEST_F(VRTSourcedRasterBandTest, MosaicStatisticsMinMaxHistogram) {
  TrackFile("/vsimem/mos1.tif");
  TrackFile("/vsimem/mos2.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poTile1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/mos1.tif", 10, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poTile1, nullptr);
  std::vector<GByte> line1(10, 20);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poTile1->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 10, 1, line1.data(), 10, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poTile1.reset();

  auto poTile2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/mos2.tif", 10, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poTile2, nullptr);
  std::vector<GByte> line2(10, 80);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poTile2->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 10, 1, line2.data(), 10, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poTile2.reset();

  VRTDataset dsMos(20, 20);
  dsMos.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(dsMos.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/mos1.tif", 1, 0, 0, 10, 20, 0, 0, 10, 20);
  poBand->AddSimpleSource("/vsimem/mos2.tif", 1, 0, 0, 10, 20, 10, 0, 10, 20);

  CPLSetConfigOption("VRT_NUM_THREADS", "2");

  double dfMin = 0.0;
  double dfMax = 0.0;
  double dfMean = 0.0;
  double dfStdDev = 0.0;
  EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean, &dfStdDev,
                                      GDALDummyProgress, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(dfMin, 20.0);
  EXPECT_DOUBLE_EQ(dfMax, 80.0);
  EXPECT_DOUBLE_EQ(dfMean, 50.0);

  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(poBand->ComputeRasterMinMax(FALSE, adfMinMax), CE_None);
  EXPECT_DOUBLE_EQ(adfMinMax[0], 20.0);
  EXPECT_DOUBLE_EQ(adfMinMax[1], 80.0);

  std::vector<GUIntBig> hist(256, 0);
  EXPECT_EQ(poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE, FALSE,
                                 GDALDummyProgress, nullptr),
            CE_None);
  EXPECT_EQ(hist[20], 200);
  EXPECT_EQ(hist[80], 200);

  CPLSetConfigOption("VRT_NUM_THREADS", nullptr);

  // Partial mosaic (uncovered pixels with nodata)
  VRTDataset dsPartial(30, 20);
  dsPartial.AddBand(GDT_Byte, nullptr);
  auto poBandPartial =
      cpl::down_cast<VRTSourcedRasterBand*>(dsPartial.GetRasterBand(1));
  poBandPartial->SetNoDataValue(0.0);
  poBandPartial->AddSimpleSource("/vsimem/mos1.tif", 1, 0, 0, 10, 20, 0, 0, 10,
                                 20);
  poBandPartial->AddSimpleSource("/vsimem/mos2.tif", 1, 0, 0, 10, 20, 10, 0, 10,
                                 20);

  EXPECT_EQ(poBandPartial->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                             &dfStdDev, nullptr, nullptr),
            CE_None);
  EXPECT_EQ(poBandPartial->ComputeRasterMinMax(FALSE, adfMinMax), CE_None);
  EXPECT_DOUBLE_EQ(adfMinMax[0], 20.0);
  EXPECT_DOUBLE_EQ(adfMinMax[1], 80.0);
}

// 14. OverviewsStatisticsMinMaxHistogramAndRasterIO
TEST_F(VRTSourcedRasterBandTest,
       OverviewsStatisticsMinMaxHistogramAndRasterIO) {
  TrackFile("/vsimem/ovr_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ovr_src.tif", 40, 40, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  std::vector<GByte> line(40, 30);
  for (int y = 0; y < 40; ++y) {
    EXPECT_EQ(poSrcDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 40, 1, line.data(), 40, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poSrcDS.reset();

  VRTDataset ds(40, 40);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);
  poBand->AddSimpleSource("/vsimem/ovr_src.tif", 1, 0, 0, 40, 40, 0, 0, 40, 40);

  int anOvr[] = {2};
  const char* apszOvrOptions[] = {"VIRTUAL=YES", nullptr};
  EXPECT_EQ(ds.BuildOverviews("NEAREST", 1, anOvr, 0, nullptr, nullptr, nullptr,
                              apszOvrOptions),
            CE_None);
  EXPECT_EQ(poBand->GetOverviewCount(), 1);

  // OverviewRasterIO through downsampled Read
  std::vector<GByte> outBuf(20 * 20, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 40, 40, outBuf.data(), 20, 20,
                             GDT_Byte, 1, 20, nullptr),
            CE_None);
  EXPECT_EQ(outBuf[0], 30);

  // ComputeRasterMinMax with bApproxOK = TRUE
  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(poBand->ComputeRasterMinMax(TRUE, adfMinMax), CE_None);
  EXPECT_DOUBLE_EQ(adfMinMax[0], 30.0);
  EXPECT_DOUBLE_EQ(adfMinMax[1], 30.0);

  // ComputeStatistics with bApproxOK = TRUE
  double dfMin = 0.0;
  double dfMax = 0.0;
  double dfMean = 0.0;
  double dfStdDev = 0.0;
  EXPECT_EQ(poBand->ComputeStatistics(TRUE, &dfMin, &dfMax, &dfMean, &dfStdDev,
                                      nullptr, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(dfMin, 30.0);
  EXPECT_DOUBLE_EQ(dfMax, 30.0);

  // GetHistogram with bApproxOK = TRUE
  std::vector<GUIntBig> hist(256, 0);
  EXPECT_EQ(poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE, TRUE,
                                 nullptr, nullptr),
            CE_None);
  EXPECT_GT(hist[30], 0);
}

// 15. GetMinimumMaximumBranches
TEST_F(VRTSourcedRasterBandTest, GetMinimumMaximumBranches) {
  TrackFile("/vsimem/minmax_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/minmax_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  std::vector<GByte> line(20, 0);
  line[19] = 255;
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(poSrcDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0),
              CE_None);
  }
  poSrcDS.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/minmax_src.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);

  // 1. STATISTICS_MINIMUM and STATISTICS_MAXIMUM metadata items
  poBand->SetMetadataItem("STATISTICS_MINIMUM", "5.5");
  poBand->SetMetadataItem("STATISTICS_MAXIMUM", "99.5");
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(poBand->GetMinimum(&bSuccess), 5.5);
  EXPECT_TRUE(bSuccess);
  bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(poBand->GetMaximum(&bSuccess), 99.5);
  EXPECT_TRUE(bSuccess);
  poBand->SetMetadataItem("STATISTICS_MINIMUM", nullptr);
  poBand->SetMetadataItem("STATISTICS_MAXIMUM", nullptr);

  // 2. VRT_MIN_MAX_FROM_SOURCES config option
  CPLSetConfigOption("VRT_MIN_MAX_FROM_SOURCES", "YES");
  poBand->GetMinimum(&bSuccess);
  poBand->GetMaximum(&bSuccess);
  CPLSetConfigOption("VRT_MIN_MAX_FROM_SOURCES", "NO");
  poBand->GetMinimum(&bSuccess);
  poBand->GetMaximum(&bSuccess);
  CPLSetConfigOption("VRT_MIN_MAX_FROM_SOURCES", nullptr);

  // 3. Remote /vsi filename returns false
  VRTDataset dsVsi(20, 20);
  dsVsi.AddBand(GDT_Byte, nullptr);
  auto poBandVsi =
      cpl::down_cast<VRTSourcedRasterBand*>(dsVsi.GetRasterBand(1));
  poBandVsi->AddSimpleSource("/vsicurl/http://example.com/test.tif", 1, 0, 0,
                             20, 20, 0, 0, 20, 20);
  poBandVsi->GetMinimum(&bSuccess);
  poBandVsi->GetMaximum(&bSuccess);

  // 4. Special character filename loop
  VRTDataset dsSpecial(20, 20);
  dsSpecial.AddBand(GDT_Byte, nullptr);
  auto poBandSpecial =
      cpl::down_cast<VRTSourcedRasterBand*>(dsSpecial.GetRasterBand(1));
  poBandSpecial->AddSimpleSource("nonexistent#special.tif", 1, 0, 0, 20, 20, 0,
                                 0, 20, 20);
  poBandSpecial->GetMinimum(&bSuccess);
  poBandSpecial->GetMaximum(&bSuccess);
}

// 16. CanIRasterIOBeForwardedBranchesAndComplexResampling
TEST_F(VRTSourcedRasterBandTest,
       CanIRasterIOBeForwardedBranchesAndComplexResampling) {
  TrackFile("/vsimem/fwd_branch_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(poDrv->Create(
      "/vsimem/fwd_branch_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  auto poSrcDSOpen =
      GDALDataset::Open("/vsimem/fwd_branch_src.tif", GA_ReadOnly);
  ASSERT_NE(poSrcDSOpen, nullptr);

  {
    VRTDataset ds(20, 20);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    auto poCS1 = std::make_unique<VRTComplexSource>();
    poCS1->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
    poSrcDSOpen->Reference();
    poCS1->SetResampling("bilinear");
    poCS1->SetSrcWindow(0, 0, 20, 20);
    poCS1->SetDstWindow(0, 0, 20, 20);
    EXPECT_EQ(poBand->AddSource(std::move(poCS1)), CE_None);

    GDALRasterIOExtraArg sArg;
    INIT_RASTERIO_EXTRA_ARG(sArg);

    // 1. Floating point window
    sArg.bFloatingPointWindowValidity = TRUE;
    sArg.dfXOff = 0.0;
    sArg.dfYOff = 0.0;
    sArg.dfXSize = 20.0;
    sArg.dfYSize = 20.0;
    EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 20,
                                                            20, 10, 10, &sArg));
    sArg.bFloatingPointWindowValidity = FALSE;

    // 2. IsMaskBand() causes false
    VRTDataset dsMask(20, 20);
    dsMask.AddBand(GDT_Byte, nullptr);
    auto poBandMask =
        cpl::down_cast<VRTSourcedRasterBand*>(dsMask.GetRasterBand(1));
    ASSERT_NE(poBandMask, nullptr);
    auto poCSMask = std::make_unique<VRTComplexSource>();
    poCSMask->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
    poSrcDSOpen->Reference();
    poCSMask->SetResampling("bilinear");
    poBandMask->AddSource(std::move(poCSMask));
    poBandMask->SetIsMaskBand();
    EXPECT_FALSE(poBandMask->CanIRasterIOBeForwardedToEachSource(
        GF_Read, 0, 0, 20, 20, 10, 10, &sArg));

    // 3. Nodata mismatch causes false
    poBand->SetNoDataValue(42.0);
    EXPECT_FALSE(poBand->CanIRasterIOBeForwardedToEachSource(
        GF_Read, 0, 0, 20, 20, 10, 10, &sArg));

    // Matching nodata allows it to continue
    poSrcDSOpen->GetRasterBand(1)->SetNoDataValue(42.0);
    EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 20,
                                                            20, 10, 10, &sArg));
    poBand->DeleteNoDataValue();

    // 4. Downsample read fallback path: when the request cannot be forwarded
    // to the sources, the band temporarily disables overviews, adopts the
    // sources' common resampling and delegates to GDALRasterBand::IRasterIO.
    std::vector<GByte> outBuf(100, 0);
    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 10, 10,
                               GDT_Byte, 1, 10, nullptr),
              CE_None);

    // 5. Multiple complex sources with different resampling
    auto poCS2 = std::make_unique<VRTComplexSource>();
    poCS2->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
    poSrcDSOpen->Reference();
    poCS2->SetResampling("cubic");
    poCS2->SetSrcWindow(0, 0, 20, 20);
    poCS2->SetDstWindow(0, 0, 20, 20);
    EXPECT_EQ(poBand->AddSource(std::move(poCS2)), CE_None);

    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 10, 10,
                               GDT_Byte, 1, 10, nullptr),
              CE_None);

    // 6. FuncSource causes CanIRasterIOBeForwardedToEachSource to return false
    VRTDataset dsFunc(20, 20);
    dsFunc.AddBand(GDT_Byte, nullptr);
    auto poBandFunc =
        cpl::down_cast<VRTSourcedRasterBand*>(dsFunc.GetRasterBand(1));
    poBandFunc->AddFuncSource(MyImageReadFunc, nullptr, 0.0);
    sArg.eResampleAlg = GRIORA_Bilinear;
    EXPECT_FALSE(poBandFunc->CanIRasterIOBeForwardedToEachSource(
        GF_Read, 0, 0, 20, 20, 10, 10, &sArg));
  }

  GDALClose(poSrcDSOpen);
}

// 17. CanMultiThreadRasterIOBranches
TEST_F(VRTSourcedRasterBandTest, CanMultiThreadRasterIOBranches) {
  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // FuncSource returns false
  poBand->AddFuncSource(MyImageReadFunc, nullptr, 0.0);
  int nContrib = 0;
  EXPECT_FALSE(poBand->CanMultiThreadRasterIO(0, 0, 20, 20, nContrib));

  // Two sources with duplicate dataset name returns false
  VRTDataset dsDup(20, 20);
  dsDup.AddBand(GDT_Byte, nullptr);
  auto poBandDup =
      cpl::down_cast<VRTSourcedRasterBand*>(dsDup.GetRasterBand(1));
  poBandDup->AddSimpleSource("/vsimem/dup.tif", 1, 0, 0, 10, 20, 0, 0, 10, 20);
  poBandDup->AddSimpleSource("/vsimem/dup.tif", 1, 0, 0, 10, 20, 10, 0, 10, 20);
  EXPECT_FALSE(poBandDup->CanMultiThreadRasterIO(0, 0, 20, 20, nContrib));
}

// 18. GetDataCoverageStatusAdvanced
TEST_F(VRTSourcedRasterBandTest, GetDataCoverageStatusAdvanced) {
  TrackFile("/vsimem/cov_adv1.tif");
  TrackFile("/vsimem/cov_adv2.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poDS1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cov_adv1.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS1, nullptr);
  poDS1.reset();
  auto poDS2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cov_adv2.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS2, nullptr);
  poDS2.reset();

  VRTDataset ds(40, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/cov_adv1.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);
  poBand->AddSimpleSource("/vsimem/cov_adv2.tif", 1, 0, 0, 20, 20, 20, 0, 20,
                          20);

  // Partial AOI query covering part of both sources: triggers GEOS difference
  // loop
  double dfDataPct = 0.0;
  int nStatus = poBand->GetDataCoverageStatus(10, 0, 20, 20, 0, &dfDataPct);
  EXPECT_NE(nStatus & GDAL_DATA_COVERAGE_STATUS_DATA, 0);
  EXPECT_DOUBLE_EQ(dfDataPct, 100.0);

  // Query with nMaskFlagStop
  nStatus = poBand->GetDataCoverageStatus(
      0, 0, 40, 20, GDAL_DATA_COVERAGE_STATUS_DATA, nullptr);
  EXPECT_NE(nStatus & GDAL_DATA_COVERAGE_STATUS_DATA, 0);

  // Add source with dfDstXSize = -1 and dfDstYSize = -1
  VRTDataset dsDefault(20, 20);
  dsDefault.AddBand(GDT_Byte, nullptr);
  auto poBandDef =
      cpl::down_cast<VRTSourcedRasterBand*>(dsDefault.GetRasterBand(1));
  auto poSS = std::make_unique<VRTSimpleSource>();
  poSS->SetSrcBand("/vsimem/cov_adv1.tif", 1);
  poSS->SetDstWindow(0, 0, -1, -1);
  poBandDef->AddSource(std::move(poSS));
  poBandDef->GetDataCoverageStatus(0, 0, 20, 20, 0, nullptr);
}

// 19. ConfigureSourceAndRefBranches
TEST_F(VRTSourcedRasterBandTest, ConfigureSourceAndRefBranches) {
  TrackFile("/vsimem/cfg_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cfg_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  auto poSrcDSOpen = GDALDataset::Open("/vsimem/cfg_src.tif", GA_ReadOnly);
  ASSERT_NE(poSrcDSOpen, nullptr);

  {
    VRTDataset ds(20, 20);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    // dfSrcYSize == -1 and dfDstYSize == -1 in ConfigureSource
    EXPECT_EQ(poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20,
                                      -1, 0, 0, 20, -1),
              CE_None);

    // poVRTSrcBandDataset && !poVRTSrcBandDataset->m_bCanTakeRef
    // BuildOverviews internally calls AddVirtualOverview which sets
    // m_bCanTakeRef = false on the dataset and adds a source to the overview
    // band.
    VRTDataset vrtOverview(20, 20);
    vrtOverview.AddBand(GDT_Byte, nullptr);
    int anOvr[] = {2};
    const char* apszOvrOptions[] = {"VIRTUAL=YES", nullptr};
    EXPECT_EQ(vrtOverview.BuildOverviews("NEAREST", 1, anOvr, 0, nullptr,
                                         nullptr, nullptr, apszOvrOptions),
              CE_None);
  }

  GDALClose(poSrcDSOpen);
}

// 20. IReadBlockEdgeBlocks
TEST_F(VRTSourcedRasterBandTest, IReadBlockEdgeBlocks) {
  VRTDataset ds(25, 25);
  auto poBand =
      std::make_unique<VRTSourcedRasterBand>(&ds, 1, GDT_Byte, 25, 25, 20, 20);
  std::vector<GByte> block(20 * 20, 0);
  EXPECT_EQ(poBand->IReadBlock(1, 0, block.data()), CE_None);
  EXPECT_EQ(poBand->IReadBlock(0, 1, block.data()), CE_None);
}

// 21. MetadataAndXMLInitErrorPaths
TEST_F(VRTSourcedRasterBandTest, MetadataAndXMLInitErrorPaths) {
  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // Bad XML for new_vrt_sources and vrt_sources
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(poBand->SetMetadataItem("new", "<bad", "new_vrt_sources"),
            CE_Failure);
  EXPECT_EQ(poBand->SetMetadataItem("source_0", "<bad", "vrt_sources"),
            CE_Failure);

  // Unknown source XML
  EXPECT_EQ(
      poBand->SetMetadataItem("new", "<UnknownSource/>", "new_vrt_sources"),
      CE_Failure);

  // SetMetadata with bad XML
  char* papszBadMD[] = {const_cast<char*>("source_0=<bad"), nullptr};
  EXPECT_EQ(poBand->SetMetadata(papszBadMD, "vrt_sources"), CE_Failure);

  // Standalone band without dataset
  VRTSourcedRasterBand standaloneBand(GDT_Byte, 10, 10);
  EXPECT_EQ(standaloneBand.SetMetadataItem("new", "<SimpleSource/>",
                                           "new_vrt_sources"),
            CE_Failure);
  EXPECT_EQ(standaloneBand.SetMetadataItem("source_0", "<SimpleSource/>",
                                           "vrt_sources"),
            CE_Failure);
  EXPECT_EQ(standaloneBand.SetMetadata(papszBadMD, "vrt_sources"), CE_Failure);

  // LocationInfo error paths
  EXPECT_EQ(standaloneBand.GetMetadataItem("GeoPixel_5.0_5.0", "LocationInfo"),
            nullptr);
  EXPECT_EQ(poBand->GetMetadataItem("GeoPixel_5.0_5.0", "LocationInfo"),
            nullptr);
  EXPECT_EQ(poBand->GetMetadataItem("GeoPixel_bad_coord", "LocationInfo"),
            nullptr);

  // XMLInit with unknown child tag
  const std::string bad_vrt =
      "<VRTRasterBand dataType=\"Byte\">\n"
      "  <UnknownTag/>\n"
      "</VRTRasterBand>\n";
  CPLXMLTreeCloser psTree(CPLParseXMLString(bad_vrt.c_str()));
  VRTMapSharedResources mapRes;
  EXPECT_EQ(poBand->XMLInit(psTree.get(), nullptr, mapRes), CE_None);

  // IRasterIO with GF_Write returns failure
  std::vector<GByte> buf(400, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(), 20, 20,
                             GDT_Byte, 1, 20, nullptr),
            CE_Failure);
  CPLPopErrorHandler();
}

// 22. RemoveCoveredSourcesMultipleUpper
TEST_F(VRTSourcedRasterBandTest, RemoveCoveredSourcesMultipleUpper) {
  TrackFile("/vsimem/rm_base.tif");
  TrackFile("/vsimem/rm_top.tif");
  TrackFile("/vsimem/rm_bottom.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poBase = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/rm_base.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poBase, nullptr);
  poBase.reset();

  auto poTop = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/rm_top.tif", 20, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poTop, nullptr);
  poTop.reset();

  auto poBottom = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/rm_bottom.tif", 20, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poBottom, nullptr);
  poBottom.reset();

  VRTDataset ds(20, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  // Lower source 0: (0, 0, 20, 20)
  poBand->AddSimpleSource("/vsimem/rm_base.tif", 1, 0, 0, 20, 20, 0, 0, 20, 20);
  // Upper source 1: (0, 0, 20, 10)
  poBand->AddSimpleSource("/vsimem/rm_top.tif", 1, 0, 0, 20, 10, 0, 0, 20, 10);
  // Upper source 2: (0, 10, 20, 10)
  poBand->AddSimpleSource("/vsimem/rm_bottom.tif", 1, 0, 0, 20, 10, 0, 10, 20,
                          10);
  EXPECT_EQ(poBand->m_papoSources.size(), 3);

  // Together source 1 and source 2 completely cover source 0
  poBand->RemoveCoveredSources(nullptr);
  EXPECT_EQ(poBand->m_papoSources.size(), 2);
}

TEST_F(VRTSourcedRasterBandTest, XMLInitErrorHandling) {
  VRTDataset ds(10, 10);
  VRTSourcedRasterBand band(&ds, 1);
  VRTMapSharedResources mapSharedSources;

  // Invalid data type in VRTRasterBand XML
  {
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<VRTRasterBand dataType=\"NonExistentDataType\" band=\"1\"/>"));
    ASSERT_NE(oTree.get(), nullptr);
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(band.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_Failure);
  }

  // Invalid Source XML that causes ParseSource to fail
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<VRTRasterBand dataType=\"Byte\" band=\"1\">"
                          "  <SimpleSource/>"  // Missing SourceFilename
                          "</VRTRasterBand>"));
    ASSERT_NE(oTree.get(), nullptr);
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(band.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_Failure);
  }
}

TEST_F(VRTSourcedRasterBandTest, OverviewRasterIOBranch) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poFull = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ovr_full.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poFull, nullptr);
  poFull.reset();

  auto poHalf = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ovr_half.tif", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poHalf, nullptr);
  std::vector<GByte> abyHalfData(100, 42);
  EXPECT_EQ(poHalf->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                               abyHalfData.data(), 10, 10,
                                               GDT_Byte, 1, 10, nullptr),
            CE_None);
  poHalf.reset();

  const char* pszVRTXML =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "    <SimpleSource>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ovr_full.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>"
      "    </SimpleSource>"
      "    <Overview>"
      "      <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ovr_half.tif</SourceFilename>"
      "      <SourceBand>1</SourceBand>"
      "    </Overview>"
      "  </VRTRasterBand>"
      "</VRTDataset>";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(pszVRTXML, GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(poDS, nullptr);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);
  EXPECT_EQ(poBand->GetOverviewCount(), 1);

  std::vector<GByte> abyBuf(100, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, abyBuf.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr),
            CE_None);
  EXPECT_EQ(abyBuf[0], 42);
  EXPECT_EQ(abyBuf[99], 42);

  // Also test GetHistogram, which is delegated to the overview band.
  GUIntBig anHistogram[256];
  EXPECT_EQ(poBand->GetHistogram(-0.5, 255.5, 256, anHistogram, FALSE, TRUE,
                                 nullptr, nullptr),
            CE_None);
  EXPECT_EQ(anHistogram[42], 100);
}

TEST_F(VRTSourcedRasterBandTest, OverviewMinMaxAndStatistics) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poSrc = std::unique_ptr<GDALDataset>(poDrv->Create(
      "/vsimem/stats_ovr_src.tif", 200, 200, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc, nullptr);
  std::vector<GByte> abyData(200 * 200, 150);
  for (int i = 0; i < 200 * 100; ++i) {
    abyData[i] = 50;
  }
  EXPECT_EQ(poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 200, 200,
                                              abyData.data(), 200, 200,
                                              GDT_Byte, 1, 200, nullptr),
            CE_None);
  poSrc.reset();

  VRTDataset ds(200, 200);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/stats_ovr_src.tif", 1, 0, 0, 200, 200, 0, 0,
                          200, 200);

  // Build virtual overviews: factor 2 (100x100 = 10,000 pixels > 2500
  // GDALSTAT_APPROX_NUMSAMPLES)
  const char* apszOvrOptions[] = {"VIRTUAL=YES", nullptr};
  int anOverviewList[] = {2};
  EXPECT_EQ(ds.BuildOverviews("NEAR", 1, anOverviewList, 0, nullptr, nullptr,
                              nullptr, const_cast<char**>(apszOvrOptions)),
            CE_None);
  EXPECT_EQ(poBand->GetOverviewCount(), 1);

  // Approximate min/max using overview
  double adfMinMax[2] = {0, 0};
  EXPECT_EQ(poBand->ComputeRasterMinMax(TRUE, adfMinMax), CE_None);
  EXPECT_LE(adfMinMax[0], 50.0);
  EXPECT_GE(adfMinMax[1], 150.0);

  // Approximate statistics using overview
  double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
  EXPECT_EQ(poBand->ComputeStatistics(TRUE, &dfMin, &dfMax, &dfMean, &dfStdDev,
                                      nullptr, nullptr),
            CE_None);
  EXPECT_STREQ(poBand->GetMetadataItem("STATISTICS_APPROXIMATE"), "YES");
  EXPECT_NE(poBand->GetMetadataItem("STATISTICS_VALID_PERCENT"), nullptr);
}

TEST_F(VRTSourcedRasterBandTest, ComputeRasterMinMaxBranches) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  // 1. NeedMaxValAdjustment: the per-source min/max is clamped to the
  // source's declared maximum value.
  {
    auto poSrc = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/maxval_src.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc, nullptr);
    std::vector<GByte> abyData(100, 80);
    abyData[0] = 20;
    EXPECT_EQ(poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                abyData.data(), 10, 10,
                                                GDT_Byte, 1, 10, nullptr),
              CE_None);
    poSrc->GetRasterBand(1)->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
    poSrc.reset();

    VRTDataset ds(10, 10);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    auto poSS = std::make_unique<VRTSimpleSource>();
    poSS->SetSrcBand("/vsimem/maxval_src.tif", 1);
    poSS->SetSrcWindow(0, 0, 10, 10);
    poSS->SetDstWindow(0, 0, 10, 10);
    poSS->SetMaxValue(50);  // Less than 80!
    poBand->AddSource(std::move(poSS));

    double adfMinMax[2] = {0, 0};
    EXPECT_EQ(poBand->ComputeRasterMinMax(FALSE, adfMinMax), CE_None);
    EXPECT_EQ(adfMinMax[0], 20.0);
    EXPECT_EQ(adfMinMax[1], 50.0);  // Clamped to 50!
  }

  // 2. Mosaic does not cover the whole VRT raster and the VRT has a hidden
  // nodata value: the nodata value is folded into the global min/max.
  {
    auto poSrc = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/part_src.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc, nullptr);
    std::vector<GByte> abyData(100, 100);
    EXPECT_EQ(poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                abyData.data(), 10, 10,
                                                GDT_Byte, 1, 10, nullptr),
              CE_None);
    poSrc.reset();

    VRTDataset ds(20, 20);  // 400 > 100
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    poBand->SetNoDataValue(42.0);
    poBand->SetMetadataItem("HideNoDataValue", "1");
    poBand->AddSimpleSource("/vsimem/part_src.tif", 1, 0, 0, 10, 10, 0, 0, 10,
                            10);

    double adfMinMax[2] = {0, 0};
    EXPECT_EQ(poBand->ComputeRasterMinMax(FALSE, adfMinMax), CE_None);
    EXPECT_EQ(adfMinMax[0], 42.0);
    EXPECT_EQ(adfMinMax[1], 100.0);
  }

  // 3. Fallback to ComputeStatistics() when a source has a nodata value: its
  // failure error message is captured and re-reported.
  {
    auto poSrc = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/all_nodata_src.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc, nullptr);
    std::vector<GByte> abyData(100, 0);
    EXPECT_EQ(poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                abyData.data(), 10, 10,
                                                GDT_Byte, 1, 10, nullptr),
              CE_None);
    poSrc->GetRasterBand(1)->SetNoDataValue(0.0);
    poSrc.reset();

    VRTDataset ds(10, 10);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    poBand->SetNoDataValue(0.0);
    poBand->AddSimpleSource("/vsimem/all_nodata_src.tif", 1, 0, 0, 10, 10, 0, 0,
                            10, 10);

    double adfMinMax[2] = {0, 0};
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(poBand->ComputeRasterMinMax(FALSE, adfMinMax), CE_Failure);
  }
}

TEST_F(VRTSourcedRasterBandTest, ComputeStatisticsMosaicAndThreading) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  // 1. Multi-threaded mosaic statistics: per-job progress is aggregated and
  // forwarded to the user progress callback, a source nodata value distinct
  // from the VRT one is accounted for as a constant value, and jobs are
  // submitted to a thread pool queue.
  {
    auto poSrc1 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/th_src1.tif", 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc1, nullptr);
    std::vector<GByte> abyData1(400, 50);
    abyData1[0] = 10;  // This pixel is nodata
    EXPECT_EQ(poSrc1->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20,
                                                 abyData1.data(), 20, 20,
                                                 GDT_Byte, 1, 20, nullptr),
              CE_None);
    poSrc1->GetRasterBand(1)->SetNoDataValue(10.0);
    poSrc1.reset();

    auto poSrc2 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/th_src2.tif", 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc2, nullptr);
    std::vector<GByte> abyData2(400, 60);
    EXPECT_EQ(poSrc2->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20,
                                                 abyData2.data(), 20, 20,
                                                 GDT_Byte, 1, 20, nullptr),
              CE_None);
    poSrc2.reset();

    VRTDataset ds(40, 20);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    // No nodata set on VRT band -> nodata in src1 (10.0) is treated as real
    // value
    poBand->AddSimpleSource("/vsimem/th_src1.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                            20);
    poBand->AddSimpleSource("/vsimem/th_src2.tif", 1, 0, 0, 20, 20, 20, 0, 20,
                            20);

    CPLSetConfigOption("VRT_NUM_THREADS", "2");

    int nProgressCount = 0;
    auto pfnProgress = [](double /*dfComplete*/, const char* /*pszMsg*/,
                          void* pData) -> int {
      (*static_cast<int*>(pData))++;
      return 1;
    };

    double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
    EXPECT_EQ(
        poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean, &dfStdDev,
                                  pfnProgress, &nProgressCount),
        CE_None);
    EXPECT_GT(nProgressCount, 0);
    EXPECT_EQ(dfMin, 10.0);
    EXPECT_EQ(dfMax, 60.0);

    // 2. An exact run clears a previously set STATISTICS_APPROXIMATE item.
    poBand->SetMetadataItem("STATISTICS_APPROXIMATE", "YES");
    EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, nullptr, nullptr),
              CE_None);
    EXPECT_EQ(poBand->GetMetadataItem("STATISTICS_APPROXIMATE"), nullptr);

    // 3. Fallback to GDALRasterBand::ComputeStatistics() when the VRT nodata
    // value lies within the [min, max] range of a source without nodata.
    poBand->SetNoDataValue(
        60.0);  // inside src2 [60, 60], and src2 has no nodata
    EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, nullptr, nullptr),
              CE_None);

    CPLSetConfigOption("VRT_NUM_THREADS", nullptr);
  }

  // 4. Non-simple source: the mosaic optimization is not applicable and the
  // generic GDALRasterBand::ComputeStatistics() is used.
  {
    VRTDataset ds(10, 10);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    poBand->AddComplexSource("/vsimem/th_src1.tif", 1);

    double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
    EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, nullptr, nullptr),
              CE_None);
  }

  // 5. Zero valid pixels in mosaic: global min/max are reset to 0 and
  // ComputeStatistics() fails with "no valid pixels found".
  {
    auto poSrc = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/zero_valid_src.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc, nullptr);
    std::vector<GByte> abyData(100, 0);
    EXPECT_EQ(poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                abyData.data(), 10, 10,
                                                GDT_Byte, 1, 10, nullptr),
              CE_None);
    poSrc->GetRasterBand(1)->SetNoDataValue(0.0);
    poSrc.reset();

    VRTDataset ds(10, 10);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);
    poBand->SetNoDataValue(0.0);
    poBand->AddSimpleSource("/vsimem/zero_valid_src.tif", 1, 0, 0, 10, 10, 0, 0,
                            10, 10);

    double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, nullptr, nullptr),
              CE_Failure);
  }
}

TEST_F(VRTSourcedRasterBandTest, GetMinimumMaximumSourcesAndRecursion) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  // 1. Sources with statistics: for Byte bands the source loop stops early
  // once a minimum of 0 / a maximum of 255 is found.
  {
    auto poSrc1 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/mm_src1.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc1, nullptr);
    poSrc1->GetRasterBand(1)->SetStatistics(10.0, 100.0, 50.0, 20.0);
    poSrc1.reset();

    auto poSrc2 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/mm_src2.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poSrc2, nullptr);
    poSrc2->GetRasterBand(1)->SetStatistics(0.0, 255.0, 128.0, 50.0);
    poSrc2.reset();

    VRTDataset ds(20, 10);
    ds.AddBand(GDT_Byte, nullptr);
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(poBand, nullptr);

    poBand->AddSimpleSource("/vsimem/mm_src1.tif", 1, 0, 0, 10, 10, 0, 0, 10,
                            10);
    poBand->AddSimpleSource("/vsimem/mm_src2.tif", 1, 0, 0, 10, 10, 10, 0, 10,
                            10);

    CPLSetConfigOption("VRT_MIN_MAX_FROM_SOURCES", "YES");

    int bSuccess = FALSE;
    double dfMin = poBand->GetMinimum(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_EQ(dfMin, 0.0);

    double dfMax = poBand->GetMaximum(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_EQ(dfMax, 255.0);

    CPLSetConfigOption("VRT_MIN_MAX_FROM_SOURCES", nullptr);
  }

  // 2. Recursion detection in GetMinimum, GetMaximum, GetHistogram: the
  // anti-recursion guard reports "Recursion detected" and fails the call.
  {
    const char* pszRecVRT =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">"
        "    <SimpleSource>"
        "      <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/rec_vrt.vrt</SourceFilename>"
        "      <SourceBand>1</SourceBand>"
        "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>"
        "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>"
        "    </SimpleSource>"
        "  </VRTRasterBand>"
        "</VRTDataset>";

    VSIFCloseL(VSIFileFromMemBuffer(
        "/vsimem/rec_vrt.vrt",
        const_cast<GByte*>(reinterpret_cast<const GByte*>(pszRecVRT)),
        strlen(pszRecVRT), FALSE));

    auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        "/vsimem/rec_vrt.vrt", GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(poDS, nullptr);
    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(poBand, nullptr);

    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    int bSuccess = FALSE;
    EXPECT_EQ(poBand->GetMinimum(&bSuccess), 0.0);
    EXPECT_FALSE(bSuccess);

    EXPECT_EQ(poBand->GetMaximum(&bSuccess), 255.0);
    EXPECT_FALSE(bSuccess);

    GUIntBig anHistogram[256];
    EXPECT_EQ(poBand->GetHistogram(-0.5, 255.5, 256, anHistogram, FALSE, FALSE,
                                   nullptr, nullptr),
              CE_Failure);

    poDS.reset();
    VSIUnlink("/vsimem/rec_vrt.vrt");
  }
}

TEST_F(VRTSourcedRasterBandTest, CanIRasterIOBeForwardedBranches) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poSrc1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/fwd_src1.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc1, nullptr);
  int anOverviewList[] = {2};
  EXPECT_EQ(poSrc1->BuildOverviews("NEAR", 1, anOverviewList, 0, nullptr,
                                   nullptr, nullptr),
            CE_None);
  poSrc1.reset();

  auto poSrc2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/fwd_src2.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc2, nullptr);
  poSrc2.reset();

  VRTDataset ds(40, 20);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(poBand, nullptr);

  poBand->AddSimpleSource("/vsimem/fwd_src1.tif", 1, 0, 0, 20, 20, 0, 0, 20,
                          20);
  poBand->AddSimpleSource("/vsimem/fwd_src2.tif", 1, 0, 0, 20, 20, 20, 0, 20,
                          20);

  // Request window (0, 0, 20, 20) with downsampled buffer (10, 10) and Bilinear
  // resampling:
  // - src1 has overviews, so bSourceHasOverviews is set when downsampling
  // - src2 does not intersect window (0, 0, 20, 20) -> GetSrcDstWindow returns
  // false -> source is skipped
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.eResampleAlg = GRIORA_Bilinear;

  std::vector<GByte> abyBuf(100, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 20, 20, abyBuf.data(), 10, 10,
                             GDT_Byte, 1, 10, &sExtraArg),
            CE_None);

  // Request window across both sources (0, 0, 40, 20) downsampled to (20, 10):
  // Neither source fully satisfies buffer request ->
  // bSourceFullySatisfiesRequest is cleared
  std::vector<GByte> abyBuf2(200, 0);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 40, 20, abyBuf2.data(), 20, 10,
                             GDT_Byte, 1, 20, &sExtraArg),
            CE_None);
}

}  // namespace
