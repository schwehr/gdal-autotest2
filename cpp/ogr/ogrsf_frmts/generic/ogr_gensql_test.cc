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

// Tests for OGRGenSQLResultsLayer and GDAL's OGRSQL execution engine.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogr_gensql.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogr_gensql.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogr_gensql.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

class FastArrowMemLayer : public OGRMemLayer {
 public:
  FastArrowMemLayer(const char* pszName, const OGRSpatialReference* poSRSIn,
                    OGRwkbGeometryType eType)
      : OGRMemLayer(pszName, poSRSIn, eType) {}

  int TestCapability(const char* pszCap) const override {
    if (EQUAL(pszCap, OLCFastGetArrowStream)) return TRUE;
    if (EQUAL(pszCap, OLCIgnoreFields)) return TRUE;
    return OGRMemLayer::TestCapability(pszCap);
  }
};

class FastArrowDataset : public GDALDataset {
 public:
  ~FastArrowDataset() override = default;

  void AddLayer(std::unique_ptr<FastArrowMemLayer> layer) {
    layers_.push_back(std::move(layer));
  }

  int GetLayerCount() const override {
    return static_cast<int>(layers_.size());
  }

  OGRLayer* GetLayer(int i) const override {
    if (i >= 0 && i < static_cast<int>(layers_.size())) {
      return layers_[i].get();
    }
    return nullptr;
  }

  OGRLayer* GetLayerByName(const char* name) override {
    for (auto& l : layers_) {
      if (EQUAL(l->GetName(), name)) return l.get();
    }
    return nullptr;
  }

 private:
  std::vector<std::unique_ptr<FastArrowMemLayer>> layers_;
};

std::unique_ptr<FastArrowDataset> CreatePopulatedDataset() {
  auto ds = std::make_unique<FastArrowDataset>();

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // layer1: 105 features
  auto l1 = std::make_unique<FastArrowMemLayer>("layer1", &srs, wkbPolygon);
  l1->SetMetadataItem(OLMD_FID64, "YES");

  OGRFieldDefn f_int("f_int", OFTInteger);
  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  OGRFieldDefn f_real("f_real", OFTReal);
  OGRFieldDefn f_str("f_str", OFTString);
  f_str.SetWidth(20);
  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  OGRFieldDefn f_date("f_date", OFTDate);
  OGRFieldDefn f_time("f_time", OFTTime);
  OGRFieldDefn f_dt("f_dt", OFTDateTime);

  l1->CreateField(&f_int);
  l1->CreateField(&f_int64);
  l1->CreateField(&f_real);
  l1->CreateField(&f_str);
  l1->CreateField(&f_bool);
  l1->CreateField(&f_date);
  l1->CreateField(&f_time);
  l1->CreateField(&f_dt);

  for (int i = 0; i < 103; ++i) {
    auto f = std::make_unique<OGRFeature>(l1->GetLayerDefn());
    f->SetFID(i);
    f->SetField(0, i);
    f->SetField(1, static_cast<GIntBig>(i * 1000));
    f->SetField(2, i * 1.5);
    f->SetField(3, CPLSPrintf("str_%03d", i));
    f->SetField(4, i % 2);
    f->SetField(5, 2026, 1, 1 + (i % 25), 0, 0, 0, 0);
    f->SetField(6, 0, 0, 0, 12, i % 60, 0, 0);
    f->SetField(7, 2026, 1, 1 + (i % 25), 12, i % 60, 0, 0);

    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(0, 1);
    ring->addPoint(1, 1);
    ring->addPoint(1, 0);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    f->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, l1->CreateFeature(f.get()));
  }

  // FIDs 103 and 104 with NULL fields
  {
    auto f103 = std::make_unique<OGRFeature>(l1->GetLayerDefn());
    f103->SetFID(103);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(0, 1);
    ring->addPoint(1, 1);
    ring->addPoint(1, 0);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    f103->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, l1->CreateFeature(f103.get()));

    auto f104 = std::make_unique<OGRFeature>(l1->GetLayerDefn());
    f104->SetFID(104);
    EXPECT_EQ(OGRERR_NONE, l1->CreateFeature(f104.get()));
  }
  ds->AddLayer(std::move(l1));

  // layer2: join target (5 features)
  auto l2 = std::make_unique<FastArrowMemLayer>("layer2", nullptr, wkbNone);
  OGRFieldDefn j_int("j_int", OFTInteger);
  OGRFieldDefn j_int64("j_int64", OFTInteger64);
  OGRFieldDefn j_real("j_real", OFTReal);
  OGRFieldDefn j_str("j_str", OFTString);
  l2->CreateField(&j_int);
  l2->CreateField(&j_int64);
  l2->CreateField(&j_real);
  l2->CreateField(&j_str);

  for (int i = 0; i < 5; ++i) {
    auto f = std::make_unique<OGRFeature>(l2->GetLayerDefn());
    f->SetFID(i);
    f->SetField(0, i);
    f->SetField(1, static_cast<GIntBig>(i * 1000));
    f->SetField(2, i * 1.5);
    f->SetField(3, CPLSPrintf("join_%03d", i));
    EXPECT_EQ(OGRERR_NONE, l2->CreateFeature(f.get()));
  }
  ds->AddLayer(std::move(l2));

  // layer_sorted: 3 features in ascending order
  auto l_sorted =
      std::make_unique<FastArrowMemLayer>("layer_sorted", &srs, wkbLineString);
  OGRFieldDefn s_int("f_int", OFTInteger);
  l_sorted->CreateField(&s_int);
  for (int i = 0; i < 3; ++i) {
    auto f = std::make_unique<OGRFeature>(l_sorted->GetLayerDefn());
    f->SetFID(i);
    f->SetField(0, i);
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0, 0);
    line->addPoint(i + 1, i + 1);
    f->SetGeometryDirectly(line.release());
    EXPECT_EQ(OGRERR_NONE, l_sorted->CreateFeature(f.get()));
  }
  ds->AddLayer(std::move(l_sorted));

  // layer_multi: MultiPolygon
  auto l_multi =
      std::make_unique<FastArrowMemLayer>("layer_multi", &srs, wkbMultiPolygon);
  {
    auto f = std::make_unique<OGRFeature>(l_multi->GetLayerDefn());
    f->SetFID(0);
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(0, 1);
    ring->addPoint(1, 1);
    ring->addPoint(1, 0);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    mp->addGeometryDirectly(poly.release());
    f->SetGeometryDirectly(mp.release());
    EXPECT_EQ(OGRERR_NONE, l_multi->CreateFeature(f.get()));
  }
  ds->AddLayer(std::move(l_multi));

  // layer_mls: MultiLineString
  auto l_mls = std::make_unique<FastArrowMemLayer>("layer_mls", &srs,
                                                   wkbMultiLineString);
  {
    auto f = std::make_unique<OGRFeature>(l_mls->GetLayerDefn());
    f->SetFID(0);
    auto mls = std::make_unique<OGRMultiLineString>();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mls->addGeometryDirectly(ls.release());
    f->SetGeometryDirectly(mls.release());
    EXPECT_EQ(OGRERR_NONE, l_mls->CreateFeature(f.get()));
  }
  ds->AddLayer(std::move(l_mls));

  return ds;
}

TEST(OGRGenSQLTest, SelectExpressionsCastsAndFilters) {
  auto ds = CreatePopulatedDataset();

  const char* sql =
      "SELECT f_int, f_int64, f_real, f_str, f_bool, "
      "CAST(f_int AS integer), CAST(f_int AS bigint), CAST(f_int AS boolean), "
      "CAST(f_int AS float), CAST(f_int AS character(15)), "
      "CAST(f_str AS timestamp), CAST(f_str AS date), CAST(f_str AS time), "
      "CAST(OGR_GEOMETRY AS GEOMETRY(MULTIPOLYGON, 4326)) AS g_mp, "
      "f_int + 1, f_real * 2.0, CONCAT(f_str, '_x'), "
      "FID, OGR_GEOMETRY, OGR_GEOM_WKT, OGR_GEOM_AREA, "
      "f_str AS OGR_STYLE HIDDEN "
      "FROM layer1 WHERE OGR_GEOM_AREA >= 0 LIMIT 5 OFFSET 1";

  OGRLayer* lyr = ds->ExecuteSQL(sql, nullptr, "OGRSQL");
  ASSERT_NE(nullptr, lyr);

  EXPECT_EQ(nullptr, lyr->GetSpatialFilter());
  const OGRFeatureDefn* defn = lyr->GetLayerDefn();
  ASSERT_NE(nullptr, defn);
  EXPECT_GT(defn->GetFieldCount(), 15);
  EXPECT_EQ(wkbMultiPolygon, lyr->GetGeomType());

  EXPECT_EQ(4, lyr->GetFeatureCount(true));

  OGREnvelope env;
  EXPECT_EQ(OGRERR_FAILURE, lyr->GetExtent(0, &env, true));

  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter("f_int >= 0"));
  EXPECT_EQ(OGRERR_NONE, lyr->SetAttributeFilter(nullptr));

  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(-1, -1);
  ring.addPoint(-1, 2);
  ring.addPoint(2, 2);
  ring.addPoint(2, -1);
  ring.addPoint(-1, -1);
  poly.addRing(&ring);
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilter(0, &poly));
  EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilter(0, nullptr));

  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(-1));
  EXPECT_EQ(OGRERR_NON_EXISTING_FEATURE, lyr->SetNextByIndex(10));
  EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));

  int count = 0;
  for (auto&& feat : *lyr) {
    count++;
    EXPECT_NE(nullptr, feat);
  }
  EXPECT_EQ(5, count);

  std::unique_ptr<OGRFeature> f0(lyr->GetFeature(0));
  EXPECT_NE(nullptr, f0);

  ds->ReleaseResultSet(lyr);

  // Initial spatial filter passed to ExecuteSQL & implicit geometry
  OGRPoint pt(0.5, 0.5);
  OGRLayer* lyr2 =
      ds->ExecuteSQL("SELECT * FROM layer1 LIMIT 2", &pt, "OGRSQL");
  ASSERT_NE(nullptr, lyr2);
  EXPECT_NE(nullptr, lyr2->GetLayerDefn());
  int cnt2 = 0;
  for (auto&& f : *lyr2) {
    cnt2++;
    EXPECT_NE(nullptr, f);
  }
  EXPECT_EQ(2, cnt2);
  ds->ReleaseResultSet(lyr2);

  // Computed OGR_STYLE HIDDEN and invalid type warning
  OGRLayer* lyr3 = ds->ExecuteSQL(
      "SELECT CONCAT(f_str, '_s') AS OGR_STYLE HIDDEN, f_int FROM layer1 LIMIT "
      "2",
      nullptr, "OGRSQL");
  ASSERT_NE(nullptr, lyr3);
  for (auto&& f : *lyr3) {
    EXPECT_NE(nullptr, f);
  }
  ds->ReleaseResultSet(lyr3);

  {
    WithQuietHandler quiet_handler;
    OGRLayer* lyr_warn = ds->ExecuteSQL(
        "SELECT f_int AS OGR_STYLE HIDDEN, f_str FROM layer1 LIMIT 1", nullptr,
        "OGRSQL");
    ASSERT_NE(nullptr, lyr_warn);
    for (auto&& f : *lyr_warn) {
      EXPECT_NE(nullptr, f);
    }
    ds->ReleaseResultSet(lyr_warn);
  }

  // Geometry type forcing
  OGRLayer* lyr_g1 = ds->ExecuteSQL(
      "SELECT CAST(OGR_GEOMETRY AS GEOMETRY(POLYGON)) FROM layer_multi",
      nullptr, "OGRSQL");
  ASSERT_NE(nullptr, lyr_g1);
  for (auto&& f : *lyr_g1) {
    EXPECT_NE(nullptr, f);
  }
  ds->ReleaseResultSet(lyr_g1);

  OGRLayer* lyr_g2 = ds->ExecuteSQL(
      "SELECT CAST(OGR_GEOMETRY AS GEOMETRY(MULTILINESTRING)) FROM "
      "layer_sorted",
      nullptr, "OGRSQL");
  ASSERT_NE(nullptr, lyr_g2);
  for (auto&& f : *lyr_g2) {
    EXPECT_NE(nullptr, f);
  }
  ds->ReleaseResultSet(lyr_g2);

  OGRLayer* lyr_g3 = ds->ExecuteSQL(
      "SELECT CAST(OGR_GEOMETRY AS GEOMETRY(LINESTRING)) FROM layer_mls",
      nullptr, "OGRSQL");
  ASSERT_NE(nullptr, lyr_g3);
  for (auto&& f : *lyr_g3) {
    EXPECT_NE(nullptr, f);
  }
  ds->ReleaseResultSet(lyr_g3);

  OSRCleanup();
}

TEST(OGRGenSQLTest, AggregatesAndDistinct) {
  auto ds = CreatePopulatedDataset();

  // COUNT(*)
  {
    OGRLayer* lyr =
        ds->ExecuteSQL("SELECT COUNT(*) FROM layer1", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(1, lyr->GetFeatureCount(true));
    EXPECT_TRUE(lyr->TestCapability(OLCFastFeatureCount));
    EXPECT_TRUE(lyr->TestCapability(OLCFastSetNextByIndex));

    OGREnvelope env;
    EXPECT_EQ(OGRERR_FAILURE, lyr->GetExtent(0, &env, true));

    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(105, f->GetFieldAsInteger(0));

    EXPECT_EQ(nullptr, lyr->GetNextFeature());
    EXPECT_EQ(nullptr, lyr->GetFeature(1));

    std::unique_ptr<OGRFeature> f_fid0(lyr->GetFeature(0));
    EXPECT_NE(nullptr, f_fid0);

    ds->ReleaseResultSet(lyr);
  }

  // Multi-aggregate
  {
    const char* sql =
        "SELECT COUNT(*), COUNT(f_int), COUNT(DISTINCT f_str), "
        "COUNT(OGR_GEOMETRY), AVG(f_real), AVG(f_date), MIN(f_int), "
        "MIN(f_str), MIN(f_date), MAX(f_int), MAX(f_str), MAX(f_date), "
        "SUM(f_real), STDDEV_POP(f_real), STDDEV_SAMP(f_real) FROM layer1";
    OGRLayer* lyr = ds->ExecuteSQL(sql, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(105, f->GetFieldAsInteger(0));  // COUNT(*)
    EXPECT_EQ(103, f->GetFieldAsInteger(1));  // COUNT(f_int)
    EXPECT_GT(f->GetFieldAsDouble(4), 0.0);   // AVG(f_real)
    ds->ReleaseResultSet(lyr);
  }

  // DISTINCT without ORDER BY
  {
    OGRLayer* lyr =
        ds->ExecuteSQL("SELECT DISTINCT f_str FROM layer1", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    GIntBig count = lyr->GetFeatureCount(true);
    EXPECT_GE(count, 103);
    EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));

    for (auto&& f : *lyr) {
      EXPECT_NE(nullptr, f);
    }
    EXPECT_EQ(nullptr, lyr->GetFeature(-1));
    EXPECT_EQ(nullptr, lyr->GetFeature(9999));
    ds->ReleaseResultSet(lyr);
  }

  // DISTINCT with ORDER BY ASC / DESC
  {
    OGRLayer* lyr_asc =
        ds->ExecuteSQL("SELECT DISTINCT f_str FROM layer1 ORDER BY f_str ASC",
                       nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr_asc);
    EXPECT_GE(lyr_asc->GetFeatureCount(true), 103);
    for (auto&& f : *lyr_asc) {
      EXPECT_NE(nullptr, f);
    }
    EXPECT_EQ(nullptr, lyr_asc->GetFeature(-1));
    EXPECT_EQ(nullptr, lyr_asc->GetFeature(9999));
    ds->ReleaseResultSet(lyr_asc);

    OGRLayer* lyr_desc =
        ds->ExecuteSQL("SELECT DISTINCT f_str FROM layer1 ORDER BY f_str DESC",
                       nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr_desc);
    for (auto&& f : *lyr_desc) {
      EXPECT_NE(nullptr, f);
    }
    ds->ReleaseResultSet(lyr_desc);
  }

  OSRCleanup();
}

TEST(OGRGenSQLTest, JoinsAndOrderBy) {
  auto ds = CreatePopulatedDataset();

  // JOIN query across layer1 and layer2
  {
    const char* join_sql =
        "SELECT layer1.f_int, layer2.j_str, layer1.f_int + layer2.j_int "
        "FROM layer1 JOIN layer2 ON layer1.f_int = layer2.j_int "
        "AND layer1.f_int64 = layer2.j_int64 AND layer1.f_real = layer2.j_real "
        "AND layer1.f_str = layer2.j_str "
        "AND layer2.j_int >= 0";

    OGRLayer* lyr = ds->ExecuteSQL(join_sql, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    int count = 0;
    for (auto&& f : *lyr) {
      count++;
      EXPECT_NE(nullptr, f);
    }
    EXPECT_EQ(105, count);
    ds->ReleaseResultSet(lyr);
  }

  // Join failure with secondary dataset
  {
    WithQuietHandler quiet_handler;
    OGRLayer* lyr_fail = ds->ExecuteSQL(
        "SELECT * FROM layer1 JOIN \"/vsimem/non_existent.shp\".layer2 ON "
        "layer1.f_int = layer2.j_int",
        nullptr, "OGRSQL");
    EXPECT_EQ(nullptr, lyr_fail);
  }

  // Fast ORDER BY ... LIMIT 1
  {
    OGRLayer* lyr = ds->ExecuteSQL(
        "SELECT f_int FROM layer1 ORDER BY f_int DESC, f_str ASC LIMIT 1",
        nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(102, f->GetFieldAsInteger(0));
    ds->ReleaseResultSet(lyr);
  }

  // Merge-sort ORDER BY (> 100 features)
  {
    const char* sort_sql =
        "SELECT f_int FROM layer1 ORDER BY f_int DESC, f_int64 ASC, "
        "f_real DESC, f_str ASC, f_date ASC, FID DESC, "
        "OGR_GEOM_AREA ASC, OGR_GEOM_WKT DESC";

    OGRLayer* lyr = ds->ExecuteSQL(sort_sql, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_TRUE(lyr->TestCapability(OLCFastSetNextByIndex));
    EXPECT_EQ(OGRERR_NONE, lyr->SetNextByIndex(0));

    int prev_val = 999999;
    int count = 0;
    for (auto&& f : *lyr) {
      count++;
      if (f->IsFieldSetAndNotNull(0)) {
        int val = f->GetFieldAsInteger(0);
        EXPECT_LE(val, prev_val);
        prev_val = val;
      }
    }
    EXPECT_EQ(105, count);
    ds->ReleaseResultSet(lyr);
  }

  // Already-sorted ORDER BY
  {
    OGRLayer* lyr = ds->ExecuteSQL(
        "SELECT f_int FROM layer_sorted ORDER BY f_int ASC", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    int count = 0;
    for (auto&& f : *lyr) {
      count++;
      EXPECT_NE(nullptr, f);
    }
    EXPECT_EQ(3, count);
    ds->ReleaseResultSet(lyr);
  }

  OSRCleanup();
}

TEST(OGRGenSQLTest, ArrowStreamForwardingAndFallback) {
  auto ds = CreatePopulatedDataset();

  // Forwarded stream query
  {
    OGRLayer* lyr = ds->ExecuteSQL(
        "SELECT f_int AS renamed_int FROM layer1 LIMIT 2", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_TRUE(lyr->TestCapability(OLCFastGetArrowStream));

    struct ArrowArrayStream stream;
    ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));

    struct ArrowSchema schema;
    ASSERT_EQ(0, stream.get_schema(&stream, &schema));
    ASSERT_GE(schema.n_children, 2);
    EXPECT_STREQ("OGC_FID", schema.children[0]->name);
    EXPECT_STREQ("renamed_int", schema.children[1]->name);
    if (schema.release) schema.release(&schema);

    struct ArrowArray array;
    ASSERT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(2, array.length);
    if (array.release) array.release(&array);

    // Next array hits EOF
    ASSERT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(0, array.length);
    if (array.release) array.release(&array);

    // Release result set before stream to test dangling layer safety
    ds->ReleaseResultSet(lyr);

    {
      WithQuietHandler quiet_handler;
      EXPECT_EQ(EINVAL, stream.get_schema(&stream, &schema));
      EXPECT_EQ(EINVAL, stream.get_next(&stream, &array));
    }
    if (stream.release) stream.release(&stream);
  }

  // Non-rewinded error
  {
    OGRLayer* lyr =
        ds->ExecuteSQL("SELECT f_int FROM layer1", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    EXPECT_NE(nullptr, f);

    struct ArrowArrayStream stream;
    {
      WithQuietHandler quiet_handler;
      EXPECT_FALSE(lyr->GetArrowStream(&stream, nullptr));
    }
    ds->ReleaseResultSet(lyr);
  }

  // Base fallback with reordered fields
  {
    OGRLayer* lyr = ds->ExecuteSQL("SELECT f_str, f_int FROM layer1 LIMIT 2",
                                   nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_FALSE(lyr->TestCapability(OLCFastGetArrowStream));

    struct ArrowArrayStream stream;
    ASSERT_TRUE(lyr->GetArrowStream(&stream, nullptr));

    struct ArrowSchema schema;
    ASSERT_EQ(0, stream.get_schema(&stream, &schema));
    if (schema.release) schema.release(&schema);

    struct ArrowArray array;
    ASSERT_EQ(0, stream.get_next(&stream, &array));
    if (array.release) array.release(&array);

    if (stream.release) stream.release(&stream);
    ds->ReleaseResultSet(lyr);
  }

  OSRCleanup();
}

TEST(OGRGenSQLTest, TargetedCoverageBoost) {
  auto ds = CreatePopulatedDataset();

  // 1. Geometry CAST expressions (Polygon -> MultiPolygon, MultiPolygon ->
  // Polygon, LineString -> MultiLineString, MultiLineString -> LineString, and
  // smallint)
  {
    const char* cast_sql =
        "SELECT "
        "CAST('POLYGON((0 0,0 1,1 1,1 0,0 0))' AS GEOMETRY(MULTIPOLYGON, "
        "4326)) AS g_mp, "
        "CAST('MULTIPOLYGON(((0 0,0 1,1 1,1 0,0 0)))' AS GEOMETRY(POLYGON)) AS "
        "g_poly, "
        "CAST('LINESTRING(0 0,1 1)' AS GEOMETRY(MULTILINESTRING)) AS g_mls, "
        "CAST('MULTILINESTRING((0 0,1 1))' AS GEOMETRY(LINESTRING)) AS g_ls, "
        "CAST(OGR_GEOMETRY AS GEOMETRY(MULTIPOLYGON)) AS g_src_mp, "
        "CAST(f_int AS smallint) AS f_smallint "
        "FROM layer1 LIMIT 2";
    OGRLayer* lyr = ds->ExecuteSQL(cast_sql, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_NE(nullptr, f->GetGeomFieldRef(0));
    EXPECT_EQ(wkbMultiPolygon,
              wkbFlatten(f->GetGeomFieldRef(0)->getGeometryType()));
    EXPECT_NE(nullptr, f->GetGeomFieldRef(1));
    EXPECT_EQ(wkbPolygon, wkbFlatten(f->GetGeomFieldRef(1)->getGeometryType()));
    EXPECT_NE(nullptr, f->GetGeomFieldRef(2));
    EXPECT_EQ(wkbMultiLineString,
              wkbFlatten(f->GetGeomFieldRef(2)->getGeometryType()));
    EXPECT_NE(nullptr, f->GetGeomFieldRef(3));
    EXPECT_EQ(wkbLineString,
              wkbFlatten(f->GetGeomFieldRef(3)->getGeometryType()));
    ds->ReleaseResultSet(lyr);
  }

  // 2. WHERE clause with special geometry fields in operations and spatial
  // filter calls
  {
    const char* geom_where =
        "SELECT f_int FROM layer1 WHERE f_int >= 0 AND OGR_GEOM_AREA > 0 AND "
        "OGR_GEOM_WKT LIKE 'POLYGON%' AND OGR_GEOMETRY IS NOT NULL";
    OGRLayer* lyr = ds->ExecuteSQL(geom_where, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilterRect(0, 0, 1, 1));
    EXPECT_EQ(OGRERR_NONE, lyr->SetSpatialFilter(nullptr));
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    EXPECT_NE(nullptr, f);
    ds->ReleaseResultSet(lyr);
  }

  // 3. Summary COUNT(*) with geometry WHERE
  {
    OGRLayer* lyr =
        ds->ExecuteSQL("SELECT COUNT(*) FROM layer1 WHERE OGR_GEOM_AREA > 0",
                       nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_GT(f->GetFieldAsInteger64(0), 0);
    ds->ReleaseResultSet(lyr);

    OGRLayer* lyr2 = ds->ExecuteSQL(
        "SELECT COUNT(f_int) FROM layer1 WHERE OGR_GEOM_AREA > 0", nullptr,
        "OGRSQL");
    ASSERT_NE(nullptr, lyr2);
    std::unique_ptr<OGRFeature> f2(lyr2->GetNextFeature());
    ASSERT_NE(nullptr, f2);
    EXPECT_GT(f2->GetFieldAsInteger64(0), 0);
    ds->ReleaseResultSet(lyr2);
  }

  // 4. JOIN with special fields on primary and secondary tables & external
  // dataset failure
  {
    const char* join_sql =
        "SELECT layer1.f_int FROM layer1 JOIN layer2 ON layer1.FID = "
        "layer2.j_int "
        "AND layer1.OGR_GEOM_AREA = layer2.j_real";
    OGRLayer* lyr = ds->ExecuteSQL(join_sql, nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
    EXPECT_NE(nullptr, f);
    ds->ReleaseResultSet(lyr);

    OGRLayer* join_str = ds->ExecuteSQL(
        "SELECT layer1.f_int FROM layer1 JOIN layer2 ON layer1.OGR_GEOM_WKT = "
        "layer2.j_str",
        nullptr, "OGRSQL");
    ASSERT_NE(nullptr, join_str);
    std::unique_ptr<OGRFeature> f_str(join_str->GetNextFeature());
    EXPECT_NE(nullptr, f_str);
    ds->ReleaseResultSet(join_str);

    WithQuietHandler quiet_handler;
    OGRLayer* failed_join = ds->ExecuteSQL(
        "SELECT * FROM layer1 JOIN \"/vsimem/nonexistent.shp\".layer2 ON "
        "layer1.f_int = layer2.j_int",
        nullptr, "OGRSQL");
    EXPECT_EQ(nullptr, failed_join);
  }

  // 5. ORDER BY on each individual field type as primary sort key
  {
    const char* const order_queries[] = {
        "SELECT f_int FROM layer1 ORDER BY f_int64 DESC LIMIT 2",
        "SELECT f_int FROM layer1 ORDER BY f_str ASC LIMIT 2",
        "SELECT f_int FROM layer1 ORDER BY f_real DESC LIMIT 2",
        "SELECT f_int FROM layer1 ORDER BY f_date ASC LIMIT 2",
        "SELECT f_int FROM layer1 ORDER BY OGR_GEOM_AREA DESC LIMIT 2",
        "SELECT f_int FROM layer1 ORDER BY OGR_GEOMETRY ASC LIMIT 2",
    };
    for (const char* q : order_queries) {
      OGRLayer* lyr = ds->ExecuteSQL(q, nullptr, "OGRSQL");
      ASSERT_NE(nullptr, lyr);
      std::unique_ptr<OGRFeature> f(lyr->GetNextFeature());
      EXPECT_NE(nullptr, f);
      ds->ReleaseResultSet(lyr);
    }
  }

  // 6. Arrow stream batch truncation when MAX_FEATURES_IN_BATCH > LIMIT
  {
    OGRLayer* lyr =
        ds->ExecuteSQL("SELECT f_int FROM layer1 LIMIT 2", nullptr, "OGRSQL");
    ASSERT_NE(nullptr, lyr);
    CPLStringList options;
    options.AddString("MAX_FEATURES_IN_BATCH=10");
    struct ArrowArrayStream stream;
    ASSERT_TRUE(lyr->GetArrowStream(&stream, options.List()));
    struct ArrowArray array;
    ASSERT_EQ(0, stream.get_next(&stream, &array));
    EXPECT_EQ(2, array.length);
    if (array.release) array.release(&array);
    if (stream.release) stream.release(&stream);
    ds->ReleaseResultSet(lyr);
  }

  OSRCleanup();
}

// TODO: b/335317901 - Cover remaining error branches in ogr_gensql.cpp.

}  // namespace
}  // namespace autotest2
