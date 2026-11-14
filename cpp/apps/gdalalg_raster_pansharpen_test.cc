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
// Tests for GDALRasterPansharpenAlgorithm (apps/gdalalg_raster_pansharpen.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_pansharpen.cpp

#include "third_party/gdal/apps/gdalalg_raster_pansharpen.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_pansharpen_test";

class GdalalgRasterPansharpenTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    VSIRmdirRecursive(kTestDir);
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

void CreateGeorefTiff(const std::string& path, int width, int height, int bands,
                      double pixel_size, uint8_t base_val) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, bands, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());
  double gt[6] = {0.0, pixel_size, 0.0, 10.0, 0.0, -pixel_size};
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt));
  OGRSpatialReference srs;
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32632));
  ASSERT_EQ(CE_None, ds->SetSpatialRef(&srs));

  for (int b = 1; b <= bands; ++b) {
    std::vector<uint8_t> pixels(width * height,
                                static_cast<uint8_t>(base_val + b * 10));
    ASSERT_EQ(CE_None, ds->GetRasterBand(b)->RasterIO(
                           GF_Write, 0, 0, width, height, pixels.data(), width,
                           height, GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterPansharpenTest, StandaloneAllOptionsAndBandSyntax) {
  const std::string pan_tif = std::string(kTestDir) + "/pan.tif";
  const std::string spec_tif = std::string(kTestDir) + "/spec.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(pan_tif, 8, 8, 1, 1.0, 100));
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(spec_tif, 4, 4, 3, 2.0, 20));

  GDALRasterPansharpenAlgorithmStandalone alg;
  EXPECT_EQ("pansharpen", alg.GetName());
  ASSERT_TRUE(alg.ParseCommandLineArguments({
      "--panchromatic",
      pan_tif,
      "--spectral",
      spec_tif + ",band=1",
      "--spectral",
      spec_tif + ",band=2",
      "--spectral",
      spec_tif + ",band=3",
      "--resampling",
      "bilinear",
      "--weights",
      "0.3,0.4,0.3",
      "--nodata",
      "0",
      "--bit-depth",
      "8",
      "--spatial-extent-adjustment",
      "none-without-warning",
      "--num-threads",
      "1",
      "--output",
      out_tif,
      "--overwrite",
  }));
  EXPECT_TRUE(alg.Run());
  EXPECT_TRUE(alg.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(8, out_ds->GetRasterXSize());
  EXPECT_EQ(8, out_ds->GetRasterYSize());
  EXPECT_EQ(3, out_ds->GetRasterCount());
}

TEST_F(GdalalgRasterPansharpenTest, PipelineWithNestedSpectralDataset) {
  const std::string pan_tif = std::string(kTestDir) + "/pan_pipe.tif";
  const std::string spec_tif = std::string(kTestDir) + "/spec_pipe.tif";
  const std::string out_tif = std::string(kTestDir) + "/out_pipe.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(pan_tif, 8, 8, 1, 1.0, 80));
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(spec_tif, 4, 4, 3, 2.0, 10));

  // Direct GDALDataset* object in spectral argument exercises GetDatasetRef().
  {
    GDALDatasetUniquePtr spec_ds(
        GDALDataset::Open(spec_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(spec_ds, NotNull());

    GDALRasterPansharpenAlgorithmStandalone alg;
    std::vector<GDALArgDatasetValue> spectral_vals;
    spectral_vals.emplace_back(spec_ds.get());
    ASSERT_TRUE(alg.GetArg("spectral")->Set(std::move(spectral_vals)));
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--panchromatic",
        pan_tif,
        "--output",
        out_tif,
        "--overwrite",
    }));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(8, out_ds->GetRasterXSize());
    EXPECT_EQ(3, out_ds->GetRasterCount());
  }

  // Filename spectral dataset without ,band= syntax in pipeline.
  {
    const std::string out2_tif = std::string(kTestDir) + "/out_pipe2.tif";
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments({
        "read",
        pan_tif,
        "!",
        "pansharpen",
        spec_tif,
        "!",
        "write",
        out2_tif,
        "--overwrite",
    }));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out2_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(3, out_ds->GetRasterCount());
  }
}

TEST_F(GdalalgRasterPansharpenTest, ErrorPaths) {
  const std::string pan_tif = std::string(kTestDir) + "/pan_err.tif";
  const std::string multi_pan_tif = std::string(kTestDir) + "/multi_pan.tif";
  const std::string spec_tif = std::string(kTestDir) + "/spec_err.tif";
  const std::string out_tif = std::string(kTestDir) + "/err_out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(pan_tif, 4, 4, 1, 1.0, 50));
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(multi_pan_tif, 4, 4, 2, 1.0, 50));
  ASSERT_NO_FATAL_FAILURE(CreateGeorefTiff(spec_tif, 4, 4, 3, 1.0, 20));

  // 1. Multi-band panchromatic input rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPansharpenAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--panchromatic",
        multi_pan_tif,
        "--spectral",
        spec_tif,
        "--output",
        out_tif,
    }));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("panchromatic dataset must have a single band"));
    CPLErrorReset();
  }

  // 2. Non-existent spectral dataset fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPansharpenAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--panchromatic",
        pan_tif,
        "--spectral",
        std::string(kTestDir) + "/missing.tif",
        "--output",
        out_tif,
    }));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 3. Out-of-range ,band= in spectral argument fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterPansharpenAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({
        "--panchromatic",
        pan_tif,
        "--spectral",
        spec_tif + ",band=9",
        "--output",
        out_tif,
    }));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Illegal band"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
