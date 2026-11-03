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
// Tests the VRTSourcedRasterBand class (vrtsourcedrasterband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtsourcedrasterband.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
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

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtsourcedrasterband_test";

CPLErr MyImageReadFunc(void* /*hCBData*/, int /*nXOff*/, int /*nYOff*/,
                       int nXSize, int nYSize, void* pData) {
  GByte* pabyData = static_cast<GByte*>(pData);
  std::fill(pabyData, pabyData + nXSize * nYSize, static_cast<GByte>(42));
  return CE_None;
}

class VrtSourcedRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateByteGeoTiff(const char* filename, int xsize, int ysize,
                         std::optional<GByte> fill_value = std::nullopt) {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poDS(
        poDrv->Create(filename, xsize, ysize, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
    if (fill_value.has_value()) {
      std::vector<GByte> line(xsize, *fill_value);
      for (int y = 0; y < ysize; ++y) {
        ASSERT_EQ(CE_None, poDS->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, y, xsize, 1, line.data(), xsize, 1,
                               GDT_Byte, 0, 0));
      }
    }
    poDS->FlushCache(false);
  }
};

TEST_F(VrtSourcedRasterBandTest, ConstructorsAndAccessors) {
  VRTDataset ds(20, 30);

  // 1. Constructor (ds, band).
  VRTSourcedRasterBand band1(&ds, 1);
  EXPECT_TRUE(band1.IsSourcedRasterBand());
  EXPECT_EQ(20, band1.GetXSize());
  EXPECT_EQ(30, band1.GetYSize());
  EXPECT_EQ(1, band1.GetBand());
  EXPECT_EQ(&ds, band1.GetDataset());

  // 2. Constructor (type, xsize, ysize).
  VRTSourcedRasterBand band2(GDT_Float32, 15, 25);
  EXPECT_TRUE(band2.IsSourcedRasterBand());
  EXPECT_EQ(GDT_Float32, band2.GetRasterDataType());
  EXPECT_EQ(15, band2.GetXSize());
  EXPECT_EQ(25, band2.GetYSize());

  // 3. Constructor (ds, band, type, xsize, ysize).
  VRTSourcedRasterBand band3(&ds, 2, GDT_Int16, 20, 30);
  EXPECT_TRUE(band3.IsSourcedRasterBand());
  EXPECT_EQ(GDT_Int16, band3.GetRasterDataType());
  EXPECT_EQ(2, band3.GetBand());

  // 4. Constructor (ds, band, type, xsize, ysize, blockX, blockY).
  VRTSourcedRasterBand band4(&ds, 3, GDT_Byte, 20, 30, 10, 15);
  EXPECT_TRUE(band4.IsSourcedRasterBand());
  int nBX = 0;
  int nBY = 0;
  band4.GetBlockSize(&nBX, &nBY);
  EXPECT_EQ(10, nBX);
  EXPECT_EQ(15, nBY);
}

TEST_F(VrtSourcedRasterBandTest, AddSourceVariantsAndCAPI) {
  constexpr char kSrc1[] = "/vsimem/vrtsourcedrasterband_test/src1.tif";
  CreateByteGeoTiff(kSrc1, 20, 20);

  GDALDatasetUniquePtr poSrcDSOpen(GDALDataset::FromHandle(GDALOpenEx(
      kSrc1, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poSrcDSOpen);

  VRTDataset ds(20, 20);
  auto poBand =
      std::make_unique<VRTSourcedRasterBand>(&ds, 1, GDT_Byte, 20, 20);

  // 1. AddSource raw pointer and unique_ptr.
  auto poSS1 = std::make_unique<VRTSimpleSource>();
  poSS1->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(CE_None, poBand->AddSource(poSS1.release()));

  auto poSS2 = std::make_unique<VRTSimpleSource>();
  poSS2->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(CE_None, poBand->AddSource(std::move(poSS2)));

  // 2. VRTAddSource C API.
  auto poSS3 = std::make_unique<VRTSimpleSource>();
  poSS3->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(CE_None,
            VRTAddSource(reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
                         reinterpret_cast<VRTSourceH>(poSS3.release())));

  // 3. AddSource with NBITS metadata.
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE"));
  auto poSS4 = std::make_unique<VRTSimpleSource>();
  poSS4->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  EXPECT_EQ(CE_None, poBand->AddSource(std::move(poSS4)));
  EXPECT_EQ(CE_None,
            poBand->SetMetadataItem("NBITS", nullptr, "IMAGE_STRUCTURE"));

  // 4. AddSimpleSource with filename (nearest and average).
  EXPECT_EQ(CE_None, poBand->AddSimpleSource(kSrc1, 1, 0, 0, 20, 20, 0, 0, 20,
                                             20, "nearest", 0.0));
  EXPECT_EQ(CE_None, poBand->AddSimpleSource(kSrc1, 1, 0, 0, 20, 20, 0, 0, 20,
                                             20, "average", 0.0));

  // 5. AddSimpleSource with GDALRasterBand*.
  EXPECT_EQ(CE_None,
            poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20, 20,
                                    0, 0, 20, 20, "nearest", 0.0));
  EXPECT_EQ(CE_None,
            poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20, 20,
                                    0, 0, 20, 20, "average", 0.0));

  // 6. VRTAddSimpleSource C API.
  EXPECT_EQ(
      CE_None,
      VRTAddSimpleSource(
          reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
          reinterpret_cast<GDALRasterBandH>(poSrcDSOpen->GetRasterBand(1)), 0,
          0, 20, 20, 0, 0, 20, 20, nullptr, VRT_NODATA_UNSET));

  // 7. AddMaskBandSource.
  EXPECT_EQ(CE_None, poBand->AddMaskBandSource(poSrcDSOpen->GetRasterBand(1), 0,
                                               0, 20, 20, 0, 0, 20, 20));

  // 8. AddComplexSource with filename and band.
  EXPECT_EQ(CE_None, poBand->AddComplexSource(kSrc1, 1, 0, 0, 20, 20, 0, 0, 20,
                                              20, 1.0, 2.0, 0.0, 0));
  EXPECT_EQ(CE_None,
            poBand->AddComplexSource(poSrcDSOpen->GetRasterBand(1), 0, 0, 20,
                                     20, 0, 0, 20, 20, 1.0, 2.0, 0.0, 0));

  // 9. VRTAddComplexSource C API.
  EXPECT_EQ(
      CE_None,
      VRTAddComplexSource(
          reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
          reinterpret_cast<GDALRasterBandH>(poSrcDSOpen->GetRasterBand(1)), 0,
          0, 20, 20, 0, 0, 20, 20, 1.0, 2.0, 0.0));

  // 10. AddFuncSource and VRTAddFuncSource C API.
  EXPECT_EQ(CE_None, poBand->AddFuncSource(MyImageReadFunc, nullptr, 0.0));
  EXPECT_EQ(CE_None, VRTAddFuncSource(
                         reinterpret_cast<VRTSourcedRasterBandH>(poBand.get()),
                         MyImageReadFunc, nullptr, 0.0));
}

TEST_F(VrtSourcedRasterBandTest, XMLInitAndSerializeToXML) {
  constexpr char kXmlSrc[] = "/vsimem/vrtsourcedrasterband_test/xml_src.tif";
  CreateByteGeoTiff(kXmlSrc, 20, 20);

  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtsourcedrasterband_test/xml_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
    <ComplexSource>
      <SourceFilename>/vsimem/vrtsourcedrasterband_test/xml_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <ScaleOffset>1.0</ScaleOffset>
      <ScaleRatio>2.0</ScaleRatio>
      <NODATA>0</NODATA>
    </ComplexSource>
    <AveragedSource>
      <SourceFilename>/vsimem/vrtsourcedrasterband_test/xml_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </AveragedSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  auto poDS = VRTDataset::OpenXML(kVrtXml, nullptr, GA_ReadOnly);
  ASSERT_NE(nullptr, poDS);
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(poDS->GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(3U, poBand->m_papoSources.size());

  bool bWarned = false;
  size_t nAccRAM = 0;
  {
    CPLXMLTreeCloser psTree(poBand->SerializeToXML(nullptr, bWarned, nAccRAM));
    ASSERT_NE(nullptr, psTree.get());
  }

  // XMLInit with unrecognized source tag is ignored with CE_None.
  constexpr char kBadVrt[] = R"xml(
<VRTRasterBand dataType="Byte" band="1">
  <UnknownSourceTag/>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser psBadTree(CPLParseXMLString(kBadVrt));
  ASSERT_NE(nullptr, psBadTree.get());
  VRTMapSharedResources mapRes;
  VRTSourcedRasterBand testBand(poDS.get(), 1);
  EXPECT_EQ(CE_None, testBand.XMLInit(psBadTree.get(), nullptr, mapRes));
}

TEST_F(VrtSourcedRasterBandTest, SkipBufferInitializationAndBufferInit) {
  constexpr char kBufSrc[] = "/vsimem/vrtsourcedrasterband_test/buf_src.tif";
  CreateByteGeoTiff(kBufSrc, 20, 20, 100);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kBufSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));

  // 1. SkipBufferInitialization is true for 1 source covering full extent.
  std::vector<GByte> outBuf(400, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 20,
                                      20, GDT_Byte, 1, 20, nullptr));
  EXPECT_EQ(100, outBuf[0]);

  // 2. Zero nodata memset.
  VRTDataset dsZero(20, 20);
  ASSERT_EQ(CE_None, dsZero.AddBand(GDT_Byte, nullptr));
  auto poBandZero =
      cpl::down_cast<VRTSourcedRasterBand*>(dsZero.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandZero);
  EXPECT_EQ(CE_None, poBandZero->SetNoDataValue(0.0));
  std::fill(outBuf.begin(), outBuf.end(), 255);
  EXPECT_EQ(CE_None, poBandZero->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(),
                                          20, 20, GDT_Byte, 1, 20, nullptr));
  EXPECT_EQ(0, outBuf[0]);

  // 3. Int64 nodata.
  VRTDataset dsInt64(20, 20);
  ASSERT_EQ(CE_None, dsInt64.AddBand(GDT_Int64, nullptr));
  auto poBandInt64 =
      cpl::down_cast<VRTSourcedRasterBand*>(dsInt64.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandInt64);
  EXPECT_EQ(CE_None,
            poBandInt64->SetNoDataValueAsInt64(static_cast<int64_t>(-999)));
  std::vector<int64_t> outInt64(400, 0);
  EXPECT_EQ(CE_None,
            poBandInt64->RasterIO(GF_Read, 0, 0, 20, 20, outInt64.data(), 20,
                                  20, GDT_Int64, sizeof(int64_t),
                                  20 * sizeof(int64_t), nullptr));
  EXPECT_EQ(-999, outInt64[0]);

  // 4. UInt64 nodata.
  VRTDataset dsUInt64(20, 20);
  ASSERT_EQ(CE_None, dsUInt64.AddBand(GDT_UInt64, nullptr));
  auto poBandUInt64 =
      cpl::down_cast<VRTSourcedRasterBand*>(dsUInt64.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandUInt64);
  EXPECT_EQ(CE_None, poBandUInt64->SetNoDataValueAsUInt64(
                         static_cast<uint64_t>(9999999999ULL)));
  std::vector<uint64_t> outUInt64(400, 0);
  EXPECT_EQ(CE_None,
            poBandUInt64->RasterIO(GF_Read, 0, 0, 20, 20, outUInt64.data(), 20,
                                   20, GDT_UInt64, sizeof(uint64_t),
                                   20 * sizeof(uint64_t), nullptr));
  EXPECT_EQ(9999999999ULL, outUInt64[0]);

  // 5. Float64 non-zero nodata.
  VRTDataset dsFloat(20, 20);
  ASSERT_EQ(CE_None, dsFloat.AddBand(GDT_Float64, nullptr));
  auto poBandFloat =
      cpl::down_cast<VRTSourcedRasterBand*>(dsFloat.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandFloat);
  EXPECT_EQ(CE_None, poBandFloat->SetNoDataValue(-9999.0));
  std::vector<double> outFloat(400, 0.0);
  EXPECT_EQ(CE_None,
            poBandFloat->RasterIO(GF_Read, 0, 0, 20, 20, outFloat.data(), 20,
                                  20, GDT_Float64, sizeof(double),
                                  20 * sizeof(double), nullptr));
  EXPECT_DOUBLE_EQ(-9999.0, outFloat[0]);

  // 6. Non-standard line spacing.
  std::vector<GByte> outSpaced(20 * 40, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, outSpaced.data(),
                                      20, 20, GDT_Byte, 1, 40, nullptr));

  // 7. IReadBlock.
  std::vector<GByte> blockBuf(20 * 20, 0);
  EXPECT_EQ(CE_None, poBand->IReadBlock(0, 0, blockBuf.data()));
}

TEST_F(VrtSourcedRasterBandTest, IRasterIOMultiThreaded) {
  constexpr char kTile1[] = "/vsimem/vrtsourcedrasterband_test/mt_tile1.tif";
  constexpr char kTile2[] = "/vsimem/vrtsourcedrasterband_test/mt_tile2.tif";
  CreateByteGeoTiff(kTile1, 500, 1000);
  CreateByteGeoTiff(kTile2, 500, 1000);

  VRTDataset ds(1000, 1000);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None, poBand->AddSimpleSource(kTile1, 1, 0, 0, 500, 1000, 0, 0,
                                             500, 1000));
  ASSERT_EQ(CE_None, poBand->AddSimpleSource(kTile2, 1, 0, 0, 500, 1000, 500, 0,
                                             500, 1000));

  // 1. CanMultiThreadRasterIO checks.
  int nContrib = 0;
  EXPECT_TRUE(poBand->CanMultiThreadRasterIO(0, 0, 1000, 1000, nContrib));
  EXPECT_EQ(2, nContrib);

  // 2. Overlapping sources -> CanMultiThreadRasterIO returns false.
  VRTDataset dsOverlap(1000, 1000);
  ASSERT_EQ(CE_None, dsOverlap.AddBand(GDT_Byte, nullptr));
  auto poBandOverlap =
      cpl::down_cast<VRTSourcedRasterBand*>(dsOverlap.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandOverlap);
  ASSERT_EQ(CE_None, poBandOverlap->AddSimpleSource(kTile1, 1, 0, 0, 500, 1000,
                                                    0, 0, 500, 1000));
  ASSERT_EQ(CE_None, poBandOverlap->AddSimpleSource(kTile2, 1, 0, 0, 500, 1000,
                                                    250, 0, 500, 1000));
  int nContribOverlap = 0;
  EXPECT_FALSE(
      poBandOverlap->CanMultiThreadRasterIO(0, 0, 1000, 1000, nContribOverlap));

  // 3. Multi-threaded RasterIO execution.
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable multi-threaded TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
  CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "1", false);
#else
  CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "2", false);
#endif
  std::vector<GByte> outBuf(1000 * 1000, 0);
  GDALRasterIOExtraArg sMTArg;
  INIT_RASTERIO_EXTRA_ARG(sMTArg);
  sMTArg.bFloatingPointWindowValidity = TRUE;
  sMTArg.dfXOff = 0.0;
  sMTArg.dfYOff = 0.0;
  sMTArg.dfXSize = 1000.0;
  sMTArg.dfYSize = 1000.0;
  sMTArg.pfnProgress = GDALDummyProgress;

  EXPECT_EQ(CE_None, poBand->IRasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(),
                                       1000, 1000, GDT_Byte, 1, 1000, &sMTArg));
}

TEST_F(VrtSourcedRasterBandTest, CanIRasterIOBeForwardedToEachSource) {
  constexpr char kFwdSrc[] = "/vsimem/vrtsourcedrasterband_test/fwd_src.tif";
  CreateByteGeoTiff(kFwdSrc, 50, 50);

  VRTDataset ds(50, 50);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kFwdSrc, 1, 0, 0, 50, 50, 0, 0, 50, 50));

  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);

  // 1. Nearest neighbour.
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 50, 50,
                                                          50, 50, &sExtraArg));

  // 2. Non-nearest neighbour (bilinear) downsampling.
  sExtraArg.eResampleAlg = GRIORA_Bilinear;
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 50, 50,
                                                          25, 25, &sExtraArg));

  // 3. ComplexSource with custom resampling.
  VRTDataset dsComplex(50, 50);
  ASSERT_EQ(CE_None, dsComplex.AddBand(GDT_Byte, nullptr));
  auto poBandComplex =
      cpl::down_cast<VRTSourcedRasterBand*>(dsComplex.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandComplex);
  ASSERT_EQ(CE_None,
            poBandComplex->AddComplexSource(kFwdSrc, 1, 0, 0, 50, 50, 0, 0, 50,
                                            50, 0.0, 1.0, 0.0, 0));
  EXPECT_TRUE(poBandComplex->CanIRasterIOBeForwardedToEachSource(
      GF_Read, 0, 0, 50, 50, 25, 25, &sExtraArg));
}

TEST_F(VrtSourcedRasterBandTest, GetDataCoverageStatus) {
  constexpr char kCovSrc[] = "/vsimem/vrtsourcedrasterband_test/cov_src.tif";
  CreateByteGeoTiff(kCovSrc, 50, 50);

  VRTDataset ds(100, 100);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kCovSrc, 1, 0, 0, 50, 50, 0, 0, 50, 50));

  double dfPct = -1.0;
  // Inside source.
  int nStatus = poBand->GetDataCoverageStatus(0, 0, 50, 50, 0, &dfPct);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_DATA);

  // Outside source.
  dfPct = -1.0;
  nStatus = poBand->GetDataCoverageStatus(60, 60, 40, 40, 0, &dfPct);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_EMPTY);

  // Single simple source covering full band.
  VRTDataset dsFull(50, 50);
  ASSERT_EQ(CE_None, dsFull.AddBand(GDT_Byte, nullptr));
  auto poBandFull =
      cpl::down_cast<VRTSourcedRasterBand*>(dsFull.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandFull);
  ASSERT_EQ(CE_None, poBandFull->AddSimpleSource(kCovSrc, 1, 0, 0, 50, 50, 0, 0,
                                                 50, 50));
  dfPct = -1.0;
  nStatus = poBandFull->GetDataCoverageStatus(0, 0, 50, 50, 0, &dfPct);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_DATA);
}

TEST_F(VrtSourcedRasterBandTest, MinMaxAndStatistics) {
  constexpr char kStatsSrc[] =
      "/vsimem/vrtsourcedrasterband_test/stats_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kStatsSrc, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    std::vector<GByte> line(20, 50);
    for (int y = 0; y < 20; ++y) {
      ASSERT_EQ(CE_None,
                poSrcDS->GetRasterBand(1)->RasterIO(
                    GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0));
    }
    int anOvr[] = {2};
    EXPECT_EQ(CE_None, poSrcDS->BuildOverviews("NEAREST", 1, anOvr, 0, nullptr,
                                               nullptr, nullptr));
  }

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kStatsSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));

  int bSuccess = FALSE;
  poBand->GetMinimum(&bSuccess);
  EXPECT_FALSE(bSuccess);

  bSuccess = FALSE;
  poBand->GetMaximum(&bSuccess);
  EXPECT_FALSE(bSuccess);

  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(FALSE, adfMinMax));
  EXPECT_DOUBLE_EQ(50.0, adfMinMax[0]);
  EXPECT_DOUBLE_EQ(50.0, adfMinMax[1]);

  double dfSMin = 0.0;
  double dfSMax = 0.0;
  double dfSMean = 0.0;
  double dfSStdDev = 0.0;
  EXPECT_EQ(CE_None,
            poBand->ComputeStatistics(FALSE, &dfSMin, &dfSMax, &dfSMean,
                                      &dfSStdDev, GDALDummyProgress, nullptr));
  EXPECT_DOUBLE_EQ(50.0, dfSMin);
  EXPECT_DOUBLE_EQ(50.0, dfSMax);
  EXPECT_DOUBLE_EQ(50.0, dfSMean);
  EXPECT_DOUBLE_EQ(0.0, dfSStdDev);

  // ComputeStatistics with bApproxOK = TRUE.
  EXPECT_EQ(CE_None, poBand->ComputeStatistics(TRUE, &dfSMin, &dfSMax, &dfSMean,
                                               &dfSStdDev, nullptr, nullptr));
}

TEST_F(VrtSourcedRasterBandTest, GetHistogram) {
  constexpr char kHistSrc[] = "/vsimem/vrtsourcedrasterband_test/hist_src.tif";
  CreateByteGeoTiff(kHistSrc, 20, 20, 10);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kHistSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));

  std::vector<GUIntBig> hist(256, 0);
  EXPECT_EQ(CE_None, poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE,
                                          FALSE, GDALDummyProgress, nullptr));
  EXPECT_EQ(400U, hist[10]);

  // Multiple sources fallback path.
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kHistSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  std::vector<GUIntBig> hist2(256, 0);
  EXPECT_EQ(CE_None, poBand->GetHistogram(0.0, 255.0, 256, hist2.data(), FALSE,
                                          FALSE, nullptr, nullptr));
}

TEST_F(VrtSourcedRasterBandTest, MetadataAndLocationInfo) {
  constexpr char kLocSrc[] = "/vsimem/vrtsourcedrasterband_test/loc_src.tif";
  CreateByteGeoTiff(kLocSrc, 20, 20);

  VRTDataset ds(20, 20);
  GDALGeoTransform gtCoeffs(0.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, ds.SetGeoTransform(gtCoeffs));

  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kLocSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  static_cast<VRTSimpleSource*>(poBand->m_papoSources[0].get())
      ->SetSourceDatasetName(kLocSrc, false);

  // 1. GetMetadataDomainList.
  {
    StringListPtr papszDomains(poBand->GetMetadataDomainList(), CSLDestroy);
    ASSERT_NE(nullptr, papszDomains.get());
    EXPECT_GE(CSLFindString(papszDomains.get(), "LocationInfo"), 0);
  }

  // 2. LocationInfo queries.
  const char* pszLocInfo = poBand->GetMetadataItem("Pixel_5_5", "LocationInfo");
  ASSERT_NE(nullptr, pszLocInfo);
  EXPECT_THAT(
      std::string_view(pszLocInfo),
      HasSubstr("<File>/vsimem/vrtsourcedrasterband_test/loc_src.tif</File>"));

  const char* pszGeoLocInfo =
      poBand->GetMetadataItem("GeoPixel_5.5_14.5", "LocationInfo");
  ASSERT_NE(nullptr, pszGeoLocInfo);
  EXPECT_THAT(
      std::string_view(pszGeoLocInfo),
      HasSubstr("<File>/vsimem/vrtsourcedrasterband_test/loc_src.tif</File>"));

  // 3. LocationInfo edge cases.
  EXPECT_EQ(nullptr, poBand->GetMetadataItem("Pixel_bad", "LocationInfo"));
  EXPECT_EQ(nullptr, poBand->GetMetadataItem("Pixel_100_100", "LocationInfo"));
  EXPECT_EQ(nullptr, poBand->GetMetadataItem("GeoPixel_bad", "LocationInfo"));

  // 4. vrt_sources domain.
  char** papszSources = poBand->GetMetadata("vrt_sources");
  ASSERT_NE(nullptr, papszSources);
  EXPECT_GE(CSLCount(papszSources), 1);

  // 5. SetMetadataItem in new_vrt_sources and vrt_sources.
  constexpr char kNewSrcXml[] = R"xml(
<SimpleSource>
  <SourceFilename>/vsimem/vrtsourcedrasterband_test/loc_src.tif</SourceFilename>
  <SourceBand>1</SourceBand>
</SimpleSource>)xml";
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("new_source", kNewSrcXml,
                                             "new_vrt_sources"));
  EXPECT_EQ(CE_None,
            poBand->SetMetadataItem("source_0", kNewSrcXml, "vrt_sources"));
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              poBand->SetMetadataItem("source_bad", kNewSrcXml, "vrt_sources"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 6. SetMetadata in vrt_sources and new_vrt_sources.
  CPLStringList aosNewSources;
  aosNewSources.AddNameValue("source_0", kNewSrcXml);
  EXPECT_EQ(CE_None, poBand->SetMetadata(aosNewSources.List(), "vrt_sources"));
  EXPECT_EQ(CE_None,
            poBand->SetMetadata(aosNewSources.List(), "new_vrt_sources"));
}

TEST_F(VrtSourcedRasterBandTest, FileListAndClosing) {
  constexpr char kFlSrc[] = "/vsimem/vrtsourcedrasterband_test/fl_src.tif";
  CreateByteGeoTiff(kFlSrc, 20, 20);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kFlSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));

  char** papszFileList = nullptr;
  absl::Cleanup free_filelist = [&papszFileList] { CSLDestroy(papszFileList); };
  int nSize = 0;
  int nMaxSize = 0;
  CPLHashSet* hSetFiles =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  ASSERT_NE(nullptr, hSetFiles);
  absl::Cleanup free_hashset = [hSetFiles] { CPLHashSetDestroy(hSetFiles); };

  poBand->GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
  EXPECT_GE(nSize, 1);
  EXPECT_NE(-1, CSLFindString(papszFileList, kFlSrc));

  EXPECT_EQ(CE_None, poBand->FlushCache(false));
  EXPECT_EQ(CE_None, poBand->FlushCache(true));
  EXPECT_EQ(TRUE, poBand->CloseDependentDatasets());
  EXPECT_EQ(FALSE, poBand->CloseDependentDatasets());
}

TEST_F(VrtSourcedRasterBandTest, RemoveCoveredSources) {
  constexpr char kCov1[] = "/vsimem/vrtsourcedrasterband_test/cov1.tif";
  constexpr char kCov2[] = "/vsimem/vrtsourcedrasterband_test/cov2.tif";
  CreateByteGeoTiff(kCov1, 20, 20);
  CreateByteGeoTiff(kCov2, 20, 20);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kCov1, 1, 0, 0, 10, 10, 0, 0, 10, 10));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kCov2, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  EXPECT_EQ(2U, poBand->m_papoSources.size());

  poBand->RemoveCoveredSources(nullptr);
  EXPECT_EQ(1U, poBand->m_papoSources.size());
}

TEST_F(VrtSourcedRasterBandTest, MosaicStatisticsMinMaxHistogram) {
  constexpr char kMos1[] = "/vsimem/vrtsourcedrasterband_test/mos1.tif";
  constexpr char kMos2[] = "/vsimem/vrtsourcedrasterband_test/mos2.tif";
  CreateByteGeoTiff(kMos1, 10, 20, 20);
  CreateByteGeoTiff(kMos2, 10, 20, 80);

  VRTDataset dsMos(20, 20);
  ASSERT_EQ(CE_None, dsMos.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(dsMos.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kMos1, 1, 0, 0, 10, 20, 0, 0, 10, 20));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kMos2, 1, 0, 0, 10, 20, 10, 0, 10, 20));

  double dfMin = 0.0;
  double dfMax = 0.0;
  double dfMean = 0.0;
  double dfStdDev = 0.0;
  double adfMinMax[2] = {0.0, 0.0};
  {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // TODO(schwehr): Re-enable multi-threaded TSan once upstream
    // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "1", false);
#else
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "2", false);
#endif
    EXPECT_EQ(CE_None,
              poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, GDALDummyProgress, nullptr));
    EXPECT_DOUBLE_EQ(20.0, dfMin);
    EXPECT_DOUBLE_EQ(80.0, dfMax);
    EXPECT_DOUBLE_EQ(50.0, dfMean);

    EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(FALSE, adfMinMax));
    EXPECT_DOUBLE_EQ(20.0, adfMinMax[0]);
    EXPECT_DOUBLE_EQ(80.0, adfMinMax[1]);

    std::vector<GUIntBig> hist(256, 0);
    EXPECT_EQ(CE_None, poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE,
                                            FALSE, GDALDummyProgress, nullptr));
    EXPECT_EQ(200U, hist[20]);
    EXPECT_EQ(200U, hist[80]);

    // Partial mosaic (uncovered pixels with nodata).
    VRTDataset dsPartial(30, 20);
    ASSERT_EQ(CE_None, dsPartial.AddBand(GDT_Byte, nullptr));
    auto poBandPartial =
        cpl::down_cast<VRTSourcedRasterBand*>(dsPartial.GetRasterBand(1));
    ASSERT_NE(nullptr, poBandPartial);
    EXPECT_EQ(CE_None, poBandPartial->SetNoDataValue(0.0));
    ASSERT_EQ(CE_None, poBandPartial->AddSimpleSource(kMos1, 1, 0, 0, 10, 20, 0,
                                                      0, 10, 20));
    ASSERT_EQ(CE_None, poBandPartial->AddSimpleSource(kMos2, 1, 0, 0, 10, 20,
                                                      10, 0, 10, 20));

    EXPECT_EQ(CE_None,
              poBandPartial->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                               &dfStdDev, nullptr, nullptr));
    EXPECT_EQ(CE_None, poBandPartial->ComputeRasterMinMax(FALSE, adfMinMax));
    EXPECT_DOUBLE_EQ(20.0, adfMinMax[0]);
    EXPECT_DOUBLE_EQ(80.0, adfMinMax[1]);
  }
}

TEST_F(VrtSourcedRasterBandTest,
       OverviewsStatisticsMinMaxHistogramAndRasterIO) {
  constexpr char kOvrSrc[] = "/vsimem/vrtsourcedrasterband_test/ovr_src.tif";
  CreateByteGeoTiff(kOvrSrc, 40, 40, 30);

  VRTDataset ds(40, 40);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kOvrSrc, 1, 0, 0, 40, 40, 0, 0, 40, 40));

  int anOvr[] = {2};
  CPLStringList aosOvrOptions;
  aosOvrOptions.AddNameValue("VIRTUAL", "YES");
  EXPECT_EQ(CE_None, ds.BuildOverviews("NEAREST", 1, anOvr, 0, nullptr, nullptr,
                                       nullptr, aosOvrOptions.List()));
  EXPECT_EQ(1, poBand->GetOverviewCount());

  // 1. OverviewRasterIO through downsampled Read.
  std::vector<GByte> outBuf(20 * 20, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 40, 40, outBuf.data(), 20,
                                      20, GDT_Byte, 1, 20, nullptr));
  EXPECT_EQ(30, outBuf[0]);

  // 2. ComputeRasterMinMax with bApproxOK = TRUE.
  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(TRUE, adfMinMax));
  EXPECT_DOUBLE_EQ(30.0, adfMinMax[0]);
  EXPECT_DOUBLE_EQ(30.0, adfMinMax[1]);

  // 3. ComputeStatistics with bApproxOK = TRUE.
  double dfMin = 0.0;
  double dfMax = 0.0;
  double dfMean = 0.0;
  double dfStdDev = 0.0;
  EXPECT_EQ(CE_None, poBand->ComputeStatistics(TRUE, &dfMin, &dfMax, &dfMean,
                                               &dfStdDev, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(30.0, dfMin);
  EXPECT_DOUBLE_EQ(30.0, dfMax);

  // 4. GetHistogram with bApproxOK = TRUE.
  std::vector<GUIntBig> hist(256, 0);
  EXPECT_EQ(CE_None, poBand->GetHistogram(0.0, 255.0, 256, hist.data(), FALSE,
                                          TRUE, nullptr, nullptr));
  EXPECT_GT(hist[30], 0U);
}

TEST_F(VrtSourcedRasterBandTest, GetMinimumMaximumBranches) {
  constexpr char kMinMaxSrc[] =
      "/vsimem/vrtsourcedrasterband_test/minmax_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kMinMaxSrc, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    std::vector<GByte> line(20, 0);
    line[19] = 255;
    for (int y = 0; y < 20; ++y) {
      ASSERT_EQ(CE_None,
                poSrcDS->GetRasterBand(1)->RasterIO(
                    GF_Write, 0, y, 20, 1, line.data(), 20, 1, GDT_Byte, 0, 0));
    }
  }

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kMinMaxSrc, 1, 0, 0, 20, 20, 0, 0, 20, 20));

  // 1. STATISTICS_MINIMUM and STATISTICS_MAXIMUM metadata items.
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MINIMUM", "5.5"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MAXIMUM", "99.5"));
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(5.5, poBand->GetMinimum(&bSuccess));
  EXPECT_TRUE(bSuccess);
  bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(99.5, poBand->GetMaximum(&bSuccess));
  EXPECT_TRUE(bSuccess);
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MINIMUM", nullptr));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MAXIMUM", nullptr));

  // 2. VRT_MIN_MAX_FROM_SOURCES config option.
  {
    CPLConfigOptionSetter opt("VRT_MIN_MAX_FROM_SOURCES", "YES", false);
    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(0.0, poBand->GetMinimum(&bSuccess));
    EXPECT_FALSE(bSuccess);
    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(255.0, poBand->GetMaximum(&bSuccess));
    EXPECT_FALSE(bSuccess);
  }
  {
    CPLConfigOptionSetter opt("VRT_MIN_MAX_FROM_SOURCES", "NO", false);
    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(0.0, poBand->GetMinimum(&bSuccess));
    EXPECT_FALSE(bSuccess);
    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(255.0, poBand->GetMaximum(&bSuccess));
    EXPECT_FALSE(bSuccess);
  }

  // 3. Remote /vsicurl filename returns false without touching network.
  {
    VRTDataset dsVsi(20, 20);
    ASSERT_EQ(CE_None, dsVsi.AddBand(GDT_Byte, nullptr));
    auto poBandVsi =
        cpl::down_cast<VRTSourcedRasterBand*>(dsVsi.GetRasterBand(1));
    ASSERT_NE(nullptr, poBandVsi);
    ASSERT_EQ(CE_None,
              poBandVsi->AddSimpleSource("/vsicurl/http://example.com/test.tif",
                                         1, 0, 0, 20, 20, 0, 0, 20, 20));
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(0.0, poBandVsi->GetMinimum(&bSuccess));
    EXPECT_FALSE(bSuccess);
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(255.0, poBandVsi->GetMaximum(&bSuccess));
    EXPECT_FALSE(bSuccess);
  }

  // 4. Special character filename loop.
  {
    VRTDataset dsSpecial(20, 20);
    ASSERT_EQ(CE_None, dsSpecial.AddBand(GDT_Byte, nullptr));
    auto poBandSpecial =
        cpl::down_cast<VRTSourcedRasterBand*>(dsSpecial.GetRasterBand(1));
    ASSERT_NE(nullptr, poBandSpecial);
    ASSERT_EQ(CE_None,
              poBandSpecial->AddSimpleSource("nonexistent#special.tif", 1, 0, 0,
                                             20, 20, 0, 0, 20, 20));
    CPLErrorReset();
    WithQuietHandler quiet;
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(0.0, poBandSpecial->GetMinimum(&bSuccess));
    EXPECT_FALSE(bSuccess);
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(255.0, poBandSpecial->GetMaximum(&bSuccess));
    EXPECT_FALSE(bSuccess);
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest,
       CanIRasterIOBeForwardedBranchesAndComplexResampling) {
  constexpr char kFwdBranchSrc[] =
      "/vsimem/vrtsourcedrasterband_test/fwd_branch_src.tif";
  CreateByteGeoTiff(kFwdBranchSrc, 20, 20);

  GDALDatasetUniquePtr poSrcDSOpen(GDALDataset::FromHandle(
      GDALOpenEx(kFwdBranchSrc, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, poSrcDSOpen);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  auto poCS1 = std::make_unique<VRTComplexSource>();
  poCS1->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  poCS1->SetResampling("bilinear");
  poCS1->SetSrcWindow(0, 0, 20, 20);
  poCS1->SetDstWindow(0, 0, 20, 20);
  EXPECT_EQ(CE_None, poBand->AddSource(std::move(poCS1)));

  GDALRasterIOExtraArg sArg;
  INIT_RASTERIO_EXTRA_ARG(sArg);

  // 1. Floating point window.
  sArg.bFloatingPointWindowValidity = TRUE;
  sArg.dfXOff = 0.0;
  sArg.dfYOff = 0.0;
  sArg.dfXSize = 20.0;
  sArg.dfYSize = 20.0;
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 20, 20,
                                                          10, 10, &sArg));
  sArg.bFloatingPointWindowValidity = FALSE;

  // 2. IsMaskBand() causes false.
  {
    VRTDataset dsMask(20, 20);
    ASSERT_EQ(CE_None, dsMask.AddBand(GDT_Byte, nullptr));
    auto poBandMask =
        cpl::down_cast<VRTSourcedRasterBand*>(dsMask.GetRasterBand(1));
    ASSERT_NE(nullptr, poBandMask);
    auto poCSMask = std::make_unique<VRTComplexSource>();
    poCSMask->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
    poSrcDSOpen->Reference();
    poCSMask->SetResampling("bilinear");
    EXPECT_EQ(CE_None, poBandMask->AddSource(std::move(poCSMask)));
    poBandMask->SetIsMaskBand();
    EXPECT_FALSE(poBandMask->CanIRasterIOBeForwardedToEachSource(
        GF_Read, 0, 0, 20, 20, 10, 10, &sArg));
  }

  // 3. Nodata mismatch causes false; matching nodata succeeds.
  EXPECT_EQ(CE_None, poBand->SetNoDataValue(42.0));
  EXPECT_FALSE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 20,
                                                           20, 10, 10, &sArg));

  EXPECT_EQ(CE_None, poSrcDSOpen->GetRasterBand(1)->SetNoDataValue(42.0));
  EXPECT_TRUE(poBand->CanIRasterIOBeForwardedToEachSource(GF_Read, 0, 0, 20, 20,
                                                          10, 10, &sArg));
  EXPECT_EQ(CE_None, poBand->DeleteNoDataValue());

  // 4. Downsample read fallback path.
  std::vector<GByte> outBuf(100, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 10,
                                      10, GDT_Byte, 1, 10, nullptr));

  // 5. Multiple complex sources with different resampling.
  auto poCS2 = std::make_unique<VRTComplexSource>();
  poCS2->SetSrcBand(poSrcDSOpen->GetRasterBand(1));
  poSrcDSOpen->Reference();
  poCS2->SetResampling("cubic");
  poCS2->SetSrcWindow(0, 0, 20, 20);
  poCS2->SetDstWindow(0, 0, 20, 20);
  EXPECT_EQ(CE_None, poBand->AddSource(std::move(poCS2)));

  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, outBuf.data(), 10,
                                      10, GDT_Byte, 1, 10, nullptr));

  // 6. FuncSource causes CanIRasterIOBeForwardedToEachSource to return false.
  {
    VRTDataset dsFunc(20, 20);
    ASSERT_EQ(CE_None, dsFunc.AddBand(GDT_Byte, nullptr));
    auto poBandFunc =
        cpl::down_cast<VRTSourcedRasterBand*>(dsFunc.GetRasterBand(1));
    ASSERT_NE(nullptr, poBandFunc);
    EXPECT_EQ(CE_None,
              poBandFunc->AddFuncSource(MyImageReadFunc, nullptr, 0.0));
    sArg.eResampleAlg = GRIORA_Bilinear;
    EXPECT_FALSE(poBandFunc->CanIRasterIOBeForwardedToEachSource(
        GF_Read, 0, 0, 20, 20, 10, 10, &sArg));
  }
}

TEST_F(VrtSourcedRasterBandTest, CanMultiThreadRasterIOBranches) {
  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  // 1. FuncSource returns false.
  EXPECT_EQ(CE_None, poBand->AddFuncSource(MyImageReadFunc, nullptr, 0.0));
  int nContrib = 0;
  EXPECT_FALSE(poBand->CanMultiThreadRasterIO(0, 0, 20, 20, nContrib));

  // 2. Two sources with duplicate dataset name returns false.
  VRTDataset dsDup(20, 20);
  ASSERT_EQ(CE_None, dsDup.AddBand(GDT_Byte, nullptr));
  auto poBandDup =
      cpl::down_cast<VRTSourcedRasterBand*>(dsDup.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandDup);
  ASSERT_EQ(CE_None, poBandDup->AddSimpleSource(
                         "/vsimem/vrtsourcedrasterband_test/dup.tif", 1, 0, 0,
                         10, 20, 0, 0, 10, 20));
  ASSERT_EQ(CE_None, poBandDup->AddSimpleSource(
                         "/vsimem/vrtsourcedrasterband_test/dup.tif", 1, 0, 0,
                         10, 20, 10, 0, 10, 20));
  EXPECT_FALSE(poBandDup->CanMultiThreadRasterIO(0, 0, 20, 20, nContrib));
}

TEST_F(VrtSourcedRasterBandTest, GetDataCoverageStatusAdvanced) {
  constexpr char kCovAdv1[] = "/vsimem/vrtsourcedrasterband_test/cov_adv1.tif";
  constexpr char kCovAdv2[] = "/vsimem/vrtsourcedrasterband_test/cov_adv2.tif";
  CreateByteGeoTiff(kCovAdv1, 20, 20);
  CreateByteGeoTiff(kCovAdv2, 20, 20);

  VRTDataset ds(40, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kCovAdv1, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kCovAdv2, 1, 0, 0, 20, 20, 20, 0, 20, 20));

  // 1. Partial AOI query covering part of both sources: triggers GEOS
  // difference loop.
  double dfDataPct = 0.0;
  int nStatus = poBand->GetDataCoverageStatus(10, 0, 20, 20, 0, &dfDataPct);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_DOUBLE_EQ(100.0, dfDataPct);

  // 2. Query with nMaskFlagStop.
  nStatus = poBand->GetDataCoverageStatus(
      0, 0, 40, 20, GDAL_DATA_COVERAGE_STATUS_DATA, nullptr);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_DATA);

  // 3. Add source with dfDstXSize = -1 and dfDstYSize = -1.
  VRTDataset dsDefault(20, 20);
  ASSERT_EQ(CE_None, dsDefault.AddBand(GDT_Byte, nullptr));
  auto poBandDef =
      cpl::down_cast<VRTSourcedRasterBand*>(dsDefault.GetRasterBand(1));
  ASSERT_NE(nullptr, poBandDef);
  auto poSS = std::make_unique<VRTSimpleSource>();
  poSS->SetSrcBand(kCovAdv1, 1);
  poSS->SetDstWindow(0, 0, -1, -1);
  EXPECT_EQ(CE_None, poBandDef->AddSource(std::move(poSS)));
  nStatus = poBandDef->GetDataCoverageStatus(0, 0, 20, 20, 0, nullptr);
  EXPECT_NE(0, nStatus & GDAL_DATA_COVERAGE_STATUS_DATA);
}

TEST_F(VrtSourcedRasterBandTest, ConfigureSourceAndRefBranches) {
  constexpr char kCfgSrc[] = "/vsimem/vrtsourcedrasterband_test/cfg_src.tif";
  CreateByteGeoTiff(kCfgSrc, 20, 20);

  GDALDatasetUniquePtr poSrcDSOpen(GDALDataset::FromHandle(GDALOpenEx(
      kCfgSrc, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poSrcDSOpen);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  // dfSrcYSize == -1 and dfDstYSize == -1 in ConfigureSource.
  EXPECT_EQ(CE_None, poBand->AddSimpleSource(poSrcDSOpen->GetRasterBand(1), 0,
                                             0, 20, -1, 0, 0, 20, -1));

  // BuildOverviews internally calls AddVirtualOverview which sets
  // m_bCanTakeRef = false on the dataset.
  VRTDataset vrtOverview(20, 20);
  ASSERT_EQ(CE_None, vrtOverview.AddBand(GDT_Byte, nullptr));
  int anOvr[] = {2};
  CPLStringList aosOvrOptions;
  aosOvrOptions.AddNameValue("VIRTUAL", "YES");
  EXPECT_EQ(CE_None,
            vrtOverview.BuildOverviews("NEAREST", 1, anOvr, 0, nullptr, nullptr,
                                       nullptr, aosOvrOptions.List()));
}

TEST_F(VrtSourcedRasterBandTest, IReadBlockEdgeBlocks) {
  VRTDataset ds(25, 25);
  auto poBand =
      std::make_unique<VRTSourcedRasterBand>(&ds, 1, GDT_Byte, 25, 25, 20, 20);
  std::vector<GByte> block(20 * 20, 0);
  EXPECT_EQ(CE_None, poBand->IReadBlock(1, 0, block.data()));
  EXPECT_EQ(CE_None, poBand->IReadBlock(0, 1, block.data()));
}

TEST_F(VrtSourcedRasterBandTest, MetadataAndXMLInitErrorPaths) {
  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  CPLStringList aosBadMD;
  aosBadMD.AddNameValue("source_0", "<bad");

  // 1. Bad XML for new_vrt_sources and vrt_sources.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              poBand->SetMetadataItem("new", "<bad", "new_vrt_sources"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              poBand->SetMetadataItem("source_0", "<bad", "vrt_sources"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, poBand->SetMetadataItem("new", "<UnknownSource/>",
                                                  "new_vrt_sources"));
    CPLErrorReset();
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, poBand->SetMetadata(aosBadMD.List(), "vrt_sources"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Standalone band without dataset.
  VRTSourcedRasterBand standaloneBand(GDT_Byte, 10, 10);
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, standaloneBand.SetMetadataItem(
                              "new", "<SimpleSource/>", "new_vrt_sources"));
    EXPECT_EQ(CE_Failure, standaloneBand.SetMetadataItem(
                              "source_0", "<SimpleSource/>", "vrt_sources"));
    EXPECT_EQ(CE_Failure,
              standaloneBand.SetMetadata(aosBadMD.List(), "vrt_sources"));
    CPLErrorReset();
  }

  // 3. LocationInfo error paths.
  EXPECT_EQ(nullptr,
            standaloneBand.GetMetadataItem("GeoPixel_5.0_5.0", "LocationInfo"));
  EXPECT_EQ(nullptr,
            poBand->GetMetadataItem("GeoPixel_5.0_5.0", "LocationInfo"));
  EXPECT_EQ(nullptr,
            poBand->GetMetadataItem("GeoPixel_bad_coord", "LocationInfo"));

  // 4. XMLInit with unknown child tag succeeds.
  constexpr char kBadVrt[] = R"xml(
<VRTRasterBand dataType="Byte">
  <UnknownTag/>
</VRTRasterBand>)xml";
  CPLXMLTreeCloser psTree(CPLParseXMLString(kBadVrt));
  ASSERT_NE(nullptr, psTree.get());
  VRTMapSharedResources mapRes;
  EXPECT_EQ(CE_None, poBand->XMLInit(psTree.get(), nullptr, mapRes));

  // 5. IRasterIO with GF_Write returns failure.
  {
    std::vector<GByte> buf(400, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, poBand->RasterIO(GF_Write, 0, 0, 20, 20, buf.data(),
                                           20, 20, GDT_Byte, 1, 20, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest, RemoveCoveredSourcesMultipleUpper) {
  constexpr char kRmBase[] = "/vsimem/vrtsourcedrasterband_test/rm_base.tif";
  constexpr char kRmTop[] = "/vsimem/vrtsourcedrasterband_test/rm_top.tif";
  constexpr char kRmBottom[] =
      "/vsimem/vrtsourcedrasterband_test/rm_bottom.tif";
  CreateByteGeoTiff(kRmBase, 20, 20);
  CreateByteGeoTiff(kRmTop, 20, 10);
  CreateByteGeoTiff(kRmBottom, 20, 10);

  VRTDataset ds(20, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kRmBase, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kRmTop, 1, 0, 0, 20, 10, 0, 0, 20, 10));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kRmBottom, 1, 0, 0, 20, 10, 0, 10, 20, 10));
  EXPECT_EQ(3U, poBand->m_papoSources.size());

  // Together source 1 and source 2 completely cover source 0.
  poBand->RemoveCoveredSources(nullptr);
  EXPECT_EQ(2U, poBand->m_papoSources.size());
}

TEST_F(VrtSourcedRasterBandTest, XMLInitErrorHandling) {
  VRTDataset ds(10, 10);
  VRTSourcedRasterBand band(&ds, 1);
  VRTMapSharedResources mapSharedSources;

  // 1. Invalid data type in VRTRasterBand XML.
  {
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<VRTRasterBand dataType=\"NonExistentDataType\" band=\"1\"/>"));
    ASSERT_NE(nullptr, oTree.get());
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band.XMLInit(oTree.get(), nullptr, mapSharedSources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Invalid Source XML that causes ParseSource to fail.
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<VRTRasterBand dataType=\"Byte\" band=\"1\">"
                          "  <SimpleSource/>"
                          "</VRTRasterBand>"));
    ASSERT_NE(nullptr, oTree.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.XMLInit(oTree.get(), nullptr, mapSharedSources));
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest, OverviewRasterIOBranch) {
  constexpr char kOvrFull[] = "/vsimem/vrtsourcedrasterband_test/ovr_full.tif";
  constexpr char kOvrHalf[] = "/vsimem/vrtsourcedrasterband_test/ovr_half.tif";
  CreateByteGeoTiff(kOvrFull, 20, 20);
  CreateByteGeoTiff(kOvrHalf, 10, 10, 42);

  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/vrtsourcedrasterband_test/ovr_full.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
    <Overview>
      <SourceFilename relativeToVRT="0">/vsimem/vrtsourcedrasterband_test/ovr_half.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </Overview>
  </VRTRasterBand>
</VRTDataset>)xml";

  GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(GDALOpenEx(
      kVrtXml, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(1, poBand->GetOverviewCount());

  std::vector<GByte> abyBuf(100, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, abyBuf.data(), 10,
                                      10, GDT_Byte, 1, 10, nullptr));
  EXPECT_EQ(42, abyBuf[0]);
  EXPECT_EQ(42, abyBuf[99]);

  // GetHistogram delegated to the overview band.
  GUIntBig anHistogram[256] = {};
  EXPECT_EQ(CE_None, poBand->GetHistogram(-0.5, 255.5, 256, anHistogram, FALSE,
                                          TRUE, nullptr, nullptr));
  EXPECT_EQ(100U, anHistogram[42]);
}

TEST_F(VrtSourcedRasterBandTest, OverviewMinMaxAndStatistics) {
  constexpr char kStatsOvrSrc[] =
      "/vsimem/vrtsourcedrasterband_test/stats_ovr_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrc(
        poDrv->Create(kStatsOvrSrc, 200, 200, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrc);
    std::vector<GByte> abyData(200 * 200, 150);
    std::fill_n(abyData.begin(), 200 * 100, static_cast<GByte>(50));
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 200, 200, abyData.data(), 200, 200,
                           GDT_Byte, 1, 200, nullptr));
  }

  VRTDataset ds(200, 200);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None, poBand->AddSimpleSource(kStatsOvrSrc, 1, 0, 0, 200, 200, 0,
                                             0, 200, 200));

  // Build virtual overviews: factor 2 (100x100 = 10,000 pixels > 2500
  // GDALSTAT_APPROX_NUMSAMPLES).
  CPLStringList aosOvrOptions;
  aosOvrOptions.AddNameValue("VIRTUAL", "YES");
  int anOverviewList[] = {2};
  EXPECT_EQ(CE_None, ds.BuildOverviews("NEAR", 1, anOverviewList, 0, nullptr,
                                       nullptr, nullptr, aosOvrOptions.List()));
  EXPECT_EQ(1, poBand->GetOverviewCount());

  double adfMinMax[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(TRUE, adfMinMax));
  EXPECT_LE(adfMinMax[0], 50.0);
  EXPECT_GE(adfMinMax[1], 150.0);

  double dfMin = 0.0;
  double dfMax = 0.0;
  double dfMean = 0.0;
  double dfStdDev = 0.0;
  EXPECT_EQ(CE_None, poBand->ComputeStatistics(TRUE, &dfMin, &dfMax, &dfMean,
                                               &dfStdDev, nullptr, nullptr));
  EXPECT_STREQ("YES", poBand->GetMetadataItem("STATISTICS_APPROXIMATE"));
  EXPECT_NE(nullptr, poBand->GetMetadataItem("STATISTICS_VALID_PERCENT"));
}

TEST_F(VrtSourcedRasterBandTest, ComputeRasterMinMaxBranches) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  // 1. NeedMaxValAdjustment: per-source min/max is clamped to the source's
  // declared maximum value.
  {
    constexpr char kMaxValSrc[] =
        "/vsimem/vrtsourcedrasterband_test/maxval_src.tif";
    {
      GDALDatasetUniquePtr poSrc(
          poDrv->Create(kMaxValSrc, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc);
      std::vector<GByte> abyData(100, 80);
      abyData[0] = 20;
      ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 10, 10, abyData.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));
      EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetMetadataItem(
                             "NBITS", "12", "IMAGE_STRUCTURE"));
    }

    VRTDataset ds(10, 10);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    auto poSS = std::make_unique<VRTSimpleSource>();
    poSS->SetSrcBand(kMaxValSrc, 1);
    poSS->SetSrcWindow(0, 0, 10, 10);
    poSS->SetDstWindow(0, 0, 10, 10);
    poSS->SetMaxValue(50);
    EXPECT_EQ(CE_None, poBand->AddSource(std::move(poSS)));

    double adfMinMax[2] = {0.0, 0.0};
    EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(FALSE, adfMinMax));
    EXPECT_DOUBLE_EQ(20.0, adfMinMax[0]);
    EXPECT_DOUBLE_EQ(50.0, adfMinMax[1]);
  }

  // 2. Mosaic does not cover the whole VRT raster and the VRT has a hidden
  // nodata value: the nodata value is folded into the global min/max.
  {
    constexpr char kPartSrc[] =
        "/vsimem/vrtsourcedrasterband_test/part_src.tif";
    CreateByteGeoTiff(kPartSrc, 10, 10, 100);

    VRTDataset ds(20, 20);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    EXPECT_EQ(CE_None, poBand->SetNoDataValue(42.0));
    EXPECT_EQ(CE_None, poBand->SetMetadataItem("HideNoDataValue", "1"));
    ASSERT_EQ(CE_None,
              poBand->AddSimpleSource(kPartSrc, 1, 0, 0, 10, 10, 0, 0, 10, 10));

    double adfMinMax[2] = {0.0, 0.0};
    EXPECT_EQ(CE_None, poBand->ComputeRasterMinMax(FALSE, adfMinMax));
    EXPECT_DOUBLE_EQ(42.0, adfMinMax[0]);
    EXPECT_DOUBLE_EQ(100.0, adfMinMax[1]);
  }

  // 3. Fallback to ComputeStatistics() when a source has only nodata values:
  // fails with an error.
  {
    constexpr char kAllNodataSrc[] =
        "/vsimem/vrtsourcedrasterband_test/all_nodata_src.tif";
    {
      GDALDatasetUniquePtr poSrc(
          poDrv->Create(kAllNodataSrc, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc);
      std::vector<GByte> abyData(100, 0);
      ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 10, 10, abyData.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));
      EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValue(0.0));
    }

    VRTDataset ds(10, 10);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    EXPECT_EQ(CE_None, poBand->SetNoDataValue(0.0));
    ASSERT_EQ(CE_None, poBand->AddSimpleSource(kAllNodataSrc, 1, 0, 0, 10, 10,
                                               0, 0, 10, 10));

    double adfMinMax[2] = {0.0, 0.0};
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, poBand->ComputeRasterMinMax(FALSE, adfMinMax));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest, ComputeStatisticsMosaicAndThreading) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kThSrc1[] = "/vsimem/vrtsourcedrasterband_test/th_src1.tif";
  constexpr char kThSrc2[] = "/vsimem/vrtsourcedrasterband_test/th_src2.tif";

  // 1. Multi-threaded mosaic statistics.
  {
    {
      GDALDatasetUniquePtr poSrc1(
          poDrv->Create(kThSrc1, 20, 20, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc1);
      std::vector<GByte> abyData1(400, 50);
      abyData1[0] = 10;
      ASSERT_EQ(CE_None, poSrc1->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 20, 20, abyData1.data(), 20, 20,
                             GDT_Byte, 1, 20, nullptr));
      EXPECT_EQ(CE_None, poSrc1->GetRasterBand(1)->SetNoDataValue(10.0));
    }

    CreateByteGeoTiff(kThSrc2, 20, 20, 60);

    VRTDataset ds(40, 20);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    ASSERT_EQ(CE_None,
              poBand->AddSimpleSource(kThSrc1, 1, 0, 0, 20, 20, 0, 0, 20, 20));
    ASSERT_EQ(CE_None,
              poBand->AddSimpleSource(kThSrc2, 1, 0, 0, 20, 20, 20, 0, 20, 20));

#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // TODO(schwehr): Re-enable multi-threaded TSan once upstream
    // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "1", false);
#else
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "2", false);
#endif

    int nProgressCount = 0;
    auto pfnProgress = [](double /*dfComplete*/, const char* /*pszMsg*/,
                          void* pData) -> int {
      (*static_cast<int*>(pData))++;
      return 1;
    };

    double dfMin = 0.0;
    double dfMax = 0.0;
    double dfMean = 0.0;
    double dfStdDev = 0.0;
    EXPECT_EQ(CE_None, poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                                 &dfStdDev, pfnProgress,
                                                 &nProgressCount));
    EXPECT_GT(nProgressCount, 0);
    EXPECT_DOUBLE_EQ(10.0, dfMin);
    EXPECT_DOUBLE_EQ(60.0, dfMax);

    // 2. An exact run clears a previously set STATISTICS_APPROXIMATE item.
    EXPECT_EQ(CE_None,
              poBand->SetMetadataItem("STATISTICS_APPROXIMATE", "YES"));
    EXPECT_EQ(CE_None, poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                                 &dfStdDev, nullptr, nullptr));
    EXPECT_EQ(nullptr, poBand->GetMetadataItem("STATISTICS_APPROXIMATE"));

    // 3. Fallback when VRT nodata lies within [min, max] of a source without
    // nodata.
    EXPECT_EQ(CE_None, poBand->SetNoDataValue(60.0));
    EXPECT_EQ(CE_None, poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                                 &dfStdDev, nullptr, nullptr));
  }

  // 4. Non-simple source falls back to GDALRasterBand::ComputeStatistics().
  {
    VRTDataset ds(10, 10);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    ASSERT_EQ(CE_None, poBand->AddComplexSource(kThSrc1, 1));

    double dfMin = 0.0;
    double dfMax = 0.0;
    double dfMean = 0.0;
    double dfStdDev = 0.0;
    EXPECT_EQ(CE_None, poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                                 &dfStdDev, nullptr, nullptr));
  }

  // 5. Zero valid pixels in mosaic fails.
  {
    constexpr char kZeroValidSrc[] =
        "/vsimem/vrtsourcedrasterband_test/zero_valid_src.tif";
    {
      GDALDatasetUniquePtr poSrc(
          poDrv->Create(kZeroValidSrc, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc);
      std::vector<GByte> abyData(100, 0);
      ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 10, 10, abyData.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));
      EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValue(0.0));
    }

    VRTDataset ds(10, 10);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);
    EXPECT_EQ(CE_None, poBand->SetNoDataValue(0.0));
    ASSERT_EQ(CE_None, poBand->AddSimpleSource(kZeroValidSrc, 1, 0, 0, 10, 10,
                                               0, 0, 10, 10));

    double dfMin = 0.0;
    double dfMax = 0.0;
    double dfMean = 0.0;
    double dfStdDev = 0.0;
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              poBand->ComputeStatistics(FALSE, &dfMin, &dfMax, &dfMean,
                                        &dfStdDev, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest, GetMinimumMaximumSourcesAndRecursion) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  // 1. Sources with statistics.
  {
    constexpr char kMmSrc1[] = "/vsimem/vrtsourcedrasterband_test/mm_src1.tif";
    constexpr char kMmSrc2[] = "/vsimem/vrtsourcedrasterband_test/mm_src2.tif";
    {
      GDALDatasetUniquePtr poSrc1(
          poDrv->Create(kMmSrc1, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc1);
      EXPECT_EQ(CE_None, poSrc1->GetRasterBand(1)->SetStatistics(10.0, 100.0,
                                                                 50.0, 20.0));
    }
    {
      GDALDatasetUniquePtr poSrc2(
          poDrv->Create(kMmSrc2, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc2);
      EXPECT_EQ(CE_None, poSrc2->GetRasterBand(1)->SetStatistics(0.0, 255.0,
                                                                 128.0, 50.0));
    }

    VRTDataset ds(20, 10);
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
    auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
    ASSERT_NE(nullptr, poBand);

    ASSERT_EQ(CE_None,
              poBand->AddSimpleSource(kMmSrc1, 1, 0, 0, 10, 10, 0, 0, 10, 10));
    ASSERT_EQ(CE_None,
              poBand->AddSimpleSource(kMmSrc2, 1, 0, 0, 10, 10, 10, 0, 10, 10));

    CPLConfigOptionSetter opt("VRT_MIN_MAX_FROM_SOURCES", "YES", false);

    int bSuccess = FALSE;
    const double dfMin = poBand->GetMinimum(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(0.0, dfMin);

    bSuccess = FALSE;
    const double dfMax = poBand->GetMaximum(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(255.0, dfMax);
  }

  // 2. Recursion detection in GetMinimum, GetMaximum, and GetHistogram.
  {
    constexpr char kRecVrt[] = "/vsimem/vrtsourcedrasterband_test/rec_vrt.vrt";
    constexpr char kRecVrtXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="0">/vsimem/vrtsourcedrasterband_test/rec_vrt.vrt</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
      <DstRect xOff="0" yOff="0" xSize="10" ySize="10"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";
    VsiMemTempWrapper rec_vrt_mem(kRecVrt, kRecVrtXml);

    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(kRecVrt, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poDS);
    GDALRasterBand* poBand = poDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);

    CPLErrorReset();
    WithQuietHandler quiet;

    int bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(0.0, poBand->GetMinimum(&bSuccess));
    EXPECT_FALSE(bSuccess);

    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(255.0, poBand->GetMaximum(&bSuccess));
    EXPECT_FALSE(bSuccess);

    GUIntBig anHistogram[256] = {};
    EXPECT_EQ(CE_Failure, poBand->GetHistogram(-0.5, 255.5, 256, anHistogram,
                                               FALSE, FALSE, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcedRasterBandTest, CanIRasterIOBeForwardedBranches) {
  constexpr char kFwdSrc1[] = "/vsimem/vrtsourcedrasterband_test/fwd_src1.tif";
  constexpr char kFwdSrc2[] = "/vsimem/vrtsourcedrasterband_test/fwd_src2.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrc1(
        poDrv->Create(kFwdSrc1, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrc1);
    int anOverviewList[] = {2};
    EXPECT_EQ(CE_None, poSrc1->BuildOverviews("NEAR", 1, anOverviewList, 0,
                                              nullptr, nullptr, nullptr));
  }
  CreateByteGeoTiff(kFwdSrc2, 20, 20);

  VRTDataset ds(40, 20);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  auto poBand = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  ASSERT_NE(nullptr, poBand);

  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kFwdSrc1, 1, 0, 0, 20, 20, 0, 0, 20, 20));
  ASSERT_EQ(CE_None,
            poBand->AddSimpleSource(kFwdSrc2, 1, 0, 0, 20, 20, 20, 0, 20, 20));

  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.eResampleAlg = GRIORA_Bilinear;

  std::vector<GByte> abyBuf(100, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 20, 20, abyBuf.data(), 10,
                                      10, GDT_Byte, 1, 10, &sExtraArg));

  std::vector<GByte> abyBuf2(200, 0);
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 40, 20, abyBuf2.data(), 20,
                                      10, GDT_Byte, 1, 20, &sExtraArg));
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and multi-threaded statistics
// accumulation error branches in VRTSourcedRasterBand in
// vrtsourcedrasterband.cpp.
