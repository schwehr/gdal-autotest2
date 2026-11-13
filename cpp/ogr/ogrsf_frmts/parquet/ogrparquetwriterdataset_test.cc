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
// Unit tests for ogrparquetwriterdataset.cpp (OGRParquetWriterDataset).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetwriterdataset.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/parquet/parquet_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/parquet/ogr_parquet.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetwriterdataset_test";

class OgrParquetWriterDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRParquet(); }
};

TEST_F(OgrParquetWriterDatasetTest, CapabilitiesLayersAndDomains) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);

  constexpr char kPath[] = "/vsimem/ogrparquetwriterdataset_test/out.parquet";
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  GDALDatasetUniquePtr ds(drv->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  // 1. Before any layer is created.
  EXPECT_EQ(0, ds->GetLayerCount());
  EXPECT_EQ(nullptr, ds->GetLayer(0));
  EXPECT_EQ(nullptr, ds->GetLayer(-1));
  EXPECT_EQ(TRUE, ds->TestCapability(ODsCCreateLayer));
  EXPECT_EQ(FALSE, ds->TestCapability(ODsCAddFieldDomain));
  EXPECT_EQ(FALSE, ds->TestCapability("UNRELATED_CAP"));
  EXPECT_TRUE(ds->GetFieldDomainNames().empty());
  EXPECT_EQ(nullptr, ds->GetFieldDomain("missing"));

  {
    std::string failure_reason;
    auto domain = std::make_unique<OGRCodedFieldDomain>(
        "pre_layer", "desc", OFTInteger, OFSTNone,
        std::vector<OGRCodedValue>{{CPLStrdup("1"), CPLStrdup("one")},
                                   {nullptr, nullptr}});
    EXPECT_FALSE(ds->AddFieldDomain(std::move(domain), failure_reason));
    EXPECT_THAT(failure_reason, HasSubstr("Layer must be created"));
  }

  // 2. Passing an invalid creation option rejects CreateLayer and resets state.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList bad_opts;
    bad_opts.SetNameValue("COMPRESSION", "NO_SUCH_CODEC");
    EXPECT_EQ(nullptr,
              ds->CreateLayer("bad", nullptr, wkbPoint, bad_opts.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  EXPECT_EQ(0, ds->GetLayerCount());

  // 3. Create the single allowed layer; second CreateLayer must fail.
  OGRLayer* lyr = ds->CreateLayer("only_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(1, ds->GetLayerCount());
  EXPECT_EQ(lyr, ds->GetLayer(0));
  EXPECT_EQ(nullptr, ds->GetLayer(1));
  EXPECT_EQ(FALSE, ds->TestCapability(ODsCCreateLayer));
  EXPECT_EQ(TRUE, ds->TestCapability(ODsCAddFieldDomain));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds->CreateLayer("second", nullptr, wkbPoint, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Can write only one layer in a Parquet file"));
    CPLErrorReset();
  }

  // 4. Add a field domain and query it back before and after Close().
  {
    std::string failure_reason;
    auto domain = std::make_unique<OGRCodedFieldDomain>(
        "status_domain", "status codes", OFTInteger, OFSTNone,
        std::vector<OGRCodedValue>{{CPLStrdup("1"), CPLStrdup("active")},
                                   {nullptr, nullptr}});
    EXPECT_TRUE(ds->AddFieldDomain(std::move(domain), failure_reason))
        << failure_reason;
  }
  EXPECT_THAT(ds->GetFieldDomainNames(), ElementsAre("status_domain"));
  ASSERT_NE(nullptr, ds->GetFieldDomain("status_domain"));
  EXPECT_EQ(nullptr, ds->GetFieldDomain("unknown_domain"));

  OGRFieldDefn fld("status", OFTInteger);
  fld.SetDomainName("status_domain");
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

  // Idempotent explicit Close() followed by destructor.
  EXPECT_EQ(CE_None, ds->Close());
  EXPECT_EQ(CE_None, ds->Close());
  ds.reset();

  // Reopen with reader and verify layer + domain persisted.
  auto ro_ds = OpenParquetDataset(kPath);
  ASSERT_NE(nullptr, ro_ds);
  EXPECT_EQ(1, ro_ds->GetLayerCount());
  EXPECT_THAT(ro_ds->GetFieldDomainNames(), ElementsAre("statusDomain"));
}

TEST_F(OgrParquetWriterDatasetTest, DirectWriterCloseFailurePropagation) {
  auto failing_stream = std::make_shared<ThresholdFailingOutputStream>(1);
  auto writer_ds = std::make_unique<OGRParquetWriterDataset>(failing_stream);
  EXPECT_NE(nullptr, writer_ds->GetMemoryPool());
  OGRLayer* lyr = writer_ds->CreateLayer("fail", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, lyr);

  OGRFeature feat(lyr->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  feat.SetGeometryDirectly(pt.release());
  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, writer_ds->Close());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// TODO: b/335317901 - Cover GDALPamDataset::Close() failure fallback in
// OGRParquetWriterDataset::Close() (lines 53-54).

}  // namespace
}  // namespace autotest2
