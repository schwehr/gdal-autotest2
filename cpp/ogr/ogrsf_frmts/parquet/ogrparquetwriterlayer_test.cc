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
// Unit tests for ogrparquetwriterlayer.cpp (OGRParquetWriterLayer).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetwriterlayer.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arrow/api.h"
#include "arrow/c/bridge.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/parquet/parquet_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/parquet/ogr_parquet.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetwriterlayer_test";

class OgrParquetWriterLayerTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRParquet(); }
};

TEST_F(OgrParquetWriterLayerTest, GeometryEncodingsCompressionAndOptions) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  // 1. Invalid layer creation options return cleanly.
  {
    constexpr char kBadOpts[] =
        "/vsimem/ogrparquetwriterlayer_test/bad_opts.parquet";
    GDALDatasetUniquePtr ds(
        drv->Create(kBadOpts, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    WithQuietHandler quiet;
    {
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("GEOMETRY_ENCODING", "INVALID_ENC");
      EXPECT_EQ(nullptr, ds->CreateLayer("t", nullptr, wkbPoint, opts.List()));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Unsupported GEOMETRY_ENCODING"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("COMPRESSION", "NO_SUCH");
      EXPECT_EQ(nullptr, ds->CreateLayer("t", nullptr, wkbPoint, opts.List()));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Unrecognized compression method"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      // PointM without OGR_PARQUET_ALLOW_ALL_DIMS=YES fails.
      EXPECT_EQ(nullptr, ds->CreateLayer("t", nullptr, wkbPointM, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Only 2D and Z geometry types"));
      CPLErrorReset();
    }
    {
      CPLErrorReset();
      CPLStringList opts;
      opts.SetNameValue("GEOMETRY_ENCODING", "GEOARROW");
      // Generic wkbUnknown cannot map to a fixed GeoArrow struct schema.
      EXPECT_EQ(nullptr,
                ds->CreateLayer("t", nullptr, wkbUnknown, opts.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
  }

  // 2. WKT encoding with precision, ZSTD default compression level, custom
  // CREATOR, CRS WKT encoding, dataset-level metadata (json/xml/regular).
  constexpr char kWktPath[] = "/vsimem/ogrparquetwriterlayer_test/wkt.parquet";
  {
    CPLConfigOptionSetter crs_enc("OGR_PARQUET_CRS_ENCODING", "WKT", false);
    CPLConfigOptionSetter omit_wgs84("OGR_PARQUET_CRS_OMIT_IF_WGS84", "NO",
                                     false);
    CPLConfigOptionSetter arrow_ext("OGR_PARQUET_WRITE_ARROW_EXTENSION_NAME",
                                    "YES", false);
    GDALDatasetUniquePtr ds(
        drv->Create(kWktPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(4326);
    srs.SetCoordinateEpoch(2021.5);

    CPLStringList opts;
    opts.SetNameValue("GEOMETRY_ENCODING", "WKT");
    opts.SetNameValue("COORDINATE_PRECISION", "3");
    opts.SetNameValue("COMPRESSION", "ZSTD");
    opts.SetNameValue("CREATOR", "autotest2_writer");
    opts.SetNameValue("EDGES", "SPHERICAL");
    opts.SetNameValue("ROW_GROUP_SIZE", "2");

    OGRLayer* lyr = ds->CreateLayer("wkt_lyr", &srs, wkbPoint25D, opts.List());
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(ds.get(), lyr->GetDataset());
    EXPECT_EQ(CE_None, lyr->SetMetadataItem("foo", "bar"));
    const char* json_md[] = {"{\"k\":1}", nullptr};
    EXPECT_EQ(CE_None,
              lyr->SetMetadata(const_cast<char**>(json_md), "json:custom"));
    const char* xml_md[] = {"<root/>", nullptr};
    EXPECT_EQ(CE_None,
              lyr->SetMetadata(const_cast<char**>(xml_md), "xml:custom"));
    // SHAPEFILE domain is intentionally ignored.
    const char* shp_md[] = {"ignored=yes", nullptr};
    EXPECT_EQ(CE_None,
              lyr->SetMetadata(const_cast<char**>(shp_md), "SHAPEFILE"));

    OGRFieldDefn id_fld("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&id_fld));

    for (int i = 0; i < 3; ++i) {
      OGRFeature f(lyr->GetLayerDefn());
      f.SetField(0, i);
      auto pt = std::make_unique<OGRPoint>(1.12345 + i, 2.54321 + i, 10.0 + i);
      f.SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }
    EXPECT_EQ(CE_None, ds->Close());
  }

  {
    auto ds = OpenParquetDataset(kWktPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("autotest2_writer",
                 lyr->GetMetadataItem("CREATOR", "_PARQUET_"));
    EXPECT_STREQ("zstd",
                 lyr->GetMetadataItem("ROW_GROUPS[0].COLUMNS[0].COMPRESSION",
                                      "_PARQUET_"));
    EXPECT_STREQ("SPHERICAL", lyr->GetMetadataItem("EDGES"));
    EXPECT_STREQ("bar", lyr->GetMetadataItem("foo"));
    EXPECT_EQ(1, CSLCount(lyr->GetMetadata("json:custom")));
    EXPECT_EQ(1, CSLCount(lyr->GetMetadata("xml:custom")));
    ASSERT_NE(nullptr, lyr->GetSpatialRef());
    EXPECT_NEAR(2021.5, lyr->GetSpatialRef()->GetCoordinateEpoch(), 1e-9);
    EXPECT_EQ(nullptr, lyr->GetMetadata("SHAPEFILE"));
    EXPECT_STREQ("2", lyr->GetMetadataItem("NUM_ROW_GROUPS", "_PARQUET_"));

    std::unique_ptr<OGRFeature> f0(lyr->GetNextFeature());
    ASSERT_NE(nullptr, f0);
    ASSERT_NE(nullptr, f0->GetGeometryRef());
    EXPECT_NEAR(1.123, f0->GetGeometryRef()->toPoint()->getX(), 1e-4);
    EXPECT_NEAR(2.543, f0->GetGeometryRef()->toPoint()->getY(), 1e-4);
    EXPECT_NEAR(10.0, f0->GetGeometryRef()->toPoint()->getZ(), 1e-4);
  }

  // 3. GEOARROW_INTERLEAVED emits a warning; verify multi-part / Z / polygon
  // winding fixup.
  constexpr char kInterleavedPath[] =
      "/vsimem/ogrparquetwriterlayer_test/interleaved.parquet";
  {
    GDALDatasetUniquePtr ds(
        drv->Create(kInterleavedPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    // 3a. GEOARROW_INTERLEAVED writes native interleaved struct coordinates.
    CPLStringList opts;
    opts.SetNameValue("GEOMETRY_ENCODING", "GEOARROW_INTERLEAVED");
    opts.SetNameValue("COMPRESSION", "NONE");
    OGRLayer* lyr = nullptr;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      lyr = ds->CreateLayer("poly", nullptr, wkbMultiPolygon, opts.List());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GEOARROW_INTERLEAVED"));
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, lyr);

    OGRGeometry* poly_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "MULTIPOLYGON(((0 0,0 2,2 2,2 0,0 0)))", nullptr, &poly_raw));
    OGRGeometryUniquePtr poly(poly_raw);
    OGRFeature f(lyr->GetLayerDefn());
    f.SetGeometry(poly.get());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    EXPECT_EQ(CE_None, ds->Close());
  }
  {
    auto ds = OpenParquetDataset(kInterleavedPath);
    ASSERT_NE(nullptr, ds);
    std::unique_ptr<OGRFeature> f(ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, f);
    ASSERT_NE(nullptr, f->GetGeometryRef());
    EXPECT_EQ(wkbMultiPolygon,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));
  }

  // 3b. WKB encoding with POLYGON_ORIENTATION=COUNTERCLOCKWISE reverses
  // clockwise exterior and CCW interior holes.
  constexpr char kOrientationPath[] =
      "/vsimem/ogrparquetwriterlayer_test/orientation.parquet";
  {
    GDALDatasetUniquePtr ds(
        drv->Create(kOrientationPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    CPLStringList opts;
    opts.SetNameValue("GEOMETRY_ENCODING", "WKB");
    opts.SetNameValue("COMPRESSION", "NONE");
    opts.SetNameValue("POLYGON_ORIENTATION", "COUNTERCLOCKWISE");
    OGRLayer* lyr =
        ds->CreateLayer("poly", nullptr, wkbMultiPolygon, opts.List());
    ASSERT_NE(nullptr, lyr);

    OGRGeometry* poly_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "MULTIPOLYGON(((0 0,0 2,2 2,2 0,0 0),"
                               "(0.2 0.2,1.8 0.2,1.8 1.8,0.2 1.8,0.2 0.2)))",
                               nullptr, &poly_raw));
    OGRGeometryUniquePtr poly(poly_raw);
    OGRFeature f(lyr->GetLayerDefn());
    f.SetGeometry(poly.get());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    EXPECT_EQ(CE_None, ds->Close());
  }
  {
    auto ds = OpenParquetDataset(kOrientationPath);
    ASSERT_NE(nullptr, ds);
    std::unique_ptr<OGRFeature> f(ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, f);
    ASSERT_NE(nullptr, f->GetGeometryRef());
    ASSERT_EQ(wkbMultiPolygon,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));
    auto* mp = f->GetGeometryRef()->toMultiPolygon();
    ASSERT_EQ(1, mp->getNumGeometries());
    EXPECT_FALSE(mp->getGeometryRef(0)->getExteriorRing()->isClockwise());
    EXPECT_TRUE(mp->getGeometryRef(0)->getInteriorRing(0)->isClockwise());
  }
}

TEST_F(OgrParquetWriterLayerTest, AllGeometryTypesAndParquetGeoTypesOnly) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  // 1. Mixed OGR_PARQUET_ALLOW_ALL_DIMS=YES and geometry types in
  // getGeoMetadata.
  constexpr char kMixedPath[] =
      "/vsimem/ogrparquetwriterlayer_test/mixed_geoms.parquet";
  {
    CPLConfigOptionSetter allow_m("OGR_PARQUET_ALLOW_ALL_DIMS", "YES", false);
    GDALDatasetUniquePtr ds(
        drv->Create(kMixedPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    CPLStringList opts;
    opts.SetNameValue("GEOMETRY_NAME", "g");
    opts.SetNameValue("USE_PARQUET_GEO_TYPES", "ONLY");
    opts.SetNameValue("COMPRESSION", "SNAPPY");
    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(4326);
    OGRLayer* lyr = ds->CreateLayer("mixed", &srs, wkbUnknown, opts.List());
    ASSERT_NE(nullptr, lyr);

    constexpr std::string_view kWkts[] = {
        "POINT (1 2)",
        "LINESTRING (0 0, 1 1)",
        "POLYGON ((0 0,1 0,1 1,0 0))",
        "MULTIPOINT ((1 2))",
        "MULTILINESTRING ((0 0, 1 1))",
        "MULTIPOLYGON (((0 0,1 0,1 1,0 0)))",
        "GEOMETRYCOLLECTION (POINT (1 2))",
        "POINT Z (1 2 3)",
        "POINT M (1 2 4)",
        "POINT ZM (1 2 3 4)",
    };
    for (std::string_view wkt : kWkts) {
      OGRGeometry* g_raw = nullptr;
      std::string wkt_str(wkt);
      ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                                 wkt_str.c_str(), nullptr, &g_raw));
      OGRFeature f(lyr->GetLayerDefn());
      f.SetGeometryDirectly(g_raw);
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }
    EXPECT_EQ(CE_None, ds->Close());
  }
  {
    auto ds = OpenParquetDataset(kMixedPath);
    ASSERT_NE(nullptr, ds);
    OGRLayer* ro_lyr = ds->GetLayer(0);
    ASSERT_NE(nullptr, ro_lyr);
    EXPECT_EQ(10, ro_lyr->GetFeatureCount(/*bForce=*/1));
    int count = 0;
    for (std::unique_ptr<OGRFeature> feat;
         (feat.reset(ro_lyr->GetNextFeature()), feat != nullptr); ++count) {
      EXPECT_NE(nullptr, feat->GetGeometryRef());
    }
    EXPECT_EQ(10, count);
    EXPECT_EQ(nullptr, ro_lyr->GetMetadataItem("geo", "_PARQUET_METADATA_"));
  }
}

TEST_F(OgrParquetWriterLayerTest, ArrowBatchWritingAndSchemaSupport) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] =
      "/vsimem/ogrparquetwriterlayer_test/arrow_batch.parquet";

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create(kPath, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);
  OGRLayer* lyr = ds->CreateLayer("ab", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, lyr);
  EXPECT_EQ(TRUE, lyr->TestCapability(OLCFastWriteArrowBatch));

  // Check unsupported Arrow schema diagnostics (float16, string_view,
  // binary_view, list_view, large_list_view).
  const std::shared_ptr<arrow::DataType> unsupported_types[] = {
      arrow::float16(),
      arrow::utf8_view(),
      arrow::binary_view(),
      arrow::list_view(arrow::int32()),
      arrow::large_list_view(arrow::int32()),
  };
  for (const auto& dtype : unsupported_types) {
    auto schema = arrow::schema({arrow::field("bad", dtype)});
    ArrowSchema c_schema{};
    ASSERT_TRUE(arrow::ExportSchema(*schema, &c_schema).ok());
    absl::Cleanup rel_schema = [&c_schema] {
      if (c_schema.release) c_schema.release(&c_schema);
    };
    std::string err_msg;
    EXPECT_FALSE(lyr->IsArrowSchemaSupported(&c_schema, nullptr, err_msg));
    EXPECT_FALSE(err_msg.empty());
  }

  // Write a valid RecordBatch with both an attribute and clockwise WKB polygon
  // to cover FixupWKBGeometryBeforeWriting + WriteArrowBatch +
  // CreateFieldFromArrowSchema.
  arrow::Int32Builder id_builder;
  ASSERT_TRUE(id_builder.AppendValues({1, 2}).ok());
  arrow::BinaryBuilder wkb_builder;
  // Clockwise triangle
  OGRPolygon poly;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 1);
  ring.addPoint(1, 1);
  ring.addPoint(0, 0);
  poly.addRing(&ring);
  std::string wkb(poly.WkbSize(), '\0');
  ASSERT_EQ(OGRERR_NONE,
            poly.exportToWkb(reinterpret_cast<unsigned char*>(wkb.data())));
  ASSERT_TRUE(wkb_builder.Append(wkb).ok());
  ASSERT_TRUE(wkb_builder.Append(wkb).ok());

  std::shared_ptr<arrow::Array> id_arr, wkb_arr;
  ASSERT_TRUE(id_builder.Finish(&id_arr).ok());
  ASSERT_TRUE(wkb_builder.Finish(&wkb_arr).ok());

  auto arrow_schema =
      arrow::schema({arrow::field("id", arrow::int32()),
                     arrow::field("geometry", arrow::binary())});
  auto batch = arrow::RecordBatch::Make(arrow_schema, 2, {id_arr, wkb_arr});

  ArrowSchema c_schema{};
  ArrowArray c_array{};
  ASSERT_TRUE(arrow::ExportSchema(*arrow_schema, &c_schema).ok());
  ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &c_array).ok());
  absl::Cleanup rel_c = [&c_schema, &c_array] {
    if (c_schema.release) c_schema.release(&c_schema);
    if (c_array.release) c_array.release(&c_array);
  };

  std::string err_msg;
  EXPECT_TRUE(lyr->IsArrowSchemaSupported(&c_schema, nullptr, err_msg))
      << err_msg;
  ASSERT_NE(nullptr, c_schema.children[0]);
  EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(c_schema.children[0], nullptr));
  EXPECT_TRUE(lyr->WriteArrowBatch(&c_schema, &c_array, nullptr));
  EXPECT_EQ(CE_None, ds->Close());

  auto ro_ds = OpenParquetDataset(kPath);
  ASSERT_NE(nullptr, ro_ds);
  OGRLayer* ro_lyr = ro_ds->GetLayer(0);
  ASSERT_NE(nullptr, ro_lyr);
  EXPECT_EQ(2, ro_lyr->GetFeatureCount(/*bForce=*/1));
  std::unique_ptr<OGRFeature> f0(ro_lyr->GetNextFeature());
  ASSERT_NE(nullptr, f0);
  EXPECT_EQ(1, f0->GetFieldAsInteger("id"));
  ASSERT_NE(nullptr, f0->GetGeometryRef());
  EXPECT_FALSE(
      f0->GetGeometryRef()->toPolygon()->getExteriorRing()->isClockwise());
}

TEST_F(OgrParquetWriterLayerTest, SortByBBoxMissingGpkgAndFullSort) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  // 1. SORT_BY_BBOX without GPKG driver registered returns clear error.
  GDALDriver* saved_gpkg = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (saved_gpkg != nullptr) {
    GetGDALDriverManager()->DeregisterDriver(saved_gpkg);
  }
  absl::Cleanup restore_gpkg = [saved_gpkg] {
    if (saved_gpkg != nullptr &&
        GetGDALDriverManager()->GetDriverByName("GPKG") == nullptr) {
      GetGDALDriverManager()->RegisterDriver(saved_gpkg);
    }
  };

  {
    constexpr char kNoGpkg[] =
        "/vsimem/ogrparquetwriterlayer_test/no_gpkg.parquet";
    GDALDatasetUniquePtr ds(
        drv->Create(kNoGpkg, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts;
    opts.SetNameValue("SORT_BY_BBOX", "YES");
    EXPECT_EQ(nullptr, ds->CreateLayer("t", nullptr, wkbPoint, opts.List()));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Driver GPKG required"));
    CPLErrorReset();
  }

  // 2. Register GPKG temporarily and test multi-level RTree SORT_BY_BBOX=YES.
  if (saved_gpkg != nullptr) {
    GetGDALDriverManager()->RegisterDriver(saved_gpkg);
  } else {
    RegisterOGRGeoPackage();
  }
  absl::Cleanup drop_temp_gpkg = [had_gpkg = (saved_gpkg != nullptr)] {
    if (!had_gpkg) {
      auto* mgr = GetGDALDriverManager();
      if (GDALDriver* g = mgr->GetDriverByName("GPKG")) {
        mgr->DeregisterDriver(g);
        delete g;
      }
    }
  };

  constexpr char kSorted[] =
      "/vsimem/ogrparquetwriterlayer_test/sorted.parquet";
  {
    GDALDatasetUniquePtr ds(
        drv->Create(kSorted, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    CPLStringList opts;
    opts.SetNameValue("SORT_BY_BBOX", "YES");
    opts.SetNameValue("ROW_GROUP_SIZE", "60");
    opts.SetNameValue("FID", "fid");
    OGRLayer* lyr = ds->CreateLayer("sorted", nullptr, wkbPoint, opts.List());
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(FALSE, lyr->TestCapability(OLCFastWriteArrowBatch));

    OGRFieldDefn idx_fld("idx", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&idx_fld));

    // Exercise SORT_BY_BBOX fallback in CreateFieldFromArrowSchema before
    // writing.
    ArrowSchema fb_c_schema{};
    ASSERT_TRUE(arrow::ExportField(*arrow::field("extra_arrow", arrow::int32()),
                                   &fb_c_schema)
                    .ok());
    absl::Cleanup rel_fb = [&fb_c_schema] {
      if (fb_c_schema.release) fb_c_schema.release(&fb_c_schema);
    };
    EXPECT_TRUE(lyr->CreateFieldFromArrowSchema(&fb_c_schema, nullptr));

    // Exercise Arrow capability checks in SORT_BY_BBOX fallback path.
    ArrowSchema c_schema{};
    ASSERT_TRUE(arrow::ExportType(*arrow::int32(), &c_schema).ok());
    absl::Cleanup rel_schema = [&c_schema] {
      if (c_schema.release) c_schema.release(&c_schema);
    };
    std::string err_msg;
    EXPECT_FALSE(lyr->IsArrowSchemaSupported(&c_schema, nullptr, err_msg));

    // Non-spatial rows
    for (int i = 0; i < 5; ++i) {
      OGRFeature f(lyr->GetLayerDefn());
      f.SetField(0, i);
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }
    // > 200 spatial rows to create multi-depth RTree (> 1 node level).
    for (int i = 0; i < 210; ++i) {
      OGRFeature f(lyr->GetLayerDefn());
      f.SetField(0, 100 + i);
      auto pt = std::make_unique<OGRPoint>(static_cast<double>(210 - i),
                                           static_cast<double>(i));
      f.SetGeometryDirectly(pt.release());
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    }

    // Exercise SORT_BY_BBOX fallback in WriteArrowBatch().
    arrow::Int32Builder id_b, ex_b;
    ASSERT_TRUE(id_b.Append(999).ok());
    ASSERT_TRUE(ex_b.Append(42).ok());
    std::shared_ptr<arrow::Array> id_a, ex_a;
    ASSERT_TRUE(id_b.Finish(&id_a).ok());
    ASSERT_TRUE(ex_b.Finish(&ex_a).ok());
    auto batch_schema =
        arrow::schema({arrow::field("idx", arrow::int32()),
                       arrow::field("extra_arrow", arrow::int32())});
    auto batch = arrow::RecordBatch::Make(batch_schema, 1, {id_a, ex_a});
    ArrowSchema batch_c_schema{};
    ArrowArray batch_c_array{};
    absl::Cleanup rel_batch = [&batch_c_schema, &batch_c_array] {
      if (batch_c_schema.release) batch_c_schema.release(&batch_c_schema);
      if (batch_c_array.release) batch_c_array.release(&batch_c_array);
    };
    ASSERT_TRUE(arrow::ExportSchema(*batch_schema, &batch_c_schema).ok());
    ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &batch_c_array).ok());
    EXPECT_TRUE(lyr->WriteArrowBatch(&batch_c_schema, &batch_c_array, nullptr));

    EXPECT_EQ(CE_None, ds->Close());
  }

  auto ro_ds = OpenParquetDataset(kSorted);
  ASSERT_NE(nullptr, ro_ds);
  OGRLayer* ro_lyr = ro_ds->GetLayer(0);
  ASSERT_NE(nullptr, ro_lyr);
  EXPECT_EQ(216, ro_lyr->GetFeatureCount(/*bForce=*/1));
  for (int i = 0; i < 6; ++i) {
    std::unique_ptr<OGRFeature> feat(ro_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(nullptr, feat->GetGeometryRef());
  }
  std::vector<int> spatial_ids;
  for (int i = 0; i < 210; ++i) {
    std::unique_ptr<OGRFeature> feat(ro_lyr->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_NE(nullptr, feat->GetGeometryRef());
    spatial_ids.push_back(feat->GetFieldAsInteger("idx"));
  }
  EXPECT_EQ(nullptr, ro_lyr->GetNextFeature());
  // Confirm spatial features were reordered by RTree traversal rather than
  // remaining in ascending insertion order.
  EXPECT_FALSE(std::ranges::is_sorted(spatial_ids));
}

TEST_F(OgrParquetWriterLayerTest, CreationOptionsCrsGeomTypesAndFailurePaths) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  ASSERT_NE(nullptr, drv);

  // 1. Named CreateGeomField, clamped ROW_GROUP_SIZE, STATISTICS=NO, ZSTD +
  // COMPRESSION_LEVEL, PROJJSON CRS, and 0-feature empty layer Close().
  constexpr char kEmptyNamedPath[] =
      "/vsimem/ogrparquetwriterlayer_test/empty_named.parquet";
  {
    CPLConfigOptionSetter omit_wgs84("OGR_PARQUET_CRS_OMIT_IF_WGS84", "NO",
                                     false);
    CPLConfigOptionSetter crs_enc("OGR_PARQUET_CRS_ENCODING", "PROJJSON",
                                  false);
    GDALDatasetUniquePtr ds(
        drv->Create(kEmptyNamedPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    CPLStringList opts;
    opts.SetNameValue("COMPRESSION", "ZSTD");
    opts.SetNameValue("COMPRESSION_LEVEL", "3");
    opts.SetNameValue("STATISTICS", "NO");
    opts.SetNameValue("ROW_GROUP_SIZE", "9999999999");
    opts.SetNameValue("USE_PARQUET_GEO_TYPES", "NO");
    opts.SetNameValue("EDGES", "SPHERICAL");
    opts.SetNameValue("FID", "fid");

    OGRSpatialReference srs;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(4326);

    OGRLayer* lyr = nullptr;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      lyr = ds->CreateLayer("empty", &srs, wkbNone, opts.List());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("STATISTICS"));
      CPLErrorReset();
    }
    ASSERT_NE(nullptr, lyr);
    OGRGeomFieldDefn geom_fld("custom_geom", wkbPoint);
    geom_fld.SetSpatialRef(&srs);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateGeomField(&geom_fld));
    EXPECT_EQ(CE_None, ds->Close());
  }
  {
    auto ro_ds = OpenParquetDataset(kEmptyNamedPath);
    ASSERT_NE(nullptr, ro_ds);
    OGRLayer* ro_lyr = ro_ds->GetLayer(0);
    ASSERT_NE(nullptr, ro_lyr);
    EXPECT_EQ(0, ro_lyr->GetFeatureCount(/*bForce=*/1));
    EXPECT_STREQ("custom_geom", ro_lyr->GetGeometryColumn());
    EXPECT_STREQ("SPHERICAL", ro_lyr->GetMetadataItem("EDGES"));
    ASSERT_NE(nullptr, ro_lyr->GetSpatialRef());
    EXPECT_STREQ("4326", ro_lyr->GetSpatialRef()->GetAuthorityCode(nullptr));
  }

  // 2. GetGeoMetadata() coverage across remaining geometry types + M/ZM +
  // OGR_PARQUET_GEO_METADATA override.
  {
    CPLConfigOptionSetter allow_dims("OGR_PARQUET_ALLOW_ALL_DIMS", "YES",
                                     false);
    struct GeomTypeCase {
      OGRwkbGeometryType geom_type;
      const char* wkt;
    };
    constexpr GeomTypeCase kTypes[] = {
        {.geom_type = wkbLineString, .wkt = "LINESTRING (0 0, 1 1)"},
        {.geom_type = wkbMultiPoint, .wkt = "MULTIPOINT ((1 2))"},
        {.geom_type = wkbMultiLineString,
         .wkt = "MULTILINESTRING ((0 0, 1 1))"},
        {.geom_type = wkbGeometryCollection,
         .wkt = "GEOMETRYCOLLECTION (POINT (1 2))"},
        {.geom_type = wkbPointM, .wkt = "POINT M (1 2 4)"},
        {.geom_type = wkbPointZM, .wkt = "POINT ZM (1 2 3 4)"},
    };
    CPLStringList gtype_opts;
    gtype_opts.SetNameValue("USE_PARQUET_GEO_TYPES", "NO");
    for (int i = 0; i < static_cast<int>(std::size(kTypes)); ++i) {
      const std::string p =
          std::string(kTestDir) + "/gtype_" + std::to_string(i) + ".parquet";
      GDALDatasetUniquePtr ds(
          drv->Create(p.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
      ASSERT_NE(nullptr, ds);
      OGRLayer* lyr =
          ds->CreateLayer("t", nullptr, kTypes[i].geom_type, gtype_opts.List());
      ASSERT_NE(nullptr, lyr);
      OGRGeometry* g_raw = nullptr;
      ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                                 kTypes[i].wkt, nullptr, &g_raw));
      OGRFeature feat(lyr->GetLayerDefn());
      feat.SetGeometryDirectly(g_raw);
      ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(&feat));
      EXPECT_EQ(CE_None, ds->Close());

      auto ro_ds = OpenParquetDataset(p.c_str());
      ASSERT_NE(nullptr, ro_ds);
      ASSERT_NE(nullptr, ro_ds->GetLayer(0));
      EXPECT_EQ(kTypes[i].geom_type, ro_ds->GetLayer(0)->GetGeomType());
    }

    constexpr char kGeoOverride[] =
        "{\"version\":\"1.1.0\",\"primary_column\":\"geometry\",\"columns\":{}"
        "}";
    CPLConfigOptionSetter geo_override("OGR_PARQUET_GEO_METADATA", kGeoOverride,
                                       false);
    const std::string p_ovr = std::string(kTestDir) + "/geo_ovr.parquet";
    GDALDatasetUniquePtr ds(
        drv->Create(p_ovr.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->CreateLayer("ovr", nullptr, wkbPoint, nullptr));
    EXPECT_EQ(CE_None, ds->Close());

    auto ro_ds = OpenParquetDataset(p_ovr.c_str());
    ASSERT_NE(nullptr, ro_ds);
    ASSERT_NE(nullptr, ro_ds->GetLayer(0));
    EXPECT_STREQ(kGeoOverride, ro_ds->GetLayer(0)->GetMetadataItem(
                                   "geo", "_PARQUET_METADATA_"));
  }

  // 3. Write failure propagation in CloseFileWriter / WriteArrays /
  // WriteArrowBatch.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto failing_stream = std::make_shared<ThresholdFailingOutputStream>(1);
    auto wr = std::make_unique<OGRParquetWriterDataset>(failing_stream);
    OGRLayer* lyr = wr->CreateLayer("fail", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn fld("x", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));
    OGRFeature f(lyr->GetLayerDefn());
    f.SetField(0, 7);
    EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(&f));
    EXPECT_EQ(CE_Failure, wr->Close());
    CPLErrorReset();
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto failing_stream = std::make_shared<ThresholdFailingOutputStream>(1);
    auto wr = std::make_unique<OGRParquetWriterDataset>(failing_stream);
    OGRLayer* lyr = wr->CreateLayer("fail_batch", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, lyr);
    OGRFieldDefn fld("x", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

    arrow::Int32Builder b;
    ASSERT_TRUE(b.Append(1).ok());
    std::shared_ptr<arrow::Array> a;
    ASSERT_TRUE(b.Finish(&a).ok());
    auto arrow_schema = arrow::schema({arrow::field("x", arrow::int32())});
    auto batch = arrow::RecordBatch::Make(arrow_schema, 1, {a});
    ArrowSchema cs1{};
    ArrowArray ca1{};
    absl::Cleanup rel_c1 = [&cs1, &ca1] {
      if (cs1.release) cs1.release(&cs1);
      if (ca1.release) ca1.release(&ca1);
    };
    ASSERT_TRUE(arrow::ExportSchema(*arrow_schema, &cs1).ok());
    ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &ca1).ok());
    // First batch opens writer ("PAR1" = write 1) and buffers row group 1.
    EXPECT_TRUE(lyr->WriteArrowBatch(&cs1, &ca1, nullptr));

    // Second WriteArrowBatch flushes row group 1 inside NewBufferedRowGroup(),
    // triggering simulated write failure.
    ArrowSchema cs2{};
    ArrowArray ca2{};
    absl::Cleanup rel_c2 = [&cs2, &ca2] {
      if (cs2.release) cs2.release(&cs2);
      if (ca2.release) ca2.release(&ca2);
    };
    ASSERT_TRUE(arrow::ExportSchema(*arrow_schema, &cs2).ok());
    ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &ca2).ok());
    EXPECT_FALSE(lyr->WriteArrowBatch(&cs2, &ca2, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                ::testing::AnyOf(HasSubstr("NewBufferedRowGroup() failed"),
                                 HasSubstr("WriteRecordBatch() failed")));
    CPLErrorReset();
  }
}

// TODO: b/335317901 - Cover defensive SORT_BY_BBOX SQLite RTree corruption
// checks, >1 GB serialized feature rejection, and unavailable Arrow codec
// errors.

}  // namespace
}  // namespace autotest2
