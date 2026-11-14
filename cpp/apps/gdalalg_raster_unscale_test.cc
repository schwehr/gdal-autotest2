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
// Tests for GDALRasterUnscaleAlgorithm (apps/gdalalg_raster_unscale.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_unscale.cpp

#include "third_party/gdal/apps/gdalalg_raster_unscale.h"

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
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_unscale_test";

class GdalalgRasterUnscaleTest : public ::testing::Test {
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

template <typename T>
void CreateScaledTiff(const std::string& path, GDALDataType dt,
                      const std::vector<T>& pixels, double scale,
                      double offset) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_THAT(drv, NotNull());
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 2, 2, 1, dt, nullptr));
  ASSERT_THAT(ds, NotNull());
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetScale(scale));
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetOffset(offset));
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 2, 2, const_cast<T*>(pixels.data()), 2,
                         2, dt, 0, 0, nullptr));
}

TEST_F(GdalalgRasterUnscaleTest, DefaultOutputTypeFromByteFloat64AndComplex) {
  // 1. Byte -> Float32 by default.
  {
    const std::string in_tif = std::string(kTestDir) + "/byte.tif";
    const std::string out_tif = std::string(kTestDir) + "/byte_out.tif";
    ASSERT_NO_FATAL_FAILURE(
        CreateScaledTiff<uint8_t>(in_tif, GDT_Byte, {1, 2, 3, 4}, 2.5, 10.0));

    GDALRasterUnscaleAlgorithmStandalone alg;
    EXPECT_EQ("unscale", alg.GetName());
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(GDT_Float32, out_ds->GetRasterBand(1)->GetRasterDataType());
    std::vector<float> buf(4, 0.0f);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, buf.data(), 2, 2, GDT_Float32,
                           0, 0, nullptr));
    EXPECT_FLOAT_EQ(12.5f, buf[0]);
    EXPECT_FLOAT_EQ(20.0f, buf[3]);
  }

  // 2. Float64 -> Float64 by default.
  {
    const std::string in_tif = std::string(kTestDir) + "/f64.tif";
    const std::string out_tif = std::string(kTestDir) + "/f64_out.tif";
    ASSERT_NO_FATAL_FAILURE(CreateScaledTiff<double>(
        in_tif, GDT_Float64, {2.0, 4.0, 6.0, 8.0}, 0.5, 1.0));

    GDALRasterUnscaleAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(GDT_Float64, out_ds->GetRasterBand(1)->GetRasterDataType());
    std::vector<double> buf(4, 0.0);
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 2, 2, buf.data(), 2, 2, GDT_Float64,
                           0, 0, nullptr));
    EXPECT_DOUBLE_EQ(2.0, buf[0]);
  }

  // 3. CFloat32 -> CFloat32, CFloat64 -> CFloat64.
  for (const auto& [dt, name] :
       {std::pair{GDT_CFloat32, "cf32"}, std::pair{GDT_CFloat64, "cf64"}}) {
    const std::string in_tif = std::string(kTestDir) + "/" + name + ".tif";
    const std::string out_tif = std::string(kTestDir) + "/" + name + "_out.tif";
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_THAT(drv, NotNull());
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(in_tif.c_str(), 2, 2, 1, dt, nullptr));
      ASSERT_THAT(ds, NotNull());
      ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetScale(2.0));
    }

    GDALRasterUnscaleAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({in_tif, out_tif, "--overwrite"}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(dt, out_ds->GetRasterBand(1)->GetRasterDataType());
  }
}

TEST_F(GdalalgRasterUnscaleTest, ExplicitOutputDataTypeInPipeline) {
  const std::string in_tif = std::string(kTestDir) + "/explicit.tif";
  const std::string out_tif = std::string(kTestDir) + "/explicit_out.tif";
  ASSERT_NO_FATAL_FAILURE(
      CreateScaledTiff<uint8_t>(in_tif, GDT_Byte, {2, 4, 6, 8}, 2.0, 1.0));

  GDALRasterPipelineAlgorithm pipeline;
  ASSERT_TRUE(pipeline.ParseCommandLineArguments({
      "read",
      in_tif,
      "!",
      "unscale",
      "--datatype",
      "Int16",
      "!",
      "write",
      out_tif,
      "--overwrite",
  }));
  EXPECT_TRUE(pipeline.Run());
  EXPECT_TRUE(pipeline.Finalize());

  std::unique_ptr<GDALDataset> out_ds(
      GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_THAT(out_ds, NotNull());
  EXPECT_EQ(GDT_Int16, out_ds->GetRasterBand(1)->GetRasterDataType());
  std::vector<int16_t> buf(4, 0);
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(),
                                               2, 2, GDT_Int16, 0, 0, nullptr));
  EXPECT_EQ(5, buf[0]);
  EXPECT_EQ(17, buf[3]);
}

TEST_F(GdalalgRasterUnscaleTest, ZeroBandDatasetAndInvalidType) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_THAT(mem_drv, NotNull());
  std::unique_ptr<GDALDataset> empty_ds(
      mem_drv->Create("", 2, 2, 0, GDT_Byte, nullptr));
  ASSERT_THAT(empty_ds, NotNull());

  {
    GDALRasterUnscaleAlgorithm alg(/*standaloneStep=*/false);
    alg.GetInputDatasets().emplace_back(empty_ds.get());
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_THAT(out_ds, NotNull());
    EXPECT_EQ(0, out_ds->GetRasterCount());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALRasterUnscaleAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"--datatype", "NotAType", "a.tif", "b.tif"}));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
