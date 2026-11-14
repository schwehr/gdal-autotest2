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
// Tests for GDALRasterCalcAlgorithm (apps/gdalalg_raster_calc.cpp and
// apps/gdalalg_raster_calc.h).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_calc.cpp

#include "third_party/gdal/apps/gdalalg_raster_calc.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_calc_test";

void CreateTiff(const std::string& path, int width, int height, int bands,
                GDALDataType dt, const std::vector<double>& values,
                int epsg = 4326,
                const std::array<double, 6>& gt = {0.0, 1.0, 0.0, 4.0, 0.0,
                                                   -1.0},
                std::optional<double> nodata = std::nullopt) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, bands, dt, nullptr));
  ASSERT_NE(nullptr, ds);

  if (epsg > 0) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg));
    ds->SetSpatialRef(&srs);
  }
  std::array<double, 6> gt_copy = gt;
  ASSERT_EQ(CE_None, ds->SetGeoTransform(gt_copy.data()));

  const size_t n_pix = static_cast<size_t>(width) * height;
  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    ASSERT_NE(nullptr, band);
    if (nodata.has_value()) {
      ASSERT_EQ(CE_None, band->SetNoDataValue(*nodata));
    }
    std::vector<double> buf(n_pix, values[static_cast<size_t>(b - 1)]);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, width, height, buf.data(), width,
                             height, GDT_Float64, 0, 0, nullptr));
  }
}

void RegisterDrivers() {
  GDALRegister_GTiff();
  GDALRegister_MEM();
  GDALRegister_VRT();
  GDALRegister_GDALG();
}

TEST(GdalalgRasterCalcTest, BasicPropertiesAndAutoComplete) {
  RegisterDrivers();

  GDALRasterCalcAlgorithmStandalone alg;
  EXPECT_EQ("calc", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_FALSE(alg.GetHelpURL().empty());

  std::vector<std::string> args = {"--dialect=builtin", "--calc", ""};
  std::vector<std::string> suggestions =
      alg.GetAutoComplete(args, /*lastWordIsComplete=*/true,
                          /*showAllOptions=*/false);
  EXPECT_THAT(suggestions, Contains("sum"));

  GDALRasterCalcAlgorithmStandalone alg_paren;
  std::vector<std::string> args_with_paren = {"--dialect=builtin", "--calc",
                                              "sum("};
  std::vector<std::string> paren_suggestions =
      alg_paren.GetAutoComplete(args_with_paren, /*lastWordIsComplete=*/false,
                                /*showAllOptions=*/false);
  EXPECT_THAT(paren_suggestions, Not(Contains("sum")));
  EXPECT_THAT(paren_suggestions, Contains("**"));
}

TEST(GdalalgRasterCalcTest, BuiltinMultiBandNamedAndMuparserUnsupported) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string a_tif = std::string(kTestDir) + "/a.tif";
  const std::string b_tif = std::string(kTestDir) + "/b.tif";
  const std::string x_tif = std::string(kTestDir) + "/x.tif";
  const std::string out_tif = std::string(kTestDir) + "/out.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(a_tif, 2, 2, 2, GDT_Byte, {10.0, 20.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(b_tif, 2, 2, 1, GDT_Byte, {3.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(x_tif, 2, 2, 1, GDT_Byte, {4.0}));

  // 1. Two-band A + single-band B with builtin sum and diff expressions.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--dialect=builtin", "--datatype=Float32", "--co=COMPRESS=NONE",
         "--calc=sum", "--calc=diff", "-i", "A=" + a_tif, "-i", "B=" + b_tif,
         "-o", out_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
  {
    std::unique_ptr<GDALDataset> out_ds(
        GDALDataset::Open(out_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterCount());
    constexpr std::array<float, 4> kExpected = {13.0f, 23.0f, 7.0f, 17.0f};
    for (int band = 1; band <= 4; ++band) {
      std::array<float, 4> buf{};
      ASSERT_EQ(CE_None, out_ds->GetRasterBand(band)->RasterIO(
                             GF_Read, 0, 0, 2, 2, buf.data(), 2, 2, GDT_Float32,
                             0, 0, nullptr));
      EXPECT_FLOAT_EQ(kExpected[static_cast<size_t>(band - 1)], buf[0]);
    }
  }

  // 2. Single unnamed input + stream output with builtin dialect.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum(k=5)", x_tif,
         "unused_out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    double val = 0.0;
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(9.0, val);
  }

  // 3. Default muparser dialect is not compiled into Google3 GDAL builds.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--calc=X * 2", x_tif, "unused_out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("A GDAL build with muparser is needed"));
    CPLErrorReset();
  }
}

TEST(GdalalgRasterCalcTest, BuiltinFlattenAndAggregateFunctions) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string multi = std::string(kTestDir) + "/multi.tif";
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(multi, 2, 2, 3, GDT_Float32, {2.0, 4.0, 6.0}));

  struct FlattenCase {
    std::string expr;
    double expected;
  };
  const FlattenCase cases[] = {
      {.expr = "sum", .expected = 12.0},
      {.expr = "mean", .expected = 4.0},
      {.expr = "min", .expected = 2.0},
      {.expr = "max", .expected = 6.0},
  };

  for (const auto& tc : cases) {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--flatten", "--calc=" + tc.expr,
         multi, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetRasterCount());
    double val = 0.0;
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(tc.expected, val) << tc.expr;
  }

  // Without --flatten, builtin sum keeps 3 bands.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", multi, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(3, out_ds->GetRasterCount());
  }
}

TEST(GdalalgRasterCalcTest, BuiltinDialectAndErrors) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string a_tif = std::string(kTestDir) + "/ba.tif";
  const std::string b_tif = std::string(kTestDir) + "/bb.tif";
  const std::string multi_tif = std::string(kTestDir) + "/bmulti.tif";
  const std::string f32_tif = std::string(kTestDir) + "/bf32.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(a_tif, 2, 2, 1, GDT_Byte, {5.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(b_tif, 2, 2, 1, GDT_Byte, {7.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(multi_tif, 2, 2, 2, GDT_Byte, {3.0, 9.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(f32_tif, 2, 2, 1, GDT_Float32, {2.0}));

  // 1. Builtin sum with unnamed inputs and valid argument k=10.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum(k=10)", a_tif, b_tif,
         "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    double val = 0.0;
    ASSERT_EQ(CE_None,
              out_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(22.0, val);
  }

  // 2. Builtin min/max preserves consistent input data type (Byte).
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "stream", "--dialect=builtin",
                                       "--calc=min", a_tif, b_tif, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(GDT_Byte, out_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 3. Builtin max with mixed data types falls back to Float64.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=max", a_tif, f32_tif,
         "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(GDT_Float64, out_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 4. Builtin multi-band non-flattened and flattened.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "stream", "--dialect=builtin",
                                       "--calc=sum", multi_tif, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(2, out_ds->GetRasterCount());
  }
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--flatten", "--calc=sum",
         multi_tif, "unused"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(1, out_ds->GetRasterCount());
  }

  // 5. Unknown builtin function.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=no_such_func", a_tif,
         "unused"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unknown builtin function"));
    CPLErrorReset();
  }

  // 6. Unrecognized argument for function accepting args (sum) vs none (real).
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum(bogus=1)", a_tif,
         "unused"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unrecognized argument"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=real(bogus=1)", a_tif,
         "unused"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not accept any argument"));
    CPLErrorReset();
  }
}

TEST(GdalalgRasterCalcTest, ExtentSrsResolutionAndNoData) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string base = std::string(kTestDir) + "/base.tif";
  const std::string coarse = std::string(kTestDir) + "/coarse.tif";
  const std::string shifted = std::string(kTestDir) + "/shifted.tif";
  const std::string diff_dim = std::string(kTestDir) + "/diff_dim.tif";
  const std::string diff_srs = std::string(kTestDir) + "/diff_srs.tif";
  const std::string with_nd = std::string(kTestDir) + "/with_nd.tif";

  ASSERT_NO_FATAL_FAILURE(CreateTiff(base, 4, 4, 1, GDT_Float32, {10.0}, 4326,
                                     {0.0, 1.0, 0.0, 4.0, 0.0, -1.0}));
  // Same extent [0,4]x[0,4] at coarser 2x2 resolution -> GCD common res 4x4.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(coarse, 2, 2, 1, GDT_Float32, {5.0}, 4326,
                                     {0.0, 2.0, 0.0, 4.0, 0.0, -2.0}));
  // Shifted origin, same 4x4 dimensions.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(shifted, 4, 4, 1, GDT_Float32, {2.0}, 4326,
                                     {10.0, 1.0, 0.0, 4.0, 0.0, -1.0}));
  // Different dimensions 3x3.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(diff_dim, 3, 3, 1, GDT_Float32, {2.0},
                                     4326, {0.0, 1.0, 0.0, 3.0, 0.0, -1.0}));
  // Different SRS (EPSG:3857), same extent.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(diff_srs, 4, 4, 1, GDT_Float32, {2.0},
                                     3857, {0.0, 1.0, 0.0, 4.0, 0.0, -1.0}));
  // Input with NoData value.
  ASSERT_NO_FATAL_FAILURE(CreateTiff(with_nd, 4, 4, 1, GDT_Float32, {3.0}, 4326,
                                     {0.0, 1.0, 0.0, 4.0, 0.0, -1.0}, -99.0));

  // 1. Common resolution calculation (4x4 + 2x2 over same extent).
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + coarse,
         "B=" + base, "out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(4, out_ds->GetRasterXSize());
    EXPECT_EQ(4, out_ds->GetRasterYSize());
  }

  // 2. Inconsistent extents fail, --no-check-extent succeeds if dims match.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + base,
         "B=" + shifted, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("extents are inconsistent"));
    CPLErrorReset();
  }
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--no-check-extent", "--calc=sum",
         "A=" + base, "B=" + shifted, "out"}));
    EXPECT_TRUE(alg.Run());
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--no-check-extent", "--calc=sum",
         "A=" + base, "B=" + diff_dim, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("do not have the same dimensions"));
    CPLErrorReset();
  }

  // 3. Inconsistent SRS fails, --no-check-srs succeeds.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + base,
         "B=" + diff_srs, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("spatial reference systems are inconsistent"));
    CPLErrorReset();
  }
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--no-check-srs", "--calc=sum",
         "A=" + base, "B=" + diff_srs, "out"}));
    EXPECT_TRUE(alg.Run());
  }

  // 4. NoData auto-selection, explicit, none, invalid, unrepresentable,
  // propagate.
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--propagate-nodata",
         "--calc=sum(k=1)", with_nd, "out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    int has_nd = 0;
    EXPECT_DOUBLE_EQ(-99.0, out_ds->GetRasterBand(1)->GetNoDataValue(&has_nd));
    EXPECT_EQ(1, has_nd);
  }
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--nodata=none",
         "--calc=sum(k=1)", with_nd, "out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    int has_nd = 0;
    out_ds->GetRasterBand(1)->GetNoDataValue(&has_nd);
    EXPECT_EQ(0, has_nd);
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--nodata=none",
         "--propagate-nodata", "--calc=sum", with_nd, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("--propagate-nodata"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    EXPECT_FALSE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--nodata=not_a_number",
         "--calc=sum", base, "out"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Value of 'nodata' should be"));
    CPLErrorReset();
  }
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--datatype=Byte", "--nodata=999",
         "--calc=sum", base, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot represent NoData"));
    CPLErrorReset();
  }
}

TEST(GdalalgRasterCalcTest, FileListVrtGdalgPipelineAndInputErrors) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string a_tif = std::string(kTestDir) + "/fa.tif";
  const std::string b_tif = std::string(kTestDir) + "/fb.tif";
  const std::string two_tif = std::string(kTestDir) + "/two.tif";
  const std::string three_tif = std::string(kTestDir) + "/three.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(a_tif, 2, 2, 1, GDT_Byte, {4.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(b_tif, 2, 2, 1, GDT_Byte, {6.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(two_tif, 2, 2, 2, GDT_Byte, {1.0, 2.0}));
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(three_tif, 2, 2, 3, GDT_Byte, {1.0, 2.0, 3.0}));

  // 1. @filelist.txt input syntax.
  const std::string list_txt = std::string(kTestDir) + "/inputs.txt";
  VsiMemTempWrapper list_file(list_txt, "A=" + a_tif + "\nB=" + b_tif + "\n");
  const std::string out_vrt = std::string(kTestDir) + "/out.vrt";
  {
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--dialect=builtin", "--calc=sum", "@" + list_txt, out_vrt}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. Missing @filelist.txt.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum",
         "@" + std::string(kTestDir) + "/none.txt", "out"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 3. GDALG output validation (valid & invalid with --no-check-expression).
  const std::string out_gdalg = std::string(kTestDir) + "/out.gdalg.json";
  {
    GDALRasterCalcAlgorithmStandalone alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"--dialect=builtin", "--calc=sum", "A=" + a_tif, out_gdalg}));
  }
  {
    GDALRasterCalcAlgorithmStandalone alg;
    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"--no-check-expression", "--calc=A +", "A=" + a_tif, out_gdalg}));
  }

  // 4. Invalid expression triggers fakeVRT evaluation error.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--calc=A +", "A=" + a_tif, "out"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 5. Source naming & opening error cases.
  const std::vector<std::vector<std::string>> error_arg_sets = {
      {"-f", "stream", "--calc=X", a_tif, b_tif, "out"},
      {"-f", "stream", "--dialect=builtin", "--calc=sum", "_A=" + a_tif, "out"},
      {"-f", "stream", "--dialect=builtin", "--calc=sum", "A-B=" + a_tif,
       "out"},
      {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + a_tif,
       "A=" + b_tif, "out"},
      {"-f", "stream", "--dialect=builtin", "--calc=sum",
       "A=" + std::string(kTestDir) + "/missing.tif", "out"},
      {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + two_tif,
       "B=" + three_tif, "out"},
  };
  for (const auto& args : error_arg_sets) {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 6. Inside GDALRasterPipelineAlgorithm (only first step can be 'calc').
  const std::string pipe_out = std::string(kTestDir) + "/pipe.tif";
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterPipelineAlgorithm bad_pipeline;
    EXPECT_FALSE(bad_pipeline.ParseCommandLineArguments(
        {"read", a_tif, "!", "calc", "--dialect=builtin", "--calc=sum"}));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("'calc' is only allowed as a first step"));
    CPLErrorReset();
  }
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"calc", "--dialect=builtin", "--calc=sum(k=3)", a_tif, "!", "write",
         pipe_out, "--overwrite"}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_TRUE(pipeline.Finalize());
  }
  std::unique_ptr<GDALDataset> pipe_ds(
      GDALDataset::Open(pipe_out.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(nullptr, pipe_ds);
  double val = 0.0;
  ASSERT_EQ(CE_None,
            pipe_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                                GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(7.0, val);
}

TEST(GdalalgRasterCalcTest, MuparserExpressionRewritingMixedTypesAndFormat) {
  RegisterDrivers();
  ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };

  const std::string two_tif = std::string(kTestDir) + "/two_m.tif";
  const std::string one_tif = std::string(kTestDir) + "/one_m.tif";
  const std::string f32_tif = std::string(kTestDir) + "/f32_m.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(two_tif, 2, 2, 2, GDT_Byte, {10.0, 20.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(one_tif, 2, 2, 1, GDT_Byte, {3.0}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(f32_tif, 2, 2, 1, GDT_Float32, {5.0}));

  // 1. Exercise muparser expression rewriting with --flatten:
  //    PosIsAggregateFunctionArgument (sum, avg, min, max, non-agg sin,
  //    pos < 2, non-arg operator '+'), MatchIsCompleteVariableNameWithNoIndex
  //    (leading/trailing alnum/'_', '[', '('),
  //    SetBandIndicesFlattenedExpression.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--flatten",
         "--calc=A + sum(A) + avg(1.0, A[1], A) + min(A) + max(A) + sin(A) + "
         "1 + A + BA + _A + AX + A_1 + A(1)",
         "A=" + two_tif, "B=" + one_tif, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("A GDAL build with muparser is needed"));
    CPLErrorReset();
  }

  // 2. Non-flattened muparser expression referencing only A[1] skips unused
  //    band A[2] when creating VRT sources.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--calc=A[1] * 2", "A=" + two_tif, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("A GDAL build with muparser is needed"));
    CPLErrorReset();
  }

  // 3. Second input failing to open inside UpdateSourceProperties.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + one_tif,
         "B=" + std::string(kTestDir) + "/missing_second.tif", "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed to open"));
    CPLErrorReset();
  }

  // 4. Single input VRT whose bands have differing data types (Byte + Float32)
  //    sets source.eDT = GDT_Unknown -> output falls back to Float64.
  {
    const std::string mixed_vrt = std::string(kTestDir) + "/mixed.vrt";
    const std::string vrt_xml =
        "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">"
        "<VRTRasterBand dataType=\"Byte\" band=\"1\">"
        "<SimpleSource><SourceFilename relativeToVRT=\"0\">" +
        one_tif +
        "</SourceFilename><SourceBand>1</SourceBand></SimpleSource>"
        "</VRTRasterBand>"
        "<VRTRasterBand dataType=\"Float32\" band=\"2\">"
        "<SimpleSource><SourceFilename relativeToVRT=\"0\">" +
        f32_tif +
        "</SourceFilename><SourceBand>1</SourceBand></SimpleSource>"
        "</VRTRasterBand>"
        "</VRTDataset>";
    VsiMemTempWrapper mixed_wrapper(mixed_vrt, vrt_xml);

    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=min", mixed_vrt, "out"}));
    EXPECT_TRUE(alg.Run());
    GDALDataset* out_ds = alg.GetOutputDataset().GetDatasetRef();
    ASSERT_NE(nullptr, out_ds);
    EXPECT_EQ(GDT_Float64, out_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 5. Empty @filelist produces empty inputs vector in
  // GDALCalcCreateVRTDerived.
  {
    const std::string empty_txt = std::string(kTestDir) + "/empty.txt";
    VsiMemTempWrapper empty_wrapper(empty_txt, "");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(
        alg.ParseCommandLineArguments({"-f", "stream", "--dialect=builtin",
                                       "--calc=sum", "@" + empty_txt, "out"}));
    EXPECT_FALSE(alg.Run());
    CPLErrorReset();
  }

  // 6. Explicit non-stream output format (-f GTiff) passes -of to
  // GDALTranslate.
  {
    const std::string out_gtiff = std::string(kTestDir) + "/explicit_of.tif";
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "GTiff", "--dialect=builtin", "--calc=sum", "A=" + one_tif,
         out_gtiff}));
    EXPECT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> out_ds(GDALDataset::Open(
        out_gtiff.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, out_ds);
    double out_val = 0.0;
    ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &out_val, 1, 1, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_DOUBLE_EQ(3.0, out_val);
  }

  // 7. Incommensurable resolutions (CPLGreatestCommonDivisor == 0) for X and Y.
  {
    const std::string base_x = std::string(kTestDir) + "/base_x.tif";
    const std::string fine_x = std::string(kTestDir) + "/fine_x.tif";
    ASSERT_NO_FATAL_FAILURE(CreateTiff(base_x, 2, 2, 1, GDT_Byte, {1.0}, 4326,
                                       {0.0, 1.5e-20, 0.0, 0.0, 0.0, -1.0}));
    ASSERT_NO_FATAL_FAILURE(CreateTiff(fine_x, 3, 2, 1, GDT_Byte, {1.0}, 4326,
                                       {0.0, 1.0e-20, 0.0, 0.0, 0.0, -1.0}));
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + base_x,
         "B=" + fine_x, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to find common resolution"));
    CPLErrorReset();
  }
  {
    const std::string base_y = std::string(kTestDir) + "/base_y.tif";
    const std::string fine_y = std::string(kTestDir) + "/fine_y.tif";
    ASSERT_NO_FATAL_FAILURE(CreateTiff(base_y, 2, 2, 1, GDT_Byte, {1.0}, 4326,
                                       {0.0, 1.0, 0.0, 0.0, 0.0, -1.5e-20}));
    ASSERT_NO_FATAL_FAILURE(CreateTiff(fine_y, 2, 3, 1, GDT_Byte, {1.0}, 4326,
                                       {0.0, 1.0, 0.0, 0.0, 0.0, -1.0e-20}));
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALRasterCalcAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"-f", "stream", "--dialect=builtin", "--calc=sum", "A=" + base_y,
         "B=" + fine_y, "out"}));
    EXPECT_FALSE(alg.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to find common resolution"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
