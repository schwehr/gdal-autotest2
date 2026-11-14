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
// Tests for GDALRasterAsFeaturesAlgorithm
// (apps/gdalalg_raster_as_features.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_as_features.cpp
//   https://gdal.org/programs/gdal_raster_as_features.html

#include "third_party/gdal/apps/gdalalg_raster_as_features.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_as_features_test";

void CreateFloat64Tiff(const std::string& path, int width, int height,
                       int bands, const std::vector<double>& values,
                       double nodata, bool has_nodata, bool tiled) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);

  CPLStringList creation_opts;
  if (tiled) {
    creation_opts.SetNameValue("TILED", "YES");
    creation_opts.SetNameValue("BLOCKXSIZE", "16");
    creation_opts.SetNameValue("BLOCKYSIZE", "16");
  }

  GDALDatasetUniquePtr ds(drv->Create(path.c_str(), width, height, bands,
                                      GDT_Float64, creation_opts.List()));
  ASSERT_NE(nullptr, ds);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);

  double gt[6] = {10.0, 2.0, 0.0, 20.0, 0.0, -4.0};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    if (has_nodata) {
      ASSERT_EQ(CE_None, band->SetNoDataValue(nodata));
    }
    const size_t offset = static_cast<size_t>(b - 1) * width * height;
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, width, height,
                             const_cast<double*>(values.data() + offset), width,
                             height, GDT_Float64, 0, 0, nullptr));
  }
}

class GdalalgRasterAsFeaturesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    RegisterOGRGeoPackage();
    RegisterOGRGeoJSON();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgRasterAsFeaturesTest, BasicProperties) {
  GDALRasterAsFeaturesAlgorithmStandalone alg;
  EXPECT_EQ("as-features", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_raster_as_features.html", alg.GetHelpURL());
  EXPECT_EQ(GDAL_OF_RASTER, alg.GetInputType());
  EXPECT_EQ(GDAL_OF_VECTOR, alg.GetOutputType());
}

TEST_F(GdalalgRasterAsFeaturesTest, DefaultNoneGeometryAndStreamOutput) {
  const std::string in_tif = std::string(kTestDir) + "/two_band.tif";
  const std::vector<double> values = {1.0,  2.0,  3.0,  4.0,
                                      10.0, 20.0, 30.0, 40.0};
  ASSERT_NO_FATAL_FAILURE(CreateFloat64Tiff(in_tif, 2, 2, 2, values, 0.0,
                                            /*has_nodata=*/false,
                                            /*tiled=*/false));

  GDALRasterAsFeaturesAlgorithmStandalone alg;
  ASSERT_TRUE(alg.ParseCommandLineArguments(
      {"--output-format", "stream", in_tif, "streamed"}));
  ASSERT_TRUE(alg.Run());

  GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
  ASSERT_NE(nullptr, out_ds);
  ASSERT_EQ(1, out_ds->GetLayerCount());

  OGRLayer* layer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbNone, layer->GetGeomType());
  EXPECT_TRUE(layer->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(4, layer->GetFeatureCount(1));

  std::vector<double> b1_vals;
  std::vector<double> b2_vals;
  while (auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
    EXPECT_EQ(nullptr, feat->GetGeometryRef());
    b1_vals.push_back(feat->GetFieldAsDouble("BAND_1"));
    b2_vals.push_back(feat->GetFieldAsDouble("BAND_2"));
  }
  ASSERT_EQ(4U, b1_vals.size());
  EXPECT_DOUBLE_EQ(1.0, b1_vals[0]);
  EXPECT_DOUBLE_EQ(10.0, b2_vals[0]);
  EXPECT_DOUBLE_EQ(4.0, b1_vals[3]);
  EXPECT_DOUBLE_EQ(40.0, b2_vals[3]);

  // Verify ResetReading rewinds cleanly.
  layer->ResetReading();
  {
    auto first_again = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
    ASSERT_NE(nullptr, first_again);
    EXPECT_DOUBLE_EQ(1.0, first_again->GetFieldAsDouble("BAND_1"));
  }

  // Close output layer before Finalize() closes the input GTiffDataset.
  EXPECT_TRUE(
      alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Get<GDALArgDatasetValue>().Close());
  EXPECT_TRUE(alg.Finalize());
}

TEST_F(GdalalgRasterAsFeaturesTest, PointGeometryIncludeXYRowColAndSkipNoData) {
  const std::string in_tif = std::string(kTestDir) + "/nodata.tif";
  const std::vector<double> values = {1.0, -9999.0, 3.0, 4.0, 5.0, 6.0};
  ASSERT_NO_FATAL_FAILURE(CreateFloat64Tiff(in_tif, 3, 2, 1, values, -9999.0,
                                            /*has_nodata=*/true,
                                            /*tiled=*/false));

  // 1. Without --skip-nodata: all 6 pixels emitted as Points.
  {
    GDALRasterAsFeaturesAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry-type", "point", "--include-xy", "--include-row-col",
         "--band", "1", "--output-format", "stream", in_tif, "streamed"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* layer = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(wkbPoint, layer->GetGeomType());
    EXPECT_TRUE(layer->TestCapability(OLCFastFeatureCount));
    EXPECT_EQ(6, layer->GetFeatureCount(1));

    {
      auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
      ASSERT_NE(nullptr, feat);
      EXPECT_EQ(0, feat->GetFieldAsInteger("ROW"));
      EXPECT_EQ(0, feat->GetFieldAsInteger("COL"));
      // Pixel (0, 0) center: x = 10 + 0.5*2 = 11, y = 20 + 0.5*(-4) = 18.
      EXPECT_DOUBLE_EQ(11.0, feat->GetFieldAsDouble("CENTER_X"));
      EXPECT_DOUBLE_EQ(18.0, feat->GetFieldAsDouble("CENTER_Y"));
      ASSERT_NE(nullptr, feat->GetGeometryRef());
      const OGRPoint* pt = feat->GetGeometryRef()->toPoint();
      EXPECT_DOUBLE_EQ(11.0, pt->getX());
      EXPECT_DOUBLE_EQ(18.0, pt->getY());
    }
    // Close output layer before Finalize() destroys input GTiffDataset::m_oSRS.
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Get<GDALArgDatasetValue>().Close());
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. With --skip-nodata: 5 features emitted, fast count disabled.
  {
    GDALRasterAsFeaturesAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--geometry-type", "point", "--skip-nodata", "--output-format",
         "stream", in_tif, "streamed"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* layer = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_FALSE(layer->TestCapability(OLCFastFeatureCount));
    EXPECT_EQ(5, layer->GetFeatureCount(1));
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Get<GDALArgDatasetValue>().Close());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgRasterAsFeaturesTest, PolygonGeometryWithNanNoDataAndWriteGpkg) {
  const std::string in_tif = std::string(kTestDir) + "/nan_nodata.tif";
  const std::string out_gpkg = std::string(kTestDir) + "/polygons.gpkg";
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> values = {1.0, nan_val, 3.0, 4.0};
  ASSERT_NO_FATAL_FAILURE(CreateFloat64Tiff(in_tif, 2, 2, 1, values, nan_val,
                                            /*has_nodata=*/true,
                                            /*tiled=*/false));

  {
    GDALRasterAsFeaturesAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({"--geometry-type", "polygon",
                                               "--skip-nodata", "--overwrite",
                                               in_tif, out_gpkg}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_gpkg.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, out_ds);
  OGRLayer* layer = out_ds->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(3, layer->GetFeatureCount(1));

  auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  ASSERT_NE(nullptr, feat->GetGeometryRef());
  EXPECT_EQ(wkbPolygon, wkbFlatten(feat->GetGeometryRef()->getGeometryType()));
  const OGRPolygon* poly = feat->GetGeometryRef()->toPolygon();
  EXPECT_DOUBLE_EQ(8.0, poly->get_Area());  // 2.0 * 4.0 pixel size
}

TEST_F(GdalalgRasterAsFeaturesTest, MultiWindowTiledAndEmptyRaster) {
  // 1. Tiled 20x20 raster with 16x16 blocks exercises multiple WindowIterator
  // windows.
  const std::string tiled_tif = std::string(kTestDir) + "/tiled.tif";
  std::vector<double> values(400, 7.0);
  ASSERT_NO_FATAL_FAILURE(CreateFloat64Tiff(tiled_tif, 20, 20, 1, values, 0.0,
                                            /*has_nodata=*/false,
                                            /*tiled=*/true));
  {
    GDALRasterAsFeaturesAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-format", "stream", tiled_tif, "streamed"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* layer = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    int count = 0;
    while (auto feat = std::unique_ptr<OGRFeature>(layer->GetNextFeature())) {
      ++count;
    }
    EXPECT_EQ(400, count);
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. Empty (0x0, 0-band) MEM dataset produces 0 features cleanly.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr empty_ds(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, empty_ds);
  {
    GDALRasterAsFeaturesAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> inputs;
    inputs.emplace_back(empty_ds.get());
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs)));
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--output-format", "stream", "--output", "streamed"}));
    ASSERT_TRUE(alg.Run());
    OGRLayer* layer = alg.GetOutputDataset().GetDatasetRef()->GetLayer(0);
    ASSERT_NE(nullptr, layer);
    EXPECT_EQ(nullptr, layer->GetNextFeature());
    EXPECT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT)->Get<GDALArgDatasetValue>().Close());
    EXPECT_TRUE(alg.Finalize());
  }
}

}  // namespace
}  // namespace autotest2
