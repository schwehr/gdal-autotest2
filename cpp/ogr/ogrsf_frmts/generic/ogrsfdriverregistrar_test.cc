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

// Tests for OGRSFDriverRegistrar and driver registrar C API.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrsfdriverregistrar.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrsfdriverregistrar.cpp

#include <memory>

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

constexpr char kCustomDriverName[] = "CustomRegistrarTestDriver";

class CustomRegistrarDataset : public OGRDataSource {
 public:
  explicit CustomRegistrarDataset(const char* name) {
    name_ = name ? name : "";
    SetDescription(name_.c_str());
  }
  ~CustomRegistrarDataset() override = default;

  const char* GetName() override { return name_.c_str(); }
  int GetLayerCount() const override { return 1; }
  const OGRLayer* GetLayer(int i) const override {
    return (i == 0) ? &mem_layer_ : nullptr;
  }
  int TestCapability(const char*) const override { return FALSE; }

 private:
  std::string name_;
  OGRMemLayer mem_layer_{"layer", nullptr, wkbPoint};
};

class CustomRegistrarDriver : public OGRSFDriver {
 public:
  CustomRegistrarDriver() = default;
  ~CustomRegistrarDriver() override = default;

  const char* GetName() override { return kCustomDriverName; }

  OGRDataSource* Open(const char* name, int) override {
    if (name != nullptr && std::string(name) == "custom:open_ok") {
      return new CustomRegistrarDataset(name);
    }
    return nullptr;
  }

  int TestCapability(const char* cap) override {
    if (EQUAL(cap, ODrCCreateDataSource) || EQUAL(cap, ODrCDeleteDataSource)) {
      return TRUE;
    }
    return FALSE;
  }

  OGRDataSource* CreateDataSource(const char* name, char**) override {
    if (name != nullptr && std::string(name) == "custom:create_ok") {
      return new CustomRegistrarDataset(name);
    }
    return nullptr;
  }

  OGRErr DeleteDataSource(const char* name) override {
    if (name != nullptr && std::string(name) == "custom:delete_ok") {
      return OGRERR_NONE;
    }
    return OGRERR_FAILURE;
  }
};

TEST(OGRSFDriverRegistrarTest, RegistrarQueriesAndStubs) {
  GDALRegister_MEM();
  auto* registrar = OGRSFDriverRegistrar::GetRegistrar();
  ASSERT_NE(nullptr, registrar);

  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(0, registrar->GetOpenDSCount());
    EXPECT_EQ(0, OGRGetOpenDSCount());
    EXPECT_EQ(nullptr, registrar->GetOpenDS(0));
    EXPECT_EQ(nullptr, OGRGetOpenDS(0));
  }

  int count = OGRGetDriverCount();
  EXPECT_GT(count, 0);
  EXPECT_EQ(count, registrar->GetDriverCount());

  OGRSFDriverH hFirst = OGRGetDriver(0);
  EXPECT_NE(nullptr, hFirst);
  EXPECT_EQ(nullptr, OGRGetDriver(-1));
  EXPECT_EQ(nullptr, OGRGetDriver(count + 1000));

  EXPECT_NE(nullptr, OGRGetDriverByName("Memory"));
  EXPECT_EQ(nullptr, OGRGetDriverByName("NonExistentDriver12345"));
  EXPECT_EQ(nullptr, OGRGetDriverByName(nullptr));
}

TEST(OGRSFDriverRegistrarTest, CustomDriverRegistrationAndOperations) {
  GDALRegister_MEM();
  auto* registrar = OGRSFDriverRegistrar::GetRegistrar();
  ASSERT_NE(nullptr, registrar);

  // Register custom driver
  auto* custom_driver = new CustomRegistrarDriver();
  struct CustomDriverCleaner {
    void operator()(CustomRegistrarDriver* d) const {
      if (d) {
        OGRDeregisterDriver(GDALDriver::ToHandle(d));
        delete d;
      }
    }
  };
  std::unique_ptr<CustomRegistrarDriver, CustomDriverCleaner> driver_guard(
      custom_driver);
  registrar->RegisterDriver(custom_driver);

  // Test finding by name (direct)
  GDALDriver* found_driver = registrar->GetDriverByName(kCustomDriverName);
  ASSERT_NE(nullptr, found_driver);
  EXPECT_STREQ(kCustomDriverName, found_driver->GetDescription());

  // Test finding driver without vector capability returns nullptr
  auto non_vector_driver = std::make_unique<GDALDriver>();
  non_vector_driver->SetDescription("RasterOnlyTest");
  GetGDALDriverManager()->RegisterDriver(non_vector_driver.get());
  EXPECT_EQ(nullptr, registrar->GetDriverByName("RasterOnlyTest"));
  GetGDALDriverManager()->DeregisterDriver(non_vector_driver.get());

  // Test driver registration duplicate (existing driver branch deletes new
  // instance)
  auto* duplicate_driver = new CustomRegistrarDriver();
  registrar->RegisterDriver(duplicate_driver);

  // Test OpenWithDriverArg through OGROpen
  OGRSFDriverH hUsedDriver = nullptr;
  OGRDataSourceH hDS = OGROpen("custom:open_ok", FALSE, &hUsedDriver);
  ASSERT_NE(nullptr, hDS);
  EXPECT_NE(nullptr, hUsedDriver);
  EXPECT_EQ(OGRERR_NONE, OGRReleaseDataSource(hDS));

  // Test OGROpenShared and OGRReleaseDataSource
  hDS = OGROpenShared("custom:open_ok", FALSE, &hUsedDriver);
  ASSERT_NE(nullptr, hDS);
  EXPECT_EQ(OGRERR_NONE, OGRReleaseDataSource(hDS));

  // Test CreateVectorOnly
  GDALDataset* created_ds =
      found_driver->Create("custom:create_ok", 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(nullptr, created_ds);
  delete created_ds;

  // Test DeleteDataSource
  EXPECT_EQ(CE_None, found_driver->Delete("custom:delete_ok"));
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(CE_Failure, found_driver->Delete("custom:delete_fail"));
  }

  // Test OGRRegisterDriver and OGRDeregisterDriver
  OGRDeregisterDriver(GDALDriver::ToHandle(found_driver));
  EXPECT_EQ(nullptr, OGRGetDriverByName(kCustomDriverName));

  OGRRegisterDriver(GDALDriver::ToHandle(found_driver));
  EXPECT_NE(nullptr, OGRGetDriverByName(kCustomDriverName));

  driver_guard.reset();
}

TEST(OGRSFDriverRegistrarTest, NullValidationsAndCleanupAll) {
  EXPECT_EQ(nullptr, OGROpen(nullptr, FALSE, nullptr));
  EXPECT_EQ(nullptr, OGROpenShared(nullptr, FALSE, nullptr));
  EXPECT_EQ(OGRERR_INVALID_HANDLE, OGRReleaseDataSource(nullptr));
  OGRRegisterDriver(nullptr);
  OGRDeregisterDriver(nullptr);

  // Test OGRCleanupAll() cleans up registered drivers
  OGRCleanupAll();
  EXPECT_EQ(0, GetGDALDriverManager()->GetDriverCount());

  // Re-register MEM driver for subsequent tests
  GDALRegister_MEM();
}

// TODO: b/335317901 - Cover remaining error branches in
// ogrsfdriverregistrar.cpp.

}  // namespace
}  // namespace autotest2
