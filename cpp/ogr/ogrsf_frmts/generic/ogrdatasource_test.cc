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

// Tests for OGRDataSource and OGR_DS_* C API functions.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrdatasource.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrdatasource.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

class DummyOGRDataSource : public OGRDataSource {
 public:
  DummyOGRDataSource() { SetDescription("dummy_ds"); }
  ~DummyOGRDataSource() override = default;

  const char* GetName() override { return "dummy_ds"; }
  int GetLayerCount() const override { return 1; }
  const OGRLayer* GetLayer(int i) const override {
    return (i == 0) ? &mem_layer_ : nullptr;
  }
  int TestCapability(const char*) const override { return TRUE; }

  CPLErr FlushCache(bool) override {
    if (simulate_flush_error_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Simulated flush error");
      return CE_Failure;
    }
    return CE_None;
  }

  void SetSimulateFlushError(bool val) { simulate_flush_error_ = val; }

 private:
  OGRMemLayer mem_layer_{"dummy_layer", nullptr, wkbPoint};
  bool simulate_flush_error_ = false;
};

TEST(OGRDataSourceTest, DestroyDataSourceMethod) {
  auto* ds = new DummyOGRDataSource();
  EXPECT_STREQ("dummy_ds", ds->GetDescription());
  OGRDataSource::DestroyDataSource(ds);
}

TEST(OGRDataSourceTest, CAPILifecycleAndReference) {
  GDALRegister_MEM();
  auto* driver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, driver);

  GDALDataset* ds = driver->Create("mem_test", 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(nullptr, ds);
  OGRDataSourceH hDS = GDALDataset::ToHandle(ds);
  struct HDSDeleter {
    void operator()(void* h) const {
      if (h) OGR_DS_Destroy(reinterpret_cast<OGRDataSourceH>(h));
    }
  };
  std::unique_ptr<void, HDSDeleter> hds_guard(hDS);

  EXPECT_EQ(1, OGR_DS_GetRefCount(hDS));
  EXPECT_EQ(1, OGR_DS_GetSummaryRefCount(hDS));

  EXPECT_EQ(2, OGR_DS_Reference(hDS));
  EXPECT_EQ(1, OGR_DS_Dereference(hDS));

  EXPECT_STREQ("mem_test", OGR_DS_GetName(hDS));
  EXPECT_NE(nullptr, OGR_DS_GetDriver(hDS));

  // Layer creation
  OGRLayerH hLayer =
      OGR_DS_CreateLayer(hDS, "pt_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, hLayer);
  EXPECT_EQ(1, OGR_DS_GetLayerCount(hDS));
  EXPECT_EQ(hLayer, OGR_DS_GetLayer(hDS, 0));
  EXPECT_EQ(nullptr, OGR_DS_GetLayer(hDS, 1));
  EXPECT_EQ(hLayer, OGR_DS_GetLayerByName(hDS, "pt_layer"));

  // Copy layer
  OGRLayerH hCopiedLayer =
      OGR_DS_CopyLayer(hDS, hLayer, "pt_layer_copy", nullptr);
  ASSERT_NE(nullptr, hCopiedLayer);
  EXPECT_EQ(2, OGR_DS_GetLayerCount(hDS));

  // TestCapability
  EXPECT_TRUE(OGR_DS_TestCapability(hDS, ODsCCreateLayer));
  EXPECT_FALSE(OGR_DS_TestCapability(hDS, "UnknownCapability"));

  // Delete layer
  EXPECT_EQ(OGRERR_NONE, OGR_DS_DeleteLayer(hDS, 1));
  EXPECT_EQ(1, OGR_DS_GetLayerCount(hDS));

  // ExecuteSQL and ReleaseResultSet
  OGRLayerH hSQLRes =
      OGR_DS_ExecuteSQL(hDS, "SELECT * FROM pt_layer", nullptr, nullptr);
  ASSERT_NE(nullptr, hSQLRes);
  OGR_DS_ReleaseResultSet(hDS, hSQLRes);

  // StyleTable methods
  EXPECT_EQ(nullptr, OGR_DS_GetStyleTable(hDS));
  OGRStyleTableH hTable1 =
      reinterpret_cast<OGRStyleTableH>(new OGRStyleTable());
  OGR_DS_SetStyleTableDirectly(hDS, hTable1);
  EXPECT_NE(nullptr, OGR_DS_GetStyleTable(hDS));

  OGRStyleTable table2;
  OGR_DS_SetStyleTable(hDS, reinterpret_cast<OGRStyleTableH>(&table2));

  // SyncToDisk
  EXPECT_EQ(OGRERR_NONE, OGR_DS_SyncToDisk(hDS));

  hds_guard.release();
  OGR_DS_Destroy(hDS);
  hDS = nullptr;
  // OGR_DS_Destroy with nullptr
  OGR_DS_Destroy(nullptr);
}

TEST(OGRDataSourceTest, SyncToDiskErrorHandling) {
  CPLErrorReset();
  auto ds = std::make_unique<DummyOGRDataSource>();
  OGRDataSourceH hDS = GDALDataset::ToHandle(ds.get());

  EXPECT_EQ(OGRERR_NONE, OGR_DS_SyncToDisk(hDS));

  ds->SetSimulateFlushError(true);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, OGR_DS_SyncToDisk(hDS));
  }
  CPLErrorReset();
}

TEST(OGRDataSourceTest, NullHandleValidations) {
  CPLErrorReset();
  EXPECT_EQ(0, OGR_DS_Reference(nullptr));
  EXPECT_EQ(0, OGR_DS_Dereference(nullptr));
  EXPECT_EQ(0, OGR_DS_GetRefCount(nullptr));
  EXPECT_EQ(0, OGR_DS_GetSummaryRefCount(nullptr));
  EXPECT_EQ(nullptr,
            OGR_DS_CreateLayer(nullptr, "test", nullptr, wkbPoint, nullptr));

  DummyOGRDataSource ds;
  OGRDataSourceH hDS = GDALDataset::ToHandle(&ds);
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(nullptr,
              OGR_DS_CreateLayer(hDS, nullptr, nullptr, wkbPoint, nullptr));
  }

  EXPECT_EQ(nullptr, OGR_DS_CopyLayer(nullptr, nullptr, "test", nullptr));
  EXPECT_EQ(nullptr, OGR_DS_CopyLayer(hDS, nullptr, "test", nullptr));
  EXPECT_EQ(nullptr, OGR_DS_CopyLayer(hDS, reinterpret_cast<OGRLayerH>(&ds),
                                      nullptr, nullptr));

  EXPECT_EQ(OGRERR_INVALID_HANDLE, OGR_DS_DeleteLayer(nullptr, 0));
  EXPECT_EQ(nullptr, OGR_DS_GetLayerByName(nullptr, "layer"));
  EXPECT_EQ(nullptr, OGR_DS_ExecuteSQL(nullptr, "SELECT 1", nullptr, nullptr));
  OGR_DS_ReleaseResultSet(nullptr, nullptr);

  EXPECT_EQ(0, OGR_DS_TestCapability(nullptr, ODsCCreateLayer));
  EXPECT_EQ(0, OGR_DS_TestCapability(hDS, nullptr));
  EXPECT_EQ(0, OGR_DS_GetLayerCount(nullptr));
  EXPECT_EQ(nullptr, OGR_DS_GetLayer(nullptr, 0));
  EXPECT_EQ(nullptr, OGR_DS_GetName(nullptr));
  EXPECT_EQ(OGRERR_INVALID_HANDLE, OGR_DS_SyncToDisk(nullptr));
  EXPECT_EQ(nullptr, OGR_DS_GetDriver(nullptr));
  EXPECT_EQ(nullptr, OGR_DS_GetStyleTable(nullptr));
  OGR_DS_SetStyleTableDirectly(nullptr, nullptr);
  OGR_DS_SetStyleTable(nullptr, nullptr);
  OGR_DS_SetStyleTable(hDS, nullptr);
  CPLErrorReset();
}

// TODO: b/335317901 - Cover remaining error branches in ogrdatasource.cpp.

}  // namespace
}  // namespace autotest2
