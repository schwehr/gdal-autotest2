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

#include "third_party/gdal/frmts/vrt/vrtdataset.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class VRTDatasetTest : public ::testing::Test {
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

  // Creates a zero-filled /vsimem file of `size` bytes owned by the VSI layer.
  void CreateMemFile(const std::string& filename, size_t size) {
    TrackFile(filename);
    VSILFILE* fp = VSIFOpenL(filename.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    std::vector<GByte> zeros(size, 0);
    ASSERT_EQ(VSIFWriteL(zeros.data(), 1, zeros.size(), fp), zeros.size());
    VSIFCloseL(fp);
  }

  void CreateGeoTiff(const std::string& filename, int xsize, int ysize,
                     int bands, GDALDataType dt, const double* gt,
                     const char* srs_wkt, double nodata,
                     const std::vector<std::vector<double>>& band_data) {
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
      srs.SetFromUserInput(srs_wkt);
      EXPECT_EQ(poDS->SetSpatialRef(&srs), CE_None);
    }

    for (int b = 1; b <= bands; ++b) {
      auto poBand = poDS->GetRasterBand(b);
      ASSERT_NE(poBand, nullptr);
      poBand->SetNoDataValue(nodata);
      if (static_cast<size_t>(b - 1) < band_data.size()) {
        const auto& data = band_data[b - 1];
        if (data.size() == static_cast<size_t>(xsize * ysize)) {
          std::vector<double> copy_data = data;
          EXPECT_EQ(
              poBand->RasterIO(GF_Write, 0, 0, xsize, ysize, copy_data.data(),
                               xsize, ysize, GDT_Float64, 0, 0, nullptr),
              CE_None);
        }
      }
    }
    poDS->FlushCache(false);
  }

  std::vector<std::string> to_unlink_;
};

class TestVRTDataset : public VRTDataset {
 public:
  using VRTDataset::CloseDependentDatasets;
  using VRTDataset::VRTDataset;
};

// 1. BasicConstructionAndAccessors
TEST_F(VRTDatasetTest, BasicConstructionAndAccessors) {
  // Test constructor block sizing
  VRTDataset ds1(100, 200, 0, 0);
  EXPECT_EQ(ds1.GetRasterXSize(), 100);
  EXPECT_EQ(ds1.GetRasterYSize(), 200);
  EXPECT_EQ(ds1.GetBlockXSize(), 100);  // std::min(128, 100)
  EXPECT_EQ(ds1.GetBlockYSize(), 128);  // std::min(128, 200)

  VRTDataset ds2(500, 500, 64, 64);
  EXPECT_EQ(ds2.GetBlockXSize(), 64);
  EXPECT_EQ(ds2.GetBlockYSize(), 64);

  // VRTCreate (C API)
  VRTDatasetH hVRT = VRTCreate(50, 60);
  ASSERT_NE(hVRT, nullptr);
  EXPECT_EQ(GDALGetRasterXSize(hVRT), 50);
  EXPECT_EQ(GDALGetRasterYSize(hVRT), 60);
  GDALClose(hVRT);

  // GeoTransform
  VRTDataset ds(10, 10);
  GDALGeoTransform gtIn;
  gtIn[0] = 100.0;
  gtIn[1] = 1.0;
  gtIn[2] = 0.0;
  gtIn[3] = 200.0;
  gtIn[4] = 0.0;
  gtIn[5] = -1.0;
  EXPECT_EQ(ds.SetGeoTransform(gtIn), CE_None);
  GDALGeoTransform gtOut;
  EXPECT_EQ(ds.GetGeoTransform(gtOut), CE_None);
  EXPECT_DOUBLE_EQ(gtOut[0], 100.0);
  EXPECT_DOUBLE_EQ(gtOut[5], -1.0);

  // Spatial reference
  OGRSpatialReference srsIn;
  srsIn.importFromEPSG(4326);
  EXPECT_EQ(ds.SetSpatialRef(&srsIn), CE_None);
  const auto* poSRS = ds.GetSpatialRef();
  ASSERT_NE(poSRS, nullptr);
  EXPECT_TRUE(poSRS->IsSame(&srsIn));

  // GCPs
  EXPECT_EQ(ds.GetGCPCount(), 0);
  EXPECT_EQ(ds.GetGCPs(), nullptr);
  GDAL_GCP asGCPs[2];
  GDALInitGCPs(2, asGCPs);
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 10.0;
  asGCPs[0].dfGCPY = 20.0;
  asGCPs[1].dfGCPPixel = 10.0;
  asGCPs[1].dfGCPLine = 10.0;
  asGCPs[1].dfGCPX = 30.0;
  asGCPs[1].dfGCPY = 40.0;
  EXPECT_EQ(ds.SetGCPs(2, asGCPs, &srsIn), CE_None);
  EXPECT_EQ(ds.GetGCPCount(), 2);
  const GDAL_GCP* pasRetGCPs = ds.GetGCPs();
  ASSERT_NE(pasRetGCPs, nullptr);
  EXPECT_DOUBLE_EQ(pasRetGCPs[0].dfGCPX, 10.0);
  EXPECT_DOUBLE_EQ(pasRetGCPs[1].dfGCPY, 40.0);
  const auto* poGCPSRS = ds.GetGCPSpatialRef();
  ASSERT_NE(poGCPSRS, nullptr);
  EXPECT_TRUE(poGCPSRS->IsSame(&srsIn));
  GDALDeinitGCPs(2, asGCPs);

  // Metadata
  char* papszMD[] = {const_cast<char*>("KEY=VALUE"), nullptr};
  EXPECT_EQ(ds.SetMetadata(papszMD, "CUSTOM_DOMAIN"), CE_None);
  EXPECT_STREQ(ds.GetMetadataItem("KEY", "CUSTOM_DOMAIN"), "VALUE");
  EXPECT_EQ(ds.SetMetadataItem("ANOTHER_KEY", "ANOTHER_VALUE", "CUSTOM_DOMAIN"),
            CE_None);
  EXPECT_STREQ(ds.GetMetadataItem("ANOTHER_KEY", "CUSTOM_DOMAIN"),
               "ANOTHER_VALUE");

  // ClearStatistics
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = ds.GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);
  poBand->SetMetadataItem("STATISTICS_MINIMUM", "0");
  poBand->SetMetadataItem("STATISTICS_MAXIMUM", "255");
  poBand->SetMetadataItem("SOME_OTHER_TAG", "KEEP");
  ds.ClearStatistics();
  EXPECT_EQ(poBand->GetMetadataItem("STATISTICS_MINIMUM"), nullptr);
  EXPECT_STREQ(poBand->GetMetadataItem("SOME_OTHER_TAG"), "KEEP");
}

// 2. BlockSizesAndSerialization
TEST_F(VRTDatasetTest, BlockSizesAndSerialization) {
  VRTDataset ds(200, 200, 32, 32);
  ds.AddBand(GDT_Byte, nullptr);
  auto poBand = ds.GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  // SerializeToXML
  CPLXMLNode* psTree = ds.SerializeToXML(nullptr);
  ASSERT_NE(psTree, nullptr);
  CPLXMLNode* psBand = CPLGetXMLNode(psTree, "=VRTDataset.VRTRasterBand");
  ASSERT_NE(psBand, nullptr);
  EXPECT_STREQ(CPLGetXMLValue(psBand, "blockXSize", ""), "32");
  EXPECT_STREQ(CPLGetXMLValue(psBand, "blockYSize", ""), "32");
  CPLDestroyXMLNode(psTree);

  // VRTSerializeToXML (C API)
  CPLXMLNode* psCAPIXML =
      VRTSerializeToXML(GDALDataset::ToHandle(&ds), nullptr);
  ASSERT_NE(psCAPIXML, nullptr);
  CPLDestroyXMLNode(psCAPIXML);

  // GetMetadata("xml:VRT")
  char** papszVRT = ds.GetMetadata("xml:VRT");
  ASSERT_NE(papszVRT, nullptr);
  ASSERT_NE(papszVRT[0], nullptr);
  EXPECT_NE(strstr(papszVRT[0], "VRTDataset"), nullptr);

  // GetMetadataItem on __DEBUG__ domain
  EXPECT_STREQ(
      ds.GetMetadataItem("MULTI_THREADED_RASTERIO_LAST_USED", "__DEBUG__"),
      "0");
  EXPECT_STREQ(ds.GetMetadataItem("CheckCompatibleForDatasetIO()", "__DEBUG__"),
               "0");
}

// 3. InitBandAndSubclassesViaXML
TEST_F(VRTDatasetTest, InitBandAndSubclassesViaXML) {
  // Test VRTSourcedRasterBand via XML
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"/>\n"
        "</VRTDataset>\n";
    auto poDS = VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly);
    ASSERT_NE(poDS, nullptr);
    EXPECT_TRUE(static_cast<VRTRasterBand*>(poDS->GetRasterBand(1))
                    ->IsSourcedRasterBand());
  }
  // Test VRTDerivedRasterBand via XML
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTDerivedRasterBand\">\n"
        "    <PixelFunctionType>add</PixelFunctionType>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly);
    ASSERT_NE(poDS, nullptr);
  }
  // Test VRTRawRasterBand via XML. The raw file must exist since the dataset
  // is opened read-only, and an absolute source path requires relaxing the
  // default SIBLING_OR_CHILD_OF_VRT_PATH source policy.
  CreateMemFile("/vsimem/raw.bin", 100);
  {
    CPLConfigOptionSetter allow_any_source(
        "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTRawRasterBand\">\n"
        "    <SourceFilename>/vsimem/raw.bin</SourceFilename>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly);
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterCount(), 1);
  }

  // Runtime disable of VRTRawRasterBand
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO");
  EXPECT_FALSE(VRTDataset::IsRawRasterBandEnabled());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTRawRasterBand\">\n"
        "    <SourceFilename>/vsimem/raw.bin</SourceFilename>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly), nullptr);
  }
  CPLPopErrorHandler();
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES");
  EXPECT_TRUE(VRTDataset::IsRawRasterBandEnabled());

  // VRTWarpedRasterBand on non-warped dataset fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTWarpedRasterBand\"/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly), nullptr);
  }
  // Unrecognized subclass fails
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"UnknownSubClass\"/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(VRTDataset::OpenXML(xml.c_str(), nullptr, GA_ReadOnly), nullptr);
  }
  CPLPopErrorHandler();
}

// 4. AddBandAndVRTAddBand
TEST_F(VRTDatasetTest, AddBandAndVRTAddBand) {
  VRTDataset ds(20, 20);

  // AddBand GDT_Unknown fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.AddBand(GDT_Unknown, nullptr), CE_Failure);
  CPLPopErrorHandler();

  // AddBand with subClass=VRTSourcedRasterBand and custom block sizes
  const char* papszOptions1[] = {"subClass=VRTSourcedRasterBand",
                                 "BlockXSize=10", "BlockYSize=10", nullptr};
  EXPECT_EQ(ds.AddBand(GDT_Byte, const_cast<char**>(papszOptions1)), CE_None);
  ASSERT_EQ(ds.GetRasterCount(), 1);
  int nBX = 0;
  int nBY = 0;
  ds.GetRasterBand(1)->GetBlockSize(&nBX, &nBY);
  EXPECT_EQ(nBX, 10);
  EXPECT_EQ(nBY, 10);

  // AddBand with subClass=VRTDerivedRasterBand and pixel function
  const char* papszOptions2[] = {
      "subClass=VRTDerivedRasterBand", "PixelFunctionType=add",
      "SourceTransferType=Byte", "BufferRadius=1", nullptr};
  EXPECT_EQ(ds.AddBand(GDT_Float32, const_cast<char**>(papszOptions2)),
            CE_None);
  ASSERT_EQ(ds.GetRasterCount(), 2);

  // AddBand with subClass=VRTRawRasterBand. The dataset is read-only, so the
  // raw file must already exist (20x20 bytes with LineOffset=20). The absolute
  // source path requires relaxing the default source policy.
  CreateMemFile("/vsimem/raw.bin", 400);
  {
    CPLConfigOptionSetter allow_any_source(
        "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);
    const char* papszOptions3[] = {"subClass=VRTRawRasterBand",
                                   "SourceFilename=/vsimem/raw.bin",
                                   "ImageOffset=0",
                                   "PixelOffset=1",
                                   "LineOffset=20",
                                   "ByteOrder=LSB",
                                   nullptr};
    EXPECT_EQ(ds.AddBand(GDT_Byte, const_cast<char**>(papszOptions3)), CE_None);
  }
  ASSERT_EQ(ds.GetRasterCount(), 3);

  // VRTAddBand (C API)
  EXPECT_EQ(VRTAddBand(GDALDataset::ToHandle(&ds), GDT_Int16, nullptr),
            CE_None);
  EXPECT_EQ(ds.GetRasterCount(), 4);
}

// 5. XMLInitAndOpenXML
TEST_F(VRTDatasetTest, XMLInitAndOpenXML) {
  // XML with OverviewList, BlockSize, SRS, GeoTransform, GCPList, Metadata
  const std::string vrt_xml =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <BlockXSize>10</BlockXSize>\n"
      "  <BlockYSize>10</BlockYSize>\n"
      "  <SRS dataAxisToSRSAxisMapping=\"1,2\">EPSG:4326</SRS>\n"
      "  <GeoTransform>0, 1, 0, 0, 0, -1</GeoTransform>\n"
      "  <Metadata>\n"
      "    <MDI key=\"ATTR\">VALUE</MDI>\n"
      "  </Metadata>\n"
      "  <GCPList Projection=\"EPSG:4326\">\n"
      "    <GCP Id=\"1\" Pixel=\"0\" Line=\"0\" X=\"10\" Y=\"20\" Z=\"0\"/>\n"
      "  </GCPList>\n"
      "  <OverviewList resampling=\"nearest\">2 4</OverviewList>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <ColorInterp>Gray</ColorInterp>\n"
      "  </VRTRasterBand>\n"
      "  <MaskBand>\n"
      "    <VRTRasterBand dataType=\"Byte\">\n"
      "      <SimpleSource>\n"
      "        <SourceFilename>/vsimem/non_existent.tif</SourceFilename>\n"
      "        <SourceBand>1</SourceBand>\n"
      "      </SimpleSource>\n"
      "    </VRTRasterBand>\n"
      "  </MaskBand>\n"
      "</VRTDataset>\n";

  auto poDS = VRTDataset::OpenXML(vrt_xml.c_str(), "/vsimem", GA_ReadOnly);
  ASSERT_NE(poDS, nullptr);
  EXPECT_EQ(poDS->GetRasterXSize(), 20);
  EXPECT_EQ(poDS->GetRasterYSize(), 20);
  EXPECT_EQ(poDS->GetRasterCount(), 1);
  EXPECT_STREQ(poDS->GetMetadataItem("ATTR"), "VALUE");
  EXPECT_EQ(poDS->GetGCPCount(), 1);

  GDALGeoTransform gt;
  EXPECT_EQ(poDS->GetGeoTransform(gt), CE_None);
  EXPECT_DOUBLE_EQ(gt[1], 1.0);

  // Test OpenXML error branches
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(VRTDataset::OpenXML("Invalid XML <><", nullptr, GA_ReadOnly),
            nullptr);
  EXPECT_EQ(VRTDataset::OpenXML("<NotVRTDataset/>", nullptr, GA_ReadOnly),
            nullptr);
  // Missing rasterXSize
  EXPECT_EQ(VRTDataset::OpenXML(
                "<VRTDataset><VRTRasterBand dataType=\"Byte\"/></VRTDataset>",
                nullptr, GA_ReadOnly),
            nullptr);
  // Negative dimensions
  EXPECT_EQ(VRTDataset::OpenXML("<VRTDataset rasterXSize=\"-10\" "
                                "rasterYSize=\"-10\"><VRTRasterBand "
                                "dataType=\"Byte\" band=\"1\"/></VRTDataset>",
                                nullptr, GA_ReadOnly),
            nullptr);
  CPLPopErrorHandler();
}

// 6. DriverIdentifyAndOpen
TEST_F(VRTDatasetTest, DriverIdentifyAndOpen) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("VRT");
  ASSERT_NE(poDriver, nullptr);

  // Identify tests
  {
    GDALOpenInfo oi("<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\"/>",
                    GA_ReadOnly);
    EXPECT_TRUE(VRTDataset::Identify(&oi));
  }
  {
    GDALOpenInfo oi("vrt:///vsimem/test.tif", GA_ReadOnly);
    EXPECT_TRUE(VRTDataset::Identify(&oi));
  }
  {
    GDALOpenInfo oi("<OGRVRTDataSource><OGRVRTLayer/></OGRVRTDataSource>",
                    GA_ReadOnly);
    EXPECT_FALSE(VRTDataset::Identify(&oi));
  }
  {
    GDALOpenInfo oi("<VRTWarpedDataset rasterXSize=\"10\" rasterYSize=\"10\"/>",
                    GA_ReadOnly);
    EXPECT_FALSE(VRTDataset::Identify(&oi));
  }
  {
    GDALOpenInfo oi("/vsimem/not_a_vrt.tif", GA_ReadOnly);
    EXPECT_FALSE(VRTDataset::Identify(&oi));
  }

  // Open with XML string directly
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(xml.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterXSize(), 10);
    EXPECT_EQ(poDS->GetRasterYSize(), 10);
    EXPECT_EQ(poDS->GetRasterCount(), 1);
  }

  // Open with ROOT_PATH open option
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
        "</VRTDataset>\n";
    const std::string vrt_file = "/vsimem/test_root_path.vrt";
    TrackFile(vrt_file);
    VSIFCloseL(VSIFileFromMemBuffer(
        vrt_file.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(xml.data())), xml.size(),
        FALSE));

    const char* papszOO[] = {"ROOT_PATH=/vsimem", nullptr};
    auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        vrt_file.c_str(), GDAL_OF_RASTER, nullptr, papszOO, nullptr));
    ASSERT_NE(poDS, nullptr);
  }

  // Open multidim raster check
  {
    const std::string xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
        "</VRTDataset>\n";
    // Opening with ONLY MULTIDIM_RASTER returns nullptr because it has no
    // multidim group
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }
}

// 7. OpenVRTProtocol
TEST_F(VRTDatasetTest, OpenVRTProtocol) {
  const std::string src_tif = "/vsimem/protocol_src.tif";
  TrackFile(src_tif);
  double adfGT[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  char* pszWkt = nullptr;
  oSRS.exportToWkt(&pszWkt);

  std::vector<std::vector<double>> bands_data;
  bands_data.push_back(std::vector<double>(100, 10.0));
  bands_data.push_back(std::vector<double>(100, 20.0));
  CreateGeoTiff(src_tif, 10, 10, 2, GDT_Float32, adfGT, pszWkt, 99.0,
                bands_data);
  CPLFree(pszWkt);

  // Palette dataset for expand option
  const std::string palette_tif = "/vsimem/protocol_palette.tif";
  TrackFile(palette_tif);
  {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);
    auto poPalDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(palette_tif.c_str(), 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poPalDS, nullptr);
    GDALColorTable oCT;
    GDALColorEntry oEntry = {255, 0, 0, 255};
    oCT.SetColorEntry(0, &oEntry);
    poPalDS->GetRasterBand(1)->SetColorTable(&oCT);
    poPalDS->FlushCache(false);
  }

  // 1. bands=1,2
  {
    std::string uri = "vrt://" + src_tif + "?bands=1";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterCount(), 1);
  }

  // 2. bands=1,mask
  {
    std::string uri = "vrt://" + src_tif + "?bands=1,mask";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterCount(), 2);
  }

  // 3. a_nodata, a_srs, a_ullr
  {
    std::string uri =
        "vrt://" + src_tif + "?a_nodata=-999&a_srs=EPSG:3857&a_ullr=0,10,10,0";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    int bHasNoData = 0;
    double dfNoData = poDS->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(dfNoData, -999.0);
  }

  // 4. a_scale, a_offset, ot=Int32
  {
    std::string uri = "vrt://" + src_tif + "?a_scale=2.5&a_offset=5.0&ot=Int32";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterBand(1)->GetRasterDataType(), GDT_Int32);
    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(poDS->GetRasterBand(1)->GetScale(&bSuccess), 2.5);
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(poDS->GetRasterBand(1)->GetOffset(&bSuccess), 5.0);
    EXPECT_TRUE(bSuccess);
  }

  // 5. expand=rgb and expand=rgba
  {
    std::string uri = "vrt://" + palette_tif + "?expand=rgb";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterCount(), 3);
  }
  {
    std::string uri = "vrt://" + palette_tif + "?expand=rgba";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterCount(), 4);
  }

  // 6. gcp option
  {
    std::string uri = "vrt://" + src_tif + "?gcp=0,0,10,20,0&gcp=10,10,30,40,0";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetGCPCount(), 2);
  }

  // 7. scale, scale_1, exponent, exponent_1
  {
    std::string uri = "vrt://" + src_tif + "?scale_1=0,100,0,1&exponent_1=0.5";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
  }
  {
    std::string uri = "vrt://" + src_tif + "?scale=0,100,0,1&exponent=0.5";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
  }

  // 8. outsize (percentage and explicit)
  {
    std::string uri = "vrt://" + src_tif + "?outsize=50%,50%";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterXSize(), 5);
    EXPECT_EQ(poDS->GetRasterYSize(), 5);
  }
  {
    std::string uri = "vrt://" + src_tif + "?outsize=4,4";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterXSize(), 4);
    EXPECT_EQ(poDS->GetRasterYSize(), 4);
  }

  // 9. projwin, projwin_srs, tr, r=bilinear
  {
    std::string uri = "vrt://" + src_tif +
                      "?projwin=0,10,10,0&projwin_srs=EPSG:4326&tr=1,1&r="
                      "bilinear";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
  }

  // 10. srcwin, a_gt, unscale, a_coord_epoch, nogcp, epo, eco, if, oo
  {
    std::string uri =
        "vrt://" + src_tif +
        "?srcwin=0,0,5,5&a_gt=0,1,0,0,0,-1&unscale=true&a_coord_epoch=2021.0&"
        "nogcp=true&epo=true&eco=true&if=GTiff&oo=NUM_THREADS=1";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uri.c_str(), GA_ReadOnly));
    ASSERT_NE(poDS, nullptr);
    EXPECT_EQ(poDS->GetRasterXSize(), 5);
    EXPECT_EQ(poDS->GetRasterYSize(), 5);
  }

  // 11. Error handling in OpenVRTProtocol
  CPLPushErrorHandler(CPLQuietErrorHandler);
  // Missing key (no =)
  EXPECT_EQ(std::unique_ptr<GDALDataset>(
                GDALDataset::Open(("vrt://" + src_tif + "?bad_param").c_str())),
            nullptr);
  // Empty if=
  EXPECT_EQ(std::unique_ptr<GDALDataset>(
                GDALDataset::Open(("vrt://" + src_tif + "?if=").c_str())),
            nullptr);
  // Invalid a_gt
  EXPECT_EQ(std::unique_ptr<GDALDataset>(
                GDALDataset::Open(("vrt://" + src_tif + "?a_gt=1,2").c_str())),
            nullptr);
  // Invalid a_ullr
  EXPECT_EQ(std::unique_ptr<GDALDataset>(GDALDataset::Open(
                ("vrt://" + src_tif + "?a_ullr=1,2").c_str())),
            nullptr);
  // Invalid outsize
  EXPECT_EQ(std::unique_ptr<GDALDataset>(GDALDataset::Open(
                ("vrt://" + src_tif + "?outsize=invalid").c_str())),
            nullptr);
  // Invalid srcwin
  EXPECT_EQ(std::unique_ptr<GDALDataset>(GDALDataset::Open(
                ("vrt://" + src_tif + "?srcwin=1,2").c_str())),
            nullptr);
  // Invalid tr
  EXPECT_EQ(std::unique_ptr<GDALDataset>(
                GDALDataset::Open(("vrt://" + src_tif + "?tr=1").c_str())),
            nullptr);
  // Unknown option
  EXPECT_EQ(std::unique_ptr<GDALDataset>(GDALDataset::Open(
                ("vrt://" + src_tif + "?unknown_xyz=1").c_str())),
            nullptr);
  CPLPopErrorHandler();
}

// 8. MaskBand
TEST_F(VRTDatasetTest, MaskBand) {
  VRTDataset ds(10, 10);
  EXPECT_EQ(ds.CreateMaskBand(0), CE_None);
  EXPECT_EQ(ds.AddBand(GDT_Byte, nullptr), CE_None);

  // Calling CreateMaskBand a second time fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.CreateMaskBand(0), CE_Failure);
  CPLPopErrorHandler();

  // Test serialization of mask band
  CPLXMLNode* psXML = ds.SerializeToXML(nullptr);
  ASSERT_NE(psXML, nullptr);
  EXPECT_NE(CPLGetXMLNode(psXML, "=VRTDataset.MaskBand"), nullptr);
  CPLDestroyXMLNode(psXML);

  // SetMaskBand
  VRTDataset ds2(10, 10);
  ds2.AddBand(GDT_Byte, nullptr);
  ds2.SetMaskBand(std::make_unique<VRTSourcedRasterBand>(&ds2, 0));
  EXPECT_NE(ds2.GetRasterBand(1)->GetMaskBand(), nullptr);
}

// 9. DatasetCreationAndManagement
TEST_F(VRTDatasetTest, DatasetCreationAndManagement) {
  // Create with SUBCLASS=VRTWarpedDataset
  const char* papszWarpedOptions[] = {"SUBCLASS=VRTWarpedDataset", nullptr};
  auto poWarped = std::unique_ptr<GDALDataset>(
      VRTDataset::Create("/vsimem/warped.vrt", 10, 10, 1, GDT_Byte,
                         const_cast<char**>(papszWarpedOptions)));
  ASSERT_NE(poWarped, nullptr);
  EXPECT_STREQ(poWarped->GetDriverName(), "VRT");

  // Create with invalid SUBCLASS fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const char* papszBadOptions[] = {"SUBCLASS=InvalidSubClass", nullptr};
  EXPECT_EQ(std::unique_ptr<GDALDataset>(
                VRTDataset::Create("/vsimem/bad.vrt", 10, 10, 1, GDT_Byte,
                                   const_cast<char**>(papszBadOptions))),
            nullptr);
  CPLPopErrorHandler();

  // Create from XML string
  const std::string xml =
      "<VRTDataset rasterXSize=\"15\" rasterYSize=\"25\">\n"
      "  <VRTRasterBand dataType=\"Int32\" band=\"1\"/>\n"
      "</VRTDataset>\n";
  auto poFromXML = std::unique_ptr<GDALDataset>(
      VRTDataset::Create(xml.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(poFromXML, nullptr);
  EXPECT_EQ(poFromXML->GetRasterXSize(), 15);
  EXPECT_EQ(poFromXML->GetRasterYSize(), 25);

  // CreateVRTMultiDimensional and CreateMultiDimensional
  const std::string md_file = "/vsimem/create_md.vrt";
  TrackFile(md_file);
  auto poMD1 = std::unique_ptr<VRTDataset>(
      VRTDataset::CreateVRTMultiDimensional(md_file.c_str(), nullptr, nullptr));
  ASSERT_NE(poMD1, nullptr);

  auto poMD2 = std::unique_ptr<GDALDataset>(
      VRTDataset::CreateMultiDimensional(md_file.c_str(), nullptr, nullptr));
  ASSERT_NE(poMD2, nullptr);

  // GetFileList and Delete
  const std::string del_file = "/vsimem/to_delete.vrt";
  TrackFile(del_file);
  auto poDS = std::unique_ptr<VRTDataset>(VRTDataset::CreateVRTDataset(
      del_file.c_str(), 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS, nullptr);
  poDS->FlushCache(true);
  char** papszFiles = poDS->GetFileList();
  ASSERT_NE(papszFiles, nullptr);
  EXPECT_GE(CSLCount(papszFiles), 1);
  CSLDestroy(papszFiles);
  poDS.reset();

  EXPECT_EQ(VRTDataset::Delete(del_file.c_str()), CE_None);
  VSIStatBufL sStat;
  EXPECT_EQ(VSIStatL(del_file.c_str(), &sStat), -1);

  // CloseDependentDatasets
  TestVRTDataset dsDep(10, 10);
  dsDep.AddBand(GDT_Byte, nullptr);
  EXPECT_EQ(dsDep.CloseDependentDatasets(), 0);
}

// 10. DatasetRasterIOAndThreading
TEST_F(VRTDatasetTest, DatasetRasterIOAndThreading) {
  const std::string src_file = "/vsimem/ds_io_src.tif";
  TrackFile(src_file);
  std::vector<std::vector<double>> bands_data;
  bands_data.push_back(std::vector<double>(100, 42.0));
  bands_data.push_back(std::vector<double>(100, 84.0));
  CreateGeoTiff(src_file, 10, 10, 2, GDT_Byte, nullptr, nullptr, 0.0,
                bands_data);

  // Create 2-band VRT sharing identical simple sources
  const std::string vrt_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/ds_io_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"2\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/ds_io_src.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GA_ReadOnly));
  ASSERT_NE(poDS, nullptr);

  // AdviseRead
  int anBands[] = {1, 2};
  EXPECT_EQ(
      poDS->AdviseRead(0, 0, 10, 10, 10, 10, GDT_Byte, 2, anBands, nullptr),
      CE_None);

  // Dataset-level RasterIO single thread
  std::vector<GByte> buf(200);
  EXPECT_EQ(poDS->RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10, GDT_Byte,
                           2, anBands, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(buf[0], 42);
  EXPECT_EQ(buf[100], 84);

  // Dataset-level RasterIO with GDAL_NUM_THREADS
  CPLSetConfigOption("GDAL_NUM_THREADS", "2");
  EXPECT_EQ(poDS->RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10, GDT_Byte,
                           2, anBands, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(buf[0], 42);
  EXPECT_EQ(buf[100], 84);
  CPLSetConfigOption("GDAL_NUM_THREADS", nullptr);

  // Write with dataset-level RasterIO on VRTRawRasterBand. The VRT must be in
  // update mode, which CreateVRTDataset() provides. The absolute source path
  // requires relaxing the default source policy.
  const std::string raw_bin = "/vsimem/raw_write.bin";
  CreateMemFile(raw_bin, 200);
  CPLConfigOptionSetter allow_any_source(
      "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);

  auto dsWrite = VRTDataset::CreateVRTDataset("", 10, 10, 0, GDT_Byte, nullptr);
  ASSERT_NE(dsWrite, nullptr);
  EXPECT_EQ(dsWrite->GetAccess(), GA_Update);
  const char* papszRaw1[] = {"subClass=VRTRawRasterBand",
                             "SourceFilename=/vsimem/raw_write.bin",
                             "ImageOffset=0",
                             "PixelOffset=1",
                             "LineOffset=10",
                             nullptr};
  const char* papszRaw2[] = {"subClass=VRTRawRasterBand",
                             "SourceFilename=/vsimem/raw_write.bin",
                             "ImageOffset=100",
                             "PixelOffset=1",
                             "LineOffset=10",
                             nullptr};
  ASSERT_EQ(dsWrite->AddBand(GDT_Byte, const_cast<char**>(papszRaw1)), CE_None);
  ASSERT_EQ(dsWrite->AddBand(GDT_Byte, const_cast<char**>(papszRaw2)), CE_None);
  std::vector<GByte> write_buf(200, 77);
  EXPECT_EQ(dsWrite->RasterIO(GF_Write, 0, 0, 10, 10, write_buf.data(), 10, 10,
                              GDT_Byte, 2, anBands, 0, 0, 0, nullptr),
            CE_None);
  dsWrite->FlushCache(false);

  // Read back and verify both bands were written.
  std::vector<GByte> read_buf(200, 0);
  EXPECT_EQ(dsWrite->RasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10, 10,
                              GDT_Byte, 2, anBands, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(read_buf[0], 77);
  EXPECT_EQ(read_buf[199], 77);
}

// 11. VirtualOverviewsAndBuildOverviews
TEST_F(VRTDatasetTest, VirtualOverviewsAndBuildOverviews) {
  const std::string src_tif = "/vsimem/ovr_src.tif";
  TrackFile(src_tif);
  std::vector<std::vector<double>> bands_data;
  bands_data.push_back(std::vector<double>(400, 50.0));
  CreateGeoTiff(src_tif, 20, 20, 1, GDT_Byte, nullptr, nullptr, 0.0,
                bands_data);

  // Build real overviews on the source GeoTIFF
  {
    auto poSrcDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(src_tif.c_str(), GA_Update));
    ASSERT_NE(poSrcDS, nullptr);
    int anOvrList[] = {2, 4};
    EXPECT_EQ(poSrcDS->BuildOverviews("NEAREST", 2, anOvrList, 0, nullptr,
                                      nullptr, nullptr),
              CE_None);
  }

  // Open VRT referencing the source dataset -> BuildVirtualOverviews()
  const std::string vrt_xml =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/ovr_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = VRTDataset::OpenXML(vrt_xml.c_str(), nullptr, GA_Update);
  ASSERT_NE(poDS, nullptr);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);
  EXPECT_GE(poBand->GetOverviewCount(), 2);

  // BuildVirtualOverviews explicit with VIRTUAL=YES
  int anOvrList[] = {2, 4};
  const char* papszOvrOptions[] = {"VIRTUAL=YES", nullptr};
  EXPECT_EQ(poDS->BuildOverviews("NEAREST", 2, anOvrList, 0, nullptr, nullptr,
                                 nullptr, const_cast<char**>(papszOvrOptions)),
            CE_None);

  // Clear virtual overviews with nOverviews = 0
  EXPECT_EQ(poDS->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr, nullptr,
                                 nullptr, const_cast<char**>(papszOvrOptions)),
            CE_None);
}

// 12. CompressedData
TEST_F(VRTDatasetTest, CompressedData) {
  const std::string src_tif = "/vsimem/comp_src.tif";
  TrackFile(src_tif);
  std::vector<std::vector<double>> bands_data;
  bands_data.push_back(std::vector<double>(100, 1.0));
  CreateGeoTiff(src_tif, 10, 10, 1, GDT_Byte, nullptr, nullptr, 0.0,
                bands_data);

  const std::string vrt_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/comp_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = VRTDataset::OpenXML(vrt_xml.c_str(), nullptr, GA_ReadOnly);
  ASSERT_NE(poDS, nullptr);

  int anBands[] = {1};
  CPLStringList aosFormats =
      poDS->GetCompressionFormats(0, 0, 10, 10, 1, anBands);
  void* pBuffer = nullptr;
  size_t nBufferSize = 0;
  char* pszDetailedFormat = nullptr;
  // ReadCompressedData on non-compressed format returns CE_Failure or CE_None
  poDS->ReadCompressedData("UNKNOWN", 0, 0, 10, 10, 1, anBands, &pBuffer,
                           &nBufferSize, &pszDetailedFormat);
  if (pBuffer) {
    VSIFree(pBuffer);
  }
  if (pszDetailedFormat) {
    CPLFree(pszDetailedFormat);
  }
}

// 13. VRTMapSharedResources
TEST_F(VRTDatasetTest, VRTMapSharedResources) {
  VRTMapSharedResources mapRes;
  EXPECT_EQ(mapRes.Get("not_found"), nullptr);

  VRTDataset ds(5, 5);
  mapRes.Insert("ds1", &ds);
  EXPECT_EQ(mapRes.Get("ds1"), &ds);

  // Test with mutex initialized
  mapRes.InitMutex();
  mapRes.Insert("ds2", &ds);
  EXPECT_EQ(mapRes.Get("ds2"), &ds);
}

// 14. SerializeComprehensive
TEST_F(VRTDatasetTest, SerializeComprehensive) {
  VRTDataset ds(20, 20);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetCoordinateEpoch(2021.5);
  ds.SetSpatialRef(&srs);

  double gtCoeffs[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  ds.SetGeoTransform(GDALGeoTransform(gtCoeffs));

  GDAL_GCP gcps[2];
  GDALInitGCPs(2, gcps);
  gcps[0].dfGCPPixel = 0;
  gcps[0].dfGCPLine = 0;
  gcps[0].dfGCPX = 10;
  gcps[0].dfGCPY = 20;
  gcps[1].dfGCPPixel = 10;
  gcps[1].dfGCPLine = 10;
  gcps[1].dfGCPX = 20;
  gcps[1].dfGCPY = 10;
  ds.SetGCPs(2, gcps, &srs);
  GDALDeinitGCPs(2, gcps);

  EXPECT_EQ(ds.CreateMaskBand(0), CE_None);
  EXPECT_EQ(ds.AddBand(GDT_Byte, nullptr), CE_None);

  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "VIRTUAL", "YES");
  int anOvr[] = {2, 4};
  EXPECT_EQ(ds.BuildOverviews("NEAREST", 2, anOvr, 0, nullptr, nullptr, nullptr,
                              papszOptions),
            CE_None);
  CSLDestroy(papszOptions);

  CPLXMLNode* psTree = ds.SerializeToXML(nullptr);
  ASSERT_NE(psTree, nullptr);
  char* pszXML = CPLSerializeXMLTree(psTree);
  CPLDestroyXMLNode(psTree);
  ASSERT_NE(pszXML, nullptr);
  std::string osXML = pszXML;
  CPLFree(pszXML);

  EXPECT_NE(osXML.find("coordinateEpoch=\"2021.5\""), std::string::npos);
  EXPECT_NE(osXML.find("<GeoTransform>"), std::string::npos);
  EXPECT_NE(osXML.find("<GCPList"), std::string::npos);
  EXPECT_NE(osXML.find("<MaskBand>"), std::string::npos);
  EXPECT_NE(
      osXML.find("<OverviewList resampling=\"NEAREST\">2 4</OverviewList>"),
      std::string::npos);

  // VRTFlushCache
  VRTFlushCache(GDALDataset::ToHandle(&ds));

  // VRTSerializeToXML
  CPLXMLNode* psTree2 = VRTSerializeToXML(GDALDataset::ToHandle(&ds), nullptr);
  ASSERT_NE(psTree2, nullptr);
  CPLDestroyXMLNode(psTree2);
}

// 15. OverviewListInXMLAndRootGroup
TEST_F(VRTDatasetTest, OverviewListInXMLAndRootGroup) {
  // Test valid OverviewList
  const std::string vrt_ovr =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <OverviewList resampling=\"bilinear\">2 4</OverviewList>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
      "</VRTDataset>\n";
  TrackFile("/vsimem/vrt_ovr.vrt");
  VSILFILE* fp = VSIFileFromMemBuffer(
      "/vsimem/vrt_ovr.vrt",
      reinterpret_cast<GByte*>(const_cast<char*>(vrt_ovr.data())),
      vrt_ovr.size(), FALSE);
  VSIFCloseL(fp);

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/vrt_ovr.vrt", GDAL_OF_RASTER));
  ASSERT_NE(poDS, nullptr);
  ASSERT_NE(poDS->GetRasterBand(1), nullptr);
  EXPECT_EQ(poDS->GetRasterBand(1)->GetOverviewCount(), 2);

  // Test invalid overview factor <= 1
  const std::string vrt_bad_ovr =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <OverviewList>1</OverviewList>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
      "</VRTDataset>\n";
  TrackFile("/vsimem/vrt_bad_ovr.vrt");
  fp = VSIFileFromMemBuffer(
      "/vsimem/vrt_bad_ovr.vrt",
      reinterpret_cast<GByte*>(const_cast<char*>(vrt_bad_ovr.data())),
      vrt_bad_ovr.size(), FALSE);
  VSIFCloseL(fp);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto poBadDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/vrt_bad_ovr.vrt", GDAL_OF_RASTER));
  CPLPopErrorHandler();
  EXPECT_EQ(poBadDS, nullptr);

  // Test Root Group XMLInit
  const std::string vrt_group =
      "<VRTDataset rasterXSize=\"5\" rasterYSize=\"5\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dim0\" size=\"5\"/>\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  TrackFile("/vsimem/vrt_group.vrt");
  fp = VSIFileFromMemBuffer(
      "/vsimem/vrt_group.vrt",
      reinterpret_cast<GByte*>(const_cast<char*>(vrt_group.data())),
      vrt_group.size(), FALSE);
  VSIFCloseL(fp);

  auto poGroupDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/vrt_group.vrt", GDAL_OF_UPDATE));
  ASSERT_NE(poGroupDS, nullptr);
  auto poRootGroup = poGroupDS->GetRootGroup();
  ASSERT_NE(poRootGroup, nullptr);
  EXPECT_STREQ(poRootGroup->GetName().c_str(), "/");

  // Test Root Group invalid name
  const std::string vrt_bad_group =
      "<VRTDataset>\n"
      "  <Group name=\"bad\">\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto poBadGroupDS =
      VRTDataset::OpenXML(vrt_bad_group.c_str(), nullptr, GA_ReadOnly);
  CPLPopErrorHandler();
  EXPECT_EQ(poBadGroupDS, nullptr);
}

// 16. AddBandAdvanced
TEST_F(VRTDatasetTest, AddBandAdvanced) {
  VRTDataset ds(10, 10);

  // 1. VRTDerivedRasterBand with pixel function options
  char** papszOptions = nullptr;
  papszOptions =
      CSLSetNameValue(papszOptions, "subclass", "VRTDerivedRasterBand");
  papszOptions = CSLSetNameValue(papszOptions, "PixelFunctionType", "add");
  papszOptions = CSLSetNameValue(papszOptions, "PixelFunctionLanguage", "C");
  papszOptions =
      CSLSetNameValue(papszOptions, "SkipNonContributingSources", "YES");
  papszOptions = CSLSetNameValue(papszOptions, "_PIXELFN_ARG_k1", "v1");
  papszOptions = CSLSetNameValue(papszOptions, "SourceTransferType", "Float32");
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_None);
  CSLDestroy(papszOptions);
  papszOptions = nullptr;

  // 2. Invalid SourceTransferType
  papszOptions =
      CSLSetNameValue(papszOptions, "subclass", "VRTDerivedRasterBand");
  papszOptions =
      CSLSetNameValue(papszOptions, "SourceTransferType", "InvalidType");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_Failure);
  CPLPopErrorHandler();
  CSLDestroy(papszOptions);
  papszOptions = nullptr;

  // 3. AddFuncSource option
  papszOptions = CSLSetNameValue(papszOptions, "AddFuncSource", "0x0,0x0,0.0");
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_None);
  CSLDestroy(papszOptions);
  papszOptions = nullptr;

  // 4. VRTRawRasterBand: missing SourceFilename
  papszOptions = CSLSetNameValue(papszOptions, "subclass", "VRTRawRasterBand");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_Failure);
  CPLPopErrorHandler();
  CSLDestroy(papszOptions);
  papszOptions = nullptr;

  // 5. VRTRawRasterBand: integer overflow
  papszOptions = CSLSetNameValue(papszOptions, "subclass", "VRTRawRasterBand");
  papszOptions =
      CSLSetNameValue(papszOptions, "SourceFilename", "/vsimem/raw.bin");
  papszOptions = CSLSetNameValue(papszOptions, "PixelOffset", "2000000000");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_Failure);
  CPLPopErrorHandler();
  CSLDestroy(papszOptions);
  papszOptions = nullptr;

  // 6. VRTRawRasterBand: valid with omitted LineOffset and relativeToVRT
  TrackFile("/vsimem/raw.bin");
  std::vector<GByte> rawData(100, 42);
  VSILFILE* fp = VSIFileFromMemBuffer("/vsimem/raw.bin", rawData.data(),
                                      rawData.size(), FALSE);
  VSIFCloseL(fp);

  ds.SetDescription("/vsimem/test.vrt");
  papszOptions = CSLSetNameValue(papszOptions, "subclass", "VRTRawRasterBand");
  papszOptions = CSLSetNameValue(papszOptions, "SourceFilename", "raw.bin");
  papszOptions = CSLSetNameValue(papszOptions, "relativeToVRT", "1");
  papszOptions = CSLSetNameValue(papszOptions, "ByteOrder", "MSB");
  EXPECT_EQ(ds.AddBand(GDT_Byte, papszOptions), CE_None);
  CSLDestroy(papszOptions);
}

// 17. GetSingleSimpleSourceAndAdviseRead
TEST_F(VRTDatasetTest, GetSingleSimpleSourceAndAdviseRead) {
  TrackFile("/vsimem/src_single.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/src_single.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  const std::string vrt_full =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/src_single.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poVRT = VRTDataset::OpenXML(vrt_full.c_str(), nullptr, GA_ReadOnly);
  ASSERT_NE(poVRT, nullptr);
  EXPECT_NE(poVRT->GetSingleSimpleSource(), nullptr);

  int anBands[] = {1};
  EXPECT_EQ(
      poVRT->AdviseRead(0, 0, 20, 20, 20, 20, GDT_Byte, 1, anBands, nullptr),
      CE_None);

  // Partial window: GetSingleSimpleSource returns nullptr
  const std::string vrt_partial =
      "<VRTDataset rasterXSize=\"20\" rasterYSize=\"20\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/src_single.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";
  auto poPartialVRT =
      VRTDataset::OpenXML(vrt_partial.c_str(), nullptr, GA_ReadOnly);
  ASSERT_NE(poPartialVRT, nullptr);
  EXPECT_EQ(poPartialVRT->GetSingleSimpleSource(), nullptr);
}

// 18. MultiThreadedRasterIO
TEST_F(VRTDatasetTest, MultiThreadedRasterIO) {
  TrackFile("/vsimem/tile1.tif");
  TrackFile("/vsimem/tile2.tif");

  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poTile1 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/tile1.tif", 500, 1000, 2, GDT_Byte, nullptr));
  ASSERT_NE(poTile1, nullptr);
  std::vector<GByte> line(500, 10);
  for (int b = 1; b <= 2; ++b) {
    for (int y = 0; y < 1000; ++y) {
      EXPECT_EQ(
          poTile1->GetRasterBand(b)->RasterIO(
              GF_Write, 0, y, 500, 1, line.data(), 500, 1, GDT_Byte, 0, 0),
          CE_None);
    }
  }
  poTile1.reset();

  auto poTile2 = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/tile2.tif", 500, 1000, 2, GDT_Byte, nullptr));
  ASSERT_NE(poTile2, nullptr);
  line.assign(500, 20);
  for (int b = 1; b <= 2; ++b) {
    for (int y = 0; y < 1000; ++y) {
      EXPECT_EQ(
          poTile2->GetRasterBand(b)->RasterIO(
              GF_Write, 0, y, 500, 1, line.data(), 500, 1, GDT_Byte, 0, 0),
          CE_None);
    }
  }
  poTile2.reset();

  const std::string vrt_mosaic =
      "<VRTDataset rasterXSize=\"1000\" rasterYSize=\"1000\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/tile1.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "    </SimpleSource>\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/tile2.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "      <DstRect xOff=\"500\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"2\">\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/tile1.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "    </SimpleSource>\n"
      "    <SimpleSource>\n"
      "      <SourceFilename>/vsimem/tile2.tif</SourceFilename>\n"
      "      <SourceBand>2</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "      <DstRect xOff=\"500\" yOff=\"0\" xSize=\"500\" ySize=\"1000\"/>\n"
      "    </SimpleSource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = VRTDataset::OpenXML(vrt_mosaic.c_str(), nullptr, GA_ReadOnly);
  ASSERT_NE(poDS, nullptr);

  CPLSetConfigOption("VRT_NUM_THREADS", "2");
  std::vector<GByte> outBuf(1000 * 1000 * 2, 0);
  int anBands[] = {1, 2};
  GDALRasterIOExtraArg sMTArg;
  INIT_RASTERIO_EXTRA_ARG(sMTArg);
  sMTArg.bFloatingPointWindowValidity = TRUE;
  sMTArg.dfXOff = 0.0;
  sMTArg.dfYOff = 0.0;
  sMTArg.dfXSize = 1000.0;
  sMTArg.dfYSize = 1000.0;
  sMTArg.pfnProgress = GDALDummyProgress;
  EXPECT_EQ(poDS->RasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(), 1000, 1000,
                           GDT_Byte, 2, anBands, 2, 1000 * 2, 1, &sMTArg),
            CE_None);
  EXPECT_STREQ(
      poDS->GetMetadataItem("MULTI_THREADED_RASTERIO_LAST_USED", "__DEBUG__"),
      "1");

  // Single-threaded path with progress callback
  CPLSetConfigOption("VRT_NUM_THREADS", "1");
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.pfnProgress = GDALDummyProgress;
  EXPECT_EQ(poDS->RasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(), 1000, 1000,
                           GDT_Byte, 2, anBands, 2, 1000 * 2, 1, &sExtraArg),
            CE_None);
  CPLSetConfigOption("VRT_NUM_THREADS", nullptr);

  // Multi-band downsampled read with a non-nearest resampling algorithm is
  // routed through GDALDataset::BandBasedRasterIO() so that each
  // VRTSourcedRasterBand can take advantage of source overviews.
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.eResampleAlg = GRIORA_Bilinear;
  std::vector<GByte> smallBuf(100 * 100 * 2, 0);
  EXPECT_EQ(poDS->RasterIO(GF_Read, 0, 0, 1000, 1000, smallBuf.data(), 100, 100,
                           GDT_Byte, 2, anBands, 2, 100 * 2, 1, &sExtraArg),
            CE_None);

  // A single-band dataset reading its only band is forwarded directly to
  // VRTSourcedRasterBand::IRasterIO().
  VRTDataset ds1Band(10, 10);
  ds1Band.AddBand(GDT_Byte, nullptr);
  std::vector<GByte> buf1Band(100, 0);
  int anBand1[] = {1};
  EXPECT_EQ(ds1Band.RasterIO(GF_Read, 0, 0, 10, 10, buf1Band.data(), 10, 10,
                             GDT_Byte, 1, anBand1, 0, 0, 0, nullptr),
            CE_None);

  // Bands whose sources are not compatible with dataset-level I/O (here:
  // bands without any source) fall back to GDALDataset::IRasterIO().
  VRTDataset dsIncomp(10, 10);
  dsIncomp.AddBand(GDT_Byte, nullptr);
  dsIncomp.AddBand(GDT_Byte, nullptr);
  std::vector<GByte> bufIncomp(200, 0);
  int anBandsIncomp[] = {1, 2};
  GDALRasterIOExtraArg sIncompArg;
  INIT_RASTERIO_EXTRA_ARG(sIncompArg);
  sIncompArg.eResampleAlg = GRIORA_NearestNeighbour;
  EXPECT_EQ(dsIncomp.RasterIO(GF_Read, 0, 0, 10, 10, bufIncomp.data(), 10, 10,
                              GDT_Byte, 2, anBandsIncomp, 0, 0, 0, &sIncompArg),
            CE_None);
}

// 19. BuildVirtualOverviewsAndCheckBand
TEST_F(VRTDatasetTest, BuildVirtualOverviewsAndCheckBand) {
  TrackFile("/vsimem/ovr_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ovr_src.tif", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  int anOvr[] = {2, 4};
  EXPECT_EQ(poSrcDS->BuildOverviews("NEAREST", 2, anOvr, 0, nullptr, nullptr,
                                    nullptr),
            CE_None);
  poSrcDS.reset();

  const std::string vrt_text =
      "<VRTDataset rasterXSize=\"100\" rasterYSize=\"100\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <ComplexSource>\n"
      "      <SourceFilename>/vsimem/ovr_src.tif</SourceFilename>\n"
      "      <SourceBand>1</SourceBand>\n"
      "      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"100\" ySize=\"100\"/>\n"
      "      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"100\" ySize=\"100\"/>\n"
      "      <ScaleOffset>1.0</ScaleOffset>\n"
      "    </ComplexSource>\n"
      "  </VRTRasterBand>\n"
      "  <MaskBand>\n"
      "    <VRTRasterBand dataType=\"Byte\">\n"
      "      <SimpleSource>\n"
      "        <SourceFilename>/vsimem/ovr_src.tif</SourceFilename>\n"
      "        <SourceBand>1</SourceBand>\n"
      "        <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"100\" ySize=\"100\"/>\n"
      "        <DstRect xOff=\"0\" yOff=\"0\" xSize=\"100\" ySize=\"100\"/>\n"
      "      </SimpleSource>\n"
      "    </VRTRasterBand>\n"
      "  </MaskBand>\n"
      "</VRTDataset>\n";

  TrackFile("/vsimem/test_ovr.vrt");
  VSILFILE* fp = VSIFileFromMemBuffer(
      "/vsimem/test_ovr.vrt",
      reinterpret_cast<GByte*>(const_cast<char*>(vrt_text.data())),
      vrt_text.size(), FALSE);
  VSIFCloseL(fp);

  auto poVRT = std::unique_ptr<VRTDataset>(cpl::down_cast<VRTDataset*>(
      GDALDataset::Open("/vsimem/test_ovr.vrt", GA_ReadOnly)));
  ASSERT_NE(poVRT, nullptr);
  auto poBand = poVRT->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  // Calling GetOverviewCount triggers BuildVirtualOverviews
  EXPECT_EQ(poBand->GetOverviewCount(), 2);
  EXPECT_NE(poBand->GetOverview(0), nullptr);
  EXPECT_NE(poBand->GetOverview(1), nullptr);

  // Non-virtual BuildOverviews while implicit virtual overviews exist: the
  // dataset lazily initializes its overview manager from the description,
  // hides the implicit overviews while GDALDataset::IBuildOverviews() runs,
  // and then clears them.
  poVRT->SetDescription("/vsimem/test_ovr.vrt");
  EXPECT_EQ(poVRT->BuildOverviews("NEAREST", 2, anOvr, 0, nullptr, nullptr,
                                  nullptr, nullptr),
            CE_None);

  // Clear virtual overviews using BuildOverviews with VIRTUAL=YES and
  // nOverviews=0
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "VIRTUAL", "YES");
  EXPECT_EQ(poVRT->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr, nullptr,
                                  nullptr, papszOptions),
            CE_None);
  CSLDestroy(papszOptions);

  // UnsetPreservedRelativeFilenames
  poVRT->UnsetPreservedRelativeFilenames();

  // ClearStatistics
  poBand->SetMetadataItem("STATISTICS_MINIMUM", "10");
  poBand->SetMetadataItem("OTHER_ITEM", "foo");
  poVRT->ClearStatistics();
  EXPECT_EQ(poBand->GetMetadataItem("STATISTICS_MINIMUM"), nullptr);
  EXPECT_STREQ(poBand->GetMetadataItem("OTHER_ITEM"), "foo");
}

// 20. OpenVRTProtocolOptions
TEST_F(VRTDatasetTest, OpenVRTProtocolOptions) {
  TrackFile("/vsimem/proto_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/proto_src.tif", 20, 20, 2, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  double gtCoeffs[6] = {0.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  poSrcDS->SetGeoTransform(GDALGeoTransform(gtCoeffs));
  poSrcDS->GetRasterBand(1)->CreateMaskBand(0);
  poSrcDS->SetMetadataItem("SUBDATASET_1_NAME", "/vsimem/proto_src.tif",
                           "SUBDATASETS");
  poSrcDS->SetMetadataItem("SUBDATASET_1_DESC", "Subdataset 1", "SUBDATASETS");
  poSrcDS->SetMetadataItem("SUBDATASET_2_NAME",
                           "GTIFF_DIR:1:/vsimem/proto_src.tif", "SUBDATASETS");
  poSrcDS->SetMetadataItem("SUBDATASET_2_DESC", "GTIFF Dir 1", "SUBDATASETS");
  poSrcDS.reset();

  // 1. Initial options & error cases
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?invalid_opt"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?if=GTiff&if=MEM"),
              nullptr);
    EXPECT_EQ(
        GDALDataset::Open("vrt:///vsimem/proto_src.tif?oo=FOO=1&oo=BAR=2"),
        nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?unknown_opt=foo"),
              nullptr);
    CPLPopErrorHandler();
  }

  // 2. Multidim transpose
  const std::string mdim_vrt =
      "<VRTDataset rasterXSize=\"5\" rasterYSize=\"4\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\"/>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"4\"/>\n"
      "    <Dimension name=\"X\" size=\"5\"/>\n"
      "    <Array name=\"var\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  TrackFile("/vsimem/md_trans.vrt");
  VSILFILE* fp = VSIFileFromMemBuffer(
      "/vsimem/md_trans.vrt",
      reinterpret_cast<GByte*>(const_cast<char*>(mdim_vrt.data())),
      mdim_vrt.size(), FALSE);
  VSIFCloseL(fp);

  auto poTransDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/md_trans.vrt?transpose=/var:1,0"));
  ASSERT_NE(poTransDS, nullptr);
  EXPECT_EQ(poTransDS->GetRasterXSize(), 5);
  EXPECT_EQ(poTransDS->GetRasterYSize(), 4);

  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/md_trans.vrt?transpose=/var"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/md_trans.vrt?transpose=/var:1"),
              nullptr);
    EXPECT_EQ(
        GDALDataset::Open("vrt:///vsimem/md_trans.vrt?transpose=/bad:1,0"),
        nullptr);
    CPLPopErrorHandler();
  }

  // 3. Subdataset options & errors
  auto poSDDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd=1"));
  ASSERT_NE(poSDDS, nullptr);

  auto poSDNameDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd_name=1"));
  ASSERT_NE(poSDNameDS, nullptr);

  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(
        GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd_name=nonexistent"),
        nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd=0"), nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd=5"), nullptr);
    EXPECT_EQ(GDALDataset::Open(
                  "vrt:///vsimem/proto_src.tif?sd=1&transpose=/var:1,0"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?sd=1&sd_name=foo"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open(
                  "vrt:///vsimem/proto_src.tif?sd_name=foo&transpose=/var:1,0"),
              nullptr);
    CPLPopErrorHandler();
  }

  // 4. Comprehensive protocol arguments
  auto poFullDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?"
                        "bands=1,mask&a_nodata=0&a_srs=EPSG:4326&"
                        "a_scale=2.0&a_offset=1.0&ot=Byte&"
                        "scale=true&exponent=1.5&outsize=10,10&"
                        "r=bilinear&srcwin=0,0,10,10&a_gt=0,1,0,0,0,1&"
                        "a_coord_epoch=2021.5&epo=true&eco=true"));
  ASSERT_NE(poFullDS, nullptr);

  auto poFullDS2 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?"
                        "unscale=true&tr=2,2"));
  ASSERT_NE(poFullDS2, nullptr);

  auto poFullDS2b = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?"
                        "a_ullr=0,20,20,0"));
  ASSERT_NE(poFullDS2b, nullptr);

  auto poFullDS3 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?"
                        "nogcp=true&projwin=0,20,20,0&projwin_srs=EPSG:4326"));
  ASSERT_NE(poFullDS3, nullptr);

  auto poFullDS4 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?"
                        "gcp=0,0,10,20,0&scale_1=0,255,0,1"));
  ASSERT_NE(poFullDS4, nullptr);

  auto poOvrDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("vrt:///vsimem/proto_src.tif?ovr=0"));
  ASSERT_NE(poOvrDS, nullptr);

  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?expand=rgb"),
              nullptr);
    CPLPopErrorHandler();
  }

  // 5. Error branches for query options
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?bands=99"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?a_ullr=0,0"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?gcp=0,0"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?scale=1,2,3"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?outsize=5"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?projwin=0,0"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?tr=1"), nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?srcwin=0,0"),
              nullptr);
    EXPECT_EQ(GDALDataset::Open("vrt:///vsimem/proto_src.tif?a_gt=0,0"),
              nullptr);
    CPLPopErrorHandler();
  }
}

// 21. CreateMaskBandAndDelete
TEST_F(VRTDatasetTest, CreateMaskBandAndDelete) {
  VRTDataset ds(10, 10);
  EXPECT_EQ(ds.CreateMaskBand(0), CE_None);
  EXPECT_EQ(ds.AddBand(GDT_Byte, nullptr), CE_None);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(ds.CreateMaskBand(0), CE_Failure);
  CPLPopErrorHandler();

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(VRTDataset::Delete("/vsimem/nonexistent_delete_file.vrt"),
            CE_Failure);
  CPLPopErrorHandler();
}

// 22. CheckCompatibleForDatasetIONoSrcDSName
TEST_F(VRTDatasetTest, CheckCompatibleForDatasetIONoSrcDSName) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(poDrv, nullptr);
  auto poMemDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("", 10, 10, 2, GDT_Byte, nullptr));
  ASSERT_NE(poMemDS, nullptr);

  VRTDataset ds(10, 10);
  ds.AddBand(GDT_Byte, nullptr);
  ds.AddBand(GDT_Byte, nullptr);

  auto poBand1 = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  auto poBand2 = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(2));

  poBand1->AddSimpleSource(poMemDS->GetRasterBand(1));
  poBand2->AddSimpleSource(poMemDS->GetRasterBand(2));

  EXPECT_STREQ(ds.GetMetadataItem("CheckCompatibleForDatasetIO()", "__DEBUG__"),
               "1");
}

}  // namespace
