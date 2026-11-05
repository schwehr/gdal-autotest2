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
// Tests for OGRGMLDriver (ogrgmldriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/ogrgmldriver.cpp

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrgmldriver_test";

constexpr std::string_view kMinimalGml =
    R"xml(<ogr:FeatureCollection xmlns:ogr="http://ogr.maptools.org/" xmlns:gml="http://www.opengis.net/gml"><gml:featureMember><ogr:layer fid="layer.0"><ogr:geom><gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point></ogr:geom><ogr:prop>value</ogr:prop></ogr:layer></gml:featureMember></ogr:FeatureCollection>)xml";

class OGRGMLDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRGML();
    driver_ = GetGDALDriverManager()->GetDriverByName("GML");
    ASSERT_NE(nullptr, driver_);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }

  GDALDriver* driver_ = nullptr;
};

TEST_F(OGRGMLDriverTest, IdempotentRegister) {
  // Calling RegisterOGRGML again should be a no-op.
  RegisterOGRGML();
  EXPECT_EQ(driver_, GetGDALDriverManager()->GetDriverByName("GML"));
}

TEST_F(OGRGMLDriverTest, IdentifyNullFileHandle) {
  ASSERT_NE(nullptr, driver_->pfnIdentify);

  // Filename with ",xsd=" returns GDAL_IDENTIFY_UNKNOWN (-1).
  GDALOpenInfo info_xsd("/vsimem/fake.gml,xsd=fake.xsd", GA_ReadOnly);
  EXPECT_EQ(-1, driver_->pfnIdentify(&info_xsd));

  // Non-existent filename without ",xsd=" returns FALSE (0).
  GDALOpenInfo info_no_file("/vsimem/does_not_exist.gml", GA_ReadOnly);
  EXPECT_EQ(0, driver_->pfnIdentify(&info_no_file));
}

TEST_F(OGRGMLDriverTest, IdentifyGzipHeader) {
  ASSERT_NE(nullptr, driver_->pfnIdentify);

  constexpr std::array<uint8_t, 10> kGzipBytes = {0x1f, 0x8b, 0x08, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x03};
  constexpr char kGzPath[] = "/vsimem/ogrgmldriver_test/test.gml.gz";
  VsiMemTempWrapper file(
      kGzPath, std::string(reinterpret_cast<const char*>(kGzipBytes.data()),
                           kGzipBytes.size()));

  GDALOpenInfo info(kGzPath, GA_ReadOnly);
  EXPECT_EQ(-1, driver_->pfnIdentify(&info));
}

TEST_F(OGRGMLDriverTest, IdentifyUtf8BomAndWhitespace) {
  ASSERT_NE(nullptr, driver_->pfnIdentify);

  std::string bom_gml = "\xEF\xBB\xBF";
  bom_gml += kMinimalGml;
  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/bom.gml";
  VsiMemTempWrapper file(kPath, bom_gml);

  GDALOpenInfo info(kPath, GA_ReadOnly);
  EXPECT_EQ(1, driver_->pfnIdentify(&info));
}

TEST_F(OGRGMLDriverTest, IdentifyNonXml) {
  ASSERT_NE(nullptr, driver_->pfnIdentify);

  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/plain.txt";
  VsiMemTempWrapper file(kPath, "Not XML at all");

  GDALOpenInfo info(kPath, GA_ReadOnly);
  EXPECT_EQ(0, driver_->pfnIdentify(&info));
}

TEST_F(OGRGMLDriverTest, IdentifySingleAllowedDriver) {
  ASSERT_NE(nullptr, driver_->pfnIdentify);

  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/other.xml";
  VsiMemTempWrapper file(kPath, "<other_root/>");

  // Without allowed driver list, CheckHeader rejects it.
  GDALOpenInfo info_no_allowed(kPath, GA_ReadOnly);
  EXPECT_EQ(0, driver_->pfnIdentify(&info_no_allowed));

  // With single allowed driver "GML", returns TRUE (1).
  const char* allowed_drivers[] = {"GML", nullptr};
  GDALOpenInfo info_allowed(kPath, GA_ReadOnly);
  info_allowed.papszAllowedDrivers = const_cast<char**>(allowed_drivers);
  EXPECT_EQ(1, driver_->pfnIdentify(&info_allowed));
}

TEST_F(OGRGMLDriverTest, OpenUpdateModeFails) {
  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/valid.gml";
  VsiMemTempWrapper file(kPath, std::string(kMinimalGml));

  GDALOpenInfo info(kPath, GA_Update);
  EXPECT_EQ(nullptr, driver_->pfnOpen(&info));
}

TEST_F(OGRGMLDriverTest, OpenNonGmlFails) {
  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/plain.txt";
  VsiMemTempWrapper file(kPath, "Not XML");

  GDALOpenInfo info(kPath, GA_ReadOnly);
  EXPECT_EQ(nullptr, driver_->pfnOpen(&info));
}

TEST_F(OGRGMLDriverTest, OpenNonexistentWithXsdOptionFails) {
  GDALOpenInfo info("/vsimem/fake.gml,xsd=fake.xsd", GA_ReadOnly);
  WithQuietHandler quiet_handler;
  EXPECT_EQ(nullptr, driver_->pfnOpen(&info));
}

TEST_F(OGRGMLDriverTest, OpenValidGml) {
  constexpr char kPath[] = "/vsimem/ogrgmldriver_test/valid.gml";
  constexpr char kGfsPath[] = "/vsimem/ogrgmldriver_test/valid.gfs";
  absl::Cleanup cleanup_gfs = [&] { EXPECT_EQ(0, VSIUnlink(kGfsPath)); };
  VsiMemTempWrapper file(kPath, std::string(kMinimalGml));

  GDALOpenInfo info(kPath, GA_ReadOnly);
  std::unique_ptr<GDALDataset> ds(
      reinterpret_cast<GDALDataset*>(driver_->pfnOpen(&info)));
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetLayerCount());
}

TEST_F(OGRGMLDriverTest, CreateSuccessAndFailure) {
  constexpr char kOutPath[] = "/vsimem/ogrgmldriver_test/out.gml";
  constexpr char kOutXsdPath[] = "/vsimem/ogrgmldriver_test/out.xsd";

  // Successful create.
  {
    absl::Cleanup cleanup = [&] {
      EXPECT_EQ(0, VSIUnlink(kOutPath));
      EXPECT_EQ(0, VSIUnlink(kOutXsdPath));
    };
    std::unique_ptr<GDALDataset> ds(
        driver_->Create(kOutPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
  }

  // Failed create with unwritable path.
  WithQuietHandler quiet_handler;
  std::unique_ptr<GDALDataset> ds_fail(driver_->Create(
      "/proc/invalid_gdal_dir/out.gml", 0, 0, 0, GDT_Unknown, nullptr));
  EXPECT_EQ(nullptr, ds_fail);
}

}  // namespace
}  // namespace autotest2
