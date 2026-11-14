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
// Tests for GDALVectorIndexAlgorithm (apps/gdalalg_vector_index.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_index.cpp

#include "third_party/gdal/apps/gdalalg_vector_index.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_index_test";

class GdalalgVectorIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    RegisterOGRShape();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

struct FieldSpec {
  const char* name;
  OGRFieldType type;
};

void CreateVectorGpkg(const std::string& path, const char* layer_name,
                      int epsg_code, double x, double y,
                      const std::vector<FieldSpec>& fields,
                      const char* custom_wkt = nullptr) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_THAT(drv, NotNull());
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_THAT(ds, NotNull());

  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (custom_wkt) {
    ASSERT_EQ(OGRERR_NONE, srs.importFromWkt(custom_wkt));
  } else if (epsg_code > 0) {
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg_code));
  }

  OGRLayer* lyr = ds->CreateLayer(
      layer_name, (epsg_code > 0 || custom_wkt) ? &srs : nullptr, wkbPolygon,
      nullptr);
  ASSERT_THAT(lyr, NotNull());
  for (const auto& f : fields) {
    OGRFieldDefn fd(f.name, f.type);
    ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&fd));
  }

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(x, y);
  ring->addPoint(x + 1.0, y);
  ring->addPoint(x + 1.0, y + 1.0);
  ring->addPoint(x, y + 1.0);
  ring->addPoint(x, y);
  auto poly = std::make_unique<OGRPolygon>();
  poly->addRingDirectly(ring.release());
  feat->SetGeometryDirectly(poly.release());
  ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
}

TEST_F(GdalalgVectorIndexTest, RecursiveDirFilterMetadataAndFormats) {
  const std::string in_dir = std::string(kTestDir) + "/inputs";
  const std::string sub_dir = in_dir + "/nested";
  ASSERT_EQ(0, VSIMkdir(in_dir.c_str(), 0755));
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0755));

  const std::string gpkg1 = in_dir + "/a.gpkg";
  const std::string gpkg2 = sub_dir + "/b.gpkg";
  const std::string ignored_json = in_dir + "/ignored.geojson";
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(gpkg1, "lyr1", 4326, 0.0, 0.0, {{"k", OFTInteger}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(gpkg2, "lyr1", 4326, 2.0, 2.0, {{"k", OFTInteger}}));

  // Ignored file that does not match --filename-filter.
  {
    VSILFILE* fp = VSIFOpenL(ignored_json.c_str(), "wb");
    ASSERT_THAT(fp, NotNull());
    constexpr char kEmpty[] = "{}";
    ASSERT_EQ(2U, VSIFWriteL(kEmpty, 1, 2, fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }

  const std::string out_gpkg = std::string(kTestDir) + "/index.gpkg";
  GDALVectorIndexAlgorithm alg;
  EXPECT_EQ("index", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--recursive",
      "--filename-filter",
      "*.gpkg",
      "--location-name",
      "loc",
      "--absolute-path",
      "--metadata",
      "TITLE=MyIndex",
      "--source-crs-field-name",
      "src_crs",
      "--source-crs-format",
      "auto",
      "--source-layer-index",
      "0",
      "--output-layer",
      "idx",
      "--lco",
      "SPATIAL_INDEX=NO",
      in_dir,
      out_gpkg,
  }));

  int progress_calls = 0;
  auto progress_cb = [](double, const char*, void* data) -> int {
    ++(*static_cast<int*>(data));
    return 1;
  };
  EXPECT_TRUE(alg.Run(progress_cb, &progress_calls));
  EXPECT_GT(progress_calls, 0);
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  OGRLayer* lyr = out_ds->GetLayerByName("idx");
  ASSERT_THAT(lyr, NotNull());
  EXPECT_STREQ("MyIndex", lyr->GetMetadataItem("TITLE"));
  EXPECT_EQ(2, lyr->GetFeatureCount());

  auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
  ASSERT_THAT(feat, NotNull());
  EXPECT_STREQ("EPSG:4326", feat->GetFieldAsString("src_crs"));
  EXPECT_THAT(feat->GetFieldAsString("loc"), HasSubstr(",0"));
}

TEST_F(GdalalgVectorIndexTest, SourceCrsFormatsAndReprojection) {
  const std::string gpkg1 = std::string(kTestDir) + "/epsg.gpkg";
  const std::string gpkg2 = std::string(kTestDir) + "/utm.gpkg";
  const std::string gpkg_custom = std::string(kTestDir) + "/custom_wkt.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateVectorGpkg(gpkg1, "l", 4326, 1.0, 1.0, {}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(gpkg2, "l", 32632, 500000.0, 100000.0, {}));
  constexpr char kNoAuthLongWkt[] =
      "PROJCS[\"Custom_Long_Projection_Name_Exceeding_Shapefile_DBF_254_Byte_"
      "Limit_For_Testing_Auto_Proj4_Fallback_And_WKT_Truncation_Warning\","
      "GEOGCS[\"CustomWGS84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Transverse_Mercator\"],"
      "PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",9],"
      "PARAMETER[\"scale_factor\",0.9996],"
      "PARAMETER[\"false_easting\",500000],"
      "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]";
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(gpkg_custom, "l", 0, 2.0, 2.0, {}, kNoAuthLongWkt));

  for (const char* fmt : {"WKT", "PROJ", "EPSG"}) {
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--dst-crs",
        "EPSG:3857",
        "--source-crs-field-name",
        "crs_val",
        "--source-crs-format",
        fmt,
        "--dataset-name-only",
        "--source-layer-name",
        "l",
        "-f",
        "MEM",
        gpkg1,
        gpkg2,
        "mem_out",
    })) << fmt;
    EXPECT_TRUE(alg.Run()) << fmt;
    GDALDataset* out_ds = alg.GetArg(GDAL_ARG_NAME_OUTPUT)
                              ->Get<GDALArgDatasetValue>()
                              .GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull()) << fmt;
    OGRLayer* lyr = out_ds->GetLayer(0);
    ASSERT_THAT(lyr, NotNull()) << fmt;
    EXPECT_EQ(2, lyr->GetFeatureCount()) << fmt;
    auto feat = std::unique_ptr<OGRFeature>(lyr->GetNextFeature());
    ASSERT_THAT(feat, NotNull());
    EXPECT_EQ(gpkg1, std::string(feat->GetFieldAsString("location")));
  }

  // Exercise shapefile max string length fallback to PROJ4 in auto mode.
  const std::string out_shp = std::string(kTestDir) + "/idx.shp";
  {
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--source-crs-field-name",
        "crs_val",
        "--source-crs-format",
        "auto",
        "-f",
        "ESRI Shapefile",
        gpkg_custom,
        out_shp,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Exercise WKT too long warning for Shapefile.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string out_shp2 = std::string(kTestDir) + "/idx_wkt.shp";
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--source-crs-field-name",
        "crs_val",
        "--source-crs-format",
        "WKT",
        "-f",
        "ESRI Shapefile",
        gpkg_custom,
        out_shp2,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("too long"));
    EXPECT_TRUE(alg.Finalize());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorIndexTest, AppendAndDifferentCrsAndSchemas) {
  const std::string g1 = std::string(kTestDir) + "/s1.gpkg";
  const std::string g2_diff_crs = std::string(kTestDir) + "/s2_crs.gpkg";
  const std::string g3_diff_count = std::string(kTestDir) + "/s3_cnt.gpkg";
  const std::string g4_diff_type = std::string(kTestDir) + "/s4_type.gpkg";
  const std::string g5_match = std::string(kTestDir) + "/s5_match.gpkg";
  const std::string idx_gpkg = std::string(kTestDir) + "/append_idx.gpkg";

  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(g1, "l", 4326, 0.0, 0.0, {{"a", OFTInteger}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(g2_diff_crs, "l", 32632, 0.0, 0.0, {{"a", OFTInteger}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(g3_diff_count, "l", 4326, 1.0, 1.0,
                       {{"a", OFTInteger}, {"b", OFTString}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(g4_diff_type, "l", 4326, 2.0, 2.0, {{"a", OFTString}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateVectorGpkg(g5_match, "l", 4326, 3.0, 3.0, {{"a", OFTInteger}}));

  // Create initial index with g1.
  {
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--source-crs-field-name", "scrs", g1, idx_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // Append: g1 triggers duplicate warning; g5_match succeeds; g2_diff_crs
  // skipped; g3_diff_count and g4_diff_type skipped for schema mismatch.
  // Place g5_match before schema mismatches so GPKG CreateFeature does not
  // clear CPLGetLastErrorMsg().
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--append",
        "--source-crs-field-name",
        "scrs",
        "--output-layer",
        "tileindex",
        g1,
        g5_match,
        g2_diff_crs,
        g3_diff_count,
        g4_diff_type,
        idx_gpkg,
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not match"));
    EXPECT_TRUE(alg.Finalize());
    CPLErrorReset();
  }

  // Verify only g1 and g5_match are in idx_gpkg.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(idx_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(2, ds->GetLayer(0)->GetFeatureCount());
  }

  // With --accept-different-crs and --accept-different-schemas, all 4 succeed.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--accept-different-crs",
        "--accept-different-schemas",
        "-f",
        "MEM",
        g1,
        g2_diff_crs,
        g3_diff_count,
        g4_diff_type,
        "mem_out",
    }));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetArg(GDAL_ARG_NAME_OUTPUT)
                              ->Get<GDALArgDatasetValue>()
                              .GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(4, out_ds->GetLayer(0)->GetFeatureCount());
    CPLErrorReset();
  }
}

TEST_F(GdalalgVectorIndexTest, ValidationAndErrorPaths) {
  const std::string g1 = std::string(kTestDir) + "/e1.gpkg";
  const std::string idx_gpkg = std::string(kTestDir) + "/e_idx.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateVectorGpkg(g1, "l", 4326, 0.0, 0.0, {}));

  // 1. Mutually exclusive accept-different-crs and skip-different-crs.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--accept-different-crs", "--skip-different-crs", g1, idx_gpkg}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("mutually exclusive"));
  }

  // 2. source-crs-format without source-crs-field-name.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--source-crs-format", "WKT", g1, idx_gpkg}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("source-crs-name"));
  }

  // 3. --dst-crs with --skip-different-crs emits warning.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--dst-crs", "EPSG:4326",
                                               "--skip-different-crs", "-f",
                                               "MEM", g1, "mem_out"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("--skip-different-crs ignored"));
  }

  // 4. Input dataset provided as object instead of name fails.
  {
    std::unique_ptr<GDALDataset> open_ds(
        GDALDataset::Open(g1.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY));
    ASSERT_THAT(open_ds, NotNull());

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(open_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"-f", "MEM", "-o", "mem_out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("must be provided by name, not as object"));
  }

  // 5. Missing input yields "No layer to index".
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "MEM", std::string(kTestDir) + "/missing.gpkg", "mem_out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No layer to index"));
  }

  // 6. Append with missing location/source-crs fields in existing layer fails.
  {
    GDALVectorIndexAlgorithm setup;
    ASSERT_TRUE(setup.ParseCommandLineArguments({g1, idx_gpkg}));
    ASSERT_TRUE(setup.Run());
    ASSERT_TRUE(setup.Finalize());

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorIndexAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--append", "--location-name",
                                               "missing_loc", "--output-layer",
                                               "tileindex", g1, idx_gpkg}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to find field"));

    CPLErrorReset();
    GDALVectorIndexAlgorithm alg2;
    ASSERT_TRUE(alg2.ParseCommandLineArguments(
        {"--append", "--source-crs-field-name", "missing_crs", "--output-layer",
         "tileindex", g1, idx_gpkg}));
    EXPECT_FALSE(alg2.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to find field"));
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
