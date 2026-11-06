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
// Tests for the OSM vector driver registration and driver-level Open/Identify
// (ogrosmdriver.cpp).
//
// google3 build caveats:
// - Default osmconf.ini is located via GoogleGdalInitSupportFiles().
// - CPLDebug messages are compiled out (WITHOUT_CPLDEBUG).
// - HAVE_GEOS and HAVE_EXPAT are enabled.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/osm/ogrosmdriver.cpp
//   https://gdal.org/drivers/vector/osm.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_osm.py

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kEmptyOsmXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<osm version=\"0.6\"></osm>\n";

class OgrosmDriverTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGROSM(); }
};

TEST_F(OgrosmDriverTest, DriverRegistrationAndMetadata) {
  // Calling RegisterOGROSM when already registered exercises the
  // early-return branch.
  RegisterOGROSM();

  GDALDriverManager* driver_manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, driver_manager);
  GDALDriver* driver = driver_manager->GetDriverByName("OSM");
  ASSERT_NE(nullptr, driver);

  // Deregister and re-register to exercise the driver creation and metadata
  // initialization code path.
  driver_manager->DeregisterDriver(driver);
  delete driver;

  EXPECT_EQ(nullptr, driver_manager->GetDriverByName("OSM"));
  RegisterOGROSM();
  driver = driver_manager->GetDriverByName("OSM");
  ASSERT_NE(nullptr, driver);

  EXPECT_STREQ("OSM", driver->GetDescription());
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("OpenStreetMap XML and PBF",
               driver->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("osm pbf", driver->GetMetadataItem(GDAL_DMD_EXTENSIONS));
  EXPECT_STREQ("drivers/vector/osm.html",
               driver->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("YES", driver->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES",
               driver->GetMetadataItem(GDAL_DCAP_MULTIPLE_VECTOR_LAYERS));
  EXPECT_STREQ("OGRSQL SQLITE",
               driver->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS));

  const char* open_options = driver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST);
  ASSERT_NE(nullptr, open_options);
  EXPECT_THAT(open_options, HasSubstr("<OpenOptionList>"));
  EXPECT_THAT(open_options, HasSubstr("INTERLEAVED_READING"));
}

TEST_F(OgrosmDriverTest, IdentifyDoesNotExist) {
  EXPECT_EQ(nullptr, GDALIdentifyDriver("/does_not_exist", nullptr));
}

TEST_F(OgrosmDriverTest, IdentifyEmptyFile) {
  const std::string path = "/vsimem/ogrosm_driver/empty.osm";
  VsiMemTempWrapper mem_file(path, "");
  EXPECT_EQ(nullptr, GDALIdentifyDriver(path.c_str(), nullptr));
}

TEST_F(OgrosmDriverTest, IdentifyXmlFile) {
  const std::string path = "/vsimem/ogrosm_driver/sample.osm";
  VsiMemTempWrapper mem_file(path, std::string(kEmptyOsmXml));

  GDALDriver* driver =
      GDALDriver::FromHandle(GDALIdentifyDriver(path.c_str(), nullptr));
  ASSERT_NE(nullptr, driver);
  EXPECT_STREQ("OSM", driver->GetDescription());
}

TEST_F(OgrosmDriverTest, IdentifyPbfFile) {
  const std::string path = "/vsimem/ogrosm_driver/sample.pbf";
  // OGROSMDriverIdentify verifies PBF files by locating the "OSMHeader" marker.
  std::string pbf_bytes;
  pbf_bytes.push_back('\x00');
  pbf_bytes.push_back('\x00');
  pbf_bytes.push_back('\x00');
  pbf_bytes.push_back('\x0d');
  pbf_bytes.append("\x0a\x09OSMHeader\x18\x00");
  VsiMemTempWrapper mem_file(path, pbf_bytes);

  GDALDriver* driver =
      GDALDriver::FromHandle(GDALIdentifyDriver(path.c_str(), nullptr));
  ASSERT_NE(nullptr, driver);
  EXPECT_STREQ("OSM", driver->GetDescription());
}

TEST_F(OgrosmDriverTest, IdentifyNonOsmFile) {
  const std::string path = "/vsimem/ogrosm_driver/non_osm.txt";
  VsiMemTempWrapper mem_file(path, "This is arbitrary text without OSM tags.");
  EXPECT_EQ(nullptr, GDALIdentifyDriver(path.c_str(), nullptr));
}

TEST_F(OgrosmDriverTest, OpenUpdateModeFails) {
  const std::string path = "/vsimem/ogrosm_driver/update_fail.osm";
  VsiMemTempWrapper mem_file(path, std::string(kEmptyOsmXml));

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OSM");
  ASSERT_NE(nullptr, driver);
  ASSERT_NE(nullptr, driver->pfnOpen);

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_UPDATE);
  EXPECT_EQ(nullptr, driver->pfnOpen(&open_info));
}

TEST_F(OgrosmDriverTest, OpenNonOsmFails) {
  const std::string path = "/vsimem/ogrosm_driver/non_osm_open.txt";
  VsiMemTempWrapper mem_file(path, "Not an OSM file.");

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OSM");
  ASSERT_NE(nullptr, driver);
  ASSERT_NE(nullptr, driver->pfnOpen);

  GDALOpenInfo open_info(path.c_str(), GDAL_OF_READONLY);
  EXPECT_EQ(nullptr, driver->pfnOpen(&open_info));
}

TEST_F(OgrosmDriverTest, OpenFailsWhenDataSourceOpenFails) {
  const std::string path = "/vsimem/ogrosm_driver/bad_config.osm";
  VsiMemTempWrapper mem_file(path, std::string(kEmptyOsmXml));

  const char* const open_options[] = {
      "CONFIG_FILE=/vsimem/ogrosm_driver/non_existent_config.ini", nullptr};

  CPLErrorReset();
  GDALDatasetUniquePtr ds;
  {
    WithQuietHandler quiet;
    ds.reset(GDALDataset::Open(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                               nullptr, open_options, nullptr));
  }
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Could not parse configuration file"));
}

TEST_F(OgrosmDriverTest, OpenValidOsmXmlSucceeds) {
  const std::string path = "/vsimem/ogrosm_driver/valid.osm";
  const std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<osm version=\"0.6\">\n"
      "  <node id=\"1\" lat=\"48.8584\" lon=\"2.2945\">\n"
      "    <tag k=\"name\" v=\"Eiffel Tower\"/>\n"
      "  </node>\n"
      "</osm>\n";
  VsiMemTempWrapper mem_file(path, xml);

  GDALDatasetUniquePtr ds(GDALDataset::Open(path.c_str(),
                                            GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                            nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);

  EXPECT_EQ(5, ds->GetLayerCount());
  EXPECT_STREQ("points", ds->GetLayer(0)->GetName());
  EXPECT_STREQ("lines", ds->GetLayer(1)->GetName());
  EXPECT_STREQ("multilinestrings", ds->GetLayer(2)->GetName());
  EXPECT_STREQ("multipolygons", ds->GetLayer(3)->GetName());
  EXPECT_STREQ("other_relations", ds->GetLayer(4)->GetName());
}

// TODO: b/335317901 - Cover GDAL_CHECK_VERSION failure branch in
// OGROSMDriverOpen.

}  // namespace
}  // namespace autotest2
