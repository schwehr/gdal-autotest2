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
// Tests for GPX driver (ogrgpxdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpx/ogrgpxdriver.cpp
//   https://gdal.org/drivers/vector/gpx.html

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(OgrGpxDriverTest, RegisterDriverAndCheckMetadata) {
  // 1. Initial or repeated registration.
  RegisterOGRGPX();
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_NE(nullptr, driver);

  // 2. Repeat call to cover early-exit when already registered.
  RegisterOGRGPX();
  EXPECT_EQ(driver, GetGDALDriverManager()->GetDriverByName("GPX"));

  // 3. Verify driver metadata items.
  EXPECT_STREQ("GPX", driver->GetDescription());
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_CREATE_LAYER));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_DELETE_LAYER));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_CREATE_FIELD));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES));
  EXPECT_STREQ("GPX", driver->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("gpx", driver->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("drivers/vector/gpx.html",
               driver->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("OGRSQL SQLITE",
               driver->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS));
  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST));
  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST));
  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES",
               driver->GetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS));
}

TEST(OgrGpxDriverTest, Identify) {
  RegisterOGRGPX();
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_NE(nullptr, driver);
  ASSERT_NE(nullptr, driver->pfnIdentify);

  // 1. Non-existent file (fpL == nullptr).
  GDALOpenInfo non_existent_info(
      "/vsimem/ogrgpxdriver_test/identify_non_existent.gpx", GDAL_OF_READONLY,
      nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&non_existent_info));

  // 2. Existing file opened with GA_Update.
  constexpr char kGpxPath[] = "/vsimem/ogrgpxdriver_test/identify_test.gpx";
  VsiMemTempWrapper gpx_file(kGpxPath, "<gpx version=\"1.1\"></gpx>");
  GDALOpenInfo update_info(kGpxPath, GDAL_OF_UPDATE, nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&update_info));

  // 3. Existing non-GPX file.
  constexpr char kNonGpxPath[] = "/vsimem/ogrgpxdriver_test/not_gpx.xml";
  VsiMemTempWrapper non_gpx_file(kNonGpxPath, "<xml><tag>text</tag></xml>");
  GDALOpenInfo non_gpx_info(kNonGpxPath, GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&non_gpx_info));

  // 4. Valid GPX header.
  GDALOpenInfo valid_info(kGpxPath, GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, driver->pfnIdentify(&valid_info));
}

TEST(OgrGpxDriverTest, Open) {
  RegisterOGRGPX();
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_NE(nullptr, driver);
  ASSERT_NE(nullptr, driver->pfnOpen);

  constexpr char kValidGpxPath[] = "/vsimem/ogrgpxdriver_test/open_valid.gpx";
  constexpr char kValidGpx[] =
      "<?xml version=\"1.0\"?>\n"
      "<gpx version=\"1.1\"></gpx>\n";
  VsiMemTempWrapper valid_file(kValidGpxPath, kValidGpx);

  // 1. Open with GA_Update should fail.
  GDALOpenInfo update_info(kValidGpxPath, GDAL_OF_UPDATE, nullptr);
  EXPECT_EQ(nullptr, driver->pfnOpen(&update_info));

  // 2. Open non-GPX file should fail.
  constexpr char kNonGpxPath[] = "/vsimem/ogrgpxdriver_test/open_not_gpx.xml";
  VsiMemTempWrapper non_gpx_file(kNonGpxPath, "<xml><not_gpx/></xml>");
  GDALOpenInfo non_gpx_info(kNonGpxPath, GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(nullptr, driver->pfnOpen(&non_gpx_info));

  // 3. File that passes Identify but fails Open.
  constexpr char kMalformedGpxPath[] =
      "/vsimem/ogrgpxdriver_test/open_malformed.gpx";
  constexpr char kMalformedGpx[] = "<?xml version=\"1.0\"?><gpx><unclosed>";
  VsiMemTempWrapper malformed_file(kMalformedGpxPath, kMalformedGpx);
  GDALOpenInfo malformed_info(kMalformedGpxPath, GDAL_OF_READONLY, nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, driver->pfnOpen(&malformed_info));
  }

  // 4. Successfully open a valid GPX file.
  GDALOpenInfo valid_info(kValidGpxPath, GDAL_OF_READONLY, nullptr);
  GDALDatasetUniquePtr ds(driver->pfnOpen(&valid_info));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(5, ds->GetLayerCount());
}

TEST(OgrGpxDriverTest, Create) {
  RegisterOGRGPX();
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_NE(nullptr, driver);

  constexpr char kCreatedPath[] = "/vsimem/ogrgpxdriver_test/create_test.gpx";
  absl::Cleanup cleanup_created = [kCreatedPath] { VSIUnlink(kCreatedPath); };

  // 1. Successful dataset creation.
  GDALDatasetUniquePtr ds(
      driver->Create(kCreatedPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(kCreatedPath, &stat_buf));

  // 2. Creation fails if file already exists (tested via pfnCreate directly
  // so that GDALDriver::Create does not pre-delete the file via QuietDelete).
  ASSERT_NE(nullptr, driver->pfnCreate);
  {
    WithQuietHandler quiet;
    GDALDatasetUniquePtr fail_ds(
        driver->pfnCreate(kCreatedPath, 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_EQ(nullptr, fail_ds);
  }
}

TEST(OgrGpxDriverTest, Delete) {
  RegisterOGRGPX();
  auto* driver = GetGDALDriverManager()->GetDriverByName("GPX");
  ASSERT_NE(nullptr, driver);

  constexpr char kToDeletePath[] = "/vsimem/ogrgpxdriver_test/to_delete.gpx";
  VSILFILE* fp = VSIFOpenL(kToDeletePath, "wb");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(5, VSIFWriteL("dummy", 1, 5, fp));
  EXPECT_EQ(0, VSIFCloseL(fp));
  absl::Cleanup cleanup = [kToDeletePath] { VSIUnlink(kToDeletePath); };

  // 1. Successful delete.
  EXPECT_EQ(CE_None, driver->Delete(kToDeletePath));

  // 2. Failed delete on non-existent file.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, driver->Delete(kToDeletePath));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Line 101 in ogrgpxdriver.cpp (!GDAL_CHECK_VERSION) is
// unreachable in Google3 build environments.
