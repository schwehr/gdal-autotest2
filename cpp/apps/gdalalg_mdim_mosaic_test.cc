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
// Tests for GDALMdimMosaicAlgorithm (apps/gdalalg_mdim_mosaic.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_mdim_mosaic.cpp
//   https://gdal.org/programs/gdal_mdim_mosaic.html

#include "third_party/gdal/apps/gdalalg_mdim_mosaic.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_mdim_mosaic_test";

struct TileSpec {
  std::string path;
  std::vector<double> y_coords{10.0, 9.0, 8.0};
  std::vector<double> x_coords{0.0, 1.0, 2.0};
  GDALDataType dt = GDT_Float32;
  bool has_nodata = false;
  double nodata = -9999.0;
  std::string y_name = "y";
  std::string x_name = "x";
  std::string arr_name = "var";
  bool add_second_array = false;
  bool create_x_indexing_var = true;
  bool x_indexing_2d = false;
  bool x_indexing_string = false;
  double fill_value = 1.0;
  std::string blocksize;
};

void CreateZarrTile(const TileSpec& spec) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional(spec.path.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_y = root->CreateDimension(spec.y_name, "HORIZONTAL_Y", "NORTH",
                                     spec.y_coords.size());
  auto dim_x = root->CreateDimension(spec.x_name, "HORIZONTAL_X", "EAST",
                                     spec.x_coords.size());
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  const auto f64_dt = GDALExtendedDataType::Create(GDT_Float64);
  {
    auto y_var = root->CreateMDArray(spec.y_name, {dim_y}, f64_dt);
    ASSERT_NE(nullptr, y_var);
    const GUInt64 s[1] = {0};
    const size_t c[1] = {spec.y_coords.size()};
    const GInt64 st[1] = {1};
    const GPtrDiff_t str[1] = {1};
    ASSERT_TRUE(y_var->Write(s, c, st, str, f64_dt, spec.y_coords.data()));
    auto attr = y_var->CreateAttribute("units", {},
                                       GDALExtendedDataType::CreateString());
    if (attr) {
      EXPECT_TRUE(attr->Write("meters"));
    }
    ASSERT_TRUE(dim_y->SetIndexingVariable(std::move(y_var)));
  }

  if (spec.create_x_indexing_var) {
    if (spec.x_indexing_2d) {
      auto x_var = root->CreateMDArray(spec.x_name, {dim_y, dim_x}, f64_dt);
      ASSERT_NE(nullptr, x_var);
      ASSERT_TRUE(dim_x->SetIndexingVariable(std::move(x_var)));
    } else if (spec.x_indexing_string) {
      // Zarr v2 does not support unlimited-length string arrays; use a fixed
      // max length so the array is created and rejected as non-numeric.
      const auto str_dt = GDALExtendedDataType::CreateString(16);
      auto x_var = root->CreateMDArray(spec.x_name, {dim_x}, str_dt);
      ASSERT_NE(nullptr, x_var);
      ASSERT_TRUE(dim_x->SetIndexingVariable(std::move(x_var)));
    } else {
      auto x_var = root->CreateMDArray(spec.x_name, {dim_x}, f64_dt);
      ASSERT_NE(nullptr, x_var);
      const GUInt64 s[1] = {0};
      const size_t c[1] = {spec.x_coords.size()};
      const GInt64 st[1] = {1};
      const GPtrDiff_t str[1] = {1};
      ASSERT_TRUE(x_var->Write(s, c, st, str, f64_dt, spec.x_coords.data()));
      ASSERT_TRUE(dim_x->SetIndexingVariable(std::move(x_var)));
    }
  }

  CPLStringList arr_opts;
  if (!spec.blocksize.empty()) {
    arr_opts.SetNameValue("BLOCKSIZE", spec.blocksize.c_str());
  }

  const auto arr_dt = GDALExtendedDataType::Create(spec.dt);
  auto arr = root->CreateMDArray(spec.arr_name, {dim_y, dim_x}, arr_dt,
                                 arr_opts.List());
  ASSERT_NE(nullptr, arr);
  if (spec.has_nodata) {
    ASSERT_TRUE(arr->SetNoDataValue(spec.nodata));
  }
  const size_t n = spec.y_coords.size() * spec.x_coords.size();
  std::vector<double> buf(n, spec.fill_value);
  const GUInt64 s2[2] = {0, 0};
  const size_t c2[2] = {spec.y_coords.size(), spec.x_coords.size()};
  const GInt64 st2[2] = {1, 1};
  const GPtrDiff_t str2[2] = {static_cast<GPtrDiff_t>(spec.x_coords.size()), 1};
  ASSERT_TRUE(arr->Write(s2, c2, st2, str2, f64_dt, buf.data()));

  if (spec.add_second_array) {
    auto arr2 = root->CreateMDArray("var2", {dim_y, dim_x}, arr_dt);
    ASSERT_NE(nullptr, arr2);
    ASSERT_TRUE(arr2->Write(s2, c2, st2, str2, f64_dt, buf.data()));
  }
}

class GdalalgMdimMosaicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_Zarr();
    GDALRegister_VRT();
    GDALRegister_GTiff();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(GdalalgMdimMosaicTest, BasicPropertiesAndOutputFormatErrors) {
  GDALMdimMosaicAlgorithm alg;
  EXPECT_EQ("mosaic", alg.GetName());
  EXPECT_FALSE(alg.GetDescription().empty());
  EXPECT_EQ("/programs/gdal_mdim_mosaic.html", alg.GetHelpURL());

  // Unknown extension -> cannot guess driver.
  {
    GDALMdimMosaicAlgorithm bad_ext;
    ASSERT_TRUE(bad_ext.ParseCommandLineArguments(
        {std::string(kTestDir) + "/in.zarr",
         std::string(kTestDir) + "/out.unknown_ext"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(bad_ext.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot guess driver"));
    CPLErrorReset();
  }

  // Missing @filelist fails.
  {
    GDALMdimMosaicAlgorithm bad_list;
    ASSERT_TRUE(bad_list.ParseCommandLineArguments(
        {"@" + std::string(kTestDir) + "/missing_list.txt",
         std::string(kTestDir) + "/out.vrt"}));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(bad_list.Run());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open"));
    CPLErrorReset();
  }
}

TEST_F(GdalalgMdimMosaicTest, RegularMosaicVrtZarrFileListGlobAndRelative) {
  const std::string t1 = std::string(kTestDir) + "/tile_a.zarr";
  const std::string t2 = std::string(kTestDir) + "/tile_b.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(TileSpec{
      .path = t1,
      .x_coords = {0.0, 1.0, 2.0},
      .add_second_array = true,
      .fill_value = 5.0,
      .blocksize = "3,3",
  }));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(TileSpec{
      .path = t2,
      .x_coords = {3.0, 4.0, 5.0},
      .add_second_array = true,
      .fill_value = 9.0,
      .blocksize = "2,2",
  }));

  // 1. VRT mosaic via @filelist.txt (no --output-format, auto-detected .vrt).
  const std::string list_path = std::string(kTestDir) + "/files.txt";
  const std::string out_vrt = std::string(kTestDir) + "/mosaic.vrt";
  VsiMemTempWrapper list_file(list_path, t1 + "\n" + t2 + "\n");
  {
    GDALMdimMosaicAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"@" + list_path, "--array", "var", "--array", "/var2", out_vrt}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_vrt.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto arr = ds->GetRootGroup()->OpenMDArray("var");
    ASSERT_NE(nullptr, arr);
    ASSERT_EQ(2U, arr->GetDimensionCount());
    EXPECT_EQ(3U, arr->GetDimensions()[0]->GetSize());
    EXPECT_EQ(6U, arr->GetDimensions()[1]->GetSize());
  }

  // 2. Materialized Zarr mosaic via glob pattern on Zarr files + relative
  // paths. Avoid VRT-of-VRT nesting which self-deadlocks on g_cacheLock during
  // cleanup.
  const std::string glob_zarr1 = std::string(kTestDir) + "/g_1.zarr";
  const std::string glob_zarr2 = std::string(kTestDir) + "/g_2.zarr";
  {
    GDALMdimMosaicAlgorithm m1;
    ASSERT_TRUE(m1.ParseCommandLineArguments({"--of", "Zarr", t1, glob_zarr1}));
    ASSERT_TRUE(m1.Run());
    EXPECT_TRUE(m1.Finalize());
    GDALMdimMosaicAlgorithm m2;
    ASSERT_TRUE(m2.ParseCommandLineArguments({"--of", "Zarr", t2, glob_zarr2}));
    ASSERT_TRUE(m2.Run());
    EXPECT_TRUE(m2.Finalize());
  }
  const std::string out_zarr = std::string(kTestDir) + "/mosaic.zarr";
  {
    GDALMdimMosaicAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--of", "Zarr", "--overwrite", std::string(kTestDir) + "/g_*.zarr",
         out_zarr}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_zarr.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto arr = ds->GetRootGroup()->OpenMDArray("var");
    ASSERT_NE(nullptr, arr);
    EXPECT_EQ(6U, arr->GetDimensions()[1]->GetSize());
  }

  // 3. Relative path resolution via SetReferencePathForRelativePaths.
  {
    GDALMdimMosaicAlgorithm alg;
    alg.SetReferencePathForRelativePaths(kTestDir);
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"tile_a.zarr", "tile_b.zarr", "--overwrite", out_vrt}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgMdimMosaicTest, IrregularIncreasingAndDecreasingMosaic) {
  // 1. Irregular increasing dimensions inserted before, middle, and after.
  const std::string i1 = std::string(kTestDir) + "/irr1.zarr";
  const std::string i2 = std::string(kTestDir) + "/irr2.zarr";
  const std::string i3 = std::string(kTestDir) + "/irr3.zarr";
  const std::string i4 = std::string(kTestDir) + "/irr4.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = i1, .x_coords = {10.0, 11.0, 15.0}, .fill_value = 1.0}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = i2, .x_coords = {1.0, 3.0}, .fill_value = 2.0}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = i3, .x_coords = {20.0, 25.0}, .fill_value = 3.0}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = i4, .x_coords = {5.0, 8.0}, .fill_value = 4.0}));

  const std::string out_inc = std::string(kTestDir) + "/irr_inc.vrt";
  {
    GDALMdimMosaicAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({i1, i2, i3, i4, out_inc}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());

    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        out_inc.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
        nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto arr = ds->GetRootGroup()->OpenMDArray("var");
    ASSERT_NE(nullptr, arr);
    EXPECT_EQ(9U, arr->GetDimensions()[1]->GetSize());
  }

  // 2. Irregular decreasing x dimensions.
  const std::string d1 = std::string(kTestDir) + "/dec1.zarr";
  const std::string d2 = std::string(kTestDir) + "/dec2.zarr";
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = d1, .x_coords = {30.0, 22.0, 20.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = d2, .x_coords = {10.0, 4.0, 1.0}}));

  const std::string out_dec = std::string(kTestDir) + "/irr_dec.vrt";
  {
    GDALMdimMosaicAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments({d1, d2, out_dec}));
    ASSERT_TRUE(alg.Run());
    EXPECT_TRUE(alg.Finalize());
  }
}

TEST_F(GdalalgMdimMosaicTest, ValidationAndCompatibilityErrorPaths) {
  const std::string out_vrt = std::string(kTestDir) + "/err.vrt";
  const std::string base = std::string(kTestDir) + "/base.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(TileSpec{.path = base}));

  auto expect_run_fails = [&](const std::vector<std::string>& args,
                              const std::string& err_substr) {
    SCOPED_TRACE(err_substr);
    GDALMdimMosaicAlgorithm alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(args));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(alg.Run()) << "Expected failure containing: " << err_substr;
    if (!err_substr.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(err_substr));
    }
    CPLErrorReset();
  };

  // 1. Missing input dataset.
  expect_run_fails({std::string(kTestDir) + "/no_such.zarr", out_vrt}, "");

  // 2. Non-multidimensional 2D GTiff fails to open with
  // GDAL_OF_MULTIDIM_RASTER.
  const std::string tif = std::string(kTestDir) + "/plain.tif";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(tif.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
  }
  expect_run_fails({tif, out_vrt}, "not recognized");

  // 3. Explicit --array not found.
  expect_run_fails({base, "--array", "missing", out_vrt}, "Cannot find array");

  // 4. Only 1D arrays when --array omitted, and 0D scalar array selected.
  const std::string only_1d = std::string(kTestDir) + "/only_1d.zarr";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Zarr");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->CreateMultiDimensional(only_1d.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    auto d = root->CreateDimension("d", "", "", 2);
    const auto dt = GDALExtendedDataType::Create(GDT_Float32);
    ASSERT_NE(nullptr, root->CreateMDArray("arr1d", {d}, dt));
    ASSERT_NE(nullptr, root->CreateMDArray("scalar0d", {}, dt));
  }
  expect_run_fails({only_1d, out_vrt}, "dimension count >= 2");
  expect_run_fails({only_1d, "--array", "scalar0d", out_vrt},
                   "has no dimensions");

  // 5. Missing indexing variable, 2D indexing variable, string indexing
  //    variable.
  const std::string no_idx = std::string(kTestDir) + "/no_idx.zarr";
  const std::string idx_2d = std::string(kTestDir) + "/idx_2d.vrt";
  const std::string idx_str = std::string(kTestDir) + "/idx_str.zarr";
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = no_idx, .create_x_indexing_var = false}));
  {
    const std::string vrt_xml = R"vrt(<VRTDataset>
  <Group name="/">
    <Dimension name="y" size="2" indexingVariable="y"/>
    <Dimension name="x" size="2" indexingVariable="x"/>
    <Array name="y">
      <DataType>Float64</DataType>
      <DimensionRef ref="y"/>
      <ConstantValue offset="0" count="2">1.0</ConstantValue>
    </Array>
    <Array name="x">
      <DataType>Float64</DataType>
      <DimensionRef ref="y"/>
      <DimensionRef ref="x"/>
      <ConstantValue offset="0,0" count="2,2">1.0</ConstantValue>
    </Array>
    <Array name="var">
      <DataType>Float32</DataType>
      <DimensionRef ref="y"/>
      <DimensionRef ref="x"/>
      <ConstantValue offset="0,0" count="2,2">1.0</ConstantValue>
    </Array>
  </Group>
</VRTDataset>)vrt";
    VSILFILE* fp = VSIFOpenL(idx_2d.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(vrt_xml.size(),
              VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = idx_str, .x_indexing_string = true}));
  expect_run_fails({no_idx, out_vrt}, "lacks an indexing variable");
  expect_run_fails({idx_2d, out_vrt}, "is not 1D");
  expect_run_fails({idx_str, out_vrt}, "non-numeric");

  // 6. NaN and non-monotonic indexing variables. Use nSize <= 2 for indices 0/1
  //    and equal endpoints for index >= 2 because
  //    GDALMDArray::IsRegularlySpaced (gcore/gdalmultidim.cpp:7604) compares
  //    NaN with `>` and otherwise returns true, triggering NaN float-to-uint64
  //    UB in GDALMdimMosaicAlgorithm::RunImpl (gdalalg_mdim_mosaic.cpp:809).
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  const std::string nan0 = std::string(kTestDir) + "/nan0.zarr";
  const std::string nan1 = std::string(kTestDir) + "/nan1.zarr";
  const std::string nan2 = std::string(kTestDir) + "/nan2.zarr";
  const std::string nonmono = std::string(kTestDir) + "/nonmono.zarr";
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = nan0, .x_coords = {nan_val, 1.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = nan1, .x_coords = {0.0, nan_val}}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = nan2, .x_coords = {0.0, 1.0, nan_val, 0.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = nonmono, .x_coords = {0.0, 5.0, 2.0}}));
  expect_run_fails({nan0, out_vrt}, "NaN");
  expect_run_fails({nan1, out_vrt}, "NaN");
  expect_run_fails({nan2, out_vrt}, "NaN");
  expect_run_fails({nonmono, out_vrt}, "strictly increasing or decreasing");

  // 7. Mismatched dtype, nodata, dim name, spacing, unaligned start,
  //    regular-vs-irregular.
  const std::string diff_dt = std::string(kTestDir) + "/diff_dt.zarr";
  const std::string diff_nd = std::string(kTestDir) + "/diff_nd.zarr";
  const std::string diff_dim = std::string(kTestDir) + "/diff_dim.zarr";
  const std::string diff_step = std::string(kTestDir) + "/diff_step.zarr";
  const std::string unaligned = std::string(kTestDir) + "/unaligned.zarr";
  const std::string irr = std::string(kTestDir) + "/irr.zarr";
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = diff_dt, .dt = GDT_Int16}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = diff_nd, .has_nodata = true}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = diff_dim, .x_name = "lon"}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = diff_step, .x_coords = {10.0, 12.0, 14.0}}));
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = unaligned, .x_coords = {10.3, 11.3, 12.3}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr, .x_coords = {10.0, 11.0, 15.0}}));

  expect_run_fails({base, diff_dt, out_vrt}, "same data type");
  expect_run_fails({base, diff_nd, out_vrt}, "same nodata");
  expect_run_fails({base, diff_dim, out_vrt}, "same name");
  expect_run_fails({base, diff_step, out_vrt}, "spacing");
  expect_run_fails({base, unaligned, out_vrt}, "not aligned");
  expect_run_fails({base, irr, out_vrt}, "irregularly-spaced");
  expect_run_fails({irr, base, out_vrt}, "regularly spaced");

  // 8. Irregular opposite sign and overlaps.
  const std::string irr_dec = std::string(kTestDir) + "/irr_dec_e.zarr";
  const std::string irr_ov_before = std::string(kTestDir) + "/ov_bef.zarr";
  const std::string irr_ov_same = std::string(kTestDir) + "/ov_same.zarr";
  const std::string irr_ov_after = std::string(kTestDir) + "/ov_aft.zarr";
  const std::string irr_ov_next = std::string(kTestDir) + "/ov_nxt.zarr";
  const std::string irr_hi = std::string(kTestDir) + "/irr_hi.zarr";
  ASSERT_NO_FATAL_FAILURE(CreateZarrTile(
      TileSpec{.path = irr_dec, .x_coords = {20.0, 18.0, 15.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr_ov_before, .x_coords = {5.0, 12.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr_ov_same, .x_coords = {10.0, 12.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr_ov_after, .x_coords = {12.0, 18.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr_hi, .x_coords = {25.0, 30.0}}));
  ASSERT_NO_FATAL_FAILURE(
      CreateZarrTile(TileSpec{.path = irr_ov_next, .x_coords = {18.0, 26.0}}));

  expect_run_fails({irr, irr_dec, out_vrt}, "increasing or decreasing");
  expect_run_fails({irr, irr_ov_before, out_vrt}, "not the same");
  expect_run_fails({irr, irr_ov_same, out_vrt}, "not the same");
  expect_run_fails({irr, irr_ov_after, out_vrt}, "overlapping");
  expect_run_fails({irr, irr_hi, irr_ov_next, out_vrt}, "overlapping");
}

}  // namespace
}  // namespace autotest2
