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
// Tests for Zarr driver core identify and metadata functions
// (zarrdrivercore.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarrdrivercore.cpp

#include "third_party/gdal/frmts/zarr/zarrdrivercore.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/frmts/zarr/vsikerchunk.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(ZarrDriverCoreTest, IdentifyPrefixes) {
  {
    GDALOpenInfo info("ZARR:/path/to/dataset",
                      GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }
  {
    GDALOpenInfo info("ZARR_DUMMY:/path/to/dataset",
                      GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }
  {
    GDALOpenInfo info("/vsikerchunk_json_ref/{...}",
                      GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(-1, ZARRDriverIdentify(&info));
  }
}

TEST(ZarrDriverCoreTest, IdentifyDirectFilenames) {
  for (const char* filename :
       {"data.zarray", "meta.zgroup", ".zmetadata", "zarr.json"}) {
    GDALOpenInfo info(filename, GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }
}

TEST(ZarrDriverCoreTest, IdentifyNonZarrFileAndNonDirectory) {
  ScopedTestDir test_dir("zarr_core");
  const std::string file_path = test_dir.File("test.txt");
  WriteVsiFile(file_path, "plain text content");

  GDALOpenInfo info(file_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
  EXPECT_EQ(FALSE, ZARRDriverIdentify(&info));
}

TEST(ZarrDriverCoreTest, IdentifyDirectoryContents) {
  ScopedTestDir test_dir("zarr_core");

  // Empty directory
  {
    GDALOpenInfo info(test_dir.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(FALSE, ZARRDriverIdentify(&info));
  }

  // Directory with .zarray
  {
    const std::string zarray_path = test_dir.File(".zarray");
    WriteVsiFile(zarray_path, "{}");
    GDALOpenInfo info(test_dir.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
    VSIUnlink(zarray_path.c_str());
  }

  // Directory with .zgroup
  {
    const std::string zgroup_path = test_dir.File(".zgroup");
    WriteVsiFile(zgroup_path, "{}");
    GDALOpenInfo info(test_dir.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
    VSIUnlink(zgroup_path.c_str());
  }

  // Directory with zarr.json
  {
    const std::string zarr_json_path = test_dir.File("zarr.json");
    WriteVsiFile(zarr_json_path, "{}");
    GDALOpenInfo info(test_dir.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
    VSIUnlink(zarr_json_path.c_str());
  }
}

TEST(ZarrDriverCoreTest, IdentifyKerchunkJSONRef) {
  ScopedTestDir test_dir("zarr_core");

  // Valid Kerchunk JSON ref (v1 with .zgroup)
  {
    const std::string kc_path = test_dir.File("refs.json");
    WriteVsiFile(
        kc_path,
        R"json({"version": 1, "refs": {".zgroup": "{\"zarr_format\": 2}"}})json");
    GDALOpenInfo info(kc_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_TRUE(ZARRIsLikelyKerchunkJSONRef(&info));
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }

  // Valid Kerchunk with .zarr extension (v1 with .zarray)
  {
    const std::string kc_zarr_path = test_dir.File("refs.zarr");
    WriteVsiFile(
        kc_zarr_path,
        R"json({"version": 1, "refs": {".zarray": "{\"zarr_format\": 2}"}})json");
    GDALOpenInfo info(kc_zarr_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_TRUE(ZARRIsLikelyKerchunkJSONRef(&info));
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }

  // Valid Kerchunk with whitespace padding
  {
    const std::string kc_ws_path = test_dir.File("padded.json");
    WriteVsiFile(
        kc_ws_path,
        "   \n\r  {\"version\": 1, \n  \"refs\": {\".zgroup\": \"{}\"}}");
    GDALOpenInfo info(kc_ws_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_TRUE(ZARRIsLikelyKerchunkJSONRef(&info));
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }

  // Valid v0 Kerchunk ref
  {
    const std::string kc_v0_path = test_dir.File("v0.json");
    WriteVsiFile(kc_v0_path, R"json({".zgroup": {"zarr_format": 2}})json");
    GDALOpenInfo info(kc_v0_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_TRUE(ZARRIsLikelyKerchunkJSONRef(&info));
    EXPECT_EQ(TRUE, ZARRDriverIdentify(&info));
  }

  // Non-Kerchunk JSON (e.g. GeoJSON)
  {
    const std::string geojson_path = test_dir.File("other.json");
    WriteVsiFile(geojson_path, R"json({"type": "FeatureCollection"})json");
    GDALOpenInfo info(geojson_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER);
    EXPECT_FALSE(ZARRIsLikelyKerchunkJSONRef(&info));
    EXPECT_EQ(FALSE, ZARRDriverIdentify(&info));
  }

  // Update mode (Kerchunk JSON ref only identified in GA_ReadOnly)
  {
    const std::string kc_path = test_dir.File("refs_update.json");
    WriteVsiFile(
        kc_path,
        R"json({"version": 1, "refs": {".zgroup": "{\"zarr_format\": 2}"}})json");
    GDALOpenInfo info(kc_path.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER);
    EXPECT_FALSE(ZARRIsLikelyKerchunkJSONRef(&info));
  }
}

TEST(ZarrDriverCoreTest, DriverSetCommonMetadata) {
  auto driver = std::make_unique<GDALDriver>();
  ZARRDriverSetCommonMetadata(driver.get());

  EXPECT_STREQ("Zarr", driver->GetDescription());
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER));
  EXPECT_STREQ("Zarr", driver->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("zarr", driver->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DMD_SUBDATASETS));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_CREATE_SUBDATASETS));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_OPEN));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_CREATE));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_CREATECOPY));
  EXPECT_STREQ("YES",
               driver->GetMetadataItem(GDAL_DCAP_CREATE_MULTIDIMENSIONAL));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_UPDATE));

  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST));
  EXPECT_NE(nullptr, driver->GetMetadataItem(
                         GDAL_DMD_MULTIDIM_DATASET_CREATIONOPTIONLIST));
  EXPECT_NE(nullptr, driver->GetMetadataItem(GDAL_DMD_UPDATE_ITEMS));
  EXPECT_EQ(ZARRDriverIdentify, driver->pfnIdentify);
}

}  // namespace
}  // namespace autotest2
