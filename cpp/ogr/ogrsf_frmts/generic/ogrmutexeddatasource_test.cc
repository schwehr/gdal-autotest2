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

// Tests for OGRMutexedDataSource.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrmutexeddatasource.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrmutexeddatasource.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrmutexeddatasource.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/port/cpl_multiproc.h"

namespace autotest2 {
namespace {

TEST(OGRMutexedDataSourceTest, AllMethodsWithMutexAndWrappedLayers) {
  GDALRegister_MEM();
  auto* driver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, driver);

  GDALDataset* base_ds =
      driver->Create("mem_ds", 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(nullptr, base_ds);
  base_ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);

  CPLMutex* mutex = CPLCreateMutex();
  ASSERT_NE(nullptr, mutex);
  CPLReleaseMutex(mutex);
  std::unique_ptr<CPLMutex, void (*)(CPLMutex*)> mutex_guard(mutex,
                                                             CPLDestroyMutex);

  {
    OGRMutexedDataSource mds(base_ds, /*bTakeOwnership=*/true, mutex,
                             /*bWrapLayersInMutexedLayer=*/true);

    EXPECT_EQ(base_ds, mds.GetBaseDataSource());
    EXPECT_EQ(1, mds.GetLayerCount());

    // First call wraps and inserts into m_oMapLayers
    OGRLayer* lyr1 = mds.GetLayer(0);
    ASSERT_NE(nullptr, lyr1);
    // Second call retrieves from cache
    EXPECT_EQ(lyr1, mds.GetLayer(0));
    EXPECT_EQ(nullptr, mds.GetLayer(10));
    EXPECT_FALSE(mds.IsLayerPrivate(0));

    EXPECT_EQ(lyr1, mds.GetLayerByName("layer1"));
    EXPECT_EQ(nullptr, mds.GetLayerByName("nonexistent"));

    // CreateLayer
    OGRGeomFieldDefn geom_defn("geom", wkbPoint);
    OGRLayer* lyr2 = mds.ICreateLayer("layer2", &geom_defn, nullptr);
    ASSERT_NE(nullptr, lyr2);
    EXPECT_EQ(2, mds.GetLayerCount());

    // CopyLayer
    OGRLayer* lyr_copy = mds.CopyLayer(lyr2, "layer2_copy", nullptr);
    ASSERT_NE(nullptr, lyr_copy);
    EXPECT_EQ(3, mds.GetLayerCount());

    // DeleteLayer (removes from map)
    EXPECT_EQ(OGRERR_NONE, mds.DeleteLayer(2));
    EXPECT_EQ(2, mds.GetLayerCount());

    // Capabilities
    EXPECT_TRUE(mds.TestCapability(ODsCCreateLayer));
    EXPECT_FALSE(mds.TestCapability("UnknownCapability"));

    // StyleTable
    EXPECT_EQ(nullptr, mds.GetStyleTable());
    mds.SetStyleTableDirectly(new OGRStyleTable());
    EXPECT_NE(nullptr, mds.GetStyleTable());
    OGRStyleTable tbl;
    mds.SetStyleTable(&tbl);

    // ExecuteSQL & ReleaseResultSet
    OGRLayer* res = mds.ExecuteSQL("SELECT * FROM layer1", nullptr, nullptr);
    ASSERT_NE(nullptr, res);
    mds.ReleaseResultSet(res);

    // FlushCache & Transactions
    EXPECT_EQ(CE_None, mds.FlushCache(false));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, mds.StartTransaction(FALSE));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, mds.CommitTransaction());
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, mds.StartTransaction(FALSE));
    EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, mds.RollbackTransaction());

    // Metadata
    EXPECT_EQ(nullptr, mds.GetMetadata());
    char* meta[] = {nullptr};
    EXPECT_EQ(CE_None, mds.SetMetadata(meta));
    EXPECT_EQ(nullptr, mds.GetMetadataItem("key"));
    EXPECT_EQ(CE_None, mds.SetMetadataItem("key", "val"));

    // Domains
    EXPECT_TRUE(mds.GetFieldDomainNames().empty());
    EXPECT_EQ(nullptr, mds.GetFieldDomain("dom"));
    std::string failureReason;
    auto dom = std::make_unique<OGRCodedFieldDomain>(
        "dom", "desc", OFTInteger, OFSTNone, std::vector<OGRCodedValue>{});
    EXPECT_TRUE(mds.AddFieldDomain(std::move(dom), failureReason));
    EXPECT_NE(nullptr, mds.GetFieldDomain("dom"));
    EXPECT_FALSE(mds.GetFieldDomainNames().empty());
    EXPECT_TRUE(mds.DeleteFieldDomain("dom", failureReason));

    // Relationships & RootGroup
    EXPECT_TRUE(mds.GetRelationshipNames().empty());
    EXPECT_EQ(nullptr, mds.GetRelationship("rel"));
    EXPECT_EQ(nullptr, mds.GetRootGroup());
  }  // destructor deletes wrapped layers and base_ds
}

TEST(OGRMutexedDataSourceTest, WithoutMutexAndWithoutLayerWrapping) {
  GDALRegister_MEM();
  auto* driver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, driver);

  GDALDataset* base_ds =
      driver->Create("mem_ds2", 0, 0, 0, GDT_Unknown, nullptr);
  ASSERT_NE(nullptr, base_ds);
  base_ds->CreateLayer("l1", nullptr, wkbPoint, nullptr);

  {
    OGRMutexedDataSource mds(base_ds, /*bTakeOwnership=*/true,
                             /*hMutexIn=*/nullptr,
                             /*bWrapLayersInMutexedLayer=*/false);
    EXPECT_EQ(1, mds.GetLayerCount());
    EXPECT_NE(nullptr, mds.GetLayer(0));
    EXPECT_STREQ("l1", mds.GetLayer(0)->GetName());
  }  // destructor deletes base_ds
}

// TODO: b/335317901 - Cover remaining error branches in
// ogrmutexeddatasource.cpp.

}  // namespace
}  // namespace autotest2
