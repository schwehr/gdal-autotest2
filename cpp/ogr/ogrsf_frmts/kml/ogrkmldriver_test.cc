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
// Tests OGRKMLDriver (ogrkmldriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/kml/ogrkmldriver.cpp

#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/ogr_kml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(OgrKmlDriverTest, RegistrationAndMetadata) {
  // Ensure driver is registered
  RegisterOGRKML();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("KML");
  ASSERT_NE(nullptr, poDriver);

  // Calling RegisterOGRKML() again should be a no-op (early return)
  RegisterOGRKML();
  EXPECT_EQ(poDriver, GetGDALDriverManager()->GetDriverByName("KML"));

  // Check driver metadata
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_CREATE_LAYER));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_CREATE_FIELD));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES));
  EXPECT_STREQ("Keyhole Markup Language (KML)",
               poDriver->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("kml", poDriver->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("drivers/vector/kml.html",
               poDriver->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("OGRSQL SQLITE",
               poDriver->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("Integer Real String",
               poDriver->GetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_FEATURE_STYLES));
  EXPECT_STREQ("YES",
               poDriver->GetMetadataItem(GDAL_DCAP_FEATURE_STYLES_WRITE));
  EXPECT_STREQ("YES",
               poDriver->GetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS));
}

TEST(OgrKmlDriverTest, IdentifyAndOpen) {
  RegisterOGRKML();
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("KML");
  ASSERT_NE(nullptr, poDriver);

  // 1. Non-existent file (fpL == nullptr)
  {
    GDALOpenInfo oOpenInfo("/vsimem/nonexistent_driver_test.kml", GA_ReadOnly);
    EXPECT_EQ(FALSE, poDriver->pfnIdentify(&oOpenInfo));
    EXPECT_EQ(nullptr, poDriver->pfnOpen(&oOpenInfo));
  }

  // 2. Non-KML file (<html ...)
  {
    constexpr char kHtml[] = "<html><body>Not KML</body></html>";
    VsiMemTempWrapper f("/vsimem/ogrkmldriver_test/not_kml.html", kHtml);
    GDALOpenInfo oOpenInfo("/vsimem/ogrkmldriver_test/not_kml.html",
                           GA_ReadOnly);
    EXPECT_EQ(FALSE, poDriver->pfnIdentify(&oOpenInfo));
    EXPECT_EQ(nullptr, poDriver->pfnOpen(&oOpenInfo));
  }

  // 3. Open with GA_Update -> returns nullptr
  {
    constexpr char kValidKml[] =
        "<kml "
        "xmlns=\"http://www.opengis.net/kml/2.2\"><Document></Document></kml>";
    VsiMemTempWrapper f("/vsimem/ogrkmldriver_test/valid.kml", kValidKml);
    GDALOpenInfo oOpenInfo("/vsimem/ogrkmldriver_test/valid.kml", GA_Update);
    EXPECT_TRUE(poDriver->pfnIdentify(&oOpenInfo));
    EXPECT_EQ(nullptr, poDriver->pfnOpen(&oOpenInfo));
  }

  // 4. File starting with <kml:kml
  {
    constexpr char kPrefixKml[] =
        "<kml:kml "
        "xmlns:kml=\"http://www.opengis.net/kml/2.2\"><kml:Document></"
        "kml:Document></kml:kml>";
    VsiMemTempWrapper f("/vsimem/ogrkmldriver_test/prefix.kml", kPrefixKml);
    GDALOpenInfo oOpenInfo("/vsimem/ogrkmldriver_test/prefix.kml", GA_ReadOnly);
    EXPECT_TRUE(poDriver->pfnIdentify(&oOpenInfo));
    GDALDatasetUniquePtr poDS(poDriver->pfnOpen(&oOpenInfo));
    ASSERT_NE(nullptr, poDS);
  }

  // 5. File that passes identify (<kml) but fails Open (broken XML)
  {
    constexpr char kBrokenKml[] =
        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><unclosed>";
    VsiMemTempWrapper f("/vsimem/ogrkmldriver_test/broken.kml", kBrokenKml);
    GDALOpenInfo oOpenInfo("/vsimem/ogrkmldriver_test/broken.kml", GA_ReadOnly);
    EXPECT_TRUE(poDriver->pfnIdentify(&oOpenInfo));
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, poDriver->pfnOpen(&oOpenInfo));
    CPLErrorReset();
  }

  // 6. Valid KML via GDALOpenEx
  {
    constexpr char kValidKml[] =
        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>"
        "<Folder><name>TestFolder</name></Folder>"
        "</Document></kml>";
    VsiMemTempWrapper f("/vsimem/ogrkmldriver_test/open_ex.kml", kValidKml);
    GDALDatasetUniquePtr poDS(static_cast<GDALDataset*>(GDALOpenEx(
        "/vsimem/ogrkmldriver_test/open_ex.kml",
        GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(poDriver, poDS->GetDriver());
  }
}

TEST(OgrKmlDriverTest, Create) {
  RegisterOGRKML();
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("KML");
  ASSERT_NE(nullptr, poDriver);

  // 1. Create with invalid path -> returns nullptr
  {
    const std::string non_existent =
        (std::filesystem::path(::testing::TempDir()) / "does_not_exist" /
         "out.kml")
            .string();
    WithQuietHandler quiet_handler;
    GDALDatasetUniquePtr poDS(poDriver->pfnCreate(non_existent.c_str(), 0, 0, 0,
                                                  GDT_Unknown, nullptr));
    EXPECT_EQ(nullptr, poDS);
    CPLErrorReset();
  }

  // 2. Create valid dataset
  {
    constexpr char kOut[] = "/vsimem/ogrkmldriver_test/create_valid.kml";
    absl::Cleanup unlink_file = [&] { VSIUnlink(kOut); };

    GDALDatasetUniquePtr poDS(
        poDriver->pfnCreate(kOut, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(0, poDS->GetLayerCount());
    poDS.reset();  // Flush and close file before stat

    // Verify file was written
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(kOut, &sStat));
    EXPECT_GT(sStat.st_size, 0);
  }
}

}  // namespace
}  // namespace autotest2
