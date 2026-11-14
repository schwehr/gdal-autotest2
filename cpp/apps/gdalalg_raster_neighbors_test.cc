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
// Tests for "neighbors" step of "raster pipeline"
// (apps/gdalalg_raster_neighbors.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_neighbors.cpp

#include "third_party/gdal/apps/gdalalg_raster_neighbors.h"

#include <cmath>
#include <cstdint>
#include <limits>
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
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_neighbors_test";
constexpr char kTmpDir[] = "/vsimem/gdalalg_raster_neighbors_tmp";

class GdalalgRasterNeighborsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
    ASSERT_EQ(0, VSIMkdir(kTmpDir, 0755));
    CPLSetConfigOption("CPL_TMPDIR", kTmpDir);
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }

  void TearDown() override {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(kTestDir);
    VSIRmdirRecursive(kTmpDir);
    CPLErrorReset();
  }
};

std::unique_ptr<GDALDataset> CreateTestMemDataset(
    int width, int height, int bands, const std::vector<double>& pixels,
    bool with_georef = true,
    double nodata = std::numeric_limits<double>::quiet_NaN()) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  auto ds = std::unique_ptr<GDALDataset>(
      drv->Create("", width, height, bands, GDT_Float64, nullptr));
  if (!ds) return nullptr;

  if (with_georef) {
    double gt[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ds->SetSpatialRef(&srs);
  }

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    if (!band) return nullptr;
    if (!std::isnan(nodata)) {
      EXPECT_EQ(CE_None, band->SetNoDataValue(nodata));
    }
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<double*>(pixels.data()), width,
                                      height, GDT_Float64, 0, 0, nullptr));
  }
  return ds;
}

TEST_F(GdalalgRasterNeighborsTest, MetadataAndAutoComplete) {
  GDALRasterNeighborsAlgorithmStandalone alg;
  EXPECT_EQ("neighbors", alg.GetName());
  EXPECT_THAT(GDALRasterNeighborsAlgorithm::GetAliasesStatic(),
              Contains("neighbours"));

  GDALAlgorithmArg* kernel_arg = alg.GetArg("kernel");
  ASSERT_THAT(kernel_arg, NotNull());
  std::vector<std::string> choices = kernel_arg->GetAutoCompleteChoices("");
  EXPECT_THAT(choices, Contains("gaussian"));
  EXPECT_THAT(choices, Contains("equal"));
  EXPECT_THAT(choices, Contains("unsharp-masking"));
  EXPECT_TRUE(kernel_arg->GetAutoCompleteChoices("[1,2").empty());
}

TEST_F(GdalalgRasterNeighborsTest, ValidationErrors) {
  auto src_ds = CreateTestMemDataset(5, 5, 1, std::vector<double>(25, 1.0));
  ASSERT_THAT(src_ds, NotNull());

  // 1. Even neighborhood size rejected.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--kernel", "equal", "--size", "4"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("must be an odd number"));
  }

  // 2. Unknown kernel name rejected.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--kernel", "bogus"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Valid values for 'kernel' argument are"));
  }

  // 3. Bracket matrix with non-odd-square count rejected.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments({"--kernel", "[1,2,3,4]"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("must be an odd square number"));
  }

  // 4. Bracket matrix with non-numeric token rejected.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        alg.ParseCommandLineArguments({"--kernel", "[1,2,3,4,abc,6,7,8,9]"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Non-numeric value"));
  }

  // 5. Multiple methods not matching multiple kernels rejected.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--kernel", "equal", "--method", "mean,sum"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("number of values for the 'method' argument"));
  }

  // 6. Incompatible size for gaussian, unsharp-masking, bracket, and edge1.
  struct SizeErrorCase {
    const char* kernel;
    const char* size;
    const char* expected_msg;
  };
  constexpr SizeErrorCase kSizeCases[] = {
      {.kernel = "gaussian", .size = "7", .expected_msg = "only size = 3 or 5"},
      {.kernel = "unsharp-masking",
       .size = "3",
       .expected_msg = "only size = 5"},
      {.kernel = "[1,1,1,1,1,1,1,1,1]",
       .size = "5",
       .expected_msg = "inconsistent with the one deduced"},
      {.kernel = "edge1", .size = "5", .expected_msg = "only size = 3"},
  };
  for (const auto& tc : kSizeCases) {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--kernel", tc.kernel, "--size", tc.size}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_msg));
  }

  // 7. Zero-sum bracket kernel with explicit method=mean rejected at Run.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--kernel", "[[0,-1,0],[-1,4,-1],[0,-1,0]]", "--method", "mean"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Use 'sum' instead"));
  }

  // 8. Nodata not representable in output data type rejected.
  {
    auto nd_ds = CreateTestMemDataset(5, 5, 1, std::vector<double>(25, 1.0),
                                      true, 999.0);
    ASSERT_THAT(nd_ds, NotNull());
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(nd_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--kernel", "equal", "--datatype", "Byte"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("cannot represent NoData value"));
  }
}

TEST_F(GdalalgRasterNeighborsTest, EqualSumMeanAndGeoref) {
  std::vector<double> pixels(25, 0.0);
  pixels[12] = 9.0;  // Center pixel (2, 2) = 9.
  auto src_ds = CreateTestMemDataset(5, 5, 1, pixels, /*with_georef=*/true);
  ASSERT_THAT(src_ds, NotNull());

  // 1. Equal kernel with default method (mean).
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--kernel", "equal", "--size", "3"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    EXPECT_EQ(GDT_Float64, out_ds->GetRasterBand(1)->GetRasterDataType());
    double gt[6] = {};
    EXPECT_EQ(CE_None, out_ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_THAT(out_ds->GetSpatialRef(), NotNull());

    double center_val = -1.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 2, 2, 1, 1, &center_val, 1, 1, GDT_Float64,
                           0, 0, nullptr));
    EXPECT_NEAR(1.0, center_val, 1e-9);
  }

  // 2. Equal kernel with method=sum.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--kernel", "equal", "--size", "3", "--method", "sum"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());

    double center_val = -1.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 2, 2, 1, 1, &center_val, 1, 1, GDT_Float64,
                           0, 0, nullptr));
    EXPECT_NEAR(9.0, center_val, 1e-9);
  }
}

TEST_F(GdalalgRasterNeighborsTest, NamedKernelsAndMethods) {
  std::vector<double> pixels(49, 10.0);
  pixels[24] = 20.0;  // Center of 7x7.
  auto src_ds = CreateTestMemDataset(7, 7, 2, pixels, /*with_georef=*/false,
                                     /*nodata=*/-9999.0);
  ASSERT_THAT(src_ds, NotNull());

  // 1. Multiple kernels with empty method (auto selects sum for zero-sum, mean
  // for non-zero-sum) across all bands.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--kernel", "u", "--kernel", "v", "--kernel", "edge1", "--kernel",
         "edge2", "--kernel", "sharpen", "--kernel", "gaussian", "--kernel",
         "[[0,-1,0],[-1,4,-1],[0,-1,0]]", "--nodata", "none"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    // 2 source bands * 7 kernels = 14 output bands.
    EXPECT_EQ(14, out_ds->GetRasterCount());
    int has_nd = 0;
    out_ds->GetRasterBand(1)->GetNoDataValue(&has_nd);
    EXPECT_EQ(FALSE, has_nd);
  }

  // 2. Gaussian 5x5 and unsharp-masking (default size 5), single band, explicit
  // nodata, single method broadcast to multiple kernels.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "2", "--kernel", "gaussian", "--size", "5", "--method",
         "mean", "--datatype", "Float32", "--nodata", "-1"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    EXPECT_EQ(GDT_Float32, out_ds->GetRasterBand(1)->GetRasterDataType());
    int has_nd = 0;
    EXPECT_DOUBLE_EQ(-1.0, out_ds->GetRasterBand(1)->GetNoDataValue(&has_nd));
    EXPECT_EQ(TRUE, has_nd);
  }

  // 3. Unsharp-masking with default size (deduces 5) and non-mean/sum method.
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--band", "1", "--kernel", "unsharp-masking"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(1, out_ds->GetRasterCount());
    std::vector<double> buf(49);
    EXPECT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 7, 7, buf.data(), 7, 7, GDT_Float64,
                           0, 0, nullptr));
  }

  // 4. Statistical method "median" (method != "sum" && method != "mean").
  {
    GDALRasterNeighborsAlgorithm alg;
    alg.GetInputDatasets().emplace_back(src_ds.get());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"--band", "1", "--kernel", "equal",
                                       "--size", "3", "--method", "median"}));
    ASSERT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    double center_val = 0.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 3, 3, 1, 1, &center_val, 1, 1, GDT_Float64,
                           0, 0, nullptr));
    EXPECT_NEAR(10.0, center_val, 1e-9);
  }
}

TEST_F(GdalalgRasterNeighborsTest, PipelineExecution) {
  const std::string in_tif = std::string(kTestDir) + "/in.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(gtiff_drv, NotNull());
  {
    std::unique_ptr<GDALDataset> ds(
        gtiff_drv->Create(in_tif.c_str(), 5, 5, 1, GDT_Float32, nullptr));
    ASSERT_THAT(ds, NotNull());
    std::vector<float> pixels(25, 2.0f);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 5, 5, pixels.data(), 5, 5,
                           GDT_Float32, 0, 0, nullptr));
  }

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments(
      {"read", in_tif, "!", "neighbors", "--kernel", "equal", "--size", "3",
       "--method", "sum", "!", "write", out_tif, "--overwrite"}));
  ASSERT_TRUE(pipeline.Run());
  ASSERT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                        nullptr, nullptr, nullptr));
  ASSERT_THAT(out_ds, NotNull());
  float center_val = 0.0f;
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 2, 2, 1, 1, &center_val, 1, 1, GDT_Float32, 0,
                         0, nullptr));
  EXPECT_FLOAT_EQ(18.0f, center_val);
}

}  // namespace
}  // namespace autotest2
