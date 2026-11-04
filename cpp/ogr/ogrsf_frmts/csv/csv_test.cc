// Copyright 2017 Google Inc. All Rights Reserved.
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
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
// Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris.org>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the
// Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

// Tests CSV OGR driver.
//
// See also:
//   https://gdal.org/drivers/vector/csv.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_csv.py

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/cord.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class CsvTest : public ::testing::Test {
  void SetUp() override {
    RegisterOGRCSV();
    RegisterOGRVRT();
  }

  void TearDown() override {
    OGRCleanupAll();
  }
};

// TODO(schwehr): Add tests from ogr_csv.py.

TEST_F(CsvTest, MultiGeomViaVrt) {
  // Use a VRT to map sane field names onto a CSV file.
  constexpr char kCsv[] =
      "a,b,c,d,e\n"
      "1,\"POINT (2 3)\",4,\"POINT (5 6)\",3.14\n";
  constexpr char kFilenameCsv[] = "/vsimem/multi-geom-col.csv";
  const std::string data_csv(kCsv, CPL_ARRAYSIZE(kCsv));
  autotest2::VsiMemTempWrapper wrapper_csv(kFilenameCsv, data_csv);

  constexpr char kVrt[] =
      R"vrt(<OGRVRTDataSource>
          <OGRVRTLayer name="remapped_layer">
              <SrcDataSource>/vsimem/multi-geom-col.csv</SrcDataSource>
              <SrcLayer>multi-geom-col</SrcLayer>
              <LayerSRS>WGS84</LayerSRS>
              <Field name="A" src="a"/>
              <GeometryField name="B" field="b" encoding="WKT">
                <GeometryType>wkbPoint</GeometryType>
              </GeometryField>
              <Field name="C" src="c" type="Integer"/>
              <GeometryField name="D" field="d" encoding="WKT"/>
              <Field name="E" src="e" type="Real"/>
          </OGRVRTLayer>
        </OGRVRTDataSource>)vrt";

  constexpr char kFilenameVrt[] = "/vsimem/multi-geom-col.vrt";
  const std::string data_vrt(kVrt, CPL_ARRAYSIZE(kVrt));
  autotest2::VsiMemTempWrapper wrapper_vrt(kFilenameVrt, data_vrt);

  std::unique_ptr<GDALDataset> dataset(static_cast<GDALDataset *>(
      GDALOpenEx(kFilenameVrt, GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr,
                 nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, dataset);

  ASSERT_EQ(1, dataset->GetLayerCount());
  OGRLayer *layer = dataset->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbPoint, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());

  OGRFeatureDefn *feature_defn = layer->GetLayerDefn();
  ASSERT_NE(nullptr, feature_defn);
  EXPECT_EQ(3, feature_defn->GetFieldCount());
  EXPECT_EQ(2, feature_defn->GetGeomFieldCount());
  EXPECT_STREQ("A", feature_defn->GetFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("C", feature_defn->GetFieldDefn(1)->GetNameRef());
  EXPECT_STREQ("E", feature_defn->GetFieldDefn(2)->GetNameRef());

  EXPECT_STREQ("B", feature_defn->GetGeomFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("D", feature_defn->GetGeomFieldDefn(1)->GetNameRef());

  auto feature = absl::WrapUnique(layer->GetNextFeature());
  EXPECT_STREQ("1", feature->GetFieldAsString("A"));
  EXPECT_EQ(4, feature->GetFieldAsInteger("C"));
  EXPECT_DOUBLE_EQ(3.14, feature->GetFieldAsDouble("E"));

  {
    auto geom = feature->GetGeomFieldRef("B");
    OGRPoint point(2, 3);
    EXPECT_TRUE(point.Equals(geom));
  }

  {
    auto geom = feature->GetGeomFieldRef("D");
    OGRPoint point(5, 6);
    EXPECT_TRUE(point.Equals(geom));
  }
}

}  // namespace
}  // namespace autotest2
