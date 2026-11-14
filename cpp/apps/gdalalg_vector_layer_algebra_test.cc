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
// Tests for GDALVectorLayerAlgebraAlgorithm
// (apps/gdalalg_vector_layer_algebra.cpp).
//
// Caveats:
//   //third_party/gdal compiles with -DHAVE_GEOS, whereas test targets do not
//   receive -DHAVE_GEOS; GEOS availability is verified at runtime via
//   OGRGeometryFactory::haveGEOS().
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_layer_algebra.cpp

#include "third_party/gdal/apps/gdalalg_vector_layer_algebra.h"

#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_layer_algebra_test";

std::unique_ptr<GDALDataset> CreatePolyGpkg(const std::string& path,
                                            const std::string& layer_name,
                                            const char* wkt,
                                            const std::string& field_name,
                                            int field_val,
                                            bool add_extra_layer = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr =
      ds->CreateLayer(layer_name.c_str(), nullptr, wkbPolygon, nullptr);
  if (!lyr) return nullptr;
  OGRFieldDefn fld(field_name.c_str(), OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr->CreateField(&fld));

  auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
  feat->SetField(field_name.c_str(), field_val);
  auto [geom, err] = OGRGeometryFactory::createFromWkt(wkt);
  EXPECT_EQ(OGRERR_NONE, err);
  feat->SetGeometryDirectly(geom.release());
  EXPECT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));

  if (add_extra_layer) {
    OGRLayer* lyr2 = ds->CreateLayer("extra", nullptr, wkbPolygon, nullptr);
    if (!lyr2) return nullptr;
  }
  return ds;
}

class GdalalgVectorLayerAlgebraTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    RegisterOGRShape();
    ASSERT_TRUE(OGRGeometryFactory::haveGEOS());
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorLayerAlgebraTest, Properties) {
  GDALVectorLayerAlgebraAlgorithm alg;
  EXPECT_EQ("layer-algebra", alg.GetName());
  EXPECT_EQ("Perform algebraic operation between 2 layers.",
            alg.GetDescription());
}

TEST_F(GdalalgVectorLayerAlgebraTest, AllAlgebraOperationsAndFieldOptions) {
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string method_gpkg = std::string(kTestDir) + "/method.gpkg";
  ASSERT_NE(nullptr,
            CreatePolyGpkg(in_gpkg, "layer_a", "POLYGON((0 0,4 0,4 4,0 4,0 0))",
                           "a_val", 10,
                           /*add_extra_layer=*/true));
  ASSERT_NE(nullptr,
            CreatePolyGpkg(method_gpkg, "layer_b",
                           "POLYGON((2 0,6 0,6 4,2 4,2 0))", "b_val", 20,
                           /*add_extra_layer=*/true));

  // 1. Test all 7 operations writing into new GPKG files.
  for (const std::string& op : {"union", "intersection", "sym-difference",
                                "identity", "update", "clip", "erase"}) {
    const std::string out_gpkg = std::string(kTestDir) + "/out_" + op + ".gpkg";
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {op, "--input-layer", "layer_a", "--method-layer", "layer_b", in_gpkg,
         method_gpkg, out_gpkg, "--overwrite"}))
        << "Parse failed for op: " << op;
    EXPECT_TRUE(alg.Run()) << "Run failed for op: " << op;
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_GT(out_lyr->GetFeatureCount(), 0);
  }

  // 2. Test explicit prefixes, specific fields, geometry-type PROMOTE_TO_MULTI,
  //    creation options, and Shapefile default layer name.
  const std::string out_shp = std::string(kTestDir) + "/shape_out.shp";
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"intersection", "--input-layer", "layer_a", "--method-layer",
         "layer_b", "--input-prefix", "in_", "--method-prefix", "m_",
         "--input-field", "a_val", "--method-field", "b_val", "--geometry-type",
         "MULTIPOLYGON", in_gpkg, method_gpkg, out_shp, "--overwrite"}));
    auto progress = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress, nullptr));
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> shp_ds(
        GDALDataset::Open(out_shp.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, shp_ds);
    OGRLayer* shp_lyr = shp_ds->GetLayerByName("shape_out");
    ASSERT_NE(nullptr, shp_lyr);
    EXPECT_GE(shp_lyr->GetLayerDefn()->GetFieldIndex("in_a_val"), 0);
    EXPECT_GE(shp_lyr->GetLayerDefn()->GetFieldIndex("m_b_val"), 0);
  }

  // 3. Test --no-input-field and --no-method-field.
  const std::string out_nofields = std::string(kTestDir) + "/nofields.gpkg";
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--input-layer", "layer_a", "--method-layer", "layer_b",
         "--no-input-field", "--no-method-field", "--nln", "custom_out",
         in_gpkg, method_gpkg, out_nofields, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_nofields.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("custom_out");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(0, lyr->GetLayerDefn()->GetFieldCount());
  }

  // 4. Test --all-input-field + --all-method-field on clip (which defaults to
  //    --no-method-field), plus --lco.
  const std::string out_clip_fields = std::string(kTestDir) + "/clip_all.gpkg";
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"clip", "--input-layer", "layer_a", "--method-layer", "layer_b",
         "--all-input-field", "--all-method-field", "--lco", "FID=custom_fid",
         in_gpkg, method_gpkg, out_clip_fields, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_clip_fields.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("output");
    ASSERT_NE(nullptr, lyr);
    EXPECT_STREQ("custom_fid", lyr->GetFIDColumn());
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("input_a_val"), 0);
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("method_b_val"), 0);
  }

  // 5. Test --no-input-field alone (method fields keep empty default prefix).
  const std::string out_no_in = std::string(kTestDir) + "/no_in_fields.gpkg";
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--input-layer", "layer_a", "--method-layer", "layer_b",
         "--no-input-field", in_gpkg, method_gpkg, out_no_in, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_no_in.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("output");
    ASSERT_NE(nullptr, lyr);
    EXPECT_LT(lyr->GetLayerDefn()->GetFieldIndex("a_val"), 0);
    EXPECT_GE(lyr->GetLayerDefn()->GetFieldIndex("b_val"), 0);
  }

  // 6. Test direct MEM output dataset object with explicit --output-layer.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> mem_out_ds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, mem_out_ds);

    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.GetArg("operation")->Set("intersection"));
    ASSERT_TRUE(alg.GetArg("input")->Set(in_gpkg));
    ASSERT_TRUE(alg.GetArg("input-layer")->Set("layer_a"));
    ASSERT_TRUE(alg.GetArg("method")->Set(method_gpkg));
    ASSERT_TRUE(alg.GetArg("method-layer")->Set("layer_b"));
    ASSERT_TRUE(alg.GetArg("output")->Set(mem_out_ds.get()));
    ASSERT_TRUE(alg.GetArg("output-layer")->Set("mem_out"));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    OGRLayer* mem_lyr = mem_out_ds->GetLayerByName("mem_out");
    ASSERT_NE(nullptr, mem_lyr);
    EXPECT_GT(mem_lyr->GetFeatureCount(), 0);
  }
}

TEST_F(GdalalgVectorLayerAlgebraTest, UpdateAppendAndOverwriteLayer) {
  const std::string in_gpkg = std::string(kTestDir) + "/u_in.gpkg";
  const std::string method_gpkg = std::string(kTestDir) + "/u_method.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/u_out.gpkg";
  ASSERT_NE(nullptr, CreatePolyGpkg(in_gpkg, "a",
                                    "POLYGON((0 0,4 0,4 4,0 4,0 0))", "id", 1));
  ASSERT_NE(nullptr,
            CreatePolyGpkg(method_gpkg, "b", "POLYGON((2 0,6 0,6 4,2 4,2 0))",
                           "mid", 2));

  // Initial write creates single layer 'res'.
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"intersection", "--output-layer",
                                               "res", in_gpkg, method_gpkg,
                                               out_gpkg, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 1. --append without --output-layer onto single-layer GPKG.
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"intersection", "--append", in_gpkg, method_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* res_lyr = out_ds->GetLayerByName("res");
    ASSERT_NE(nullptr, res_lyr);
    EXPECT_EQ(2, res_lyr->GetFeatureCount());
  }

  // 2. --overwrite-layer without --output-layer onto single-layer GPKG.
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"intersection", "--overwrite-layer", in_gpkg, method_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* out_lyr = out_ds->GetLayer(0);
    ASSERT_NE(nullptr, out_lyr);
    EXPECT_EQ(1, out_lyr->GetFeatureCount());
  }

  // 3. --update creating a second distinct layer, then --append /
  //    --overwrite-layer with explicit --output-layer.
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--update", "--output-layer", "second_lyr", in_gpkg,
         method_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--append", "--output-layer", "second_lyr", in_gpkg,
         method_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--overwrite-layer", "--output-layer", "second_lyr", in_gpkg,
         method_gpkg, out_gpkg}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgVectorLayerAlgebraTest, ErrorPaths) {
  const std::string in_gpkg = std::string(kTestDir) + "/e_in.gpkg";
  const std::string method_gpkg = std::string(kTestDir) + "/e_method.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/e_out.gpkg";
  ASSERT_NE(nullptr, CreatePolyGpkg(in_gpkg, "a",
                                    "POLYGON((0 0,4 0,4 4,0 4,0 0))", "id", 1));
  ASSERT_NE(nullptr,
            CreatePolyGpkg(method_gpkg, "b", "POLYGON((2 0,6 0,6 4,2 4,2 0))",
                           "mid", 2));
  ASSERT_NE(nullptr, CreatePolyGpkg(out_gpkg, "existing",
                                    "POLYGON((0 0,1 0,1 1,0 1,0 0))", "x", 1,
                                    /*add_extra_layer=*/true));

  // 1. Input and method dataset are the same object -> error.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> mem_ds(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, mem_ds);
    mem_ds->CreateLayer("l", nullptr, wkbPolygon, nullptr);

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.GetArg("operation")->Set("union"));
    ASSERT_TRUE(alg.GetArg("input")->Set(mem_ds.get()));
    ASSERT_TRUE(alg.GetArg("method")->Set(mem_ds.get()));
    ASSERT_TRUE(alg.GetArg("output-format")->Set("MEM"));
    ASSERT_TRUE(alg.GetArg("output")->Set(""));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 2. Cannot guess driver for unknown extension.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", in_gpkg, method_gpkg,
         std::string(kTestDir) + "/out.unknown_ext"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 3. Multi-layer existing output without --output-layer fails for
  //    --update, --append, and --overwrite-layer.
  for (const std::string& flag :
       {"--update", "--append", "--overwrite-layer"}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", flag, in_gpkg, method_gpkg, out_gpkg}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 4. --overwrite-layer / --append with non-existent --output-layer fails.
  for (const std::string& flag : {"--overwrite-layer", "--append"}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"union", flag, "--output-layer",
                                               "missing_lyr", in_gpkg,
                                               method_gpkg, out_gpkg}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 5. --update with already existing --output-layer fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--update", "--output-layer", "existing", in_gpkg,
         method_gpkg, out_gpkg}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 6. Missing --input-layer or --method-layer fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--input-layer", "missing", in_gpkg, method_gpkg,
         std::string(kTestDir) + "/never.gpkg"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", "--method-layer", "missing", in_gpkg, method_gpkg,
         std::string(kTestDir) + "/never2.gpkg"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 7. Output dataset creation failure (missing POSIX parent directory).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorLayerAlgebraAlgorithm alg;
    const std::string fail_gpkg = (std::filesystem::path(::testing::TempDir()) /
                                   "missing_lalg_dir" / "fail.gpkg")
                                      .string();
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"union", in_gpkg, method_gpkg, fail_gpkg}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
