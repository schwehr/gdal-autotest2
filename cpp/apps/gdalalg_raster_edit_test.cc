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
// Tests for GDALRasterEditAlgorithm (apps/gdalalg_raster_edit.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_edit.cpp

#include "third_party/gdal/apps/gdalalg_raster_edit.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_edit_test";

class FailingEditDataset final : public GDALDataset {
 public:
  FailingEditDataset(int width, int height, bool fail_srs = false,
                     bool fail_gt = false, bool fail_set_meta = false,
                     bool fail_unset_domain = false, bool fail_gcps = false)
      : fail_srs_(fail_srs),
        fail_gt_(fail_gt),
        fail_set_meta_(fail_set_meta),
        fail_unset_domain_(fail_unset_domain),
        fail_gcps_(fail_gcps) {
    eAccess = GA_Update;
    nRasterXSize = width;
    nRasterYSize = height;
  }

  CPLErr SetSpatialRef(const OGRSpatialReference* poSRS) override {
    return fail_srs_ ? CE_Failure : GDALDataset::SetSpatialRef(poSRS);
  }

  CPLErr SetGeoTransform(const GDALGeoTransform& gt) override {
    return fail_gt_ ? CE_Failure : GDALDataset::SetGeoTransform(gt);
  }

  CPLErr SetMetadata(char** papszMetadata, const char* pszDomain) override {
    if (fail_unset_domain_ && papszMetadata == nullptr) return CE_Failure;
    return GDALDataset::SetMetadata(papszMetadata, pszDomain);
  }

  CPLErr SetMetadataItem(const char* pszName, const char* pszValue,
                         const char* pszDomain) override {
    if (fail_set_meta_) return CE_Failure;
    return GDALDataset::SetMetadataItem(pszName, pszValue, pszDomain);
  }

  CPLErr SetGCPs(int nGCPCount, const GDAL_GCP* pasGCPList,
                 const OGRSpatialReference* poSRS) override {
    return fail_gcps_ ? CE_Failure
                      : GDALDataset::SetGCPs(nGCPCount, pasGCPList, poSRS);
  }

 private:
  bool fail_srs_;
  bool fail_gt_;
  bool fail_set_meta_;
  bool fail_unset_domain_;
  bool fail_gcps_;
};

class GdalalgRasterEditTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    RegisterOGRGeoJSON();
    RegisterOGRGeoPackage();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateTinyTiff(const std::string& path, bool with_srs = false) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  if (with_srs) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));
  }
  const std::vector<uint8_t> pixels = {1, 2,  3,  4,  5,  6,  7,  8,
                                       9, 10, 11, 12, 13, 14, 15, 16};
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4,
                                           const_cast<uint8_t*>(pixels.data()),
                                           4, 4, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalalgRasterEditTest, BasicProperties) {
  GDALRasterEditAlgorithm alg_step(false);
  EXPECT_EQ("edit", alg_step.GetName());
  EXPECT_FALSE(alg_step.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_edit.html", alg_step.GetHelpURL());

  GDALRasterEditAlgorithmStandalone alg_standalone;
  EXPECT_EQ("edit", alg_standalone.GetName());
}

TEST_F(GdalalgRasterEditTest, StandaloneEditMetadataBboxCrsNodataStatsHist) {
  const std::string tif_path = std::string(kTestDir) + "/edit_me.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path, /*with_srs=*/false));

  // Set pre-existing metadata to later unset it.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->SetMetadataItem("REMOVE_KEY", "val"));
    ASSERT_EQ(CE_None, ds->SetMetadataItem("DOM_KEY", "val", "CUSTOM_DOMAIN"));
  }

  // 1. Edit CRS, bbox, nodata, metadata, unset-metadata, unset-metadata-domain,
  // stats, and histogram with progress callback.
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif_path, "--crs", "EPSG:4326", "--bbox", "10,20,14,24", "--nodata",
         "0", "--metadata", "FOO=BAR", "--unset-metadata", "REMOVE_KEY",
         "--unset-metadata-domain", "CUSTOM_DOMAIN", "--stats", "--hist"}));
    int progress_calls = 0;
    auto progress = [](double, const char*, void* data) -> int {
      ++(*static_cast<int*>(data));
      return 1;
    };
    ASSERT_TRUE(alg.Run(progress, &progress_calls));
    EXPECT_GT(progress_calls, 0);
  }

  // Reopen and verify all edits.
  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_STREQ("4326", srs->GetAuthorityCode(nullptr));

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
    EXPECT_DOUBLE_EQ(24.0, gt[3]);
    EXPECT_DOUBLE_EQ(-1.0, gt[5]);

    int has_nodata = 0;
    EXPECT_DOUBLE_EQ(0.0, ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_EQ(1, has_nodata);

    EXPECT_STREQ("BAR", ds->GetMetadataItem("FOO"));
    EXPECT_EQ(nullptr, ds->GetMetadataItem("REMOVE_KEY"));
    EXPECT_EQ(nullptr, ds->GetMetadataItem("DOM_KEY", "CUSTOM_DOMAIN"));

    double min_v = 0, max_v = 0, mean_v = 0, std_v = 0;
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetStatistics(
                           FALSE, FALSE, &min_v, &max_v, &mean_v, &std_v));
    EXPECT_DOUBLE_EQ(1.0, min_v);
    EXPECT_DOUBLE_EQ(16.0, max_v);
  }

  // 2. Clear CRS ("none"), clear nodata ("none"), and compute approx-stats.
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif_path, "--crs", "none", "--nodata", "none", "--approx-stats"}));
    ASSERT_TRUE(alg.Run());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetSpatialRef());
    int has_nodata = 1;
    ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
    EXPECT_EQ(0, has_nodata);
  }
}

TEST_F(GdalalgRasterEditTest, InlineAndFileGCPs) {
  const std::string tif_path = std::string(kTestDir) + "/gcp.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));

  // 1. Inline 4-value and 5-value GCPs with CRS.
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({tif_path, "--crs", "EPSG:4326", "--gcp",
                                       "0,0,10,20", "--gcp", "4,4,14,24,100"}));
    ASSERT_TRUE(alg.Run());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetGCPCount());
    const GDAL_GCP* gcps = ds->GetGCPs();
    ASSERT_NE(nullptr, gcps);
    EXPECT_DOUBLE_EQ(10.0, gcps[0].dfGCPX);
    EXPECT_DOUBLE_EQ(100.0, gcps[1].dfGCPZ);
  }

  // 2. GCPs loaded from @filename vector dataset (with id, info, z, and null
  // z).
  const std::string gcp_geojson = std::string(kTestDir) + "/gcps.geojson";
  VsiMemTempWrapper geojson_file(gcp_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "geometry": null,
        "properties": {
          "id": "p1", "info": "corner",
          "column": 0.0, "line": 0.0, "x": 1.0, "y": 2.0, "z": 3.0
        }
      },
      {
        "type": "Feature",
        "geometry": null,
        "properties": {
          "id": "p2", "info": "other",
          "column": 4.0, "line": 4.0, "x": 5.0, "y": 6.0, "z": null
        }
      }
    ]
  })json");

  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({tif_path, "--gcp", "@" + gcp_geojson}));
    ASSERT_TRUE(alg.Run());
  }

  {
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetGCPCount());
    const GDAL_GCP* gcps = ds->GetGCPs();
    ASSERT_NE(nullptr, gcps);
    // GeoTIFF tiepoint tags do not persist pszId/pszInfo.
    EXPECT_DOUBLE_EQ(1.0, gcps[0].dfGCPX);
    EXPECT_DOUBLE_EQ(2.0, gcps[0].dfGCPY);
    EXPECT_DOUBLE_EQ(3.0, gcps[0].dfGCPZ);
    EXPECT_DOUBLE_EQ(5.0, gcps[1].dfGCPX);
    EXPECT_DOUBLE_EQ(0.0, gcps[1].dfGCPZ);
  }

  // Verify pszId and pszInfo preservation on a MEM dataset.
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    std::unique_ptr<GDALDataset> mem_ds(
        mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mem_ds);
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("dataset")->Set(mem_ds.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--gcp", "@" + gcp_geojson}));
    ASSERT_TRUE(alg.Run());

    EXPECT_EQ(2, mem_ds->GetGCPCount());
    const GDAL_GCP* gcps = mem_ds->GetGCPs();
    ASSERT_NE(nullptr, gcps);
    EXPECT_STREQ("p1", gcps[0].pszId);
    EXPECT_STREQ("corner", gcps[0].pszInfo);
    EXPECT_DOUBLE_EQ(3.0, gcps[0].dfGCPZ);
    EXPECT_STREQ("p2", gcps[1].pszId);
    EXPECT_STREQ("other", gcps[1].pszInfo);
  }
}

TEST_F(GdalalgRasterEditTest, PipelineMode) {
  const std::string in_tif = std::string(kTestDir) + "/pipe_in.tif";
  const std::string out_tif = std::string(kTestDir) + "/pipe_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(in_tif));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "edit", "--crs", "EPSG:4326", "--bbox", "0,0,4,4",
       "--metadata", "PIPE=YES", "!", "write", out_tif, "--overwrite"}));
  ASSERT_TRUE(pipeline.Run());

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  EXPECT_STREQ("YES", ds->GetMetadataItem("PIPE"));
  ASSERT_NE(nullptr, ds->GetSpatialRef());
  EXPECT_STREQ("4326", ds->GetSpatialRef()->GetAuthorityCode(nullptr));
}

TEST_F(GdalalgRasterEditTest, ValidationAndErrorPaths) {
  const std::string tif_path = std::string(kTestDir) + "/err.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTinyTiff(tif_path));

  // 1. Bad GCP format (wrong token count and non-numeric token).
  {
    GDALRasterEditAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.ParseCommandLineArguments({tif_path, "--gcp", "1,2,3"}));
  }
  {
    GDALRasterEditAlgorithmStandalone alg;
    WithQuietHandler quiet;
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({tif_path, "--gcp", "1,2,abc,4"}));
  }

  // 2. Read-only dataset without --auxiliary fails.
  {
    std::unique_ptr<GDALDataset> ro_ds(
        GDALDataset::Open(tif_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                          nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ro_ds);
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("dataset")->Set(ro_ds.get()));
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--metadata", "A=B"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  // 3. @filename GCP errors: missing file, multi-layer dataset, missing field.
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif_path, "--gcp", "@/vsimem/no_such_gcp_file.geojson"}));
    WithQuietHandler quiet;
    EXPECT_FALSE(alg.Run());
  }

  const std::string multi_gpkg = std::string(kTestDir) + "/multi.gpkg";
  {
    GDALDriver* gpkg_drv = GetGDALDriverManager()->GetDriverByName("GPKG");
    ASSERT_NE(nullptr, gpkg_drv);
    std::unique_ptr<GDALDataset> ds(
        gpkg_drv->Create(multi_gpkg.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->CreateLayer("l1", nullptr, wkbPoint, nullptr));
    ASSERT_NE(nullptr, ds->CreateLayer("l2", nullptr, wkbPoint, nullptr));
  }
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({tif_path, "--gcp", "@" + multi_gpkg}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("single-layer"));
  }

  const std::string missing_field_geojson =
      std::string(kTestDir) + "/missing_field.geojson";
  VsiMemTempWrapper missing_field_file(missing_field_geojson, R"json({
    "type": "FeatureCollection",
    "features": [
      {
        "type": "Feature",
        "geometry": null,
        "properties": { "column": 0.0, "line": 0.0, "x": 1.0 }
      }
    ]
  })json");
  {
    GDALRasterEditAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {tif_path, "--gcp", "@" + missing_field_geojson}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot be found"));
  }

  // 4. FailingEditDataset covers SetSpatialRef, null width/height bbox,
  // SetGeoTransform, SetMetadataItem, SetMetadata, and SetGCPs failures.
  {
    auto fail_ds = std::make_unique<FailingEditDataset>(
        0, 0, /*fail_srs=*/true, /*fail_gt=*/true, /*fail_set_meta=*/true,
        /*fail_unset_domain=*/true, /*fail_gcps=*/true);
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--crs", "null"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--crs", "EPSG:4326"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,1,1"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
  }

  {
    auto fail_ds = std::make_unique<FailingEditDataset>(
        4, 4, /*fail_srs=*/false, /*fail_gt=*/true, /*fail_set_meta=*/true,
        /*fail_unset_domain=*/true, /*fail_gcps=*/true);
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--bbox", "0,0,1,1"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--metadata", "K=V"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--unset-metadata", "K"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(
          alg.ParseCommandLineArguments({"--unset-metadata-domain", "DOM"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
    {
      GDALRasterEditAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("dataset")->Set(fail_ds.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--gcp", "0,0,1,1"}));
      WithQuietHandler quiet;
      EXPECT_FALSE(alg.Run());
    }
  }
}

}  // namespace
}  // namespace autotest2
