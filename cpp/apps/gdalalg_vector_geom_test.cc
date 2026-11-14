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
// Tests for GDALVectorGeomAlgorithm, GDALVectorGeomAbstractAlgorithm,
// GDALVectorGeomOneToOneAlgorithmLayer, and
// GDALGeosNonStreamingAlgorithmDataset (apps/gdalalg_vector_geom.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_vector_geom.cpp

#include "third_party/gdal/apps/gdalalg_vector_geom.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_vector_buffer.h"
#include "third_party/gdal/apps/gdalalg_vector_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_geos.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_vector_geom_test";

std::unique_ptr<GDALDataset> CreateTwoLayerMemDataset() {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  std::unique_ptr<GDALDataset> ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  if (!ds) return nullptr;

  OGRLayer* lyr1 = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
  if (!lyr1) return nullptr;
  OGRFieldDefn id_fld("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, lyr1->CreateField(&id_fld));

  for (int i = 0; i < 3; ++i) {
    auto feat = std::make_unique<OGRFeature>(lyr1->GetLayerDefn());
    feat->SetField("id", i + 1);
    auto pt = std::make_unique<OGRPoint>(static_cast<double>(i),
                                         static_cast<double>(i));
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, lyr1->CreateFeature(feat.get()));
  }

  OGRLayer* lyr2 = ds->CreateLayer("pass", nullptr, wkbPoint, nullptr);
  if (!lyr2) return nullptr;
  auto feat2 = std::make_unique<OGRFeature>(lyr2->GetLayerDefn());
  auto pt2 = std::make_unique<OGRPoint>(10.0, 10.0);
  feat2->SetGeometryDirectly(pt2.release());
  EXPECT_EQ(OGRERR_NONE, lyr2->CreateFeature(feat2.get()));
  return ds;
}

#ifdef HAVE_GEOS
class TestGeosNonStreamingDataset final
    : public GDALGeosNonStreamingAlgorithmDataset {
 public:
  TestGeosNonStreamingDataset(bool polygons_only, bool skip_empty,
                              bool fail_process = false)
      : m_polygons_only(polygons_only),
        m_skip_empty(skip_empty),
        m_fail_process(fail_process) {}

  bool ProcessGeos() override {
    if (m_fail_process) return false;
    std::vector<GEOSGeometry*> clones;
    clones.reserve(m_apoGeosInputs.size());
    for (GEOSGeometry* g : m_apoGeosInputs) {
      clones.push_back(GEOSGeom_clone_r(m_poGeosContext, g));
    }
    m_poGeosResultAsCollection = GEOSGeom_createCollection_r(
        m_poGeosContext, GEOS_GEOMETRYCOLLECTION, clones.data(),
        static_cast<unsigned int>(clones.size()));
    return m_poGeosResultAsCollection != nullptr;
  }

  bool PolygonsOnly() const override { return m_polygons_only; }

  bool SkipEmpty() const override { return m_skip_empty; }

 private:
  bool m_polygons_only;
  bool m_skip_empty;
  bool m_fail_process;
};
#endif  // HAVE_GEOS

class GdalalgVectorGeomTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgVectorGeomTest, DeprecatedGeomRunAndSubcommands) {
  // 1. Calling Run() directly on GDALVectorGeomAlgorithm fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGeomAlgorithm alg;
    EXPECT_EQ("geom", alg.GetName());
    EXPECT_FALSE(alg.Run());
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Running subcommand via GDALVectorGeomAlgorithmStandalone triggers
  //    WarnIfDeprecated() and executes the subalgorithm.
  const std::string in_gpkg = std::string(kTestDir) + "/in.gpkg";
  const std::string out_gpkg = std::string(kTestDir) + "/out.gpkg";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(in_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* lyr = ds->CreateLayer("pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, lyr);
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    feat->SetGeometryDirectly(pt.release());
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALVectorGeomAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"swap-xy", in_gpkg, out_gpkg, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
    CPLErrorReset();
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, out_ds);
    auto feat =
        std::unique_ptr<OGRFeature>(out_ds->GetLayer(0)->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
    ASSERT_NE(nullptr, pt);
    EXPECT_DOUBLE_EQ(2.0, pt->getX());
    EXPECT_DOUBLE_EQ(1.0, pt->getY());
  }
}

TEST_F(GdalalgVectorGeomTest, AbstractAlgorithmAndOneToOneLayer) {
  auto src_ds = CreateTwoLayerMemDataset();
  ASSERT_NE(nullptr, src_ds);

  // 1. Non-existent --active-geometry skips geometry transformation.
  {
    GDALVectorBufferAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "pts", "--active-geometry", "nonexistent_geom",
         "--distance", "1.0"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    auto feat = std::unique_ptr<OGRFeature>(
        out_ds->GetLayerByName("pts")->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbPoint, wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
  }

  {
    GDALVectorBufferAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--active-layer", "pts", "--distance", "1.0"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetLayerCount());

    OGRLayer* pts_lyr = out_ds->GetLayerByName("pts");
    ASSERT_NE(nullptr, pts_lyr);
    EXPECT_EQ(3, pts_lyr->GetFeatureCount(TRUE));
    EXPECT_TRUE(pts_lyr->TestCapability(OLCRandomRead));
    EXPECT_TRUE(pts_lyr->TestCapability(OLCFastFeatureCount));
    EXPECT_FALSE(pts_lyr->TestCapability(OLCRandomWrite));

    OGREnvelope env;
    EXPECT_EQ(OGRERR_NONE, pts_lyr->GetExtent(0, &env, TRUE));

    // Random read valid and invalid FIDs.
    auto feat = std::unique_ptr<OGRFeature>(pts_lyr->GetFeature(0));
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(wkbPolygon,
              wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
    EXPECT_EQ(nullptr, pts_lyr->GetFeature(999));

    // Attribute filter exercises non-fast GetFeatureCount and filtered
    // TestCapability(OLCFastFeatureCount).
    ASSERT_EQ(OGRERR_NONE, pts_lyr->SetAttributeFilter("id = 2"));
    EXPECT_FALSE(pts_lyr->TestCapability(OLCFastFeatureCount));
    EXPECT_EQ(1, pts_lyr->GetFeatureCount(TRUE));
    ASSERT_EQ(OGRERR_NONE, pts_lyr->SetAttributeFilter(nullptr));

    // Second layer was passed through unchanged.
    OGRLayer* pass_lyr = out_ds->GetLayerByName("pass");
    ASSERT_NE(nullptr, pass_lyr);
    auto pass_feat = std::unique_ptr<OGRFeature>(pass_lyr->GetNextFeature());
    ASSERT_NE(nullptr, pass_feat);
    EXPECT_EQ(wkbPoint,
              wkbFlatten(pass_feat->GetGeometryRef()->getGeometryType()));
  }
}

#ifdef HAVE_GEOS
TEST_F(GdalalgVectorGeomTest, GeosNonStreamingAlgorithmDataset) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);

  // Build source MEM dataset with polygon, empty polygon, and null geometry.
  std::unique_ptr<GDALDataset> src_ds(
      drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, src_ds);
  OGRLayer* src_lyr = src_ds->CreateLayer("poly", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, src_lyr);
  OGRFieldDefn fld("tag", OFTString);
  ASSERT_EQ(OGRERR_NONE, src_lyr->CreateField(&fld));

  {
    auto f1 = std::make_unique<OGRFeature>(src_lyr->GetLayerDefn());
    f1->SetField("tag", "valid");
    auto [g1, e1] =
        OGRGeometryFactory::createFromWkt("POLYGON((0 0,2 0,2 2,0 2,0 0))");
    ASSERT_EQ(OGRERR_NONE, e1);
    f1->SetGeometryDirectly(g1.release());
    ASSERT_EQ(OGRERR_NONE, src_lyr->CreateFeature(f1.get()));
  }
  {
    auto f2 = std::make_unique<OGRFeature>(src_lyr->GetLayerDefn());
    f2->SetField("tag", "empty");
    f2->SetGeometryDirectly(new OGRPolygon());
    ASSERT_EQ(OGRERR_NONE, src_lyr->CreateFeature(f2.get()));
  }
  {
    auto f3 = std::make_unique<OGRFeature>(src_lyr->GetLayerDefn());
    f3->SetField("tag", "null");
    ASSERT_EQ(OGRERR_NONE, src_lyr->CreateFeature(f3.get()));
  }

  // 1. sameDefn = true, SkipEmpty = true -> skips empty & null features.
  {
    TestGeosNonStreamingDataset geos_ds(/*polygons_only=*/true,
                                        /*skip_empty=*/true);
    geos_ds.SetSourceGeometryField(0);
    EXPECT_TRUE(geos_ds.AddProcessedLayer(*src_lyr));
    ASSERT_EQ(1, geos_ds.GetLayerCount());
    EXPECT_EQ(1, geos_ds.GetLayer(0)->GetFeatureCount());
  }

  // 2. sameDefn = false (extra field on dstDefn) and SkipEmpty = false.
  {
    auto* dst_defn = src_lyr->GetLayerDefn()->Clone();
    dst_defn->Reference();
    absl::Cleanup rel = [dst_defn] { dst_defn->Release(); };
    OGRFieldDefn extra("extra", OFTInteger);
    dst_defn->AddFieldDefn(&extra);

    TestGeosNonStreamingDataset geos_ds(/*polygons_only=*/false,
                                        /*skip_empty=*/false);
    EXPECT_TRUE(geos_ds.AddProcessedLayer(*src_lyr, *dst_defn));
    EXPECT_EQ(3, geos_ds.GetLayer(0)->GetFeatureCount());
  }

  // 3. ProcessGeos failure branch.
  {
    TestGeosNonStreamingDataset geos_ds(/*polygons_only=*/false,
                                        /*skip_empty=*/false,
                                        /*fail_process=*/true);
    EXPECT_FALSE(geos_ds.AddProcessedLayer(*src_lyr));
  }

  // 4. PolygonsOnly rejects non-polygonal layer features.
  {
    auto pt_ds = CreateTwoLayerMemDataset();
    ASSERT_NE(nullptr, pt_ds);
    WithQuietHandler quiet;
    CPLErrorReset();
    TestGeosNonStreamingDataset geos_ds(/*polygons_only=*/true,
                                        /*skip_empty=*/false);
    EXPECT_FALSE(geos_ds.AddProcessedLayer(*pt_ds->GetLayer(0)));
    EXPECT_NE(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }
}
#endif  // HAVE_GEOS

void CreateGpkgWithPolygonsAndAttrs(const std::string& path,
                                    const char* layer_name,
                                    OGRwkbGeometryType geom_type,
                                    const std::vector<std::string>& wkts) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GPKG");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* lyr = ds->CreateLayer(layer_name, nullptr, geom_type, nullptr);
  ASSERT_NE(nullptr, lyr);
  OGRFieldDefn id_fld("id", OFTInteger);
  ASSERT_EQ(OGRERR_NONE, lyr->CreateField(&id_fld));

  int next_id = 1;
  for (const std::string& wkt : wkts) {
    auto feat = std::make_unique<OGRFeature>(lyr->GetLayerDefn());
    feat->SetField("id", next_id++);
    if (!wkt.empty()) {
      auto [geom, err] = OGRGeometryFactory::createFromWkt(wkt.c_str());
      ASSERT_EQ(OGRERR_NONE, err);
      feat->SetGeometryDirectly(geom.release());
    }
    ASSERT_EQ(OGRERR_NONE, lyr->CreateFeature(feat.get()));
  }
}

TEST_F(GdalalgVectorGeomTest, GeosNonStreamingViaCoverageAlgorithms) {
  // Exercises GDALGeosNonStreamingAlgorithmDataset (compiled with HAVE_GEOS
  // inside //third_party/gdal) via vector pipeline coverage steps.

  // 1. simplify-coverage exercises sameDefn == true and SkipEmpty() == false.
  const std::string simp_in = std::string(kTestDir) + "/simp_in.gpkg";
  const std::string simp_out = std::string(kTestDir) + "/simp_out.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateGpkgWithPolygonsAndAttrs(
      simp_in, "polys", wkbPolygon,
      {"POLYGON ((0 0, 5 0.1, 10 0, 10 10, 0 10, 0 0))",
       "POLYGON ((0 10, 10 10, 10 20, 0 20, 0 10))"}));
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", simp_in, "!", "simplify-coverage", "1.0", "!", "write",
         simp_out, "--overwrite"}));
    ASSERT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(simp_out.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    OGRLayer* lyr = out_ds->GetLayerByName("polys");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount());
  }

  // 2. check-coverage exercises sameDefn == false, SkipEmpty() == true/false,
  //    and forceTo() when output geometry type differs from layer geom type.
  const std::string valid_in = std::string(kTestDir) + "/valid_cov.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgWithPolygonsAndAttrs(valid_in, "polys", wkbPolygon,
                                     {"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                                      "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))"}));
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", valid_in, "!", "check-coverage"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* lyr = pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
        "invalid_edge");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(0, lyr->GetFeatureCount(TRUE));
    EXPECT_TRUE(pipeline.Finalize());
  }
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", valid_in, "!", "check-coverage", "--include-valid"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* lyr = pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
        "invalid_edge");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount(TRUE));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 3. Overlapping polygons write non-empty invalid edges via CreateFeature().
  const std::string overlap_in = std::string(kTestDir) + "/overlap_cov.gpkg";
  ASSERT_NO_FATAL_FAILURE(
      CreateGpkgWithPolygonsAndAttrs(overlap_in, "polys", wkbPolygon,
                                     {"POLYGON ((0 0, 1.5 0, 1.5 1, 0 1, 0 0))",
                                      "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0))"}));
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", overlap_in, "!", "check-coverage"}));
    ASSERT_TRUE(pipeline.Run());
    OGRLayer* lyr = pipeline.GetOutputDataset().GetDatasetRef()->GetLayerByName(
        "invalid_edge");
    ASSERT_NE(nullptr, lyr);
    EXPECT_EQ(2, lyr->GetFeatureCount(TRUE));
    EXPECT_TRUE(pipeline.Finalize());
  }

  // 4. Polygon followed by non-polygon fails PolygonsOnly() and cleans up
  //    already-converted m_apoGeosInputs in destructor.
  const std::string mixed_in = std::string(kTestDir) + "/mixed_geom.gpkg";
  ASSERT_NO_FATAL_FAILURE(CreateGpkgWithPolygonsAndAttrs(
      mixed_in, "mixed", wkbUnknown,
      {"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))", "POINT (5 5)"}));
  {
    GDALVectorPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", mixed_in, "!", "check-coverage"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(pipeline.Run());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("polygonal geometries"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
