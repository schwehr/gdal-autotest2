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
// Tests the VRTDataset class and vrt:// protocol driver (vrtdataset.cpp).
//
// Build caveats in google3:
// - PAM (.aux.xml sidecar files) is compiled out, so dataset metadata and
//   georeferencing mutations are held in memory rather than persisted to PAM.
// - //third_party/gdal/BUILD passes -DGDAL_VRT_ENABLE_RAWRASTERBAND=ON (see
//   b/429626570), and non-sibling VRTRawRasterBand source files require
//   setting GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE=ALL.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtdataset.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include "third_party/gdal/frmts/vrt/vrtdataset.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtdataset_test";

// TODO(schwehr): Extract ScopedErrorCapturer to util/error_handler.h.
class ScopedErrorCapturer {
 public:
  ScopedErrorCapturer() {
    CPLErrorReset();
    CPLPushErrorHandlerEx(&ScopedErrorCapturer::Callback, this);
  }
  ~ScopedErrorCapturer() {
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  bool has_failure() const { return has_failure_; }

 private:
  static void CPL_STDCALL Callback(CPLErr err_class, int /*err_num*/,
                                   const char* /*msg*/) {
    auto* self =
        static_cast<ScopedErrorCapturer*>(CPLGetErrorHandlerUserData());
    if (self != nullptr && err_class == CE_Failure) {
      self->has_failure_ = true;
    }
  }

  bool has_failure_ = false;
};

class VrtDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateMemFile(const char* filename, size_t size) {
    VSILFILE* fp = VSIFOpenL(filename, "wb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    std::vector<GByte> zeros(size, 0);
    ASSERT_EQ(zeros.size(), VSIFWriteL(zeros.data(), 1, zeros.size(), fp));
  }

  void CreateGeoTiff(const char* filename, int xsize, int ysize, int bands,
                     GDALDataType dt,
                     std::optional<std::span<const double, 6>> gt,
                     const char* srs_wkt, double nodata,
                     const std::vector<std::vector<double>>& band_data) {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDriver);

    GDALDatasetUniquePtr poDS(
        poDriver->Create(filename, xsize, ysize, bands, dt, nullptr));
    ASSERT_NE(nullptr, poDS);

    if (gt.has_value()) {
      EXPECT_EQ(CE_None, poDS->SetGeoTransform(GDALGeoTransform(gt->data())));
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      ASSERT_EQ(OGRERR_NONE, srs.SetFromUserInput(srs_wkt));
      EXPECT_EQ(CE_None, poDS->SetSpatialRef(&srs));
    }

    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* poBand = poDS->GetRasterBand(b);
      ASSERT_NE(nullptr, poBand);
      EXPECT_EQ(CE_None, poBand->SetNoDataValue(nodata));
      if (static_cast<size_t>(b - 1) < band_data.size()) {
        const auto& data = band_data[b - 1];
        if (data.size() == static_cast<size_t>(xsize) * ysize) {
          std::vector<double> copy_data = data;
          EXPECT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                              copy_data.data(), xsize, ysize,
                                              GDT_Float64, 0, 0, nullptr));
        }
      }
    }
    poDS->FlushCache(false);
  }
};

class TestVRTDataset : public VRTDataset {
 public:
  using VRTDataset::CloseDependentDatasets;
  using VRTDataset::VRTDataset;
};

TEST_F(VrtDatasetTest, BasicConstructionAndAccessors) {
  // 1. Constructor block sizing.
  VRTDataset ds1(100, 200, 0, 0);
  EXPECT_EQ(100, ds1.GetRasterXSize());
  EXPECT_EQ(200, ds1.GetRasterYSize());
  EXPECT_EQ(100, ds1.GetBlockXSize());  // std::min(128, 100)
  EXPECT_EQ(128, ds1.GetBlockYSize());  // std::min(128, 200)

  VRTDataset ds2(500, 500, 64, 64);
  EXPECT_EQ(64, ds2.GetBlockXSize());
  EXPECT_EQ(64, ds2.GetBlockYSize());

  // 2. VRTCreate (C API) wrapped via RAII.
  {
    GDALDatasetUniquePtr poVRT(GDALDataset::FromHandle(VRTCreate(50, 60)));
    ASSERT_NE(nullptr, poVRT);
    EXPECT_EQ(50, poVRT->GetRasterXSize());
    EXPECT_EQ(60, poVRT->GetRasterYSize());
  }

  // 3. GeoTransform.
  VRTDataset ds(10, 10);
  GDALGeoTransform gtIn(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, ds.SetGeoTransform(gtIn));
  GDALGeoTransform gtOut;
  EXPECT_EQ(CE_None, ds.GetGeoTransform(gtOut));
  EXPECT_DOUBLE_EQ(100.0, gtOut[0]);
  EXPECT_DOUBLE_EQ(-1.0, gtOut[5]);

  // 4. Spatial reference.
  OGRSpatialReference srsIn;
  ASSERT_EQ(OGRERR_NONE, srsIn.importFromEPSG(4326));
  EXPECT_EQ(CE_None, ds.SetSpatialRef(&srsIn));
  const auto* poSRS = ds.GetSpatialRef();
  ASSERT_NE(nullptr, poSRS);
  EXPECT_THAT(*poSRS, IsSameAs(srsIn));

  // 5. GCPs.
  EXPECT_EQ(0, ds.GetGCPCount());
  EXPECT_EQ(nullptr, ds.GetGCPs());
  GDAL_GCP asGCPs[2];
  GDALInitGCPs(2, asGCPs);
  absl::Cleanup deinit_gcps = [&asGCPs] { GDALDeinitGCPs(2, asGCPs); };
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 10.0;
  asGCPs[0].dfGCPY = 20.0;
  asGCPs[1].dfGCPPixel = 10.0;
  asGCPs[1].dfGCPLine = 10.0;
  asGCPs[1].dfGCPX = 30.0;
  asGCPs[1].dfGCPY = 40.0;
  EXPECT_EQ(CE_None, ds.SetGCPs(2, asGCPs, &srsIn));
  EXPECT_EQ(2, ds.GetGCPCount());
  const GDAL_GCP* pasRetGCPs = ds.GetGCPs();
  ASSERT_NE(nullptr, pasRetGCPs);
  EXPECT_DOUBLE_EQ(10.0, pasRetGCPs[0].dfGCPX);
  EXPECT_DOUBLE_EQ(40.0, pasRetGCPs[1].dfGCPY);
  const auto* poGCPSRS = ds.GetGCPSpatialRef();
  ASSERT_NE(nullptr, poGCPSRS);
  EXPECT_THAT(*poGCPSRS, IsSameAs(srsIn));

  // 6. Metadata.
  CPLStringList aosMD;
  aosMD.AddNameValue("KEY", "VALUE");
  EXPECT_EQ(CE_None, ds.SetMetadata(aosMD.List(), "CUSTOM_DOMAIN"));
  EXPECT_STREQ("VALUE", ds.GetMetadataItem("KEY", "CUSTOM_DOMAIN"));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("ANOTHER_KEY", "ANOTHER_VALUE",
                                        "CUSTOM_DOMAIN"));
  EXPECT_STREQ("ANOTHER_VALUE",
               ds.GetMetadataItem("ANOTHER_KEY", "CUSTOM_DOMAIN"));

  // 7. ClearStatistics.
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  GDALRasterBand* poBand = ds.GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MINIMUM", "0"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MAXIMUM", "255"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("SOME_OTHER_TAG", "KEEP"));
  ds.ClearStatistics();
  EXPECT_EQ(nullptr, poBand->GetMetadataItem("STATISTICS_MINIMUM"));
  EXPECT_STREQ("KEEP", poBand->GetMetadataItem("SOME_OTHER_TAG"));
}

TEST_F(VrtDatasetTest, BlockSizesAndSerialization) {
  VRTDataset ds(200, 200, 32, 32);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds.GetRasterBand(1));

  // 1. SerializeToXML.
  {
    CPLXMLTreeCloser psTree(ds.SerializeToXML(nullptr));
    ASSERT_NE(nullptr, psTree.get());
    CPLXMLNode* psBand =
        CPLGetXMLNode(psTree.get(), "=VRTDataset.VRTRasterBand");
    ASSERT_NE(nullptr, psBand);
    EXPECT_STREQ("32", CPLGetXMLValue(psBand, "blockXSize", ""));
    EXPECT_STREQ("32", CPLGetXMLValue(psBand, "blockYSize", ""));
  }

  // 2. VRTSerializeToXML (C API).
  {
    CPLXMLTreeCloser psCAPIXML(
        VRTSerializeToXML(GDALDataset::ToHandle(&ds), nullptr));
    ASSERT_NE(nullptr, psCAPIXML.get());
  }

  // 3. GetMetadata("xml:VRT").
  char** papszVRT = ds.GetMetadata("xml:VRT");
  ASSERT_NE(nullptr, papszVRT);
  ASSERT_NE(nullptr, papszVRT[0]);
  EXPECT_THAT(std::string_view(papszVRT[0]), HasSubstr("VRTDataset"));

  // 4. GetMetadataItem on __DEBUG__ domain.
  EXPECT_STREQ("0", ds.GetMetadataItem("MULTI_THREADED_RASTERIO_LAST_USED",
                                       "__DEBUG__"));
  EXPECT_STREQ(
      "0", ds.GetMetadataItem("CheckCompatibleForDatasetIO()", "__DEBUG__"));
}

TEST_F(VrtDatasetTest, InitBandAndSubclassesViaXML) {
  // 1. VRTSourcedRasterBand via XML.
  {
    constexpr char kXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTSourcedRasterBand"/>
</VRTDataset>)xml";
    auto poDS = VRTDataset::OpenXML(kXml, nullptr, GA_ReadOnly);
    ASSERT_NE(nullptr, poDS);
    EXPECT_TRUE(static_cast<VRTRasterBand*>(poDS->GetRasterBand(1))
                    ->IsSourcedRasterBand());
  }

  // 2. VRTDerivedRasterBand via XML.
  {
    constexpr char kXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTDerivedRasterBand">
    <PixelFunctionType>add</PixelFunctionType>
  </VRTRasterBand>
</VRTDataset>)xml";
    auto poDS = VRTDataset::OpenXML(kXml, nullptr, GA_ReadOnly);
    ASSERT_NE(nullptr, poDS);
  }

  // 3. VRTRawRasterBand via XML. The raw file must exist since the dataset
  // is opened read-only, and an absolute source path requires relaxing the
  // default SIBLING_OR_CHILD_OF_VRT_PATH source policy.
  constexpr char kRawPath[] = "/vsimem/vrtdataset_test/raw.bin";
  CreateMemFile(kRawPath, 100);
  {
    CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                     false);
    CPLConfigOptionSetter allow_any_source(
        "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);
    constexpr char kXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTRawRasterBand">
    <SourceFilename>/vsimem/vrtdataset_test/raw.bin</SourceFilename>
  </VRTRasterBand>
</VRTDataset>)xml";
    auto poDS = VRTDataset::OpenXML(kXml, nullptr, GA_ReadOnly);
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(1, poDS->GetRasterCount());
  }

  // 4. Runtime disable of VRTRawRasterBand.
  {
    CPLConfigOptionSetter disable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO",
                                      false);
    EXPECT_FALSE(VRTDataset::IsRawRasterBandEnabled());
    constexpr char kXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTRawRasterBand">
    <SourceFilename>/vsimem/vrtdataset_test/raw.bin</SourceFilename>
  </VRTRasterBand>
</VRTDataset>)xml";
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VRTDataset::OpenXML(kXml, nullptr, GA_ReadOnly));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  EXPECT_TRUE(VRTDataset::IsRawRasterBandEnabled());

  // 5. VRTWarpedRasterBand on non-warped dataset and unrecognized subclass
  // fail.
  {
    constexpr char kWarpedXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTWarpedRasterBand"/>
</VRTDataset>)xml";
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VRTDataset::OpenXML(kWarpedXml, nullptr, GA_ReadOnly));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    constexpr char kUnknownXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="UnknownSubClass"/>
</VRTDataset>)xml";
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VRTDataset::OpenXML(kUnknownXml, nullptr, GA_ReadOnly));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtDatasetTest, AddBandAndVRTAddBand) {
  VRTDataset ds(20, 20);

  // 1. AddBand GDT_Unknown fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ds.AddBand(GDT_Unknown, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. AddBand with subClass=VRTSourcedRasterBand and custom block sizes.
  CPLStringList aosOptions1;
  aosOptions1.AddNameValue("subClass", "VRTSourcedRasterBand");
  aosOptions1.AddNameValue("BlockXSize", "10");
  aosOptions1.AddNameValue("BlockYSize", "10");
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, aosOptions1.List()));
  ASSERT_EQ(1, ds.GetRasterCount());
  int nBX = 0;
  int nBY = 0;
  ds.GetRasterBand(1)->GetBlockSize(&nBX, &nBY);
  EXPECT_EQ(10, nBX);
  EXPECT_EQ(10, nBY);

  // 3. AddBand with subClass=VRTDerivedRasterBand and pixel function.
  CPLStringList aosOptions2;
  aosOptions2.AddNameValue("subClass", "VRTDerivedRasterBand");
  aosOptions2.AddNameValue("PixelFunctionType", "add");
  aosOptions2.AddNameValue("SourceTransferType", "Byte");
  aosOptions2.AddNameValue("BufferRadius", "1");
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Float32, aosOptions2.List()));
  ASSERT_EQ(2, ds.GetRasterCount());

  // 4. AddBand with subClass=VRTRawRasterBand.
  constexpr char kRawPath[] = "/vsimem/vrtdataset_test/raw_add.bin";
  CreateMemFile(kRawPath, 400);
  {
    CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                     false);
    CPLConfigOptionSetter allow_any_source(
        "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);
    CPLStringList aosOptions3;
    aosOptions3.AddNameValue("subClass", "VRTRawRasterBand");
    aosOptions3.AddNameValue("SourceFilename", kRawPath);
    aosOptions3.AddNameValue("ImageOffset", "0");
    aosOptions3.AddNameValue("PixelOffset", "1");
    aosOptions3.AddNameValue("LineOffset", "20");
    aosOptions3.AddNameValue("ByteOrder", "LSB");
    ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, aosOptions3.List()));
  }
  ASSERT_EQ(3, ds.GetRasterCount());

  // 5. VRTAddBand (C API).
  EXPECT_EQ(CE_None,
            VRTAddBand(GDALDataset::ToHandle(&ds), GDT_Int16, nullptr));
  EXPECT_EQ(4, ds.GetRasterCount());
}

TEST_F(VrtDatasetTest, XMLInitAndOpenXML) {
  // 1. XML with OverviewList, BlockSize, SRS, GeoTransform, GCPList, Metadata.
  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <BlockXSize>10</BlockXSize>
  <BlockYSize>10</BlockYSize>
  <SRS dataAxisToSRSAxisMapping="1,2">EPSG:4326</SRS>
  <GeoTransform>0, 1, 0, 0, 0, -1</GeoTransform>
  <Metadata>
    <MDI key="ATTR">VALUE</MDI>
  </Metadata>
  <GCPList Projection="EPSG:4326">
    <GCP Id="1" Pixel="0" Line="0" X="10" Y="20" Z="0"/>
  </GCPList>
  <OverviewList resampling="nearest">2 4</OverviewList>
  <VRTRasterBand dataType="Byte" band="1">
    <ColorInterp>Gray</ColorInterp>
  </VRTRasterBand>
  <MaskBand>
    <VRTRasterBand dataType="Byte">
      <SimpleSource>
        <SourceFilename>/vsimem/vrtdataset_test/non_existent.tif</SourceFilename>
        <SourceBand>1</SourceBand>
      </SimpleSource>
    </VRTRasterBand>
  </MaskBand>
</VRTDataset>)xml";

  auto poDS = VRTDataset::OpenXML(kVrtXml, kTestDir, GA_ReadOnly);
  ASSERT_NE(nullptr, poDS);
  EXPECT_EQ(20, poDS->GetRasterXSize());
  EXPECT_EQ(20, poDS->GetRasterYSize());
  EXPECT_EQ(1, poDS->GetRasterCount());
  EXPECT_STREQ("VALUE", poDS->GetMetadataItem("ATTR"));
  EXPECT_EQ(1, poDS->GetGCPCount());

  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, poDS->GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(1.0, gt[1]);

  // 2. OpenXML error branches.
  constexpr std::string_view kInvalidXmls[] = {
      "Invalid XML <><",
      "<NotVRTDataset/>",
      // Missing rasterXSize.
      "<VRTDataset><VRTRasterBand dataType=\"Byte\"/></VRTDataset>",
      // Negative dimensions.
      "<VRTDataset rasterXSize=\"-10\" rasterYSize=\"-10\">"
      "<VRTRasterBand dataType=\"Byte\" band=\"1\"/></VRTDataset>",
  };
  for (std::string_view xml : kInvalidXmls) {
    const std::string xml_str(xml);
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              VRTDataset::OpenXML(xml_str.c_str(), nullptr, GA_ReadOnly));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtDatasetTest, DriverIdentifyAndOpen) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("VRT");
  ASSERT_NE(nullptr, poDriver);

  // 1. Identify tests.
  struct IdentifyCase {
    std::string_view input;
    bool expected;
  };
  constexpr IdentifyCase kIdentifyCases[] = {
      {.input = "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\"/>",
       .expected = true},
      {.input = "vrt:///vsimem/vrtdataset_test/test.tif", .expected = true},
      {.input = "<OGRVRTDataSource><OGRVRTLayer/></OGRVRTDataSource>",
       .expected = false},
      {.input = "<VRTWarpedDataset rasterXSize=\"10\" rasterYSize=\"10\"/>",
       .expected = false},
      {.input = "/vsimem/vrtdataset_test/not_a_vrt.tif", .expected = false},
  };
  for (const auto& tc : kIdentifyCases) {
    const std::string input_str(tc.input);
    GDALOpenInfo oi(input_str.c_str(), GA_ReadOnly);
    EXPECT_EQ(tc.expected, static_cast<bool>(VRTDataset::Identify(&oi)))
        << tc.input;
  }

  // 2. Open with XML string directly via GDALOpenEx.
  constexpr char kXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1"/>
</VRTDataset>)xml";
  {
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(GDALOpenEx(
        kXml, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(10, poDS->GetRasterXSize());
    EXPECT_EQ(10, poDS->GetRasterYSize());
    EXPECT_EQ(1, poDS->GetRasterCount());
  }

  // 3. Open with ROOT_PATH open option.
  {
    constexpr char kVrtFile[] = "/vsimem/vrtdataset_test/test_root_path.vrt";
    VsiMemTempWrapper vrt_mem(kVrtFile, kXml);
    CPLStringList aosOpenOptions;
    aosOpenOptions.AddNameValue("ROOT_PATH", kTestDir);
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(kVrtFile, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   aosOpenOptions.List(), nullptr)));
    ASSERT_NE(nullptr, poDS);
  }

  // 4. Opening with ONLY GDAL_OF_MULTIDIM_RASTER returns nullptr because the
  // dataset has no multidim group.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(kXml, GDAL_OF_READONLY | GDAL_OF_MULTIDIM_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS);
    CPLErrorReset();
  }
}

TEST_F(VrtDatasetTest, OpenVRTProtocol) {
  constexpr char kSrcTif[] = "/vsimem/vrtdataset_test/protocol_src.tif";
  constexpr double kGT[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  OGRSpatialReference oSRS;
  ASSERT_EQ(OGRERR_NONE, oSRS.importFromEPSG(4326));
  char* pszWktRaw = nullptr;
  ASSERT_EQ(OGRERR_NONE, oSRS.exportToWkt(&pszWktRaw));
  std::unique_ptr<char, CplFreeDeleter> pszWkt(pszWktRaw);

  std::vector<std::vector<double>> bands_data = {
      std::vector<double>(100, 10.0),
      std::vector<double>(100, 20.0),
  };
  CreateGeoTiff(kSrcTif, 10, 10, 2, GDT_Float32,
                std::span<const double, 6>(kGT, 6), pszWkt.get(), 99.0,
                bands_data);

  // Palette dataset for expand option.
  constexpr char kPaletteTif[] = "/vsimem/vrtdataset_test/protocol_palette.tif";
  {
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDriver);
    GDALDatasetUniquePtr poPalDS(
        poDriver->Create(kPaletteTif, 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poPalDS);
    GDALColorTable oCT;
    GDALColorEntry oEntry = {255, 0, 0, 255};
    oCT.SetColorEntry(0, &oEntry);
    EXPECT_EQ(CE_None, poPalDS->GetRasterBand(1)->SetColorTable(&oCT));
    poPalDS->FlushCache(false);
  }

  // 1. bands=1
  {
    const std::string uri = std::string("vrt://") + kSrcTif + "?bands=1";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(1, poDS->GetRasterCount());
  }

  // 2. bands=1,mask
  {
    const std::string uri = std::string("vrt://") + kSrcTif + "?bands=1,mask";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(2, poDS->GetRasterCount());
  }

  // 3. a_nodata, a_srs, a_ullr
  {
    const std::string uri = std::string("vrt://") + kSrcTif +
                            "?a_nodata=-999&a_srs=EPSG:3857&a_ullr=0,10,10,0";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    int bHasNoData = 0;
    const double dfNoData = poDS->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(-999.0, dfNoData);
  }

  // 4. a_scale, a_offset, ot=Int32
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?a_scale=2.5&a_offset=5.0&ot=Int32";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(GDT_Int32, poDS->GetRasterBand(1)->GetRasterDataType());
    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(2.5, poDS->GetRasterBand(1)->GetScale(&bSuccess));
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(5.0, poDS->GetRasterBand(1)->GetOffset(&bSuccess));
    EXPECT_TRUE(bSuccess);
  }

  // 5. expand=rgb and expand=rgba
  {
    const std::string uri = std::string("vrt://") + kPaletteTif + "?expand=rgb";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(3, poDS->GetRasterCount());
  }
  {
    const std::string uri =
        std::string("vrt://") + kPaletteTif + "?expand=rgba";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterCount());
  }

  // 6. gcp option
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?gcp=0,0,10,20,0&gcp=10,10,30,40,0";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(2, poDS->GetGCPCount());
  }

  // 7. scale, scale_1, exponent, exponent_1
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?scale_1=0,100,0,1&exponent_1=0.5";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?scale=0,100,0,1&exponent=0.5";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }

  // 8. outsize (percentage and explicit)
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?outsize=50%,50%";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(5, poDS->GetRasterXSize());
    EXPECT_EQ(5, poDS->GetRasterYSize());
  }
  {
    const std::string uri = std::string("vrt://") + kSrcTif + "?outsize=4,4";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterXSize());
    EXPECT_EQ(4, poDS->GetRasterYSize());
  }

  // 9. projwin, projwin_srs, tr, r=bilinear
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif +
        "?projwin=0,10,10,0&projwin_srs=EPSG:4326&tr=1,1&r=bilinear";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }

  // 10. srcwin, a_gt, unscale, a_coord_epoch, nogcp, epo, eco, if, oo
  {
    const std::string uri =
        std::string("vrt://") + kSrcTif +
        "?srcwin=0,0,5,5&a_gt=0,1,0,0,0,-1&unscale=true&a_coord_epoch=2021.0&"
        "nogcp=true&epo=true&eco=true&if=GTiff&oo=NUM_THREADS=1";
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(5, poDS->GetRasterXSize());
    EXPECT_EQ(5, poDS->GetRasterYSize());
  }

  // 11. Error handling in OpenVRTProtocol.
  constexpr std::string_view kBadQueries[] = {
      "bad_param",       "if=",        "a_gt=1,2", "a_ullr=1,2",
      "outsize=invalid", "srcwin=1,2", "tr=1",     "unknown_xyz=1",
  };
  for (std::string_view query : kBadQueries) {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?" + std::string(query);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS) << query;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << query;
    CPLErrorReset();
  }
}

TEST_F(VrtDatasetTest, MaskBand) {
  VRTDataset ds(10, 10);
  EXPECT_EQ(CE_None, ds.CreateMaskBand(0));
  EXPECT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));

  // Calling CreateMaskBand a second time fails.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ds.CreateMaskBand(0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Serialization of mask band.
  {
    CPLXMLTreeCloser psXML(ds.SerializeToXML(nullptr));
    ASSERT_NE(nullptr, psXML.get());
    EXPECT_NE(nullptr, CPLGetXMLNode(psXML.get(), "=VRTDataset.MaskBand"));
  }

  // SetMaskBand.
  VRTDataset ds2(10, 10);
  ASSERT_EQ(CE_None, ds2.AddBand(GDT_Byte, nullptr));
  ds2.SetMaskBand(std::make_unique<VRTSourcedRasterBand>(&ds2, 0));
  EXPECT_NE(nullptr, ds2.GetRasterBand(1)->GetMaskBand());
}

TEST_F(VrtDatasetTest, DatasetCreationAndManagement) {
  // 1. Create with SUBCLASS=VRTWarpedDataset.
  {
    CPLStringList aosWarpedOptions;
    aosWarpedOptions.AddNameValue("SUBCLASS", "VRTWarpedDataset");
    GDALDatasetUniquePtr poWarped(
        VRTDataset::Create("/vsimem/vrtdataset_test/warped.vrt", 10, 10, 1,
                           GDT_Byte, aosWarpedOptions.List()));
    ASSERT_NE(nullptr, poWarped);
    EXPECT_STREQ("VRT", poWarped->GetDriverName());
  }

  // 2. Create with invalid SUBCLASS fails.
  {
    CPLStringList aosBadOptions;
    aosBadOptions.AddNameValue("SUBCLASS", "InvalidSubClass");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetUniquePtr poBad(
        VRTDataset::Create("/vsimem/vrtdataset_test/bad.vrt", 10, 10, 1,
                           GDT_Byte, aosBadOptions.List()));
    EXPECT_EQ(nullptr, poBad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Create from XML string.
  {
    constexpr char kXml[] = R"xml(<VRTDataset rasterXSize="15" rasterYSize="25">
  <VRTRasterBand dataType="Int32" band="1"/>
</VRTDataset>)xml";
    GDALDatasetUniquePtr poFromXML(
        VRTDataset::Create(kXml, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poFromXML);
    EXPECT_EQ(15, poFromXML->GetRasterXSize());
    EXPECT_EQ(25, poFromXML->GetRasterYSize());
  }

  // 4. CreateVRTMultiDimensional and CreateMultiDimensional.
  constexpr char kMdFile[] = "/vsimem/vrtdataset_test/create_md.vrt";
  {
    auto poMD1 = std::unique_ptr<VRTDataset>(
        VRTDataset::CreateVRTMultiDimensional(kMdFile, nullptr, nullptr));
    ASSERT_NE(nullptr, poMD1);

    GDALDatasetUniquePtr poMD2(
        VRTDataset::CreateMultiDimensional(kMdFile, nullptr, nullptr));
    ASSERT_NE(nullptr, poMD2);
  }

  // 5. GetFileList and Delete.
  constexpr char kDelFile[] = "/vsimem/vrtdataset_test/to_delete.vrt";
  {
    auto poDS = std::unique_ptr<VRTDataset>(
        VRTDataset::CreateVRTDataset(kDelFile, 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
    poDS->FlushCache(true);
    StringListPtr papszFiles(poDS->GetFileList(), CSLDestroy);
    ASSERT_NE(nullptr, papszFiles.get());
    EXPECT_GE(CSLCount(papszFiles.get()), 1);
  }

  EXPECT_EQ(CE_None, VRTDataset::Delete(kDelFile));
  VSIStatBufL sStat;
  EXPECT_EQ(-1, VSIStatL(kDelFile, &sStat));

  // 6. CloseDependentDatasets.
  TestVRTDataset dsDep(10, 10);
  ASSERT_EQ(CE_None, dsDep.AddBand(GDT_Byte, nullptr));
  EXPECT_EQ(0, dsDep.CloseDependentDatasets());
}

TEST_F(VrtDatasetTest, DatasetRasterIOAndThreading) {
  constexpr char kSrcFile[] = "/vsimem/vrtdataset_test/ds_io_src.tif";
  std::vector<std::vector<double>> bands_data = {
      std::vector<double>(100, 42.0),
      std::vector<double>(100, 84.0),
  };
  CreateGeoTiff(kSrcFile, 10, 10, 2, GDT_Byte, std::nullopt, nullptr, 0.0,
                bands_data);

  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/ds_io_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
      <DstRect xOff="0" yOff="0" xSize="10" ySize="10"/>
    </SimpleSource>
  </VRTRasterBand>
  <VRTRasterBand dataType="Byte" band="2">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/ds_io_src.tif</SourceFilename>
      <SourceBand>2</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
      <DstRect xOff="0" yOff="0" xSize="10" ySize="10"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(GDALOpenEx(
      kVrtXml, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);

  // 1. AdviseRead.
  int anBands[] = {1, 2};
  EXPECT_EQ(CE_None, poDS->AdviseRead(0, 0, 10, 10, 10, 10, GDT_Byte, 2,
                                      anBands, nullptr));

  // 2. Dataset-level RasterIO single thread.
  std::vector<GByte> buf(200, 0);
  EXPECT_EQ(CE_None, poDS->RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                    GDT_Byte, 2, anBands, 0, 0, 0, nullptr));
  EXPECT_EQ(42, buf[0]);
  EXPECT_EQ(84, buf[100]);

  // 3. Dataset-level RasterIO with GDAL_NUM_THREADS=2.
  {
#if defined(THREAD_SANITIZER) || defined(__SANITIZE_THREAD__)
    // TODO(schwehr): Re-enable multi-threaded TSan once upstream
    // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
    CPLConfigOptionSetter num_threads("GDAL_NUM_THREADS", "1", false);
#else
    CPLConfigOptionSetter num_threads("GDAL_NUM_THREADS", "2", false);
#endif
    std::fill(buf.begin(), buf.end(), 0);
    EXPECT_EQ(CE_None, poDS->RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                      GDT_Byte, 2, anBands, 0, 0, 0, nullptr));
    EXPECT_EQ(42, buf[0]);
    EXPECT_EQ(84, buf[100]);
  }

  // 4. Write with dataset-level RasterIO on VRTRawRasterBand.
  constexpr char kRawBin[] = "/vsimem/vrtdataset_test/raw_write.bin";
  CreateMemFile(kRawBin, 200);
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_any_source(
      "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);

  auto dsWrite = VRTDataset::CreateVRTDataset("", 10, 10, 0, GDT_Byte, nullptr);
  ASSERT_NE(nullptr, dsWrite);
  EXPECT_EQ(GA_Update, dsWrite->GetAccess());

  CPLStringList aosRaw1;
  aosRaw1.AddNameValue("subClass", "VRTRawRasterBand");
  aosRaw1.AddNameValue("SourceFilename", kRawBin);
  aosRaw1.AddNameValue("ImageOffset", "0");
  aosRaw1.AddNameValue("PixelOffset", "1");
  aosRaw1.AddNameValue("LineOffset", "10");

  CPLStringList aosRaw2;
  aosRaw2.AddNameValue("subClass", "VRTRawRasterBand");
  aosRaw2.AddNameValue("SourceFilename", kRawBin);
  aosRaw2.AddNameValue("ImageOffset", "100");
  aosRaw2.AddNameValue("PixelOffset", "1");
  aosRaw2.AddNameValue("LineOffset", "10");

  ASSERT_EQ(CE_None, dsWrite->AddBand(GDT_Byte, aosRaw1.List()));
  ASSERT_EQ(CE_None, dsWrite->AddBand(GDT_Byte, aosRaw2.List()));

  std::vector<GByte> write_buf(200, 77);
  EXPECT_EQ(CE_None,
            dsWrite->RasterIO(GF_Write, 0, 0, 10, 10, write_buf.data(), 10, 10,
                              GDT_Byte, 2, anBands, 0, 0, 0, nullptr));
  dsWrite->FlushCache(false);

  std::vector<GByte> read_buf(200, 0);
  EXPECT_EQ(CE_None,
            dsWrite->RasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10, 10,
                              GDT_Byte, 2, anBands, 0, 0, 0, nullptr));
  EXPECT_EQ(77, read_buf[0]);
  EXPECT_EQ(77, read_buf[199]);
}

TEST_F(VrtDatasetTest, VirtualOverviewsAndBuildOverviews) {
  constexpr char kSrcTif[] = "/vsimem/vrtdataset_test/ovr_src.tif";
  std::vector<std::vector<double>> bands_data = {
      std::vector<double>(400, 50.0),
  };
  CreateGeoTiff(kSrcTif, 20, 20, 1, GDT_Byte, std::nullopt, nullptr, 0.0,
                bands_data);

  // 1. Build real overviews on the source GeoTIFF.
  {
    GDALDatasetUniquePtr poSrcDS(GDALDataset::FromHandle(GDALOpenEx(
        kSrcTif, GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poSrcDS);
    int anOvrList[] = {2, 4};
    EXPECT_EQ(CE_None, poSrcDS->BuildOverviews("NEAREST", 2, anOvrList, 0,
                                               nullptr, nullptr, nullptr));
  }

  // 2. Open VRT referencing the source dataset -> BuildVirtualOverviews().
  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/ovr_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  auto poDS = VRTDataset::OpenXML(kVrtXml, nullptr, GA_Update);
  ASSERT_NE(nullptr, poDS);
  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_GE(poBand->GetOverviewCount(), 2);

  // 3. BuildVirtualOverviews explicit with VIRTUAL=YES.
  int anOvrList[] = {2, 4};
  CPLStringList aosOvrOptions;
  aosOvrOptions.AddNameValue("VIRTUAL", "YES");
  EXPECT_EQ(CE_None,
            poDS->BuildOverviews("NEAREST", 2, anOvrList, 0, nullptr, nullptr,
                                 nullptr, aosOvrOptions.List()));

  // 4. Clear virtual overviews with nOverviews = 0.
  EXPECT_EQ(CE_None,
            poDS->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr, nullptr,
                                 nullptr, aosOvrOptions.List()));
}

TEST_F(VrtDatasetTest, CompressedData) {
  constexpr char kSrcTif[] = "/vsimem/vrtdataset_test/comp_src.tif";
  std::vector<std::vector<double>> bands_data = {
      std::vector<double>(100, 1.0),
  };
  CreateGeoTiff(kSrcTif, 10, 10, 1, GDT_Byte, std::nullopt, nullptr, 0.0,
                bands_data);

  constexpr char kVrtXml[] = R"xml(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/comp_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
      <DstRect xOff="0" yOff="0" xSize="10" ySize="10"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  auto poDS = VRTDataset::OpenXML(kVrtXml, nullptr, GA_ReadOnly);
  ASSERT_NE(nullptr, poDS);

  int anBands[] = {1};
  const CPLStringList aosFormats(
      poDS->GetCompressionFormats(0, 0, 10, 10, 1, anBands), TRUE);
  void* pBuffer = nullptr;
  size_t nBufferSize = 0;
  char* pszDetailedFormat = nullptr;
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    poDS->ReadCompressedData("UNKNOWN", 0, 0, 10, 10, 1, anBands, &pBuffer,
                             &nBufferSize, &pszDetailedFormat);
    CPLErrorReset();
  }
  std::unique_ptr<void, decltype(&VSIFree)> buffer_guard(pBuffer, VSIFree);
  std::unique_ptr<char, CplFreeDeleter> detailed_format(pszDetailedFormat);
}

TEST_F(VrtDatasetTest, VRTMapSharedResources) {
  VRTMapSharedResources mapRes;
  EXPECT_EQ(nullptr, mapRes.Get("not_found"));

  VRTDataset ds(5, 5);
  mapRes.Insert("ds1", &ds);
  EXPECT_EQ(&ds, mapRes.Get("ds1"));

  mapRes.InitMutex();
  mapRes.Insert("ds2", &ds);
  EXPECT_EQ(&ds, mapRes.Get("ds2"));
}

TEST_F(VrtDatasetTest, SerializeComprehensive) {
  VRTDataset ds(20, 20);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetCoordinateEpoch(2021.5);
  EXPECT_EQ(CE_None, ds.SetSpatialRef(&srs));

  GDALGeoTransform gtCoeffs(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, ds.SetGeoTransform(gtCoeffs));

  GDAL_GCP gcps[2];
  GDALInitGCPs(2, gcps);
  absl::Cleanup deinit_gcps = [&gcps] { GDALDeinitGCPs(2, gcps); };
  gcps[0].dfGCPPixel = 0;
  gcps[0].dfGCPLine = 0;
  gcps[0].dfGCPX = 10;
  gcps[0].dfGCPY = 20;
  gcps[1].dfGCPPixel = 10;
  gcps[1].dfGCPLine = 10;
  gcps[1].dfGCPX = 20;
  gcps[1].dfGCPY = 10;
  EXPECT_EQ(CE_None, ds.SetGCPs(2, gcps, &srs));

  EXPECT_EQ(CE_None, ds.CreateMaskBand(0));
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));

  CPLStringList aosOptions;
  aosOptions.AddNameValue("VIRTUAL", "YES");
  int anOvr[] = {2, 4};
  EXPECT_EQ(CE_None, ds.BuildOverviews("NEAREST", 2, anOvr, 0, nullptr, nullptr,
                                       nullptr, aosOptions.List()));

  CPLXMLTreeCloser psTree(ds.SerializeToXML(nullptr));
  ASSERT_NE(nullptr, psTree.get());
  std::unique_ptr<char, CplFreeDeleter> pszXML(
      CPLSerializeXMLTree(psTree.get()));
  ASSERT_NE(nullptr, pszXML);
  const std::string_view osXML(pszXML.get());

  EXPECT_THAT(osXML, HasSubstr("coordinateEpoch=\"2021.5\""));
  EXPECT_THAT(osXML, HasSubstr("<GeoTransform>"));
  EXPECT_THAT(osXML, HasSubstr("<GCPList"));
  EXPECT_THAT(osXML, HasSubstr("<MaskBand>"));
  EXPECT_THAT(
      osXML,
      HasSubstr("<OverviewList resampling=\"NEAREST\">2 4</OverviewList>"));

  VRTFlushCache(GDALDataset::ToHandle(&ds));

  CPLXMLTreeCloser psTree2(
      VRTSerializeToXML(GDALDataset::ToHandle(&ds), nullptr));
  ASSERT_NE(nullptr, psTree2.get());
}

TEST_F(VrtDatasetTest, OverviewListInXMLAndRootGroup) {
  // 1. Valid OverviewList.
  {
    constexpr char kVrtOvrFile[] = "/vsimem/vrtdataset_test/vrt_ovr.vrt";
    constexpr char kVrtOvr[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <OverviewList resampling="bilinear">2 4</OverviewList>
  <VRTRasterBand dataType="Byte" band="1"/>
</VRTDataset>)xml";
    VsiMemTempWrapper vrt_mem(kVrtOvrFile, kVrtOvr);

    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(kVrtOvrFile, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    ASSERT_NE(nullptr, poDS->GetRasterBand(1));
    EXPECT_EQ(2, poDS->GetRasterBand(1)->GetOverviewCount());
  }

  // 2. Invalid overview factor <= 1.
  {
    constexpr char kVrtBadOvrFile[] = "/vsimem/vrtdataset_test/vrt_bad_ovr.vrt";
    constexpr char kVrtBadOvr[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <OverviewList>1</OverviewList>
  <VRTRasterBand dataType="Byte" band="1"/>
</VRTDataset>)xml";
    VsiMemTempWrapper vrt_mem(kVrtBadOvrFile, kVrtBadOvr);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetUniquePtr poBadDS(GDALDataset::FromHandle(
        GDALOpenEx(kVrtBadOvrFile, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poBadDS);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Root Group XMLInit.
  {
    constexpr char kVrtGroupFile[] = "/vsimem/vrtdataset_test/vrt_group.vrt";
    constexpr char kVrtGroup[] = R"xml(
<VRTDataset rasterXSize="5" rasterYSize="5">
  <VRTRasterBand dataType="Byte" band="1"/>
  <Group name="/">
    <Dimension name="dim0" size="5"/>
  </Group>
</VRTDataset>)xml";
    VsiMemTempWrapper vrt_mem(kVrtGroupFile, kVrtGroup);

    GDALDatasetUniquePtr poGroupDS(GDALDataset::FromHandle(
        GDALOpenEx(kVrtGroupFile, GDAL_OF_UPDATE | GDAL_OF_MULTIDIM_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poGroupDS);
    auto poRootGroup = poGroupDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);
    EXPECT_EQ("/", poRootGroup->GetName());
  }

  // 4. Root Group invalid name.
  {
    constexpr char kVrtBadGroup[] = R"xml(
<VRTDataset>
  <Group name="bad">
  </Group>
</VRTDataset>)xml";
    CPLErrorReset();
    WithQuietHandler quiet;
    auto poBadGroupDS = VRTDataset::OpenXML(kVrtBadGroup, nullptr, GA_ReadOnly);
    EXPECT_EQ(nullptr, poBadGroupDS);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtDatasetTest, AddBandAdvanced) {
  VRTDataset ds(10, 10);

  // 1. VRTDerivedRasterBand with pixel function options.
  {
    CPLStringList aosOptions;
    aosOptions.AddNameValue("subclass", "VRTDerivedRasterBand");
    aosOptions.AddNameValue("PixelFunctionType", "add");
    aosOptions.AddNameValue("PixelFunctionLanguage", "C");
    aosOptions.AddNameValue("SkipNonContributingSources", "YES");
    aosOptions.AddNameValue("_PIXELFN_ARG_k1", "v1");
    aosOptions.AddNameValue("SourceTransferType", "Float32");
    EXPECT_EQ(CE_None, ds.AddBand(GDT_Byte, aosOptions.List()));
  }

  // 2. Invalid SourceTransferType.
  {
    CPLStringList aosOptions;
    aosOptions.AddNameValue("subclass", "VRTDerivedRasterBand");
    aosOptions.AddNameValue("SourceTransferType", "InvalidType");
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ds.AddBand(GDT_Byte, aosOptions.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. AddFuncSource option.
  {
    CPLStringList aosOptions;
    aosOptions.AddNameValue("AddFuncSource", "0x0,0x0,0.0");
    EXPECT_EQ(CE_None, ds.AddBand(GDT_Byte, aosOptions.List()));
  }

  // 4. VRTRawRasterBand: missing SourceFilename.
  {
    CPLStringList aosOptions;
    aosOptions.AddNameValue("subclass", "VRTRawRasterBand");
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ds.AddBand(GDT_Byte, aosOptions.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 5. VRTRawRasterBand: integer overflow.
  {
    CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                     false);
    CPLConfigOptionSetter allow_any_source(
        "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL", false);
    CPLStringList aosOptions;
    aosOptions.AddNameValue("subclass", "VRTRawRasterBand");
    aosOptions.AddNameValue("SourceFilename",
                            "/vsimem/vrtdataset_test/raw.bin");
    aosOptions.AddNameValue("PixelOffset", "2000000000");
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ds.AddBand(GDT_Byte, aosOptions.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 6. VRTRawRasterBand: valid with omitted LineOffset and relativeToVRT.
  constexpr char kRawRelBin[] = "/vsimem/vrtdataset_test/raw_rel.bin";
  CreateMemFile(kRawRelBin, 100);
  {
    ds.SetDescription("/vsimem/vrtdataset_test/test.vrt");
    CPLStringList aosOptions;
    aosOptions.AddNameValue("subclass", "VRTRawRasterBand");
    aosOptions.AddNameValue("SourceFilename", "raw_rel.bin");
    aosOptions.AddNameValue("relativeToVRT", "1");
    aosOptions.AddNameValue("ByteOrder", "MSB");
    EXPECT_EQ(CE_None, ds.AddBand(GDT_Byte, aosOptions.List()));
  }
}

TEST_F(VrtDatasetTest, GetSingleSimpleSourceAndAdviseRead) {
  constexpr char kSrcSingle[] = "/vsimem/vrtdataset_test/src_single.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kSrcSingle, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
  }

  constexpr char kVrtFull[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/src_single.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  auto poVRT = VRTDataset::OpenXML(kVrtFull, nullptr, GA_ReadOnly);
  ASSERT_NE(nullptr, poVRT);
  EXPECT_NE(nullptr, poVRT->GetSingleSimpleSource());

  int anBands[] = {1};
  EXPECT_EQ(CE_None, poVRT->AdviseRead(0, 0, 20, 20, 20, 20, GDT_Byte, 1,
                                       anBands, nullptr));

  // Partial window: GetSingleSimpleSource returns nullptr.
  constexpr char kVrtPartial[] = R"xml(
<VRTDataset rasterXSize="20" rasterYSize="20">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/src_single.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
      <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";
  auto poPartialVRT = VRTDataset::OpenXML(kVrtPartial, nullptr, GA_ReadOnly);
  ASSERT_NE(nullptr, poPartialVRT);
  EXPECT_EQ(nullptr, poPartialVRT->GetSingleSimpleSource());
}

TEST_F(VrtDatasetTest, MultiThreadedRasterIO) {
  constexpr char kTile1[] = "/vsimem/vrtdataset_test/tile1.tif";
  constexpr char kTile2[] = "/vsimem/vrtdataset_test/tile2.tif";

  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  {
    GDALDatasetUniquePtr poTile1(
        poDrv->Create(kTile1, 500, 1000, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poTile1);
    std::vector<GByte> line(500, 10);
    for (int b = 1; b <= 2; ++b) {
      for (int y = 0; y < 1000; ++y) {
        ASSERT_EQ(CE_None, poTile1->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, y, 500, 1, line.data(), 500, 1,
                               GDT_Byte, 0, 0));
      }
    }
  }

  {
    GDALDatasetUniquePtr poTile2(
        poDrv->Create(kTile2, 500, 1000, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poTile2);
    std::vector<GByte> line(500, 20);
    for (int b = 1; b <= 2; ++b) {
      for (int y = 0; y < 1000; ++y) {
        ASSERT_EQ(CE_None, poTile2->GetRasterBand(b)->RasterIO(
                               GF_Write, 0, y, 500, 1, line.data(), 500, 1,
                               GDT_Byte, 0, 0));
      }
    }
  }

  constexpr char kVrtMosaic[] = R"xml(
<VRTDataset rasterXSize="1000" rasterYSize="1000">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/tile1.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
      <DstRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
    </SimpleSource>
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/tile2.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
      <DstRect xOff="500" yOff="0" xSize="500" ySize="1000"/>
    </SimpleSource>
  </VRTRasterBand>
  <VRTRasterBand dataType="Byte" band="2">
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/tile1.tif</SourceFilename>
      <SourceBand>2</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
      <DstRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
    </SimpleSource>
    <SimpleSource>
      <SourceFilename>/vsimem/vrtdataset_test/tile2.tif</SourceFilename>
      <SourceBand>2</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="500" ySize="1000"/>
      <DstRect xOff="500" yOff="0" xSize="500" ySize="1000"/>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>)xml";

  auto poDS = VRTDataset::OpenXML(kVrtMosaic, nullptr, GA_ReadOnly);
  ASSERT_NE(nullptr, poDS);

  std::vector<GByte> outBuf(1000 * 1000 * 2, 0);
  int anBands[] = {1, 2};
#if !defined(THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__)
  // TODO(schwehr): Re-enable multi-threaded TSan once upstream
  // GDALRasterBlock::TakeLock()/Touch() block-cache race is fixed.
  {
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "2", false);
    GDALRasterIOExtraArg sMTArg;
    INIT_RASTERIO_EXTRA_ARG(sMTArg);
    sMTArg.bFloatingPointWindowValidity = TRUE;
    sMTArg.dfXOff = 0.0;
    sMTArg.dfYOff = 0.0;
    sMTArg.dfXSize = 1000.0;
    sMTArg.dfYSize = 1000.0;
    sMTArg.pfnProgress = GDALDummyProgress;
    EXPECT_EQ(CE_None, poDS->RasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(),
                                      1000, 1000, GDT_Byte, 2, anBands, 2,
                                      1000 * 2, 1, &sMTArg));
    EXPECT_STREQ("1", poDS->GetMetadataItem("MULTI_THREADED_RASTERIO_LAST_USED",
                                            "__DEBUG__"));
  }
#endif

  // Single-threaded path with progress callback.
  {
    CPLConfigOptionSetter vrt_threads("VRT_NUM_THREADS", "1", false);
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    sExtraArg.pfnProgress = GDALDummyProgress;
    EXPECT_EQ(CE_None, poDS->RasterIO(GF_Read, 0, 0, 1000, 1000, outBuf.data(),
                                      1000, 1000, GDT_Byte, 2, anBands, 2,
                                      1000 * 2, 1, &sExtraArg));
  }

  // Multi-band downsampled read with non-nearest resampling algorithm routes
  // through GDALDataset::BandBasedRasterIO().
  {
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    sExtraArg.eResampleAlg = GRIORA_Bilinear;
    std::vector<GByte> smallBuf(100 * 100 * 2, 0);
    EXPECT_EQ(CE_None, poDS->RasterIO(GF_Read, 0, 0, 1000, 1000,
                                      smallBuf.data(), 100, 100, GDT_Byte, 2,
                                      anBands, 2, 100 * 2, 1, &sExtraArg));
  }

  // Single-band dataset reading its only band forwards to
  // VRTSourcedRasterBand::IRasterIO().
  {
    VRTDataset ds1Band(10, 10);
    ASSERT_EQ(CE_None, ds1Band.AddBand(GDT_Byte, nullptr));
    std::vector<GByte> buf1Band(100, 0);
    int anBand1[] = {1};
    EXPECT_EQ(CE_None,
              ds1Band.RasterIO(GF_Read, 0, 0, 10, 10, buf1Band.data(), 10, 10,
                               GDT_Byte, 1, anBand1, 0, 0, 0, nullptr));
  }

  // Bands without any source fall back to GDALDataset::IRasterIO().
  {
    VRTDataset dsIncomp(10, 10);
    ASSERT_EQ(CE_None, dsIncomp.AddBand(GDT_Byte, nullptr));
    ASSERT_EQ(CE_None, dsIncomp.AddBand(GDT_Byte, nullptr));
    std::vector<GByte> bufIncomp(200, 0);
    int anBandsIncomp[] = {1, 2};
    GDALRasterIOExtraArg sIncompArg;
    INIT_RASTERIO_EXTRA_ARG(sIncompArg);
    sIncompArg.eResampleAlg = GRIORA_NearestNeighbour;
    EXPECT_EQ(CE_None, dsIncomp.RasterIO(GF_Read, 0, 0, 10, 10,
                                         bufIncomp.data(), 10, 10, GDT_Byte, 2,
                                         anBandsIncomp, 0, 0, 0, &sIncompArg));
  }
}

TEST_F(VrtDatasetTest, BuildVirtualOverviewsAndCheckBand) {
  constexpr char kSrcTif[] = "/vsimem/vrtdataset_test/ovr_check_src.tif";
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  int anOvr[] = {2, 4};
  {
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kSrcTif, 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    EXPECT_EQ(CE_None, poSrcDS->BuildOverviews("NEAREST", 2, anOvr, 0, nullptr,
                                               nullptr, nullptr));
  }

  constexpr char kVrtFile[] = "/vsimem/vrtdataset_test/test_ovr.vrt";
  constexpr char kVrtText[] = R"xml(
<VRTDataset rasterXSize="100" rasterYSize="100">
  <VRTRasterBand dataType="Byte" band="1">
    <ComplexSource>
      <SourceFilename>/vsimem/vrtdataset_test/ovr_check_src.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="100" ySize="100"/>
      <DstRect xOff="0" yOff="0" xSize="100" ySize="100"/>
      <ScaleOffset>1.0</ScaleOffset>
    </ComplexSource>
  </VRTRasterBand>
  <MaskBand>
    <VRTRasterBand dataType="Byte">
      <SimpleSource>
        <SourceFilename>/vsimem/vrtdataset_test/ovr_check_src.tif</SourceFilename>
        <SourceBand>1</SourceBand>
        <SrcRect xOff="0" yOff="0" xSize="100" ySize="100"/>
        <DstRect xOff="0" yOff="0" xSize="100" ySize="100"/>
      </SimpleSource>
    </VRTRasterBand>
  </MaskBand>
</VRTDataset>)xml";
  VsiMemTempWrapper vrt_mem(kVrtFile, kVrtText);

  GDALDatasetUniquePtr poVRT(GDALDataset::FromHandle(GDALOpenEx(
      kVrtFile, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poVRT);
  GDALRasterBand* poBand = poVRT->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);

  EXPECT_EQ(2, poBand->GetOverviewCount());
  EXPECT_NE(nullptr, poBand->GetOverview(0));
  EXPECT_NE(nullptr, poBand->GetOverview(1));

  // Non-virtual BuildOverviews while implicit virtual overviews exist.
  poVRT->SetDescription(kVrtFile);
  EXPECT_EQ(CE_None, poVRT->BuildOverviews("NEAREST", 2, anOvr, 0, nullptr,
                                           nullptr, nullptr, nullptr));

  // Clear virtual overviews using BuildOverviews with VIRTUAL=YES and
  // nOverviews=0.
  CPLStringList aosOptions;
  aosOptions.AddNameValue("VIRTUAL", "YES");
  EXPECT_EQ(CE_None,
            poVRT->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr, nullptr,
                                  nullptr, aosOptions.List()));

  // UnsetPreservedRelativeFilenames and ClearStatistics.
  cpl::down_cast<VRTDataset*>(poVRT.get())->UnsetPreservedRelativeFilenames();
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("STATISTICS_MINIMUM", "10"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("OTHER_ITEM", "foo"));
  cpl::down_cast<VRTDataset*>(poVRT.get())->ClearStatistics();
  EXPECT_EQ(nullptr, poBand->GetMetadataItem("STATISTICS_MINIMUM"));
  EXPECT_STREQ("foo", poBand->GetMetadataItem("OTHER_ITEM"));
}

TEST_F(VrtDatasetTest, OpenVRTProtocolOptions) {
  constexpr char kSrcTif[] = "/vsimem/vrtdataset_test/proto_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kSrcTif, 20, 20, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    GDALGeoTransform gtCoeffs(0.0, 1.0, 0.0, 20.0, 0.0, -1.0);
    EXPECT_EQ(CE_None, poSrcDS->SetGeoTransform(gtCoeffs));
    EXPECT_EQ(CE_None, poSrcDS->GetRasterBand(1)->CreateMaskBand(0));
    EXPECT_EQ(CE_None, poSrcDS->SetMetadataItem("SUBDATASET_1_NAME", kSrcTif,
                                                "SUBDATASETS"));
    EXPECT_EQ(CE_None, poSrcDS->SetMetadataItem("SUBDATASET_1_DESC",
                                                "Subdataset 1", "SUBDATASETS"));
    EXPECT_EQ(CE_None, poSrcDS->SetMetadataItem(
                           "SUBDATASET_2_NAME",
                           "GTIFF_DIR:1:/vsimem/vrtdataset_test/proto_src.tif",
                           "SUBDATASETS"));
    EXPECT_EQ(CE_None, poSrcDS->SetMetadataItem("SUBDATASET_2_DESC",
                                                "GTIFF Dir 1", "SUBDATASETS"));
  }

  // 1. Initial options & error cases.
  constexpr std::string_view kInvalidInitialQueries[] = {
      "invalid_opt",
      "if=GTiff&if=MEM",
      "oo=FOO=1&oo=BAR=2",
      "unknown_opt=foo",
  };
  for (std::string_view query : kInvalidInitialQueries) {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?" + std::string(query);
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS) << query;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << query;
    CPLErrorReset();
  }

  // 2. Multidim transpose.
  constexpr char kMdTransFile[] = "/vsimem/vrtdataset_test/md_trans.vrt";
  constexpr char kMdimVrt[] = R"xml(
<VRTDataset rasterXSize="5" rasterYSize="4">
  <VRTRasterBand dataType="Byte" band="1"/>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="5"/>
    <Array name="var">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
    </Array>
  </Group>
</VRTDataset>)xml";
  VsiMemTempWrapper md_mem(kMdTransFile, kMdimVrt);

  {
    const std::string uri =
        std::string("vrt://") + kMdTransFile + "?transpose=/var:1,0";
    GDALDatasetUniquePtr poTransDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_MULTIDIM_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poTransDS);
    EXPECT_EQ(5, poTransDS->GetRasterXSize());
    EXPECT_EQ(4, poTransDS->GetRasterYSize());
  }

  constexpr std::string_view kBadTransposeQueries[] = {
      "transpose=/var",
      "transpose=/var:1",
      "transpose=/bad:1,0",
  };
  for (std::string_view query : kBadTransposeQueries) {
    const std::string uri =
        std::string("vrt://") + kMdTransFile + "?" + std::string(query);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_MULTIDIM_RASTER,
                   nullptr, nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS) << query;
  }

  // 3. Subdataset options & errors.
  {
    const std::string uri = std::string("vrt://") + kSrcTif + "?sd=1";
    GDALDatasetUniquePtr poSDDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poSDDS);
  }
  {
    const std::string uri = std::string("vrt://") + kSrcTif + "?sd_name=1";
    GDALDatasetUniquePtr poSDNameDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poSDNameDS);
  }

  constexpr std::string_view kBadSubdatasetQueries[] = {
      "sd_name=nonexistent",
      "sd=0",
      "sd=5",
      "sd=1&transpose=/var:1,0",
      "sd=1&sd_name=foo",
      "sd_name=foo&transpose=/var:1,0",
  };
  for (std::string_view query : kBadSubdatasetQueries) {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?" + std::string(query);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS) << query;
  }

  // 4. Comprehensive protocol arguments.
  constexpr std::string_view kValidQueries[] = {
      "bands=1,mask&a_nodata=0&a_srs=EPSG:4326&a_scale=2.0&a_offset=1.0&"
      "ot=Byte&scale=true&exponent=1.5&outsize=10,10&r=bilinear&"
      "srcwin=0,0,10,10&a_gt=0,1,0,0,0,1&a_coord_epoch=2021.5&epo=true&eco="
      "true",
      "unscale=true&tr=2,2",
      "a_ullr=0,20,20,0",
      "nogcp=true&projwin=0,20,20,0&projwin_srs=EPSG:4326",
      "gcp=0,0,10,20,0&scale_1=0,255,0,1",
      "ovr=0",
  };
  for (std::string_view query : kValidQueries) {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?" + std::string(query);
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS) << query;
  }

  // 5. Error branches for query options.
  constexpr std::string_view kBadOptionQueries[] = {
      "expand=rgb", "bands=99",    "a_ullr=0,0", "gcp=0,0",    "scale=1,2,3",
      "outsize=5",  "projwin=0,0", "tr=1",       "srcwin=0,0", "a_gt=0,0",
  };
  for (std::string_view query : kBadOptionQueries) {
    const std::string uri =
        std::string("vrt://") + kSrcTif + "?" + std::string(query);
    ScopedErrorCapturer errors;
    GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(
        GDALOpenEx(uri.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS) << query;
    EXPECT_TRUE(errors.has_failure()) << query;
  }
}

TEST_F(VrtDatasetTest, CreateMaskBandAndDelete) {
  VRTDataset ds(10, 10);
  EXPECT_EQ(CE_None, ds.CreateMaskBand(0));
  EXPECT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds.CreateMaskBand(0));
  }

  EXPECT_EQ(CE_Failure,
            VRTDataset::Delete("/vsimem/vrtdataset_test/nonexistent.vrt"));
  {
    constexpr char kTempVrt[] = "/vsimem/vrtdataset_test/to_delete.vrt";
    constexpr std::string_view kVrtXml =
        "<VRTDataset rasterXSize='1' rasterYSize='1'/>";
    {
      VSILFILE* fp = VSIFOpenL(kTempVrt, "wb");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup fp_closer = [fp] { ASSERT_EQ(0, VSIFCloseL(fp)); };
      ASSERT_EQ(kVrtXml.size(),
                VSIFWriteL(kVrtXml.data(), 1, kVrtXml.size(), fp));
    }
    EXPECT_EQ(CE_None, VRTDataset::Delete(kTempVrt));
  }
}

TEST_F(VrtDatasetTest, CheckCompatibleForDatasetIONoSrcDSName) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poDrv);
  GDALDatasetUniquePtr poMemDS(poDrv->Create("", 10, 10, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poMemDS);

  VRTDataset ds(10, 10);
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));
  ASSERT_EQ(CE_None, ds.AddBand(GDT_Byte, nullptr));

  auto poBand1 = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(1));
  auto poBand2 = cpl::down_cast<VRTSourcedRasterBand*>(ds.GetRasterBand(2));

  EXPECT_EQ(CE_None, poBand1->AddSimpleSource(poMemDS->GetRasterBand(1)));
  EXPECT_EQ(CE_None, poBand2->AddSimpleSource(poMemDS->GetRasterBand(2)));

  EXPECT_STREQ(
      "1", ds.GetMetadataItem("CheckCompatibleForDatasetIO()", "__DEBUG__"));
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and driver-registration edge
// cases in VRTDataset in vrtdataset.cpp.
