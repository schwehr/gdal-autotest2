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
// Tests the OGR VRT driver registration, identify, open, and error paths
// (ogrvrtdriver.cpp).
//
// Caveat: libxml2 (HAVE_LIBXML2) is disabled in google3, so CPLValidateXML
// falls back to its stub and OGRVRTErrorHandler suppresses the
// "missing libxml2 support" message.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/vrt/ogrvrtdriver.cpp
//   https://gdal.org/drivers/vector/vrt.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_vrt.py

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/vrt/ogr_vrt.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrvrtdriver_test";

class OgrVrtDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRVRT();
    poDriver_ = GetGDALDriverManager()->GetDriverByName("OGR_VRT");
    ASSERT_NE(nullptr, poDriver_);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
  }

  GDALDriver* poDriver_ = nullptr;
};

TEST_F(OgrVrtDriverTest, RegisterAgainAndMetadata) {
  // Calling RegisterOGRVRT when already registered hits the early return.
  RegisterOGRVRT();
  EXPECT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("OGR_VRT"));

  // Verify driver metadata entries.
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("VRT - Virtual Datasource",
               poDriver_->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("vrt", poDriver_->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("drivers/vector/vrt.html",
               poDriver_->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_FEATURE_STYLES));
  EXPECT_STREQ("YES",
               poDriver_->GetMetadataItem(GDAL_DCAP_FEATURE_STYLES_READ));
  EXPECT_STREQ("YES",
               poDriver_->GetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES));
  EXPECT_STREQ("YES",
               poDriver_->GetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES));
  EXPECT_STREQ("YES", poDriver_->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES));
  EXPECT_STREQ("OGRSQL SQLITE",
               poDriver_->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS));
  EXPECT_STREQ(
      "WidthPrecision Nullable Unique Default Comment AlternativeName",
      poDriver_->GetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS));
}

TEST_F(OgrVrtDriverTest, Identify) {
  // Non-existent file.
  GDALOpenInfo missing_info("/does_not_exist/missing.vrt",
                            GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(FALSE, poDriver_->pfnIdentify(&missing_info));

  // Non-existent file name that starts with inline XML.
  GDALOpenInfo inline_xml_info("   <OGRVRTDataSource>\n</OGRVRTDataSource>",
                               GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(TRUE, poDriver_->pfnIdentify(&inline_xml_info));

  // Non-existent file name that starts with leading spaces but not XML.
  GDALOpenInfo non_xml_info("   <NotVrtDataSource/>",
                            GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(FALSE, poDriver_->pfnIdentify(&non_xml_info));

  // Valid VRT file on disk/vsimem.
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/valid.vrt";
  constexpr char kVrtContent[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"test\">\n"
      "    <SrcDataSource>@dummy@</SrcDataSource>\n"
      "    <SrcLayer>@dummy@</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";
  VsiMemTempWrapper valid_vrt(kVrtPath, kVrtContent);
  GDALOpenInfo valid_file_info(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(TRUE, poDriver_->pfnIdentify(&valid_file_info));

  // File on disk/vsimem that does not contain <OGRVRTDataSource.
  constexpr char kNonVrtPath[] = "/vsimem/ogrvrtdriver_test/other.txt";
  VsiMemTempWrapper other_file(kNonVrtPath, "not a vrt at all");
  GDALOpenInfo other_file_info(kNonVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(FALSE, poDriver_->pfnIdentify(&other_file_info));

  // Directory: bStatOK is TRUE but fpL is nullptr.
  static constexpr char kDirPath[] = "/vsimem/ogrvrtdriver_test/dir";
  EXPECT_EQ(0, VSIMkdir(kDirPath, 0755));
  absl::Cleanup dir_cleaner = [] { EXPECT_EQ(0, VSIRmdir(kDirPath)); };
  GDALOpenInfo dir_info(kDirPath, GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(FALSE, poDriver_->pfnIdentify(&dir_info));
}

TEST_F(OgrVrtDriverTest, OpenInlineXml) {
  constexpr char kInlineVrt[] =
      "  \t\n <OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"test\">\n"
      "    <SrcDataSource>@dummy@</SrcDataSource>\n"
      "    <SrcLayer>@dummy@</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kInlineVrt, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
  OGRLayer* layer = ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_STREQ("test", layer->GetName());
}

TEST_F(OgrVrtDriverTest, OpenFile) {
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/file.vrt";
  constexpr char kVrtContent[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"dummy\">\n"
      "    <SrcDataSource>@dummy@</SrcDataSource>\n"
      "    <SrcLayer>@dummy@</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";
  VsiMemTempWrapper file_vrt(kVrtPath, kVrtContent);

  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
}

TEST_F(OgrVrtDriverTest, OpenIdentifyFalseFails) {
  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx("/does_not_exist/missing.vrt",
                 GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
  EXPECT_EQ(nullptr, ds);

  GDALOpenInfo missing_info("/does_not_exist/missing.vrt",
                            GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_EQ(nullptr, poDriver_->pfnOpen(&missing_info));
}

TEST_F(OgrVrtDriverTest, OpenStatFails) {
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/stat_fail.vrt";
  constexpr char kVrtContent[] = "<OGRVRTDataSource/>";
  VSILFILE* fp = VSIFOpenL(kVrtPath, "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(1, VSIFWriteL(kVrtContent, sizeof(kVrtContent) - 1, 1, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));

  GDALOpenInfo open_info(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY);
  EXPECT_NE(nullptr, open_info.fpL);

  // Unlink the file so VSIStatL inside pfnOpen fails.
  EXPECT_EQ(0, VSIUnlink(kVrtPath));

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(poDriver_->pfnOpen(&open_info));
  EXPECT_EQ(nullptr, ds);
}

TEST_F(OgrVrtDriverTest, OpenSuspiciouslyLongVrt) {
  constexpr char kHugeVrtPath[] = "/vsimem/ogrvrtdriver_test/huge.vrt";
  // Create a file slightly larger than 10 MB with valid VRT prefix.
  constexpr size_t kSize = 10 * 1024 * 1024 + 1024;
  constexpr std::string_view kPrefix = "<OGRVRTDataSource>\n";
  constexpr std::string_view kSuffix = "</OGRVRTDataSource>";
  std::string huge_content(kPrefix);
  huge_content.append(kSize - kPrefix.size() - kSuffix.size(), ' ');
  huge_content.append(kSuffix);
  VsiMemTempWrapper huge_vrt(kHugeVrtPath, huge_content);

  // 1. Without OGR_VRT_FORCE_LOADING -> rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kHugeVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    EXPECT_EQ(nullptr, ds);
    // Pins upstream typo in ogrvrtdriver.cpp.
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Suscipicously long VRT file"));
  }

  // 2. With OGR_VRT_FORCE_LOADING=YES -> accepted.
  {
    CPLSetConfigOption("OGR_VRT_FORCE_LOADING", "YES");
    absl::Cleanup config_cleaner = [] {
      CPLSetConfigOption("OGR_VRT_FORCE_LOADING", nullptr);
    };

    std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
        GDALOpenEx(kHugeVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(OgrVrtDriverTest, OpenShortReadFails) {
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/short_read.vrt";
  constexpr char kEmptyPath[] = "/vsimem/ogrvrtdriver_test/empty.bin";
  VsiMemTempWrapper valid_vrt(kVrtPath, "<OGRVRTDataSource/>");
  VsiMemTempWrapper empty_file(kEmptyPath, "");

  GDALOpenInfo open_info(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY);
  ASSERT_NE(nullptr, open_info.fpL);

  // Close the original fpL and substitute with a file handle from the empty file,
  // causing VSIFReadL != nLen inside pfnOpen.
  EXPECT_EQ(0, VSIFCloseL(open_info.fpL));
  open_info.fpL = VSIFOpenL(kEmptyPath, "rb");
  ASSERT_NE(nullptr, open_info.fpL);

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(poDriver_->pfnOpen(&open_info));
  EXPECT_EQ(nullptr, ds);
}

TEST_F(OgrVrtDriverTest, OpenMalformedXmlFails) {
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/broken.vrt";
  VsiMemTempWrapper broken_vrt(kVrtPath, "<OGRVRTDataSource><BrokenXml");

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(OgrVrtDriverTest, OpenXmlValidationWithSchema) {
  // Place a dummy ogrvrt.xsd in /vsimem/ and add it to finder locations.
  constexpr char kXsdPath[] = "/vsimem/ogrvrtdriver_test/ogrvrt.xsd";
  constexpr char kXsdContent[] =
      "<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"></xs:schema>";
  VsiMemTempWrapper xsd_file(kXsdPath, kXsdContent);

  CPLPushFinderLocation(kTestDir);
  absl::Cleanup finder_cleaner = [] { CPLPopFinderLocation(); };

  EXPECT_STREQ(kXsdPath, CPLFindFile("gdal", "ogrvrt.xsd"));

  CPLSetConfigOption("GDAL_XML_VALIDATION", "YES");
  absl::Cleanup opt_cleaner = [] {
    CPLSetConfigOption("GDAL_XML_VALIDATION", nullptr);
  };

  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/validated.vrt";
  constexpr char kVrtContent[] =
      "<OGRVRTDataSource>\n"
      "  <OGRVRTLayer name=\"dummy\">\n"
      "    <SrcDataSource>@dummy@</SrcDataSource>\n"
      "    <SrcLayer>@dummy@</SrcLayer>\n"
      "  </OGRVRTLayer>\n"
      "</OGRVRTDataSource>";
  VsiMemTempWrapper file_vrt(kVrtPath, kVrtContent);

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_STREQ("", CPLGetLastErrorMsg());
}

TEST_F(OgrVrtDriverTest, OpenInitializeReturnsFalse) {
  // A file that passes Identify because it contains "<OGRVRTDataSource",
  // but fails poDS->Initialize because the XML root element is not <OGRVRTDataSource>.
  constexpr char kVrtPath[] = "/vsimem/ogrvrtdriver_test/not_root.vrt";
  constexpr char kVrtContent[] =
      "<!-- <OGRVRTDataSource -->\n"
      "<NotOGRVRTDataSource/>";
  VsiMemTempWrapper file_vrt(kVrtPath, kVrtContent);

  WithQuietHandler quiet;
  CPLErrorReset();
  std::unique_ptr<GDALDataset> ds(static_cast<GDALDataset*>(
      GDALOpenEx(kVrtPath, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  EXPECT_EQ(nullptr, ds);
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Did not find the <OGRVRTDataSource> node"));
  // TODO: b/335317901 - Cover CPLValidateXML warning loop in OGRVRTDriverOpen
  // (requires libxml2, disabled in google3).
}

}  // namespace
}  // namespace autotest2
