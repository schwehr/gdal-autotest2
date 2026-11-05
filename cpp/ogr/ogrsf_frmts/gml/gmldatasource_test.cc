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

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/ogr_gml.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

TEST(GmlDatasourceTest, DoNotOpenKml) {
  // https://trac.osgeo.org/gdal/changeset/40256
  constexpr std::string_view kKml =
      "<kml><Document>  <!-- opengis.net/gml -->"
      "  <Placemark><Point>"
      "    <coordinates>-1,2,0</coordinates>"
      "  </Point></Placemark>"
      "</Document></kml>";

  constexpr char kFilename[] = "/vsimem/a.gml";
  VsiMemTempWrapper wrapper(kFilename, std::string(kKml));
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRGMLDataSource>();
  EXPECT_FALSE(dataset->Open(open_info.get()));
}

TEST(GmlDatasourceTest, Empty) {
  CPLErrorReset();
  // ogr_gml_54
  constexpr std::string_view kGml =
      R"gml(<?xml version="1.0" encoding="utf-8" ?>
      <ogr:FeatureCollection
           xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
           xmlns:ogr="http://ogr.maptools.org/"
           xmlns:gml="http://www.opengis.net/gml">
        <gml:boundedBy><gml:null>missing</gml:null></gml:boundedBy>
      </ogr:FeatureCollection>
      )gml";
  constexpr char kFilename[] = "/vsimem/empty.gml";
  VsiMemTempWrapper wrapper(kFilename, std::string(kGml));
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRGMLDataSource>();
  EXPECT_TRUE(dataset->Open(open_info.get()));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(0, dataset->GetLayerCount());
}

TEST(GmlDatasourceTest, Point) {
  CPLErrorReset();
  // ogr_gml_54
  constexpr std::string_view kGml =
      R"gml(<?xml version="1.0" encoding="utf-8" ?>
      <ogr:FeatureCollection
           xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
           xmlns:ogr="http://ogr.maptools.org/"
           xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Box>
      <gml:coord><gml:X>100</gml:X><gml:Y>0</gml:Y></gml:coord>
      <gml:coord><gml:X>100</gml:X><gml:Y>0</gml:Y></gml:coord>
    </gml:Box>
  </gml:boundedBy>

  <gml:featureMember>
    <ogr:point fid="point.0">
      <ogr:geometryProperty><gml:Point srsName="EPSG:4326">
        <gml:coordinates>100,0</gml:coordinates>
      </gml:Point></ogr:geometryProperty>
    </ogr:point>
  </gml:featureMember>
</ogr:FeatureCollection>)gml";
  constexpr char kFilename[] = "/vsimem/point.gml";
  VsiMemTempWrapper wrapper(kFilename, std::string(kGml));
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRGMLDataSource>();
  EXPECT_TRUE(dataset->Open(open_info.get()));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  ASSERT_EQ(1, dataset->GetLayerCount());

  // TODO(schwehr): Test the layer and the feature.
}

// TODO(schwehr): Add more test coverage.

}  // namespace
}  // namespace autotest2
