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

// Tests for OGRSFDriver and OGR_Dr_* C API functions.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrsfdriver.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrsfdriver.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

class DummyOGRSFDriver : public OGRSFDriver {
 public:
  DummyOGRSFDriver() = default;
  ~DummyOGRSFDriver() override = default;

  const char* GetName() override { return "Dummy"; }
  OGRDataSource* Open(const char*, int) override { return nullptr; }
  int TestCapability(const char*) override { return FALSE; }

  // Call base class implementations.
  OGRDataSource* CallBaseCreateDataSource(const char* name, char** options) {
    return OGRSFDriver::CreateDataSource(name, options);
  }

  OGRErr CallBaseDeleteDataSource(const char* name) {
    return OGRSFDriver::DeleteDataSource(name);
  }
};

class DummyDatasetWithNullLayer : public GDALDataset {
 public:
  int GetLayerCount() const override { return 2; }
  const OGRLayer* GetLayer(int i) const override {
    if (i == 0) {
      return nullptr;
    }
    return &mem_layer_;
  }

 private:
  OGRMemLayer mem_layer_{"layer1", nullptr, wkbPoint};
};

TEST(OGRSFDriverTest, BaseMethodsAndDestructor) {
  auto driver = std::make_unique<DummyOGRSFDriver>();
  ASSERT_NE(nullptr, driver);
  EXPECT_STREQ("Dummy", driver->GetName());

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr, driver->CallBaseCreateDataSource("dummy", nullptr));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
              driver->CallBaseDeleteDataSource("dummy"));
  }

  driver.reset();
}

TEST(OGRSFDriverTest, CAPIHandlesAndCapabilities) {
  GDALRegister_MEM();
  auto* driver_manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, driver_manager);

  auto* mem_driver = driver_manager->GetDriverByName("Memory");
  ASSERT_NE(nullptr, mem_driver);
  OGRSFDriverH hMemDriver = GDALDriver::ToHandle(mem_driver);

  // OGR_Dr_GetName
  EXPECT_STREQ("Memory", OGR_Dr_GetName(hMemDriver));
  EXPECT_EQ(nullptr, OGR_Dr_GetName(nullptr));

  // OGR_Dr_TestCapability
  EXPECT_TRUE(OGR_Dr_TestCapability(hMemDriver, ODrCCreateDataSource));
  EXPECT_FALSE(OGR_Dr_TestCapability(hMemDriver, "UnknownCapability"));
  EXPECT_FALSE(OGR_Dr_TestCapability(nullptr, ODrCCreateDataSource));
  EXPECT_FALSE(OGR_Dr_TestCapability(hMemDriver, nullptr));

  // OGR_Dr_CreateDataSource
  OGRDataSourceH hDS = OGR_Dr_CreateDataSource(hMemDriver, "test_ds", nullptr);
  ASSERT_NE(nullptr, hDS);
  OGR_DS_Destroy(hDS);

  // Calling with nullptr name should default to ""
  OGRDataSourceH hDSEmptyName =
      OGR_Dr_CreateDataSource(hMemDriver, nullptr, nullptr);
  ASSERT_NE(nullptr, hDSEmptyName);
  OGR_DS_Destroy(hDSEmptyName);

  EXPECT_EQ(nullptr, OGR_Dr_CreateDataSource(nullptr, "test", nullptr));

  // OGR_Dr_Open
  EXPECT_EQ(nullptr, OGR_Dr_Open(hMemDriver, "Memory", FALSE));
  EXPECT_EQ(nullptr, OGR_Dr_Open(nullptr, "test", FALSE));

  // OGR_Dr_DeleteDataSource
  EXPECT_EQ(OGRERR_INVALID_HANDLE,
            OGR_Dr_DeleteDataSource(nullptr, "/vsimem/dummy"));

  GDALDriver deleting_driver;
  deleting_driver.pfnDelete = [](const char* name) -> CPLErr {
    if (name != nullptr && std::string_view(name) == "ok") {
      return CE_None;
    }
    return CE_Failure;
  };
  OGRSFDriverH hDelDriver = GDALDriver::ToHandle(&deleting_driver);
  EXPECT_TRUE(OGR_Dr_TestCapability(hDelDriver, ODrCDeleteDataSource));
  EXPECT_EQ(OGRERR_NONE, OGR_Dr_DeleteDataSource(hDelDriver, "ok"));
  EXPECT_EQ(OGRERR_FAILURE, OGR_Dr_DeleteDataSource(hDelDriver, "fail"));
}

TEST(OGRSFDriverTest, CopyDataSource) {
  GDALRegister_MEM();
  auto* driver_manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, driver_manager);

  auto* mem_driver = driver_manager->GetDriverByName("Memory");
  ASSERT_NE(nullptr, mem_driver);
  OGRSFDriverH hMemDriver = GDALDriver::ToHandle(mem_driver);

  // Test error when driver does not support creation
  GDALDriver non_creatable_driver;
  non_creatable_driver.SetDescription("NonCreatable");
  OGRSFDriverH hNonCreatable =
      reinterpret_cast<OGRSFDriverH>(&non_creatable_driver);

  DummyDatasetWithNullLayer src_ds;
  OGRDataSourceH hSrcDS = reinterpret_cast<OGRDataSourceH>(&src_ds);

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr,
              OGR_Dr_CopyDataSource(hNonCreatable, hSrcDS, "out", nullptr));
  }

  // Test successful copy with a dataset containing a null layer
  OGRDataSourceH hCopiedDS =
      OGR_Dr_CopyDataSource(hMemDriver, hSrcDS, "copied_ds", nullptr);
  ASSERT_NE(nullptr, hCopiedDS);
  OGR_DS_Destroy(hCopiedDS);

  // Invalid pointers
  EXPECT_EQ(nullptr, OGR_Dr_CopyDataSource(nullptr, hSrcDS, "out", nullptr));
  EXPECT_EQ(nullptr,
            OGR_Dr_CopyDataSource(hMemDriver, nullptr, "out", nullptr));
  EXPECT_EQ(nullptr,
            OGR_Dr_CopyDataSource(hMemDriver, hSrcDS, nullptr, nullptr));
}

// TODO: b/335317901 - Cover remaining error branches in ogrsfdriver.cpp.

}  // namespace
}  // namespace autotest2
