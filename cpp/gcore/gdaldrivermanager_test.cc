// Test gcore/gdaldrivermanager.cpp.
//
// Copyright 2016 Google Inc. All Rights Reserved.
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

#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_majorobject.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

#define class struct
#define private public
#define protected public
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdalplugindriverproxy.h"
#undef protected
#undef private
#undef class

#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

int DummyIdentify(GDALOpenInfo*) { return FALSE; }
GDALDataset* DummyOpen(GDALOpenInfo*) { return nullptr; }
GDALDataset* DummyCreate(const char*, int, int, int, GDALDataType, char**) {
  return nullptr;
}

void TouchVsiFile(const char* path, const char* content = "dummy") {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(content, 1, strlen(content), fp);
  VSIFCloseL(fp);
}

// This test checks loading all gives a reasonable number of drivers.
TEST(GDALDriverManagerTest, DriverCount) {
  GDALDriverManager* manager_ = GetGDALDriverManager();
  // Never expect this to happen, but good to check at least once.
  ASSERT_NE(nullptr, manager_);

  GDALAllRegister();

  // Must have at least 1 vector and 1 raster driver.
  EXPECT_LT(2, manager_->GetDriverCount());
  // Cannot have more than some ridiculously high number of drivers.
  EXPECT_GT(300, manager_->GetDriverCount());

  // Just for google3, we know how many drivers should be available.
  // If you enable more drivers, set this number to the new total number of
  // drivers and make sure that there are C++ and Python tests for
  // every active driver.
  EXPECT_EQ(38, manager_->GetDriverCount());
}

// This test make sure this can remove one of many drivers.
TEST(GDALDriverManagerTest, RemoveSingleDriver) {
  GDALDriverManager* manager_ = GetGDALDriverManager();
  GDALAllRegister();
  const int driver_count = manager_->GetDriverCount();
  ASSERT_LT(2, driver_count);

  const std::string name = "VRT";
  GDALDriver* driver = manager_->GetDriverByName(name.c_str());
  std::string driver_name = driver->GetDescription();
  EXPECT_STREQ(name.c_str(), driver->GetDescription());

  manager_->DeregisterDriver(driver);
  delete driver;
  driver = nullptr;

  EXPECT_EQ(driver_count - 1, manager_->GetDriverCount());
  EXPECT_EQ(nullptr, manager_->GetDriverByName(name.c_str()));
}

TEST(GDALDriverManagerTest, RegistrationAndSkipAndPlugins) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDriverManager* dm = GetGDALDriverManager();
  ASSERT_NE(dm, nullptr);
  EXPECT_NE(GDALGetphDMMutex(), nullptr);
  EXPECT_EQ(GDALGetDriverCount(), dm->GetDriverCount());
  EXPECT_EQ(GDALGetDriver(0), dm->GetDriver(0));
  EXPECT_EQ(dm->GetDriver(-1), nullptr);
  EXPECT_EQ(dm->GetDriver(99999, true), nullptr);

  // Memory deprecation warning and CartoDB alias.
  EXPECT_NE(dm->GetDriverByName("Memory"), nullptr);
  EXPECT_EQ(dm->GetDriverByName("CartoDB"), nullptr);
  EXPECT_TRUE(dm->IsKnownDriver("MEM"));
  EXPECT_FALSE(dm->IsKnownDriver("NonExistentDriver123"));

  // Register custom driver with callbacks and capabilities.
  auto* custom_drv = new GDALDriver();
  custom_drv->SetDescription("CustomTestDrv");
  custom_drv->pfnOpen = DummyOpen;
  custom_drv->pfnCreate = DummyCreate;
  custom_drv->pfnCreateCopy = [](const char*, GDALDataset*, int, char**,
                                 GDALProgressFunc,
                                 void*) -> GDALDataset* { return nullptr; };
  custom_drv->pfnCreateMultiDimensional = [](const char*, CSLConstList,
                                             CSLConstList) -> GDALDataset* {
    return nullptr;
  };
  custom_drv->pfnVectorTranslateFrom =
      [](const char*, GDALDataset*, CSLConstList, GDALProgressFunc,
         void*) -> GDALDataset* { return nullptr; };
  custom_drv->SetMetadataItem(GDAL_DMD_OPENOPTIONLIST, "<OpenOptionList/>");
  int idx = GDALRegisterDriver(custom_drv);
  EXPECT_GE(idx, 0);
  // Registering same driver pointer again returns same index.
  EXPECT_EQ(dm->RegisterDriver(custom_drv), idx);
  GDALDeregisterDriver(custom_drv);
  // Deregistering again is a no-op.
  dm->DeregisterDriver(custom_drv);
  delete custom_drv;

  // AutoSkipDrivers with GDAL_SKIP (both comma and space separated) and
  // OGR_SKIP.
  auto* skip1 = new GDALDriver();
  skip1->SetDescription("SkipDrv1");
  dm->RegisterDriver(skip1);
  auto* skip2 = new GDALDriver();
  skip2->SetDescription("SkipDrv2");
  dm->RegisterDriver(skip2);
  auto* skip3 = new GDALDriver();
  skip3->SetDescription("SkipDrv3");
  dm->RegisterDriver(skip3);
  {
    CPLConfigOptionSetter s1("GDAL_SKIP", "SkipDrv1,MissingSkipDrv", false);
    CPLConfigOptionSetter s2("OGR_SKIP", "SkipDrv2", false);
    dm->AutoSkipDrivers();
  }
  {
    CPLConfigOptionSetter s3("GDAL_SKIP", "SkipDrv3", false);
    dm->AutoSkipDrivers();
  }
  EXPECT_EQ(dm->GetDriverByName("SkipDrv1"), nullptr);
  EXPECT_EQ(dm->GetDriverByName("SkipDrv2"), nullptr);
  EXPECT_EQ(dm->GetDriverByName("SkipDrv3"), nullptr);

  // Plugin loading, AutoLoadDrivers, ReorderDrivers, and GDALPluginDriverProxy.
  VSIMkdir("/vsimem/gdal_plugins", 0755);
  TouchVsiFile("/vsimem/gdal_plugins/gdal_Deferred1.so");

  {
    CPLConfigOptionSetter disable_setter("GDAL_DRIVER_PATH", "disable", false);
    dm->AutoLoadDrivers();
    EXPECT_EQ(dm->GetPluginFullPath("gdal_Deferred1.so"), "");
  }

  {
    CPLConfigOptionSetter path_setter("OGR_DRIVER_PATH", "/vsimem/gdal_plugins",
                                      false);
    // LoadPlugin under GDAL_NO_AUTOLOAD returns CE_Failure.
    EXPECT_EQ(dm->LoadPlugin("TestPlug1"), CE_Failure);
    dm->AutoLoadDrivers();
    dm->ReorderDrivers();

    // DeclareDeferredPluginDriver tests:
    // 1. Invalid plugin filename:
    auto invalid_proxy = std::make_unique<GDALPluginDriverProxy>("bad_name.so");
    invalid_proxy->SetDescription("BadProxy");
    dm->DeclareDeferredPluginDriver(invalid_proxy.get());

    // 2. Already registered driver name:
    auto* dup_proxy = new GDALPluginDriverProxy("gdal_GTiff.so");
    dup_proxy->SetDescription("GTiff");
    dm->DeclareDeferredPluginDriver(dup_proxy);  // deletes dup_proxy

    // 3. Missing plugin file -> becomes hidden driver:
    dm->m_osLastTriedDirectory.clear();
    auto* missing_proxy = new GDALPluginDriverProxy("gdal_MissingPlug.so");
    missing_proxy->SetDescription("MissingPlug");
    missing_proxy->SetMetadataItem(GDAL_DMD_EXTENSIONS, "ext1 ext2");
    missing_proxy->SetMetadataItem("CUSTOM_PROXY_ITEM", "custom_val");
    dm->DeclareDeferredPluginDriver(missing_proxy);
    // Second registration of same missing plugin hits deduplication loop:
    auto* missing_proxy2 = new GDALPluginDriverProxy("gdal_MissingPlug.so");
    missing_proxy2->SetDescription("MissingPlug");
    dm->DeclareDeferredPluginDriver(missing_proxy2);

    EXPECT_TRUE(dm->IsKnownDriver("MissingPlug"));
    EXPECT_GT(dm->GetDriverCount(true), dm->GetDriverCount(false));
    GDALDriver* hidden = dm->GetHiddenDriverByName("MissingPlug");
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(dm->GetHiddenDriverByName("NotHidden"), nullptr);
    EXPECT_NE(dm->GetDriver(dm->GetDriverCount(false), true), nullptr);

    EXPECT_STREQ(hidden->GetMetadataItem("IS_NON_LOADED_PLUGIN"), "YES");
    EXPECT_STREQ(hidden->GetMetadataItem("MISSING_PLUGIN_FILENAME"),
                 "gdal_MissingPlug.so");
    EXPECT_STREQ(hidden->GetMetadataItem(GDAL_DMD_EXTENSION), "ext1 ext2");
    EXPECT_STREQ(hidden->GetMetadataItem("CUSTOM_PROXY_ITEM"), "custom_val");
    EXPECT_EQ(hidden->GetMetadataItem("OTHER_UNLOADED_ITEM"), nullptr);
    EXPECT_EQ(hidden->GetMetadata(nullptr), nullptr);
    EXPECT_EQ(hidden->GetOpenCallback(), nullptr);
    EXPECT_EQ(hidden->GetCreateCallback(), nullptr);
    EXPECT_EQ(hidden->GetCreateMultiDimensionalCallback(), nullptr);
    EXPECT_EQ(hidden->GetCreateCopyCallback(), nullptr);
    EXPECT_EQ(hidden->GetDeleteCallback(), nullptr);
    EXPECT_EQ(hidden->GetRenameCallback(), nullptr);
    EXPECT_EQ(hidden->GetCopyFilesCallback(), nullptr);
    EXPECT_EQ(hidden->GetInstantiateAlgorithmCallback(), nullptr);

    // 4. Valid deferred plugin (gdal_Deferred1.so) found via
    // m_osLastTriedDirectory:
    dm->m_osLastTriedDirectory = "/vsimem/gdal_plugins";
    auto* def1_proxy = new GDALPluginDriverProxy("gdal_Deferred1.so");
    def1_proxy->SetDescription("Deferred1");
    def1_proxy->SetMetadataItem(GDAL_DMD_EXTENSION, "def1");
    def1_proxy->SetMetadataItem(GDAL_DMD_LONGNAME, "Proxy Mismatched Longname");
    def1_proxy->SetMetadataItem("PROXY_CUSTOM", "proxy_val");
    def1_proxy->SetMetadataItem(GDAL_DCAP_CREATECOPY, "YES");
    def1_proxy->SetMetadataItem(GDAL_DMD_PLUGIN_INSTALLATION_MESSAGE, "msg");
    def1_proxy->pfnGetSubdatasetInfoFunc =
        [](const char*) -> GDALSubdatasetInfo* { return nullptr; };
    dm->DeclareDeferredPluginDriver(def1_proxy);

    EXPECT_STREQ(def1_proxy->GetMetadataItem("IS_NON_LOADED_PLUGIN"), "YES");
    EXPECT_EQ(def1_proxy->GetMetadataItem("MISSING_PLUGIN_FILENAME"), nullptr);
    EXPECT_STREQ(def1_proxy->GetMetadataItem(GDAL_DMD_EXTENSIONS), "def1");
    EXPECT_STREQ(def1_proxy->GetMetadataItem("PROXY_CUSTOM"), "proxy_val");

    // First call to GetRealDriver() before real driver is in m_oMapRealDrivers
    // hits the #ifdef GDAL_NO_AUTOLOAD return nullptr branch:
    EXPECT_EQ(def1_proxy->GetOpenCallback(), nullptr);

    // Now register the real driver while m_bInDeferredDriverLoading is true:
    dm->m_bInDeferredDriverLoading = true;
    EXPECT_EQ(dm->GetDriverByName("Deferred1"), nullptr);

    auto* real_def1 = new GDALDriver();
    real_def1->SetDescription("Deferred1");
    real_def1->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    real_def1->SetMetadataItem(GDAL_DMD_LONGNAME, "Deferred 1 Real");
    real_def1->SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
    real_def1->SetMetadataItem("REAL_ONLY_META", "VAL");
    real_def1->pfnIdentify = DummyIdentify;
    real_def1->pfnOpen = DummyOpen;
    real_def1->pfnCreate = DummyCreate;
    EXPECT_EQ(dm->RegisterDriver(real_def1), -1);

    // Duplicate registration in m_bInDeferredDriverLoading logs error and
    // deletes driver:
    auto* dup_real_def1 = new GDALDriver();
    dup_real_def1->SetDescription("Deferred1");
    EXPECT_EQ(dm->RegisterDriver(dup_real_def1), -1);
    dm->m_bInDeferredDriverLoading = false;

    // Trigger GetRealDriver() which moves real_def1 from m_oMapRealDrivers and
    // validates metadata/callbacks:
    EXPECT_NE(def1_proxy->GetOpenCallback(), nullptr);
    EXPECT_NE(def1_proxy->GetCreateCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetCreateMultiDimensionalCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetCreateCopyCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetDeleteCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetRenameCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetCopyFilesCallback(), nullptr);
    EXPECT_EQ(def1_proxy->GetInstantiateAlgorithmCallback(), nullptr);
    EXPECT_NE(def1_proxy->GetMetadata(nullptr), nullptr);
    EXPECT_STREQ(def1_proxy->GetMetadataItem("REAL_ONLY_META"), "VAL");
    EXPECT_STREQ(def1_proxy->GetMetadataItem("REAL_ONLY_META", "OTHER_DOM"),
                 nullptr);
    EXPECT_EQ(def1_proxy->GetMetadataItem("IS_NON_LOADED_PLUGIN"), nullptr);
  }

  VSIRmdirRecursive("/vsimem/gdal_plugins");
  CPLPopErrorHandler();

  // Test GDALDestroyDriverManager() with an open dataset at the end of the
  // test suite.
  GDALDriver* mem_drv = dm->GetDriverByName("MEM");
  ASSERT_NE(mem_drv, nullptr);
  GDALDataset* open_mem =
      mem_drv->Create("/vsimem/open_ds", 2, 2, 1, GDT_Byte, nullptr);
  ASSERT_NE(open_mem, nullptr);
  GDALDestroyDriverManager();
}

}  // namespace
