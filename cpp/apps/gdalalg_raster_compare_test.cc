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
// Tests for GDALRasterCompareAlgorithm (apps/gdalalg_raster_compare.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_raster_compare.cpp

#include "third_party/gdal/apps/gdalalg_raster_compare.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdalalg_raster_pipeline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdalalgorithm_cpp.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalalg_raster_compare_test";

class GdalalgRasterCompareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_MEM();
    GDALRegister_VRT();
    ASSERT_EQ(0, VSIMkdir(kTestDir, 0755));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

bool SetInputDataset(GDALAlgorithm& alg, GDALDataset* ds) {
  std::vector<GDALArgDatasetValue> inputs;
  inputs.emplace_back(ds);
  return alg.GetArg(GDAL_ARG_NAME_INPUT)->Set(std::move(inputs));
}

std::unique_ptr<GDALDataset> CreateMemDataset(
    const char* name, int width, int height, int bands, GDALDataType dt,
    const char* interleave = nullptr) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  char** options = nullptr;
  if (interleave) {
    options = CSLSetNameValue(options, "INTERLEAVE", interleave);
  }
  std::unique_ptr<GDALDataset> ds(
      drv->Create(name, width, height, bands, dt, options));
  CSLDestroy(options);
  return ds;
}

void CreateTiff(const std::string& path, int width, int height, int bands,
                GDALDataType dt, const std::vector<uint8_t>& data) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->Create(path.c_str(), width, height, bands, dt, nullptr));
  ASSERT_NE(nullptr, ds);
  if (!data.empty()) {
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, width, height,
                           const_cast<uint8_t*>(data.data()), width, height,
                           GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalalgRasterCompareTest, StepProperties) {
  GDALRasterCompareAlgorithmStandalone alg;
  EXPECT_EQ("compare", alg.GetName());
  EXPECT_FALSE(alg.IsNativelyStreamingCompatible());
  EXPECT_TRUE(alg.CanBeLastStep());
}

TEST_F(GdalalgRasterCompareTest, BinaryComparisonPaths) {
  const std::string ref_tif = std::string(kTestDir) + "/ref.tif";
  const std::string same_tif = std::string(kTestDir) + "/same.tif";
  const std::string diff_byte_tif = std::string(kTestDir) + "/diff_byte.tif";
  const std::string diff_size_tif = std::string(kTestDir) + "/diff_size.tif";

  ASSERT_NO_FATAL_FAILURE(CreateTiff(ref_tif, 2, 2, 1, GDT_Byte, {1, 2, 3, 4}));
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(same_tif, 2, 2, 1, GDT_Byte, {1, 2, 3, 4}));
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(diff_byte_tif, 2, 2, 1, GDT_Byte, {1, 2, 3, 5}));
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(diff_size_tif, 3, 2, 1, GDT_Byte, {1, 2, 3, 4, 5, 6}));

  // 1. Identical files -> binary comparison succeeds immediately.
  {
    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--reference", ref_tif, "--input", same_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("", alg.GetOutputString());
    EXPECT_EQ(0, alg.GetArg("return-code")->Get<int>());
  }

  // 2. Different file sizes.
  {
    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--reference", ref_tif, "--input", diff_size_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("bytes, whereas input file"));
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("width is 2"));
    EXPECT_GT(alg.GetArg("return-code")->Get<int>(), 0);
  }

  // 3. Same size, different binary content and pixel diff.
  {
    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--reference", ref_tif, "--input", diff_byte_tif}));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("differ at the binary level"));
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("1: pixels differing: 1"));
    EXPECT_THAT(alg.GetOutputString(),
                HasSubstr("1: maximum pixel value difference: 1"));
  }

  // 4. Unnamed and named MEM datasets trigger binary comparison warnings.
  {
    auto mem_unnamed = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    auto mem_named = CreateMemDataset("named_mem", 2, 2, 1, GDT_Byte);
    ASSERT_NE(nullptr, mem_unnamed);
    ASSERT_NE(nullptr, mem_named);
    std::array<uint8_t, 4> vals = {1, 2, 3, 4};
    ASSERT_EQ(CE_None, mem_unnamed->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, vals.data(), 2, 2, GDT_Byte, 0,
                           0, nullptr));
    ASSERT_EQ(CE_None, mem_named->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, vals.data(), 2, 2, GDT_Byte, 0,
                           0, nullptr));

    WithQuietHandler quiet;
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("reference")->Set(mem_unnamed.get()));
      ASSERT_TRUE(SetInputDataset(alg, mem_named.get()));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no name"));
    }
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("reference")->Set(mem_named.get()));
      ASSERT_TRUE(SetInputDataset(alg, mem_unnamed.get()));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("in-memory"));
    }
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(SetInputDataset(alg, mem_unnamed.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--reference", ref_tif}));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("no name"));
    }
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(SetInputDataset(alg, mem_named.get()));
      ASSERT_TRUE(alg.ParseCommandLineArguments({"--reference", ref_tif}));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("in-memory"));
    }
    CPLErrorReset();
  }

  // 5. Inline VRT XML string is not a file -> VSIFOpenL fails with warning.
  {
    constexpr char kInlineVrt[] =
        "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">"
        "<VRTRasterBand dataType=\"Byte\" band=\"1\"/>"
        "</VRTDataset>";
    WithQuietHandler quiet;
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--reference", kInlineVrt, "--input", ref_tif}));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a file"));
    }
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.ParseCommandLineArguments(
          {"--reference", ref_tif, "--input", kInlineVrt}));
      CPLErrorReset();
      EXPECT_TRUE(alg.Run());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a file"));
    }
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterCompareTest, CrsGeoTransformDimsAndMetadataMismatches) {
  OGRSpatialReference srs_4326;
  ASSERT_EQ(OGRERR_NONE, srs_4326.importFromEPSG(4326));
  OGRSpatialReference srs_3857;
  ASSERT_EQ(OGRERR_NONE, srs_3857.importFromEPSG(3857));

  GDALGeoTransform gt1{10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  GDALGeoTransform gt2{15.0, 2.0, 0.0, 25.0, 0.0, -2.0};

  // 1. Ref has no CRS/GT, input has CRS/GT, different band count and height.
  {
    auto ref = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 2, 3, 2, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    inp->SetSpatialRef(&srs_4326);
    inp->SetGeoTransform(gt1);

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("Reference dataset has no CRS"));
    EXPECT_THAT(out, HasSubstr("Reference dataset has no geotransform"));
    EXPECT_THAT(out, HasSubstr("band(s), but input dataset has 2"));
    EXPECT_THAT(out, HasSubstr("height is 2, but input dataset height is 3"));
  }

  // 2. Ref has CRS/GT, input has none.
  {
    auto ref = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    ref->SetSpatialRef(&srs_4326);
    ref->SetGeoTransform(gt1);

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("input dataset has none"));
    EXPECT_THAT(out, HasSubstr("input one has none"));
  }

  // 3. Both have CRS/GT but differing, plus dataset & RPC/GEOLOCATION metadata.
  {
    auto ref = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    ref->SetSpatialRef(&srs_4326);
    inp->SetSpatialRef(&srs_3857);
    ref->SetGeoTransform(gt1);
    inp->SetGeoTransform(gt2);

    // Ignored keys + NITF_FDT + missing + differing.
    ref->SetMetadataItem("backend", "foo");
    ref->SetMetadataItem("NITF_FDT", "20260101");
    inp->SetMetadataItem("NITF_FDT", "20260102");
    ref->SetMetadataItem("ONLY_REF", "v1");
    inp->SetMetadataItem("ONLY_INP", "v2");
    ref->SetMetadataItem("SHARED", "a");
    inp->SetMetadataItem("SHARED", "b");

    // RPC whitespace stripping & ignored ERR_BIAS/ERR_RAND.
    ref->SetMetadataItem("ERR_BIAS", "1.0", "RPC");
    ref->SetMetadataItem("LINE_OFF", "  100  ", "RPC");
    inp->SetMetadataItem("LINE_OFF", "100", "RPC");
    ref->SetMetadataItem("SAMP_OFF", "200", "RPC");
    inp->SetMetadataItem("SAMP_OFF", "201", "RPC");

    ref->SetMetadataItem("SRS", "EPSG:4326", "GEOLOCATION");

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-binary")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("CRS are not equivalent"));
    EXPECT_THAT(out, HasSubstr("Geotransform of reference and input"));
    EXPECT_THAT(out, HasSubstr("ONLY_REF"));
    EXPECT_THAT(out, HasSubstr("ONLY_INP"));
    EXPECT_THAT(out, HasSubstr("SHARED"));
    EXPECT_THAT(out, HasSubstr("SAMP_OFF"));
    EXPECT_THAT(out, HasSubstr("GEOLOCATION"));
    EXPECT_EQ(std::string::npos, out.find("LINE_OFF"));
    EXPECT_EQ(std::string::npos, out.find("NITF_FDT"));
    EXPECT_EQ(std::string::npos, out.find("backend"));
    EXPECT_EQ(std::string::npos, out.find("ERR_BIAS"));
  }
}

TEST_F(GdalalgRasterCompareTest, BandAttributesOverviewsMasksAndSkipFlags) {
  const std::string ref_tif = std::string(kTestDir) + "/band_ref.tif";
  const std::string inp_tif = std::string(kTestDir) + "/band_inp.tif";

  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(ref_tif, 4, 4, 1, GDT_Byte, std::vector<uint8_t>(16, 10)));
  ASSERT_NO_FATAL_FAILURE(
      CreateTiff(inp_tif, 4, 4, 1, GDT_Byte, std::vector<uint8_t>(16, 10)));

  // 1. Add overview, mask, description, nodata, color interpretation diffs.
  {
    std::unique_ptr<GDALDataset> ref(
        GDALDataset::Open(ref_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    std::unique_ptr<GDALDataset> inp(
        GDALDataset::Open(inp_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);

    ref->GetRasterBand(1)->SetDescription("ref_desc");
    inp->GetRasterBand(1)->SetDescription("inp_desc");
    ref->GetRasterBand(1)->SetNoDataValue(0.0);
    inp->GetRasterBand(1)->SetNoDataValue(255.0);
    ref->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    inp->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    ref->GetRasterBand(1)->SetMetadataItem("BAND_KEY", "1");

    int ovr_level = 2;
    ASSERT_EQ(CE_None, ref->BuildOverviews("NEAREST", 1, &ovr_level, 0, nullptr,
                                           nullptr, nullptr, nullptr));
    ASSERT_EQ(CE_None, inp->BuildOverviews("NEAREST", 1, &ovr_level, 0, nullptr,
                                           nullptr, nullptr, nullptr));
    ASSERT_EQ(CE_None, ref->CreateMaskBand(GMF_PER_DATASET));
    ASSERT_EQ(CE_None, inp->CreateMaskBand(GMF_PER_DATASET));
  }

  {
    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.ParseCommandLineArguments(
        {"--reference", ref_tif, "--input", inp_tif, "--skip-binary"}));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* data) -> int {
      *static_cast<bool*>(data) = true;
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
    EXPECT_TRUE(progress_called);
    const std::string out = alg.GetOutputString();
    EXPECT_THAT(out, HasSubstr("has description ref_desc"));
    EXPECT_THAT(out, HasSubstr("has nodata value"));
    EXPECT_THAT(out, HasSubstr("has color interpretation"));
    EXPECT_THAT(out, HasSubstr("BAND_KEY"));
  }

  // 2. Skip-all-optional suppresses metadata, overview, CRS, etc., and tests
  // differing overview count and mask flags in another run.
  {
    auto ref = CreateMemDataset("", 4, 4, 1, GDT_Float32);
    auto inp = CreateMemDataset("", 4, 4, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    ref->GetRasterBand(1)->SetNoDataValue(
        std::numeric_limits<double>::quiet_NaN());

    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
      ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
      ASSERT_TRUE(SetInputDataset(alg, inp.get()));
      EXPECT_TRUE(alg.Run());
      const std::string out = alg.GetOutputString();
      EXPECT_THAT(out, HasSubstr("has data type Float32"));
      EXPECT_THAT(out, HasSubstr("input band has none"));
    }

    // Flip nodata presence and test overview count mismatch.
    inp->GetRasterBand(1)->SetNoDataValue(5.0);
    ref->GetRasterBand(1)->DeleteNoDataValue();
    std::unique_ptr<GDALDataset> ref_ovr_ds(
        GDALDataset::Open(ref_tif.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ref_ovr_ds);
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("skip-binary")->Set(true));
      ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
      ASSERT_TRUE(SetInputDataset(alg, inp.get()));
      EXPECT_TRUE(alg.Run());
      EXPECT_THAT(alg.GetOutputString(), HasSubstr("has no nodata value"));
    }
    {
      GDALRasterCompareAlgorithmStandalone alg;
      ASSERT_TRUE(alg.GetArg("skip-binary")->Set(true));
      ASSERT_TRUE(alg.GetArg("reference")->Set(ref_ovr_ds.get()));
      ASSERT_TRUE(SetInputDataset(alg, inp.get()));
      EXPECT_TRUE(alg.Run());
      EXPECT_THAT(alg.GetOutputString(), HasSubstr("overview band(s)"));
      EXPECT_THAT(alg.GetOutputString(), HasSubstr("mask flags"));
    }
  }
}

template <typename T>
void CheckBandCompareForType(GDALDataType dt, T v1, T v2) {
  auto ref = CreateMemDataset("", 2, 2, 1, dt);
  auto inp = CreateMemDataset("", 2, 2, 1, dt);
  ASSERT_NE(nullptr, ref);
  ASSERT_NE(nullptr, inp);
  std::array<T, 4> ref_buf = {v1, v1, v1, v1};
  std::array<T, 4> inp_buf = {v1, v2, v1, v1};
  ASSERT_EQ(CE_None,
            ref->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 2, 2, ref_buf.data(), 2, 2, dt, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            inp->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 2, 2, inp_buf.data(), 2, 2, dt, 0, 0, nullptr));

  GDALRasterCompareAlgorithmStandalone alg;
  ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
  ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
  ASSERT_TRUE(SetInputDataset(alg, inp.get()));
  EXPECT_TRUE(alg.Run());
  EXPECT_THAT(alg.GetOutputString(), HasSubstr("pixels differing: 1"));
}

template <typename T>
void CheckComplexBandCompareForType(GDALDataType dt) {
  auto ref = CreateMemDataset("", 2, 1, 1, dt);
  auto inp = CreateMemDataset("", 2, 1, 1, dt);
  ASSERT_NE(nullptr, ref);
  ASSERT_NE(nullptr, inp);
  std::array<T, 4> ref_buf = {1, 2, 3, 4};
  std::array<T, 4> inp_buf = {1, 2, 6, 8};
  ASSERT_EQ(CE_None,
            ref->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 2, 1, ref_buf.data(), 2, 1, dt, 0, 0, nullptr));
  ASSERT_EQ(CE_None,
            inp->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 2, 1, inp_buf.data(), 2, 1, dt, 0, 0, nullptr));

  GDALRasterCompareAlgorithmStandalone alg;
  ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
  ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
  ASSERT_TRUE(SetInputDataset(alg, inp.get()));
  EXPECT_TRUE(alg.Run());
  EXPECT_THAT(alg.GetOutputString(), HasSubstr("pixels differing: 1"));
}

TEST_F(GdalalgRasterCompareTest, CompareAllDataTypesAndNaNs) {
  ASSERT_NO_FATAL_FAILURE(CheckBandCompareForType<uint8_t>(GDT_Byte, 1, 4));
  ASSERT_NO_FATAL_FAILURE(CheckBandCompareForType<int8_t>(GDT_Int8, -5, 5));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<uint16_t>(GDT_UInt16, 10, 20));
  ASSERT_NO_FATAL_FAILURE(CheckBandCompareForType<int16_t>(GDT_Int16, -10, 20));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<uint32_t>(GDT_UInt32, 100, 200));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<int32_t>(GDT_Int32, -100, 200));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<uint64_t>(GDT_UInt64, 1000, 2000));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<int64_t>(GDT_Int64, -1000, 2000));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<float>(GDT_Float32, 1.5f, 3.5f));
  ASSERT_NO_FATAL_FAILURE(
      CheckBandCompareForType<double>(GDT_Float64, 1.5, 4.5));

  ASSERT_NO_FATAL_FAILURE(CheckComplexBandCompareForType<int16_t>(GDT_CInt16));
  ASSERT_NO_FATAL_FAILURE(CheckComplexBandCompareForType<int32_t>(GDT_CInt32));
  ASSERT_NO_FATAL_FAILURE(CheckComplexBandCompareForType<float>(GDT_CFloat32));
  ASSERT_NO_FATAL_FAILURE(CheckComplexBandCompareForType<double>(GDT_CFloat64));

  // NaN comparisons in Float32 and CFloat32.
  {
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    auto ref = CreateMemDataset("", 3, 1, 1, GDT_Float32);
    auto inp = CreateMemDataset("", 3, 1, 1, GDT_Float32);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    std::array<float, 3> ref_vals = {nan_f, nan_f, 1.0f};
    std::array<float, 3> inp_vals = {nan_f, 2.0f, nan_f};
    ASSERT_EQ(CE_None, ref->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 3, 1, ref_vals.data(), 3, 1,
                           GDT_Float32, 0, 0, nullptr));
    ASSERT_EQ(CE_None, inp->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 3, 1, inp_vals.data(), 3, 1,
                           GDT_Float32, 0, 0, nullptr));

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("1: pixels differing: 2"));
  }
  {
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    auto ref = CreateMemDataset("", 2, 1, 1, GDT_CFloat32);
    auto inp = CreateMemDataset("", 2, 1, 1, GDT_CFloat32);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    std::array<float, 4> ref_vals = {nan_f, nan_f, 1.0f, 2.0f};
    std::array<float, 4> inp_vals = {nan_f, nan_f, 1.0f, 2.0f};
    ASSERT_EQ(CE_None, ref->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 1, ref_vals.data(), 2, 1,
                           GDT_CFloat32, 0, 0, nullptr));
    ASSERT_EQ(CE_None, inp->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 1, inp_vals.data(), 2, 1,
                           GDT_CFloat32, 0, 0, nullptr));

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    EXPECT_EQ("", alg.GetOutputString());
  }
}

TEST_F(GdalalgRasterCompareTest, MultiBandPixelInterleaveAndProgressCancel) {
  // > 10 bands with INTERLEAVE=PIXEL triggers DatasetPixelComparison.
  constexpr int kBands = 11;
  auto check_multi = [&](GDALDataType dt) {
    auto ref = CreateMemDataset("", 2, 2, kBands, dt, "PIXEL");
    auto inp = CreateMemDataset("", 2, 2, kBands, dt, "PIXEL");
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);

    std::vector<double> ones(4 * kBands, 1.0);
    ASSERT_EQ(CE_None,
              ref->RasterIO(GF_Write, 0, 0, 2, 2, ones.data(), 2, 2,
                            GDT_Float64, kBands, nullptr, 0, 0, 0, nullptr));
    ones[0] = 5.0;
    ASSERT_EQ(CE_None,
              inp->RasterIO(GF_Write, 0, 0, 2, 2, ones.data(), 2, 2,
                            GDT_Float64, kBands, nullptr, 0, 0, 0, nullptr));

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    bool progress_called = false;
    auto progress_cb = [](double, const char*, void* data) -> int {
      *static_cast<bool*>(data) = true;
      return 1;
    };
    EXPECT_TRUE(alg.Run(progress_cb, &progress_called));
    EXPECT_TRUE(progress_called);
    EXPECT_THAT(alg.GetOutputString(),
                HasSubstr("Band 1: pixels differing: 1"));
  };

  for (GDALDataType dt :
       {GDT_Byte, GDT_Int8, GDT_UInt16, GDT_Int16, GDT_UInt32, GDT_Int32,
        GDT_UInt64, GDT_Int64, GDT_Float32, GDT_Float64, GDT_CInt16, GDT_CInt32,
        GDT_CFloat32, GDT_CFloat64}) {
    ASSERT_NO_FATAL_FAILURE(check_multi(dt));
  }

  // Test progress interruption inside DatasetPixelComparison and ComparePixels.
  {
    auto ref = CreateMemDataset("", 2, 2, kBands, GDT_Byte, "PIXEL");
    auto inp = CreateMemDataset("", 2, 2, kBands, GDT_Byte, "PIXEL");
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto cancel_cb = [](double, const char*, void*) -> int { return 0; };
    EXPECT_TRUE(alg.Run(cancel_cb, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    auto ref = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 2, 2, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-all-optional")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto cancel_cb = [](double, const char*, void*) -> int { return 0; };
    EXPECT_TRUE(alg.Run(cancel_cb, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalalgRasterCompareTest, SubdatasetsAndPipelineUsage) {
  const std::string sub1 = std::string(kTestDir) + "/sub1.tif";
  const std::string sub2 = std::string(kTestDir) + "/sub2.tif";
  ASSERT_NO_FATAL_FAILURE(CreateTiff(sub1, 2, 2, 1, GDT_Byte, {1, 2, 3, 4}));
  ASSERT_NO_FATAL_FAILURE(CreateTiff(sub2, 2, 2, 1, GDT_Byte, {1, 2, 3, 9}));

  // 1. Subdataset comparison with progress callback.
  {
    auto ref = CreateMemDataset("", 1, 1, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 1, 1, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    ref->SetMetadataItem("SUBDATASET_1_NAME", sub1.c_str(), "SUBDATASETS");
    ref->SetMetadataItem("SUBDATASET_1_DESC", "sub1", "SUBDATASETS");
    inp->SetMetadataItem("SUBDATASET_1_NAME", sub2.c_str(), "SUBDATASETS");
    inp->SetMetadataItem("SUBDATASET_1_DESC", "sub2", "SUBDATASETS");

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-binary")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    auto progress_cb = [](double, const char*, void*) -> int { return 1; };
    EXPECT_TRUE(alg.Run(progress_cb, nullptr));
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("1: pixels differing: 1"));
  }

  // 2. Subdataset count mismatch.
  {
    auto ref = CreateMemDataset("", 1, 1, 1, GDT_Byte);
    auto inp = CreateMemDataset("", 1, 1, 1, GDT_Byte);
    ASSERT_NE(nullptr, ref);
    ASSERT_NE(nullptr, inp);
    ref->SetMetadataItem("SUBDATASET_1_NAME", sub1.c_str(), "SUBDATASETS");
    ref->SetMetadataItem("SUBDATASET_1_DESC", "sub1", "SUBDATASETS");

    GDALRasterCompareAlgorithmStandalone alg;
    ASSERT_TRUE(alg.GetArg("skip-binary")->Set(true));
    ASSERT_TRUE(alg.GetArg("reference")->Set(ref.get()));
    ASSERT_TRUE(SetInputDataset(alg, inp.get()));
    EXPECT_TRUE(alg.Run());
    EXPECT_THAT(alg.GetOutputString(), HasSubstr("subdataset(s)"));
  }

  // 3. Compare inside GDALRasterPipelineAlgorithm.
  {
    GDALRasterPipelineAlgorithm pipeline;
    ASSERT_TRUE(pipeline.ParseCommandLineArguments(
        {"read", sub2, "!", "compare", sub1}));
    EXPECT_TRUE(pipeline.Run());
    EXPECT_THAT(pipeline.GetOutputString(), HasSubstr("pixels differing: 1"));
  }
}

}  // namespace
}  // namespace autotest2
