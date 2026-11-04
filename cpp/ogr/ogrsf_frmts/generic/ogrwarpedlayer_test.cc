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

// Tests for OGRWarpedLayer.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrwarpedlayer.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrwarpedlayer.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrwarpedlayer.h"

#include <cmath>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

TEST(OGRWarpedLayerTest, FullLifecycleWithReversedCT) {
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto mem_layer =
      std::make_unique<OGRMemLayer>("src_layer", &srs4326, wkbPoint);

  OGRFieldDefn fld("val", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, mem_layer->CreateField(&fld));

  {
    auto feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat->SetField(0, 10);
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    feat->SetGeometry(pt.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(feat.get()));
  }

  auto* poCT = OGRCreateCoordinateTransformation(&srs4326, &srs3857);
  ASSERT_NE(nullptr, poCT);
  auto* poReversedCT = OGRCreateCoordinateTransformation(&srs3857, &srs4326);
  ASSERT_NE(nullptr, poReversedCT);

  OGRWarpedLayer warped(mem_layer.get(), /*iGeomField=*/0,
                        /*bTakeOwnership=*/false, poCT, poReversedCT);

  EXPECT_NE(nullptr, warped.GetLayerDefn());
  EXPECT_NE(nullptr, warped.GetSpatialRef());
  EXPECT_EQ(1, warped.GetFeatureCount(FALSE));
  EXPECT_EQ(1, warped.GetFeatureCount(TRUE));

  // TranslateFeature
  {
    auto src_feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    src_feat->SetField(0, 20);
    auto pt = std::make_unique<OGRPoint>(0.0, 0.0);
    src_feat->SetGeometry(pt.get());

    std::vector<std::unique_ptr<OGRFeature>> out_features;
    warped.TranslateFeature(std::move(src_feat), out_features);
    ASSERT_EQ(1, out_features.size());
    EXPECT_EQ(20, out_features[0]->GetFieldAsInteger(0));
  }

  // Feature reading
  warped.ResetReading();
  OGRFeature* read_feat = warped.GetNextFeature();
  ASSERT_NE(nullptr, read_feat);
  EXPECT_EQ(10, read_feat->GetFieldAsInteger(0));
  OGRFeature::DestroyFeature(read_feat);

  OGRFeature* by_fid = warped.GetFeature(0);
  ASSERT_NE(nullptr, by_fid);
  by_fid->SetField(0, 30);
  EXPECT_EQ(OGRERR_NONE, warped.ISetFeature(by_fid));
  OGRFeature::DestroyFeature(by_fid);

  // Write operations
  {
    auto new_feat = std::make_unique<OGRFeature>(warped.GetLayerDefn());
    new_feat->SetFID(1);
    new_feat->SetField(0, 40);
    auto pt = std::make_unique<OGRPoint>(1113194.9, 2273030.9);
    new_feat->SetGeometry(pt.get());
    EXPECT_EQ(OGRERR_NONE, warped.ICreateFeature(new_feat.get()));
    EXPECT_EQ(OGRERR_NONE, warped.IUpsertFeature(new_feat.get()));
    int updated = 0;
    EXPECT_EQ(OGRERR_NONE, warped.IUpdateFeature(new_feat.get(), 1, &updated, 0,
                                                 nullptr, false));
  }

  // Extent
  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, warped.IGetExtent(0, &env, true));
  EXPECT_EQ(OGRERR_FAILURE, warped.IGetExtent(10, &env, true));

  // SetExtent overrides computed extent
  warped.SetExtent(0.0, 0.0, 100.0, 100.0);
  OGREnvelope env2;
  EXPECT_EQ(OGRERR_NONE, warped.IGetExtent(0, &env2, true));
  EXPECT_DOUBLE_EQ(0.0, env2.MinX);
  EXPECT_DOUBLE_EQ(100.0, env2.MaxX);

  // Spatial filter with infinite / null / valid envelope
  EXPECT_EQ(OGRERR_NONE, warped.ISetSpatialFilter(0, nullptr));

  OGRPoint filter_pt(1113194.9, 2273030.9);
  EXPECT_EQ(OGRERR_NONE, warped.ISetSpatialFilter(0, &filter_pt));

  EXPECT_EQ(OGRERR_FAILURE, warped.ISetSpatialFilter(5, &filter_pt));

  // ArrowStream
  ArrowArrayStream stream;
  EXPECT_TRUE(warped.GetArrowStream(&stream, nullptr));
  if (stream.release) {
    stream.release(&stream);
  }

  // Capabilities
  EXPECT_TRUE(warped.TestCapability(OLCRandomRead));
  EXPECT_TRUE(warped.TestCapability(OLCRandomWrite));
  EXPECT_TRUE(warped.TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(warped.TestCapability("Unknown"));
}

TEST(OGRWarpedLayerTest, WithoutReversedCT) {
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto mem_layer =
      std::make_unique<OGRMemLayer>("src_layer2", &srs4326, wkbPoint);

  auto* poCT = OGRCreateCoordinateTransformation(&srs4326, &srs3857);
  ASSERT_NE(nullptr, poCT);

  // Pass poReversedCT = nullptr
  OGRWarpedLayer warped(mem_layer.get(), /*iGeomField=*/0,
                        /*bTakeOwnership=*/false, poCT,
                        /*poReversedCT=*/nullptr);

  // When poReversedCT is nullptr and feature has geometry, write methods should
  // fail
  {
    auto feat = std::make_unique<OGRFeature>(warped.GetLayerDefn());
    OGRPoint pt(10.0, 20.0);
    feat->SetGeometry(&pt);
    EXPECT_EQ(OGRERR_FAILURE, warped.ISetFeature(feat.get()));
    EXPECT_EQ(OGRERR_FAILURE, warped.ICreateFeature(feat.get()));
    EXPECT_EQ(OGRERR_FAILURE, warped.IUpsertFeature(feat.get()));
    int updated = 0;
    EXPECT_EQ(OGRERR_FAILURE, warped.IUpdateFeature(feat.get(), 0, &updated, 0,
                                                    nullptr, false));
  }

  OGRPoint pt(10.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, warped.ISetSpatialFilter(0, &pt));
  EXPECT_FALSE(warped.TestCapability(OLCRandomWrite));
}

TEST(OGRWarpedLayerTest, ExtentAcrossDiscontinuity) {
  OGRSpatialReference srs_src;
  srs_src.importFromProj4("+proj=lonlat +datum=WGS84 +no_defs");
  srs_src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs_dst;
  srs_dst.importFromProj4("+proj=merc +lon_0=150 +datum=WGS84 +no_defs");
  srs_dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto mem_layer =
      std::make_unique<OGRMemLayer>("discont_layer", &srs_src, wkbPoint);

  {
    auto f1 = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    auto pt1 = std::make_unique<OGRPoint>(-179.0, 10.0);
    f1->SetGeometry(pt1.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(f1.get()));

    auto f2 = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    auto pt2 = std::make_unique<OGRPoint>(179.0, 10.0);
    f2->SetGeometry(pt2.get());
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(f2.get()));
  }

  auto* poCT = OGRCreateCoordinateTransformation(&srs_src, &srs_dst);
  ASSERT_NE(nullptr, poCT);
  {
    OGRWarpedLayer warped(mem_layer.get(), 0, false, poCT, nullptr);
    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, warped.IGetExtent(0, &env, true));
  }
}

// TODO: b/335317901 - Cover remaining error branches in ogrwarpedlayer.cpp.

}  // namespace
}  // namespace autotest2
