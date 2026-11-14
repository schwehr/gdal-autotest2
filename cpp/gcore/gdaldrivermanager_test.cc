// Copyright 2016 Google LLC
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
// Unit tests for gcore/gdaldrivermanager.cpp and GDALPluginDriverProxy.
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with
// -DGDAL_NO_AUTOLOAD, so GDALDriverManager::GetSearchPaths() always returns
// nullptr. Consequently ~115 lines in GDALPluginDriverProxy::GetRealDriver() /
// GetPluginFullPath() and DeclareDeferredPluginDriver()'s active-plugin path
// in gcore/gdaldrivermanager.cpp are unreachable in google3 builds.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaldrivermanager.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalplugindriverproxy.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdaldrivermanager_test";

GDALDataset* DummyOpen(GDALOpenInfo*) { return nullptr; }
GDALDataset* DummyCreate(const char*, int, int, int, GDALDataType, char**) {
  return nullptr;
}

TEST(GDALDriverManagerTest, DriverCount) {
  GDALAllRegister();

  GDALDriverManager* const manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, manager);

  // Must have at least 1 vector and 1 raster driver.
  EXPECT_LT(2, manager->GetDriverCount());
  // Cannot have more than some ridiculously high number of drivers.
  EXPECT_GT(300, manager->GetDriverCount());

  // Just for google3, we know how many drivers should be available.
  // If you enable more drivers, set this number to the new total number of
  // drivers and make sure that there are C++ and Python tests for
  // every active driver.
  EXPECT_EQ(39, manager->GetDriverCount());
}

TEST(GDALDriverManagerTest, RemoveSingleDriver) {
  absl::Cleanup restore_dm = []() {
    GDALDestroyDriverManager();
    GDALAllRegister();
  };
  GDALAllRegister();

  GDALDriverManager* const manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, manager);
  const int driver_count = manager->GetDriverCount();
  ASSERT_LT(2, driver_count);

  constexpr char kDriverName[] = "VRT";
  GDALDriver* driver = manager->GetDriverByName(kDriverName);
  ASSERT_NE(nullptr, driver);
  EXPECT_STREQ(kDriverName, driver->GetDescription());

  manager->DeregisterDriver(driver);
  delete driver;

  EXPECT_EQ(driver_count - 1, manager->GetDriverCount());
  EXPECT_EQ(nullptr, manager->GetDriverByName(kDriverName));
}

TEST(GDALDriverManagerTest, RegistrationAndSkipAndPlugins) {
  absl::Cleanup restore_dm = []() {
    GDALDestroyDriverManager();
    GDALAllRegister();
    VSIRmdirRecursive(kTestDir);
  };
  GDALAllRegister();

  GDALDriverManager* const dm = GetGDALDriverManager();
  ASSERT_NE(nullptr, dm);
  EXPECT_NE(nullptr, GDALGetphDMMutex());
  EXPECT_EQ(dm->GetDriverCount(), GDALGetDriverCount());
  EXPECT_EQ(dm->GetDriver(0), GDALGetDriver(0));
  EXPECT_EQ(nullptr, dm->GetDriver(-1));
  EXPECT_EQ(nullptr, dm->GetDriver(99999, true));

  // Legacy "Memory" driver name alias and CartoDB alias.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, dm->GetDriverByName("Memory"));
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, dm->GetDriverByName("CartoDB"));
  EXPECT_TRUE(dm->IsKnownDriver("MEM"));
  EXPECT_FALSE(dm->IsKnownDriver("NonExistentDriver123"));

  // Register custom driver with callbacks and capabilities.
  auto custom_drv = std::make_unique<GDALDriver>();
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

  const int idx = GDALRegisterDriver(custom_drv.get());
  EXPECT_GE(idx, 0);
  // Registering same driver pointer again returns same index.
  EXPECT_EQ(idx, dm->RegisterDriver(custom_drv.get()));
  GDALDeregisterDriver(custom_drv.get());
  // Deregistering again is a no-op.
  dm->DeregisterDriver(custom_drv.get());

  // AutoSkipDrivers with GDAL_SKIP (comma and space separated) and OGR_SKIP.
  auto* skip1 = new GDALDriver();
  skip1->SetDescription("SkipDrv1");
  dm->RegisterDriver(skip1);
  auto* skip2 = new GDALDriver();
  skip2->SetDescription("SkipDrv2");
  dm->RegisterDriver(skip2);
  auto* skip3 = new GDALDriver();
  skip3->SetDescription("SkipDrv3");
  dm->RegisterDriver(skip3);
  auto* skip4 = new GDALDriver();
  skip4->SetDescription("SkipDrv4");
  dm->RegisterDriver(skip4);
  {
    CPLConfigOptionSetter s1("GDAL_SKIP", "SkipDrv1,MissingSkipDrv", false);
    CPLConfigOptionSetter s2("OGR_SKIP", "SkipDrv2", false);
    WithQuietHandler quiet;
    CPLErrorReset();
    dm->AutoSkipDrivers();
    CPLErrorReset();
  }
  {
    CPLConfigOptionSetter s3("GDAL_SKIP", "SkipDrv3 SkipDrv4", false);
    dm->AutoSkipDrivers();
  }
  EXPECT_EQ(nullptr, dm->GetDriverByName("SkipDrv1"));
  EXPECT_EQ(nullptr, dm->GetDriverByName("SkipDrv2"));
  EXPECT_EQ(nullptr, dm->GetDriverByName("SkipDrv3"));
  EXPECT_EQ(nullptr, dm->GetDriverByName("SkipDrv4"));

  // Plugin loading, AutoLoadDrivers, ReorderDrivers, and GDALPluginDriverProxy.
  const std::string plugins_dir = std::string(kTestDir) + "/gdal_plugins";
  EXPECT_EQ(0, VSIMkdir(kTestDir, 0755));
  EXPECT_EQ(0, VSIMkdir(plugins_dir.c_str(), 0755));
  VsiMemTempWrapper dummy_so(plugins_dir + "/gdal_Deferred1.so", "dummy");
  VsiMemTempWrapper drivers_ini(plugins_dir + "/drivers.ini",
                                "# comment\n\n[order]\nGTiff\nMEM\n");

  {
    CPLConfigOptionSetter disable_setter("GDAL_DRIVER_PATH", "disable", false);
    dm->AutoLoadDrivers();
  }

  {
    CPLConfigOptionSetter path_setter("OGR_DRIVER_PATH", plugins_dir.c_str(),
                                      false);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, dm->LoadPlugin("TestPlug1"));
      dm->AutoLoadDrivers();
      CPLErrorReset();
    }
    dm->ReorderDrivers();

    // 1. Invalid plugin filename:
    auto invalid_proxy = std::make_unique<GDALPluginDriverProxy>("bad_name.so");
    invalid_proxy->SetDescription("BadProxy");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      dm->DeclareDeferredPluginDriver(invalid_proxy.get());
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    // 2. Already registered driver name:
    auto* dup_proxy = new GDALPluginDriverProxy("gdal_GTiff.so");
    dup_proxy->SetDescription("GTiff");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      dm->DeclareDeferredPluginDriver(dup_proxy);  // deletes dup_proxy
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    // 3. Missing plugin file becomes a hidden driver:
    auto* missing_proxy = new GDALPluginDriverProxy("gdal_MissingPlug.so");
    missing_proxy->SetDescription("MissingPlug");
    missing_proxy->SetMetadataItem(GDAL_DMD_EXTENSIONS, "ext1 ext2");
    missing_proxy->SetMetadataItem("CUSTOM_PROXY_ITEM", "custom_val");
    dm->DeclareDeferredPluginDriver(missing_proxy);
    // Second registration of same missing plugin deduplicates and deletes it.
    auto* missing_proxy2 = new GDALPluginDriverProxy("gdal_MissingPlug.so");
    missing_proxy2->SetDescription("MissingPlug");
    dm->DeclareDeferredPluginDriver(missing_proxy2);

    EXPECT_TRUE(dm->IsKnownDriver("MissingPlug"));
    EXPECT_GT(dm->GetDriverCount(true), dm->GetDriverCount(false));
    GDALDriver* hidden = dm->GetHiddenDriverByName("MissingPlug");
    ASSERT_NE(nullptr, hidden);
    EXPECT_EQ(nullptr, dm->GetHiddenDriverByName("NotHidden"));
    EXPECT_NE(nullptr, dm->GetDriver(dm->GetDriverCount(false), true));

    EXPECT_STREQ("YES", hidden->GetMetadataItem("IS_NON_LOADED_PLUGIN"));
    EXPECT_STREQ("gdal_MissingPlug.so",
                 hidden->GetMetadataItem("MISSING_PLUGIN_FILENAME"));
    EXPECT_STREQ("ext1 ext2", hidden->GetMetadataItem(GDAL_DMD_EXTENSION));
    EXPECT_STREQ("custom_val", hidden->GetMetadataItem("CUSTOM_PROXY_ITEM"));
    EXPECT_EQ(nullptr, hidden->GetMetadataItem("OTHER_UNLOADED_ITEM"));
    EXPECT_EQ(nullptr, hidden->GetMetadata(nullptr));
    EXPECT_EQ(nullptr, hidden->GetOpenCallback());
    EXPECT_EQ(nullptr, hidden->GetCreateCallback());
    EXPECT_EQ(nullptr, hidden->GetCreateMultiDimensionalCallback());
    EXPECT_EQ(nullptr, hidden->GetCreateCopyCallback());
    EXPECT_EQ(nullptr, hidden->GetDeleteCallback());
    EXPECT_EQ(nullptr, hidden->GetRenameCallback());
    EXPECT_EQ(nullptr, hidden->GetCopyFilesCallback());
    EXPECT_EQ(nullptr, hidden->GetInstantiateAlgorithmCallback());

    // 4. Deferred plugin proxy file with GDAL_NO_AUTOLOAD defined (google3
    //    build): GetSearchPaths() returns nullptr, so
    //    DeclareDeferredPluginDriver places def1_proxy into hidden drivers.
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
    EXPECT_EQ(nullptr, dm->GetDriverByName("Deferred1"));
    EXPECT_EQ(def1_proxy, dm->GetHiddenDriverByName("Deferred1"));
    EXPECT_STREQ("gdal_Deferred1.so",
                 def1_proxy->GetMetadataItem("MISSING_PLUGIN_FILENAME"));
    EXPECT_STREQ("def1", def1_proxy->GetMetadataItem(GDAL_DMD_EXTENSION));
    EXPECT_STREQ("YES", def1_proxy->GetMetadataItem("IS_NON_LOADED_PLUGIN"));
    EXPECT_STREQ("proxy_val", def1_proxy->GetMetadataItem("PROXY_CUSTOM"));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, def1_proxy->GetOpenCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetCreateCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetCreateMultiDimensionalCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetCreateCopyCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetDeleteCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetRenameCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetCopyFilesCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetInstantiateAlgorithmCallback());
      EXPECT_EQ(nullptr, def1_proxy->GetMetadata(nullptr));
      EXPECT_EQ(nullptr, def1_proxy->GetMetadataItem("REAL_ONLY_META"));
      CPLErrorReset();
    }
  }
}

TEST(GDALDriverManagerTest, DestroyDriverManagerWithOpenDataset) {
  absl::Cleanup restore_dm = []() {
    GDALDestroyDriverManager();
    GDALAllRegister();
    VSIRmdirRecursive(kTestDir);
  };
  GDALAllRegister();

  GDALDriver* const mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  const std::string ds_path = std::string(kTestDir) + "/open_ds";
  // ~GDALDriverManager() force-deletes open datasets, so keep raw pointer.
  GDALDataset* const open_mem =
      mem_drv->Create(ds_path.c_str(), 2, 2, 1, GDT_Byte, nullptr);
  ASSERT_NE(nullptr, open_mem);

  GDALDestroyDriverManager();
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Plugin autoloading (.so dlopen in GDALPluginDriverProxy::
// GetRealDriver and GetPluginFullPath) requires -DGDAL_NO_AUTOLOAD to be
// disabled in //third_party/gdal/BUILD.
