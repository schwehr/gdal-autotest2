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

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

constexpr char kWgs84Wkt[] =
    "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS "
    "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0."
    "0174532925199433]]";

TEST(OgrSrsDictTest, ImportFromDict) {
  const std::string kSubDictData =
      std::string("# Sub dict\n9999,") + kWgs84Wkt + "\n";
  VsiMemTempWrapper sub_dict("/vsimem/ogr_srs_dict_test/sub_dict.wkt",
                             kSubDictData);

  const std::string kMainDictData = std::string("# Comment\n") +
                                    "include sub_dict.wkt\n"
                                    "include nonexistent_sub.wkt\n"
                                    "line_without_comma\n"
                                    "4326," +
                                    kWgs84Wkt +
                                    "\n"
                                    "43260,LOCAL_CS[\"Test\"]\n"
                                    "corrupt,NOT_VALID_WKT\n";
  VsiMemTempWrapper main_dict("/vsimem/ogr_srs_dict_test/test_dict.wkt",
                              kMainDictData);

  const std::string kEsriDictData = std::string("4326,") + kWgs84Wkt + "\n";
  VsiMemTempWrapper esri_dict("/vsimem/ogr_srs_dict_test/esri_dict.wkt",
                              kEsriDictData);

  CPLPushFinderLocation("/vsimem/ogr_srs_dict_test");
  auto cleanup_finder = absl::MakeCleanup([] { CPLPopFinderLocation(); });

  OGRSpatialReference srs;

  // File not found
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromDict("nonexistent.wkt", "4326"));

  // Code not found
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS,
            srs.importFromDict("test_dict.wkt", "99999"));

  // Exact code match vs prefix match (looking for 432 should fail, 4326
  // succeed)
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.importFromDict("test_dict.wkt", "432"));
  EXPECT_EQ(OGRERR_NONE, srs.importFromDict("test_dict.wkt", "4326"));

  // Lookup in included dictionary
  EXPECT_EQ(OGRERR_NONE, srs.importFromDict("test_dict.wkt", "9999"));

  // Lookup in dictionary with "esri_" in name (skips morphFromESRI)
  EXPECT_EQ(OGRERR_NONE, srs.importFromDict("esri_dict.wkt", "4326"));

  // Corrupt WKT
  {
    WithQuietHandler quiet_handler;
    EXPECT_NE(OGRERR_NONE, srs.importFromDict("test_dict.wkt", "corrupt"));
  }
}

TEST(OgrSrsDictTest, CapiImportFromDict) {
  EXPECT_EQ(OGRERR_FAILURE,
            OSRImportFromDict(nullptr, "test_dict.wkt", "4326"));

  const std::string kMainDictData = std::string("4326,") + kWgs84Wkt + "\n";
  VsiMemTempWrapper main_dict("/vsimem/ogr_srs_dict_test/test_capi.wkt",
                              kMainDictData);

  CPLPushFinderLocation("/vsimem/ogr_srs_dict_test");
  auto cleanup_finder = absl::MakeCleanup([] { CPLPopFinderLocation(); });

  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  ASSERT_NE(nullptr, hSRS);
  auto cleanup_srs =
      absl::MakeCleanup([hSRS] { OSRDestroySpatialReference(hSRS); });

  EXPECT_EQ(OGRERR_NONE, OSRImportFromDict(hSRS, "test_capi.wkt", "4326"));
}

}  // namespace
}  // namespace autotest2
