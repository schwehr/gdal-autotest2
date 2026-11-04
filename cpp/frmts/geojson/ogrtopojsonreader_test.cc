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

// Tests for the TopoJSON reader.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrtopojsonreader.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrtopojsonreader.cpp

#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogr_geojson.h"
#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrgeojsonreader.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/ogrtopojsonreader_test";

class OGRTopoJSONReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRTopoJSON();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(OGRTopoJSONReaderTest, ParseAndLifecycle) {
  OGRTopoJSONReader reader;

  // ReadLayers before Parse does nothing
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  reader.ReadLayers(poDS.get());
  EXPECT_EQ(poDS->GetLayerCount(), 0);

  // Parse nullptr
  EXPECT_EQ(reader.Parse(nullptr, false), OGRERR_CORRUPT_DATA);

  // Parse corrupt with bLooseIdentification = false (emits error)
  {
    WithQuietHandler quiet;
    EXPECT_EQ(reader.Parse("{corrupt", false), OGRERR_CORRUPT_DATA);
    CPLErrorReset();
  }

  // Parse corrupt with bLooseIdentification = true (quiet)
  EXPECT_EQ(reader.Parse("{corrupt", true), OGRERR_CORRUPT_DATA);
}

TEST_F(OGRTopoJSONReaderTest, InvalidOrMissingArcsAndObjects) {
  // 1. Missing arcs
  {
    const char kJson[] = R"json({"type": "Topology"})json";
    OGRTopoJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get());
    EXPECT_EQ(poDS->GetLayerCount(), 0);
  }

  // 2. arcs is not array
  {
    const char kJson[] = R"json({"type": "Topology", "arcs": 123})json";
    OGRTopoJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get());
    EXPECT_EQ(poDS->GetLayerCount(), 0);
  }

  // 3. objects is not object or array
  {
    const char kJson[] =
        R"json({"type": "Topology", "arcs": [], "objects": "bad"})json";
    OGRTopoJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get());
    EXPECT_EQ(poDS->GetLayerCount(), 0);
  }
}

TEST_F(OGRTopoJSONReaderTest, FullTopologyWithGeometryCollection) {
  // Comprehensive TopoJSON exercising:
  // - transform with scale and translate
  // - crs
  // - Point (2D, 3D, invalid)
  // - MultiPoint (2D, 3D, invalid)
  // - LineString (forward arc, reverse arc, chained, 1-pt reverse,
  // out-of-bounds arc)
  // - MultiLineString
  // - Polygon (valid ring >=4 pts and degenerate ring <4 pts)
  // - MultiPolygon (valid polygon and degenerate empty polygon)
  // - properties & id
  const char kJson[] = R"json({
    "type": "Topology",
    "transform": {
      "scale": [0.1, 0.1],
      "translate": [10.0, 20.0]
    },
    "crs": {
      "type": "name",
      "properties": {"name": "urn:ogc:def:crs:OGC:1.3:CRS84"}
    },
    "objects": {
      "my_layer": {
        "type": "GeometryCollection",
        "id": "my_layer_id",
        "geometries": [
          {
            "type": "Point",
            "coordinates": [10, 20],
            "id": 101,
            "properties": {"name": "pt2d"}
          },
          {
            "type": "Point",
            "coordinates": [10, 20, 30],
            "id": "str_id_102",
            "properties": {"name": "pt3d"}
          },
          {
            "type": "Point",
            "coordinates": [1],
            "properties": null
          },
          {
            "type": "MultiPoint",
            "coordinates": [[10, 20], [30, 40, 50], [1], null],
            "properties": {"name": "mp"}
          },
          {
            "type": "LineString",
            "arcs": [0, 1, -2, -4, 999, "invalid"],
            "properties": {"name": "ls"}
          },
          {
            "type": "MultiLineString",
            "arcs": [[0, -1], "invalid"],
            "properties": {"name": "mls"}
          },
          {
            "type": "Polygon",
            "arcs": [
              [0, 2],
              [3]
            ],
            "properties": {"name": "poly"}
          },
          {
            "type": "MultiPolygon",
            "arcs": [
              [[0, 2]],
              [[3]]
            ],
            "properties": {"name": "mpoly"}
          }
        ]
      }
    },
    "arcs": [
      [[0, 0], [10, 10], [10, 0]],
      [[20, 10], [10, 10]],
      [[-20, -10], [0, -10]],
      [[5, 5]],
      [[1]]
    ]
  })json";

  WithQuietHandler quiet;
  OGRTopoJSONReader reader;
  EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);

  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  reader.ReadLayers(poDS.get());
  ASSERT_EQ(poDS->GetLayerCount(), 1);

  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(poLayer, nullptr);
  EXPECT_STREQ(poLayer->GetName(), "my_layer");
  EXPECT_EQ(poLayer->GetFeatureCount(), 8);

  CPLErrorReset();
}

TEST_F(OGRTopoJSONReaderTest, TopLevelGeometriesSecondPassObjectAndArray) {
  // 1. "objects" as a JSON Object with direct geometry objects (not inside
  // GeometryCollection) This sets bNeedSecondPass = true and executes
  // ParseObjectMainSecondPass
  {
    const char kJson[] = R"json({
      "type": "Topology",
      "objects": {
        "pt": {
          "type": "Point",
          "coordinates": [100.0, 200.0],
          "properties": {"name": "direct_point"}
        },
        "ls": {
          "type": "LineString",
          "arcs": [0],
          "properties": {"name": "direct_linestring"}
        }
      },
      "arcs": [
        [[10.0, 20.0], [30.0, 40.0]]
      ]
    })json";

    OGRTopoJSONReader reader;
    EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    poDS->SetDescription("TopoJSON:/vsimem/direct.topojson");
    reader.ReadLayers(poDS.get());
    ASSERT_EQ(poDS->GetLayerCount(), 1);
    OGRLayer* poLayer = poDS->GetLayer(0);
    ASSERT_NE(poLayer, nullptr);
    EXPECT_STREQ(poLayer->GetName(), "TopoJSON");
    EXPECT_EQ(poLayer->GetFeatureCount(), 2);
  }

  // 2. "objects" as a JSON Array with both GeometryCollection and direct
  // geometries
  {
    const char kJsonArray[] = R"json({
      "type": "Topology",
      "objects": [
        {
          "type": "GeometryCollection",
          "id": 12345,
          "geometries": [
            {
              "type": "Point",
              "coordinates": [5.0, 6.0],
              "properties": {"k": "v"}
            }
          ]
        },
        {
          "type": "Point",
          "coordinates": [7.0, 8.0],
          "properties": {"k2": "v2"}
        }
      ],
      "arcs": []
    })json";

    OGRTopoJSONReader reader;
    EXPECT_EQ(reader.Parse(kJsonArray, false), OGRERR_NONE);
    auto poDS = std::make_unique<OGRGeoJSONDataSource>();
    reader.ReadLayers(poDS.get());
    ASSERT_EQ(poDS->GetLayerCount(), 2);
  }
}

TEST_F(OGRTopoJSONReaderTest, InvalidTransformHandling) {
  // Invalid scale (not 2 elements) and invalid translate
  const char kJson[] = R"json({
    "type": "Topology",
    "transform": {
      "scale": [1.0],
      "translate": "invalid"
    },
    "objects": {
      "layer": {
        "type": "Point",
        "coordinates": [10.0, 20.0]
      }
    },
    "arcs": []
  })json";

  OGRTopoJSONReader reader;
  EXPECT_EQ(reader.Parse(kJson, false), OGRERR_NONE);
  auto poDS = std::make_unique<OGRGeoJSONDataSource>();
  reader.ReadLayers(poDS.get());
  ASSERT_EQ(poDS->GetLayerCount(), 1);
}

}  // namespace
}  // namespace autotest2
