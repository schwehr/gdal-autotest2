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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class VRTWarpedTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }

  static void TearDownTestSuite() {
    OSRCleanup();
    GDALDestroyDriverManager();
  }

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

  GDALWarpOptions* CreateDefaultWarpOptions(int num_bands = 1) {
    GDALWarpOptions* psWO = GDALCreateWarpOptions();
    psWO->nBandCount = num_bands;
    psWO->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int) * num_bands));
    psWO->panDstBands = static_cast<int*>(CPLMalloc(sizeof(int) * num_bands));
    for (int i = 0; i < num_bands; ++i) {
      psWO->panSrcBands[i] = i + 1;
      psWO->panDstBands[i] = i + 1;
    }
    return psWO;
  }

  void CreateGeoTiff(const std::string& filename, int xsize, int ysize,
                     int bands, GDALDataType dt, const double* gt,
                     const char* srs_wkt, const double* nodata,
                     const std::vector<std::vector<double>>& band_data,
                     const std::vector<int>& overviews = {}) {
    TrackFile(filename);
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);

    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), xsize, ysize, bands, dt, nullptr));
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
      if (b - 1 < static_cast<int>(band_data.size())) {
        const auto& data = band_data[b - 1];
        if (!data.empty()) {
          EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                     const_cast<double*>(data.data()), xsize,
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

  std::string GetWGS84WKT() {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    char* pszWKT = nullptr;
    srs.exportToWkt(&pszWKT);
    std::string result(pszWKT);
    CPLFree(pszWKT);
    return result;
  }

  std::string GetWebMercatorWKT() {
    OGRSpatialReference srs;
    srs.importFromEPSG(3857);
    char* pszWKT = nullptr;
    srs.exportToWkt(&pszWKT);
    std::string result(pszWKT);
    CPLFree(pszWKT);
    return result;
  }

  std::string GetUTM31NWKT() {
    OGRSpatialReference srs;
    srs.importFromEPSG(32631);
    char* pszWKT = nullptr;
    srs.exportToWkt(&pszWKT);
    std::string result(pszWKT);
    CPLFree(pszWKT);
    return result;
  }

 private:
  std::vector<std::string> to_unlink_;
};

// =========================================================================
// GDALAutoCreateWarpedVRT & GDALAutoCreateWarpedVRTEx
// =========================================================================

TEST_F(VRTWarpedTest, AutoCreateWarpedVRTBasic) {
  const std::string filename = "/vsimem/warp_src1.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(20 * 20);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<double>(i + 1);
  }
  const double nodata = 0.0;
  CreateGeoTiff(filename, 20, 20, 1, GDT_Byte, gt, wgs84.c_str(), &nodata,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  // 1. AutoCreateWarpedVRT to UTM
  auto hWarped = GDALAutoCreateWarpedVRT(
      poSrcDS.get(), wgs84.c_str(), utm.c_str(), GRA_Bilinear, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);
  EXPECT_GT(poWarped->GetRasterXSize(), 0);
  EXPECT_GT(poWarped->GetRasterYSize(), 0);
  EXPECT_EQ(poWarped->GetRasterCount(), 1);

  // Read full raster
  int nOutX = poWarped->GetRasterXSize();
  int nOutY = poWarped->GetRasterYSize();
  std::vector<GByte> out_buf(nOutX * nOutY);
  EXPECT_EQ(
      poWarped->RasterIO(GF_Read, 0, 0, nOutX, nOutY, out_buf.data(), nOutX,
                         nOutY, GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
      CE_None);

  // Read full raster as Float32 (different type -> CreateDestinationBuffer +
  // GDALCopyWords)
  std::vector<float> f_out(nOutX * nOutY);
  EXPECT_EQ(
      poWarped->RasterIO(GF_Read, 0, 0, nOutX, nOutY, f_out.data(), nOutX,
                         nOutY, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
      CE_None);

  // Read full raster with non-sequential spacing
  std::vector<GByte> nonseq_out(nOutX * nOutY * 2);
  int anBandMap[1] = {1};
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, nOutX, nOutY, nonseq_out.data(),
                               nOutX, nOutY, GDT_Byte, 1, anBandMap, 2,
                               2 * nOutX, 0, nullptr),
            CE_None);

  // Subsampled whole-image read: the dataset builds a temporary rescaled
  // warped dataset using a cloned transformer instead of the generic path.
  if (nOutX >= 4 && nOutY >= 4) {
    int subX = nOutX / 2;
    int subY = nOutY / 2;
    std::vector<GByte> sub_buf(subX * subY);
    EXPECT_EQ(
        poWarped->RasterIO(GF_Read, 0, 0, nOutX, nOutY, sub_buf.data(), subX,
                           subY, GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
        CE_None);

    // Subsampled non-whole-image read falls back to the generic
    // GDALDataset::IRasterIO path.
    std::vector<GByte> part_buf(subX * subY);
    EXPECT_EQ(
        poWarped->RasterIO(GF_Read, 1, 1, nOutX - 2, nOutY - 2, part_buf.data(),
                           subX, subY, GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
        CE_None);
  }

  // Read single block
  int bx = 0, by = 0;
  poWarped->GetRasterBand(1)->GetBlockSize(&bx, &by);
  std::vector<GByte> block_buf(bx * by);
  EXPECT_EQ(poWarped->GetRasterBand(1)->ReadBlock(0, 0, block_buf.data()),
            CE_None);

  // File list contains source file
  char** papszFiles = poWarped->GetFileList();
  EXPECT_NE(papszFiles, nullptr);
  EXPECT_GE(CSLCount(papszFiles), 1);
  CSLDestroy(papszFiles);

  GDALClose(hWarped);
}

TEST_F(VRTWarpedTest, AutoCreateWarpedVRTWithApproxTransformer) {
  const std::string filename = "/vsimem/warp_approx_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(10 * 10, 42.0);
  CreateGeoTiff(filename, 10, 10, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  // Use dfMaxError > 0 to test GDALCreateApproxTransformer
  char** papszTransformerOptions = nullptr;
  papszTransformerOptions =
      CSLSetNameValue(papszTransformerOptions, "SRC_METHOD", "NO_GEOTRANSFORM");
  auto hWarped = GDALAutoCreateWarpedVRTEx(
      poSrcDS.get(), wgs84.c_str(), utm.c_str(), GRA_NearestNeighbour, 0.25,
      nullptr, papszTransformerOptions);
  CSLDestroy(papszTransformerOptions);
  ASSERT_NE(hWarped, nullptr);

  auto* poWarped = GDALDataset::FromHandle(hWarped);
  std::vector<float> buf(poWarped->GetRasterXSize() *
                         poWarped->GetRasterYSize());
  EXPECT_EQ(
      poWarped->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                         poWarped->GetRasterYSize(), buf.data(),
                         poWarped->GetRasterXSize(), poWarped->GetRasterYSize(),
                         GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
      CE_None);

  GDALClose(hWarped);
}

TEST_F(VRTWarpedTest, AutoCreateWarpedVRTWithDstAlphaBand) {
  const std::string filename = "/vsimem/warp_alpha_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(10 * 10, 128.0);
  CreateGeoTiff(filename, 10, 10, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  GDALWarpOptions* psWO = CreateDefaultWarpOptions(1);
  psWO->nDstAlphaBand = 2;  // Create destination alpha band

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, psWO);
  GDALDestroyWarpOptions(psWO);
  ASSERT_NE(hWarped, nullptr);

  auto* poWarped = GDALDataset::FromHandle(hWarped);
  EXPECT_EQ(poWarped->GetRasterCount(), 2);
  EXPECT_EQ(poWarped->GetRasterBand(2)->GetColorInterpretation(),
            GCI_AlphaBand);

  // WriteBlock on alpha band should succeed (CE_None), while on band 1 should
  // fail
  int bx = 0, by = 0;
  poWarped->GetRasterBand(2)->GetBlockSize(&bx, &by);
  std::vector<GByte> block(bx * by, 255);
  EXPECT_EQ(poWarped->GetRasterBand(2)->WriteBlock(0, 0, block.data()),
            CE_None);
  EXPECT_EQ(poWarped->GetRasterBand(2)->ReadBlock(0, 0, block.data()), CE_None);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(poWarped->GetRasterBand(1)->WriteBlock(0, 0, block.data()),
            CE_None);
  CPLPopErrorHandler();

  GDALClose(hWarped);
}

TEST_F(VRTWarpedTest, AutoCreateWarpedVRTNodataHandling) {
  const std::string filename = "/vsimem/warp_nodata_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  // Test with nodata value within byte range
  const double nodata = 255.0;
  std::vector<double> data(10 * 10, 100.0);
  data[0] = nodata;
  CreateGeoTiff(filename, 10, 10, 1, GDT_Byte, gt, wgs84.c_str(), &nodata,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  int bHasNoData = FALSE;
  double dfNoData = poWarped->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
  EXPECT_TRUE(bHasNoData);
  EXPECT_DOUBLE_EQ(dfNoData, 255.0);

  GDALClose(hWarped);
}

TEST_F(VRTWarpedTest, AutoCreateWarpedVRTValidation) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Null source dataset
  EXPECT_EQ(GDALAutoCreateWarpedVRT(nullptr, nullptr, nullptr,
                                    GRA_NearestNeighbour, 0.0, nullptr),
            nullptr);
  EXPECT_EQ(
      GDALAutoCreateWarpedVRTEx(nullptr, nullptr, nullptr, GRA_NearestNeighbour,
                                0.0, nullptr, nullptr),
      nullptr);

  // GDALCreateWarpedVRT with null pointers
  EXPECT_EQ(GDALCreateWarpedVRT(nullptr, 10, 10, nullptr, nullptr), nullptr);

  // GDALInitializeWarpedVRT validation
  EXPECT_EQ(GDALInitializeWarpedVRT(nullptr, nullptr), CE_Failure);

  VRTWarpedDataset ds(10, 10);
  GDALWarpOptions* psWO = CreateDefaultWarpOptions(1);
  EXPECT_EQ(GDALInitializeWarpedVRT(&ds, psWO), CE_Failure);
  GDALDestroyWarpOptions(psWO);

  CPLPopErrorHandler();
}

// =========================================================================
// XML Serialization and Deserialization
// =========================================================================

TEST_F(VRTWarpedTest, XMLSerializationAndDeserialization) {
  const std::string filename = "/vsimem/warp_ser_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(10 * 10, 50.0);
  CreateGeoTiff(filename, 10, 10, 1, GDT_Int16, gt, wgs84.c_str(), nullptr,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  // Serialize to XML
  auto* poVRT = dynamic_cast<VRTWarpedDataset*>(poWarped);
  ASSERT_NE(poVRT, nullptr);
  auto* psTree = poVRT->SerializeToXML("/vsimem/warped_test.vrt");
  ASSERT_NE(psTree, nullptr);

  // Also test SerializeToXML with relative filename
  auto* psTreeRel = poVRT->SerializeToXML("relative_warped.vrt");
  if (psTreeRel != nullptr) {
    CPLDestroyXMLNode(psTreeRel);
  }

  char* pszXML = CPLSerializeXMLTree(psTree);
  ASSERT_NE(pszXML, nullptr);
  CPLDestroyXMLNode(psTree);

  // Save XML to /vsimem/ and open with GDALOpen
  TrackFile("/vsimem/warped_test.vrt");
  VSILFILE* fp = VSIFOpenL("/vsimem/warped_test.vrt", "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(pszXML, 1, strlen(pszXML), fp);
  VSIFCloseL(fp);
  CPLFree(pszXML);

  auto poOpened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/warped_test.vrt", GDAL_OF_RASTER));
  ASSERT_NE(poOpened, nullptr);
  EXPECT_EQ(poOpened->GetRasterXSize(), poWarped->GetRasterXSize());
  EXPECT_EQ(poOpened->GetRasterYSize(), poWarped->GetRasterYSize());
  EXPECT_EQ(poOpened->GetRasterCount(), 1);

  // Read data from reopened dataset
  std::vector<int16_t> buf(poOpened->GetRasterXSize() *
                           poOpened->GetRasterYSize());
  EXPECT_EQ(
      poOpened->RasterIO(GF_Read, 0, 0, poOpened->GetRasterXSize(),
                         poOpened->GetRasterYSize(), buf.data(),
                         poOpened->GetRasterXSize(), poOpened->GetRasterYSize(),
                         GDT_Int16, 1, nullptr, 0, 0, 0, nullptr),
      CE_None);

  GDALClose(hWarped);
}

TEST_F(VRTWarpedTest, XMLInitErrorBranches) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Missing GDALWarpOptions
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <VRTRasterBand dataType='Byte' band='1' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 2. Inconsistent band block size
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <BlockXSize>128</BlockXSize>\n"
        "  <BlockYSize>128</BlockYSize>\n"
        "  <VRTRasterBand dataType='Byte' band='1' "
        "subClass='VRTWarpedRasterBand'>\n"
        "    <BlockXSize>64</BlockXSize>\n"
        "    <BlockYSize>64</BlockYSize>\n"
        "  </VRTRasterBand>\n"
        "  <GDALWarpOptions/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 3. Invalid WarpOptions deserialization
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <VRTRasterBand dataType='Byte' band='1' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "  <GDALWarpOptions>\n"
        "    <WorkingDataType>InvalidDataType</WorkingDataType>\n"
        "  </GDALWarpOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 4. WarpOptions where m_poWarper->Initialize fails (dfWarpMemoryLimit = 1 is
  // invalid)
  {
    const std::string filename = "/vsimem/warp_err_src.tif";
    const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
    std::string wgs84 = GetWGS84WKT();
    std::vector<double> data(10 * 10, 1.0);
    CreateGeoTiff(filename, 10, 10, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                  {data});

    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <VRTRasterBand dataType='Byte' band='1' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "  <GDALWarpOptions>\n"
        "    <SourceDataset "
        "relativeToVRT='0'>/vsimem/warp_err_src.tif</SourceDataset>\n"
        "    <WarpMemoryLimit>1</WarpMemoryLimit>\n"
        "  </GDALWarpOptions>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 5. XML with VerticalShiftGrid
  {
    const std::string gridname = "/vsimem/vgrid.tif";
    const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
    std::string wgs84 = GetWGS84WKT();
    std::vector<double> gdata(10 * 10, 0.5);
    CreateGeoTiff(gridname, 10, 10, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                  {gdata});

    const std::string srcname = "/vsimem/vsrc.tif";
    std::vector<double> sdata(10 * 10, 10.0);
    CreateGeoTiff(srcname, 10, 10, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                  {sdata});

    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <VRTRasterBand dataType='Float32' band='1' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "  <BlockXSize>128</BlockXSize>\n"
        "  <BlockYSize>128</BlockYSize>\n"
        "  <GDALWarpOptions>\n"
        "    <SourceDataset "
        "relativeToVRT='0'>/vsimem/vsrc.tif</SourceDataset>\n"
        "    <Transformer>\n"
        "      <ApproxTransformer>\n"
        "        <MaxError>0.125</MaxError>\n"
        "        <BaseTransformer>\n"
        "          <GenImgProjTransformer>\n"
        "            <SrcGeoTransform>2,0.01,0,49,0,-0.01</SrcGeoTransform>\n"
        "            <DstGeoTransform>2,0.01,0,49,0,-0.01</DstGeoTransform>\n"
        "          </GenImgProjTransformer>\n"
        "        </BaseTransformer>\n"
        "      </ApproxTransformer>\n"
        "    </Transformer>\n"
        "    <BandList>\n"
        "      <BandMapping src='1' dst='1'/>\n"
        "    </BandList>\n"
        "  </GDALWarpOptions>\n"
        "  <VerticalShiftGrid inverse='1' toMeterSrc='1' "
        "toMeterDest='1'>/vsimem/vgrid.tif</VerticalShiftGrid>\n"
        "</VRTDataset>\n";
    auto poVGridDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    EXPECT_NE(poVGridDS, nullptr);
  }

  // 6. XML with multiple bands, relativeToVRT="1", and DstDataset
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='10' rasterYSize='10' "
        "subClass='VRTWarpedDataset'>\n"
        "  <VRTRasterBand dataType='Float32' band='1' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "  <VRTRasterBand dataType='Float32' band='2' "
        "subClass='VRTWarpedRasterBand'/>\n"
        "  <BlockXSize>128</BlockXSize>\n"
        "  <BlockYSize>128</BlockYSize>\n"
        "  <GDALWarpOptions>\n"
        "    <SourceDataset relativeToVRT='1'>vsrc.tif</SourceDataset>\n"
        "    <DstDataset>/vsimem/vsrc.tif</DstDataset>\n"
        "    <Transformer>\n"
        "      <ApproxTransformer>\n"
        "        <MaxError>0.125</MaxError>\n"
        "        <BaseTransformer>\n"
        "          <GenImgProjTransformer>\n"
        "            <SrcGeoTransform>2,0.01,0,49,0,-0.01</SrcGeoTransform>\n"
        "            <DstGeoTransform>2,0.01,0,49,0,-0.01</DstGeoTransform>\n"
        "          </GenImgProjTransformer>\n"
        "        </BaseTransformer>\n"
        "      </ApproxTransformer>\n"
        "    </Transformer>\n"
        "    <BandList>\n"
        "      <BandMapping src='1' dst='1'/>\n"
        "      <BandMapping src='1' dst='2'/>\n"
        "    </BandList>\n"
        "  </GDALWarpOptions>\n"
        "</VRTDataset>\n";
    TrackFile("/vsimem/multi_band.vrt");
    VSILFILE* fp = VSIFOpenL("/vsimem/multi_band.vrt", "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(vrt_xml.c_str(), 1, vrt_xml.size(), fp);
    VSIFCloseL(fp);

    auto poMulti = std::unique_ptr<GDALDataset>(
        GDALDataset::Open("/vsimem/multi_band.vrt", GDAL_OF_RASTER));
    EXPECT_NE(poMulti, nullptr);
    if (poMulti) {
      EXPECT_EQ(poMulti->GetRasterCount(), 2);
    }
  }

  CPLPopErrorHandler();
}

// =========================================================================
// SrcOvrLevel Metadata and Overview Handling
// =========================================================================

TEST_F(VRTWarpedTest, SrcOvrLevelMetadata) {
  VRTWarpedDataset ds(10, 10);

  // "AUTO" -> -2
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", "AUTO"), CE_None);
  // "AUTO-1" -> -3
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", "AUTO-1"), CE_None);
  // "NONE" -> -1
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", "NONE"), CE_None);
  // Integer "2" -> 2
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", "2"), CE_None);
  // Null value
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", nullptr), CE_None);
  // Other domain
  EXPECT_EQ(ds.SetMetadataItem("SrcOvrLevel", "AUTO", "CUSTOM_DOMAIN"),
            CE_None);
  EXPECT_STREQ(ds.GetMetadataItem("SrcOvrLevel", "CUSTOM_DOMAIN"), "AUTO");
}

TEST_F(VRTWarpedTest, OverviewsAndResampling) {
  const std::string filename = "/vsimem/warp_ovr_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(40 * 40, 10.0);
  CreateGeoTiff(filename, 40, 40, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                {data}, {2, 4});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  // Overview count on band
  auto* poBand = poWarped->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);
  int nOvrCount = poBand->GetOverviewCount();
  EXPECT_GT(nOvrCount, 0);

  // Out of bound overview
  EXPECT_EQ(poBand->GetOverview(-1), nullptr);
  EXPECT_EQ(poBand->GetOverview(99), nullptr);

  // Valid overview
  auto* poOvr = poBand->GetOverview(0);
  ASSERT_NE(poOvr, nullptr);
  EXPECT_GT(poOvr->GetXSize(), 0);
  EXPECT_GT(poOvr->GetYSize(), 0);

  // Read from overview
  std::vector<float> ovr_data(poOvr->GetXSize() * poOvr->GetYSize());
  EXPECT_EQ(poOvr->RasterIO(GF_Read, 0, 0, poOvr->GetXSize(), poOvr->GetYSize(),
                            ovr_data.data(), poOvr->GetXSize(),
                            poOvr->GetYSize(), GDT_Float32, 0, 0),
            CE_None);

  // Downsampled read on dataset triggering overview / downsampling code
  std::vector<float> ds_data(10 * 10);
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                               poWarped->GetRasterYSize(), ds_data.data(), 10,
                               10, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);

  // Subsampled read with GDAL_VRT_WARP_USE_DATASET_RASTERIO="NO"
  CPLSetConfigOption("GDAL_VRT_WARP_USE_DATASET_RASTERIO", "NO");
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                               poWarped->GetRasterYSize(), ds_data.data(), 10,
                               10, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);
  CPLSetConfigOption("GDAL_VRT_WARP_USE_DATASET_RASTERIO", nullptr);

  // BuildOverviews on VRTWarpedDataset
  int anOvrList[1] = {2};
  EXPECT_EQ(poWarped->BuildOverviews("NEAREST", 1, anOvrList, 0, nullptr,
                                     GDALDummyProgress, nullptr),
            CE_None);

  GDALClose(hWarped);
}

// =========================================================================
// AddBand and BlockSize
// =========================================================================

TEST_F(VRTWarpedTest, AddBandAndBlockSize) {
  VRTWarpedDataset ds(100, 100, 32, 32);
  int bx = 0, by = 0;
  ds.GetBlockSize(&bx, &by);
  EXPECT_EQ(bx, 32);
  EXPECT_EQ(by, 32);

  EXPECT_EQ(ds.AddBand(GDT_Int16, nullptr), CE_None);
  EXPECT_EQ(ds.GetRasterCount(), 1);
  EXPECT_EQ(ds.GetRasterBand(1)->GetRasterDataType(), GDT_Int16);
  ds.GetRasterBand(1)->GetBlockSize(&bx, &by);
  EXPECT_EQ(bx, 32);
  EXPECT_EQ(by, 32);
}

// =========================================================================
// Cutline and Coord Rescaler
// =========================================================================

TEST_F(VRTWarpedTest, CutlineAndCoordRescaler) {
  const std::string filename = "/vsimem/warp_cutline_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(40 * 40, 100.0);
  CreateGeoTiff(filename, 40, 40, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                {data}, {2, 4});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  OGRPolygon polygon;
  OGRLinearRing ring;
  ring.addPoint(2.0, 49.0);
  ring.addPoint(2.2, 49.0);
  ring.addPoint(2.2, 48.8);
  ring.addPoint(2.0, 48.8);
  ring.addPoint(2.0, 49.0);
  polygon.addRing(&ring);

  GDALWarpOptions* psWO = CreateDefaultWarpOptions(1);
  psWO->hCutline = polygon.clone();

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, psWO);
  GDALDestroyWarpOptions(psWO);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  // Trigger CreateImplicitOverview which runs GDALWarpCoordRescaler
  auto* poBand = poWarped->GetRasterBand(1);
  EXPECT_GT(poBand->GetOverviewCount(), 0);
  auto* poOvr = poBand->GetOverview(0);
  ASSERT_NE(poOvr, nullptr);
  EXPECT_GT(poOvr->GetXSize(), 0);

  std::vector<GByte> buf(poOvr->GetXSize() * poOvr->GetYSize());
  EXPECT_EQ(poOvr->RasterIO(GF_Read, 0, 0, poOvr->GetXSize(), poOvr->GetYSize(),
                            buf.data(), poOvr->GetXSize(), poOvr->GetYSize(),
                            GDT_Byte, 0, 0),
            CE_None);

  GDALClose(hWarped);
}

// =========================================================================
// BuildOverviews and Overview Transformer
// =========================================================================

TEST_F(VRTWarpedTest, BuildOverviewsAndOverviewTransformer) {
  const std::string filename = "/vsimem/warp_bldovr_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(64 * 64, 10.0);
  CreateGeoTiff(filename, 64, 64, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                {data}, {2});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  // Build new overview levels 4 and 8
  int anOvrList[2] = {4, 8};

  // Progress function returning 0 (user cancelled)
  auto failProgress = [](double, const char*, void*) -> int { return 0; };
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(poWarped->BuildOverviews("NEAREST", 1, anOvrList, 0, nullptr,
                                     failProgress, nullptr),
            CE_None);
  CPLPopErrorHandler();

  EXPECT_EQ(poWarped->BuildOverviews("NEAREST", 2, anOvrList, 0, nullptr,
                                     GDALDummyProgress, nullptr),
            CE_None);

  auto* poBand = poWarped->GetRasterBand(1);
  EXPECT_GE(poBand->GetOverviewCount(), 2);

  // Read from the newly created overview (runs VRTWarpedOverviewTransform)
  auto* poOvr = poBand->GetOverview(poBand->GetOverviewCount() - 1);
  ASSERT_NE(poOvr, nullptr);
  std::vector<float> buf(poOvr->GetXSize() * poOvr->GetYSize());
  EXPECT_EQ(poOvr->RasterIO(GF_Read, 0, 0, poOvr->GetXSize(), poOvr->GetYSize(),
                            buf.data(), poOvr->GetXSize(), poOvr->GetYSize(),
                            GDT_Float32, 0, 0),
            CE_None);

  // BuildOverviews with no warper should fail
  VRTWarpedDataset dsNoWarper(10, 10);
  EXPECT_EQ(dsNoWarper.BuildOverviews("NEAREST", 1, anOvrList, 0, nullptr,
                                      GDALDummyProgress, nullptr),
            CE_Failure);

  // Serialize to XML (serializes OverviewList)
  auto* poVRT = dynamic_cast<VRTWarpedDataset*>(poWarped);
  ASSERT_NE(poVRT, nullptr);
  auto* psTree = poVRT->SerializeToXML("/vsimem/ovr_list.vrt");
  ASSERT_NE(psTree, nullptr);
  EXPECT_NE(CPLGetXMLNode(psTree, "OverviewList"), nullptr);
  CPLDestroyXMLNode(psTree);

  GDALClose(hWarped);
}

// =========================================================================
// WarpMemoryLimit splitting and buffer conversion
// =========================================================================

TEST_F(VRTWarpedTest, WarpMemoryLimitSplittingAndBufferConversion) {
  const std::string filename = "/vsimem/warp_mem_src.tif";
  const double gt[6] = {2.0, 0.001, 0.0, 49.0, 0.0, -0.001};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  constexpr int kXSize = 160;
  constexpr int kYSize = 60;
  std::vector<double> data(kXSize * kYSize, 5.0);
  CreateGeoTiff(filename, kXSize, kYSize, 1, GDT_Float64, gt, wgs84.c_str(),
                nullptr, {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  GDALWarpOptions* psWO = CreateDefaultWarpOptions(1);
  psWO->dfWarpMemoryLimit =
      100000.0;  // 100KB, triggers splitting for 120x120 Float64

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, psWO);
  GDALDestroyWarpOptions(psWO);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped = GDALDataset::FromHandle(hWarped);

  // Case 1: nXSize >= nYSize (horizontal split)
  int nW = poWarped->GetRasterXSize();
  int nH = poWarped->GetRasterYSize();
  std::vector<double> buf(nW * nH);
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, nW, nH, buf.data(), nW, nH,
                               GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);

  // Case 2: nYSize > nXSize (vertical split)
  if (nH >= 4) {
    int subW = 4;
    int subH = nH;
    std::vector<double> sub_buf(subW * subH);
    EXPECT_EQ(
        poWarped->RasterIO(GF_Read, 0, 0, subW, subH, sub_buf.data(), subW,
                           subH, GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
        CE_None);
  }

  // Case 3: Read into Float32 buffer (conversion from Float64)
  std::vector<float> f_buf(nW * nH);
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, nW, nH, f_buf.data(), nW, nH,
                               GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);

  // Case 4: Non-sequential buffer spacing (nPixelSpace = 2 * sizeof(double))
  std::vector<double> nonseq_buf(nW * nH * 2);
  int anBandMap[1] = {1};
  EXPECT_EQ(poWarped->RasterIO(GF_Read, 0, 0, nW, nH, nonseq_buf.data(), nW, nH,
                               GDT_Float64, 1, anBandMap, 2 * sizeof(double),
                               2 * nW * sizeof(double), 0, nullptr),
            CE_None);

  // Case 5: Force horizontal split (nXSize >= nYSize)
  if (nW >= 10) {
    int subW = nW;
    int subH = 4;
    std::vector<double> sub_h_buf(subW * subH);
    EXPECT_EQ(
        poWarped->RasterIO(GF_Read, 0, 0, subW, subH, sub_h_buf.data(), subW,
                           subH, GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
        CE_None);
  }

  GDALClose(hWarped);

  // Vertical split: dataset where height > width (nXSize < nYSize)
  const std::string filename_v = "/vsimem/warp_mem_v_src.tif";
  constexpr int kXSizeV = 60;
  constexpr int kYSizeV = 160;
  std::vector<double> data_v(kXSizeV * kYSizeV, 5.0);
  CreateGeoTiff(filename_v, kXSizeV, kYSizeV, 1, GDT_Float64, gt, wgs84.c_str(),
                nullptr, {data_v});

  auto poSrcDSV = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename_v.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDSV, nullptr);

  GDALWarpOptions* psWOV = CreateDefaultWarpOptions(1);
  psWOV->dfWarpMemoryLimit = 100000.0;

  auto hWarpedV =
      GDALAutoCreateWarpedVRT(poSrcDSV.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, psWOV);
  GDALDestroyWarpOptions(psWOV);
  ASSERT_NE(hWarpedV, nullptr);
  auto* poWarpedV = GDALDataset::FromHandle(hWarpedV);

  int nWV = poWarpedV->GetRasterXSize();
  int nHV = poWarpedV->GetRasterYSize();
  std::vector<double> buf_v(nWV * nHV);
  EXPECT_EQ(poWarpedV->RasterIO(GF_Read, 0, 0, nWV, nHV, buf_v.data(), nWV, nHV,
                                GDT_Float64, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);

  GDALClose(hWarpedV);
}

// =========================================================================
// ProcessBlock with partial edge block
// =========================================================================

TEST_F(VRTWarpedTest, ProcessBlockPartialBlock) {
  const std::string filename = "/vsimem/warp_partblock_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(25 * 25, 7.0);
  CreateGeoTiff(filename, 25, 25, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped =
      dynamic_cast<VRTWarpedDataset*>(GDALDataset::FromHandle(hWarped));
  ASSERT_NE(poWarped, nullptr);

  int bx = 0, by = 0;
  poWarped->GetBlockSize(&bx, &by);
  int nBlocksX = (poWarped->GetRasterXSize() + bx - 1) / bx;
  int nBlocksY = (poWarped->GetRasterYSize() + by - 1) / by;

  EXPECT_EQ(poWarped->ProcessBlock(nBlocksX - 1, nBlocksY - 1), CE_None);

  // Partial block with explicit BlockXSize and BlockYSize
  auto* psTree = poWarped->SerializeToXML("/vsimem/partblock.vrt");
  ASSERT_NE(psTree, nullptr);
  CPLSetXMLValue(psTree, "BlockXSize", "16");
  CPLSetXMLValue(psTree, "BlockYSize", "16");
  CPLSetXMLValue(psTree, "VRTRasterBand.BlockXSize", "16");
  CPLSetXMLValue(psTree, "VRTRasterBand.BlockYSize", "16");
  char* pszXML = CPLSerializeXMLTree(psTree);
  CPLDestroyXMLNode(psTree);
  ASSERT_NE(pszXML, nullptr);

  TrackFile("/vsimem/partblock.vrt");
  VSILFILE* fp = VSIFOpenL("/vsimem/partblock.vrt", "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(pszXML, 1, strlen(pszXML), fp);
  VSIFCloseL(fp);
  CPLFree(pszXML);

  auto poOpened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/partblock.vrt", GDAL_OF_RASTER));
  ASSERT_NE(poOpened, nullptr);
  auto* poWarpedBlock = dynamic_cast<VRTWarpedDataset*>(poOpened.get());
  ASSERT_NE(poWarpedBlock, nullptr);

  int bx2 = 0, by2 = 0;
  poWarpedBlock->GetBlockSize(&bx2, &by2);
  EXPECT_EQ(bx2, 16);
  EXPECT_EQ(by2, 16);
  int nBlocksX2 = (poWarpedBlock->GetRasterXSize() + bx2 - 1) / bx2;
  int nBlocksY2 = (poWarpedBlock->GetRasterYSize() + by2 - 1) / by2;

  EXPECT_EQ(poWarpedBlock->ProcessBlock(nBlocksX2 - 1, nBlocksY2 - 1), CE_None);

  GDALClose(hWarped);
}

// =========================================================================
// SrcOvrLevel and GetBestOverviewLevel
// =========================================================================

TEST_F(VRTWarpedTest, SrcOvrLevelAndGetBestOverviewLevel) {
  const std::string filename = "/vsimem/warp_bestovr_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(64 * 64, 3.0);
  CreateGeoTiff(filename, 64, 64, 2, GDT_Float32, gt, wgs84.c_str(), nullptr,
                {data, data}, {2, 4});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped =
      dynamic_cast<VRTWarpedDataset*>(GDALDataset::FromHandle(hWarped));
  ASSERT_NE(poWarped, nullptr);

  auto* poBand = poWarped->GetRasterBand(1);

  // Test with different SrcOvrLevel settings
  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "AUTO-1"), CE_None);
  std::vector<float> buf(16 * 16);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf.data(), 16, 16,
                             GDT_Float32, 0, 0),
            CE_None);

  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "0"), CE_None);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf.data(), 16, 16,
                             GDT_Float32, 0, 0),
            CE_None);

  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "NONE"), CE_None);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf.data(), 16, 16,
                             GDT_Float32, 0, 0),
            CE_None);

  // Test with GRIORA_Bilinear to exercise floating point window adjustments
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.eResampleAlg = GRIORA_Bilinear;
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf.data(), 16, 16,
                             GDT_Float32, 0, 0, &sExtraArg),
            CE_None);

  sExtraArg.bFloatingPointWindowValidity = true;
  sExtraArg.dfXOff = 0.0;
  sExtraArg.dfYOff = 0.0;
  sExtraArg.dfXSize = poWarped->GetRasterXSize();
  sExtraArg.dfYSize = poWarped->GetRasterYSize();
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf.data(), 16, 16,
                             GDT_Float32, 0, 0, &sExtraArg),
            CE_None);

  // Read near bottom-right corner to exercise clipping
  EXPECT_EQ(poBand->RasterIO(GF_Read, poWarped->GetRasterXSize() - 4,
                             poWarped->GetRasterYSize() - 4, 4, 4, buf.data(),
                             4, 4, GDT_Float32, 0, 0),
            CE_None);

  // Subsampled read with nBufYSize == 1: the desired downsampling factor is
  // taken from the X axis regardless of the Y ratio.
  std::vector<float> buf_y1(16);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf_y1.data(), 16, 1,
                             GDT_Float32, 0, 0),
            CE_None);

  // Subsampled read with (nXSize/nBufXSize) > (nYSize/nBufYSize): the
  // desired downsampling factor is taken from the Y axis.
  std::vector<float> buf_xy(8 * 16);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, poWarped->GetRasterXSize(),
                             poWarped->GetRasterYSize(), buf_xy.data(), 8, 16,
                             GDT_Float32, 0, 0),
            CE_None);

  // Serialize with SrcOvrLevel == "AUTO-1": a level below -2 is written back
  // as "AUTO<n>".
  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "AUTO-1"), CE_None);
  auto* psTree1 = poWarped->SerializeToXML("/vsimem/ovr1.vrt");
  ASSERT_NE(psTree1, nullptr);
  CPLDestroyXMLNode(psTree1);

  // Serialize with SrcOvrLevel == "2": a non-negative level is written as its
  // integer value.
  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "2"), CE_None);
  auto* psTree2 = poWarped->SerializeToXML("/vsimem/ovr2.vrt");
  ASSERT_NE(psTree2, nullptr);
  CPLDestroyXMLNode(psTree2);

  // Serialize with SrcOvrLevel == "NONE": level -1 is written as "NONE".
  EXPECT_EQ(poWarped->SetMetadataItem("SrcOvrLevel", "NONE"), CE_None);
  auto* psTreeNone = poWarped->SerializeToXML("/vsimem/ovr_none.vrt");
  ASSERT_NE(psTreeNone, nullptr);
  CPLDestroyXMLNode(psTreeNone);

  // Serialize with SrcOvrLevel != -2
  auto* psTree = poWarped->SerializeToXML("/vsimem/src_ovr_level.vrt");
  ASSERT_NE(psTree, nullptr);
  EXPECT_NE(CPLGetXMLNode(psTree, "SrcOvrLevel"), nullptr);
  CPLDestroyXMLNode(psTree);

  GDALClose(hWarped);
}

// =========================================================================
// XMLInit additional elements (VerticalShiftGrids, OverviewList, etc.)
// =========================================================================

TEST_F(VRTWarpedTest, XMLInitAdditionalElements) {
  const std::string filename = "/vsimem/warp_xml_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();
  std::string utm = GetUTM31NWKT();

  std::vector<double> data(16 * 16, 1.0);
  CreateGeoTiff(filename, 16, 16, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                {data}, {2});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  auto hWarped =
      GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped =
      dynamic_cast<VRTWarpedDataset*>(GDALDataset::FromHandle(hWarped));
  ASSERT_NE(poWarped, nullptr);

  auto* psTree = poWarped->SerializeToXML("/vsimem/xml_extra.vrt");
  ASSERT_NE(psTree, nullptr);

  // Inject VerticalShiftGrids element
  CPLXMLNode* psVGrids =
      CPLCreateXMLNode(psTree, CXT_Element, "VerticalShiftGrids");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGrids, CXT_Attribute, "Grids"), CXT_Text,
                   "/vsimem/non_existent_grid.tif");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGrids, CXT_Attribute, "Inverse"),
                   CXT_Text, "1");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGrids, CXT_Attribute, "ToMeterSrc"),
                   CXT_Text, "1.0");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGrids, CXT_Attribute, "ToMeterDest"),
                   CXT_Text, "1.0");
  CPLXMLNode* psOpt = CPLCreateXMLNode(psVGrids, CXT_Element, "Option");
  CPLCreateXMLNode(CPLCreateXMLNode(psOpt, CXT_Attribute, "name"), CXT_Text,
                   "TEST_OPT");
  CPLCreateXMLNode(psOpt, CXT_Text, "VAL");

  // Inject SrcOvrLevel
  CPLCreateXMLElementAndValue(psTree, "SrcOvrLevel", "AUTO-1");

  // Inject OverviewList
  CPLCreateXMLElementAndValue(psTree, "OverviewList", "4 8");

  char* pszXML = CPLSerializeXMLTree(psTree);
  CPLDestroyXMLNode(psTree);
  ASSERT_NE(pszXML, nullptr);

  TrackFile("/vsimem/xml_extra.vrt");
  VSILFILE* fp = VSIFOpenL("/vsimem/xml_extra.vrt", "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(pszXML, 1, strlen(pszXML), fp);
  VSIFCloseL(fp);
  CPLFree(pszXML);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto poOpened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/xml_extra.vrt", GDAL_OF_RASTER));
  CPLPopErrorHandler();
  ASSERT_NE(poOpened, nullptr);

  // Also test with an existing valid vertical shift grid
  const std::string valid_grid = "/vsimem/valid_grid.tif";
  std::vector<double> gdata(16 * 16, 2.0);
  CreateGeoTiff(valid_grid, 16, 16, 1, GDT_Float32, gt, wgs84.c_str(), nullptr,
                {gdata});

  auto* psTreeValid = poWarped->SerializeToXML("/vsimem/xml_vgrid.vrt");
  ASSERT_NE(psTreeValid, nullptr);
  CPLXMLNode* psVGridsValid =
      CPLCreateXMLNode(psTreeValid, CXT_Element, "VerticalShiftGrids");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGridsValid, CXT_Attribute, "Grids"),
                   CXT_Text, valid_grid.c_str());
  CPLCreateXMLNode(CPLCreateXMLNode(psVGridsValid, CXT_Attribute, "Inverse"),
                   CXT_Text, "0");
  CPLCreateXMLNode(CPLCreateXMLNode(psVGridsValid, CXT_Attribute, "ToMeterSrc"),
                   CXT_Text, "1.0");
  CPLCreateXMLNode(
      CPLCreateXMLNode(psVGridsValid, CXT_Attribute, "ToMeterDest"), CXT_Text,
      "1.0");
  char* pszXMLValid = CPLSerializeXMLTree(psTreeValid);
  CPLDestroyXMLNode(psTreeValid);
  ASSERT_NE(pszXMLValid, nullptr);

  TrackFile("/vsimem/xml_vgrid.vrt");
  VSILFILE* fpValid = VSIFOpenL("/vsimem/xml_vgrid.vrt", "wb");
  ASSERT_NE(fpValid, nullptr);
  VSIFWriteL(pszXMLValid, 1, strlen(pszXMLValid), fpValid);
  VSIFCloseL(fpValid);
  CPLFree(pszXMLValid);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto poOpenedValid = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/xml_vgrid.vrt", GDAL_OF_RASTER));
  CPLPopErrorHandler();

  GDALClose(hWarped);
}

// =========================================================================
// Projection and CloseDependentDatasets
// =========================================================================

TEST_F(VRTWarpedTest, ProjectionAndCloseDependentDatasets) {
  const std::string filename = "/vsimem/warp_proj_src.tif";
  const double gt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  std::string wgs84 = GetWGS84WKT();

  std::vector<double> data(10 * 10, 1.0);
  CreateGeoTiff(filename, 10, 10, 1, GDT_Byte, gt, wgs84.c_str(), nullptr,
                {data});

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(filename.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poSrcDS, nullptr);

  // 1. pszDstWKT == nullptr, pszSrcWKT != nullptr
  auto hWarped = GDALAutoCreateWarpedVRT(poSrcDS.get(), wgs84.c_str(), nullptr,
                                         GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped, nullptr);
  auto* poWarped =
      dynamic_cast<VRTWarpedDataset*>(GDALDataset::FromHandle(hWarped));
  ASSERT_NE(poWarped, nullptr);
  EXPECT_NE(poWarped->GetProjectionRef(), nullptr);

  // GDALClose will invoke CloseDependentDatasets()
  GDALClose(hWarped);

  // 2. Both null
  auto hWarped2 = GDALAutoCreateWarpedVRT(poSrcDS.get(), nullptr, nullptr,
                                          GRA_NearestNeighbour, 0.0, nullptr);
  ASSERT_NE(hWarped2, nullptr);
  GDALClose(hWarped2);
}

}  // namespace
