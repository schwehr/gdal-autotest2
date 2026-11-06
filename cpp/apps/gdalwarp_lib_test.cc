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
// Tests for GDALWarp (apps/gdalwarp_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalwarp_lib.cpp
//   https://gdal.org/programs/gdalwarp.html

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct GDALWarpAppOptionsDeleter {
  void operator()(GDALWarpAppOptions* opts) const {
    GDALWarpAppOptionsFree(opts);
  }
};
using GDALWarpAppOptionsUniquePtr =
    std::unique_ptr<GDALWarpAppOptions, GDALWarpAppOptionsDeleter>;

class GdalwarpLibTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
    GDALRegister_PNG();
    GDALRegister_COG();
    RegisterOGRShape();
  }

  static GDALDatasetUniquePtr CreateRaster(const std::string& path,
                                           int width = 20, int height = 20,
                                           int bands = 1,
                                           GDALDataType type = GDT_Byte,
                                           const char* srs = "EPSG:4326",
                                           const double* custom_gt = nullptr) {
    const std::string drv_name = path.empty() ? "MEM" : "GTiff";
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName(drv_name.c_str());
    if (!driver) return nullptr;
    GDALDatasetUniquePtr ds(
        driver->Create(path.c_str(), width, height, bands, type, nullptr));
    if (!ds) return nullptr;

    if (srs) {
      OGRSpatialReference oSRS;
      oSRS.SetFromUserInput(srs);
      oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
      char* wkt = nullptr;
      oSRS.exportToWkt(&wkt);
      ds->SetProjection(wkt);
      CPLFree(wkt);
    }
    double default_gt[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
    ds->SetGeoTransform(custom_gt ? const_cast<double*>(custom_gt)
                                  : default_gt);

    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      std::vector<uint8_t> data(width * height);
      for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(((i + b * 17) % 250) + 1);
      }
      if (band->RasterIO(GF_Write, 0, 0, width, height, data.data(), width,
                         height, GDT_Byte, 0, 0, nullptr) != CE_None) {
        return nullptr;
      }
    }
    return ds;
  }
};

TEST_F(GdalwarpLibTest, BasicUsage) {
  const std::string usage = GDALWarpAppGetParserUsage();
  EXPECT_THAT(usage, HasSubstr("-s_srs"));
  EXPECT_THAT(usage, HasSubstr("-t_srs"));
  EXPECT_THAT(usage, HasSubstr("-r"));
  EXPECT_THAT(usage, HasSubstr("-te"));
  EXPECT_THAT(usage, HasSubstr("-tr"));
}

TEST_F(GdalwarpLibTest, OptionsNullArgv) {
  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, psOptions);
}

TEST_F(GdalwarpLibTest, OptionsSetters) {
  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, psOptions);

  int call_count = 0;
  auto progress_func = [](double dfComplete, const char* pszMessage,
                          void* pProgressArg) -> int {
    int* pCount = static_cast<int*>(pProgressArg);
    if (pCount) (*pCount)++;
    return 1;
  };
  GDALWarpAppOptionsSetProgress(psOptions.get(), progress_func, &call_count);
  GDALWarpAppOptionsSetProgress(psOptions.get(), GDALTermProgress, nullptr);
  GDALWarpAppOptionsSetProgress(psOptions.get(), nullptr, nullptr);

  GDALWarpAppOptionsSetQuiet(psOptions.get(), 1);
  GDALWarpAppOptionsSetQuiet(psOptions.get(), 0);

  GDALWarpAppOptionsSetWarpOption(psOptions.get(), "NUM_THREADS", "4");
  GDALWarpAppOptionsSetWarpOption(psOptions.get(), "SAMPLE_STEPS", "100");
}

TEST_F(GdalwarpLibTest, OptionsForBinary) {
  CPLStringList args;
  args.AddString("-overwrite");
  args.AddString("src_path.tif");
  args.AddString("dst_path.tif");

  GDALWarpAppOptionsForBinary options_for_binary;
  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), &options_for_binary));
  ASSERT_NE(nullptr, psOptions);
  EXPECT_TRUE(options_for_binary.bOverwrite);
  EXPECT_EQ(1, options_for_binary.aosSrcFiles.size());
  EXPECT_STREQ("src_path.tif", options_for_binary.aosSrcFiles[0]);
  EXPECT_EQ("dst_path.tif", options_for_binary.osDstFilename);
}

TEST_F(GdalwarpLibTest, OptionsAllFlagsParsing) {
  CPLStringList args;
  args.AddString("-s_srs");
  args.AddString("EPSG:4326");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");
  args.AddString("-r");
  args.AddString("bilinear");
  args.AddString("-tr");
  args.AddString("30");
  args.AddString("30");
  args.AddString("-te");
  args.AddString("10");
  args.AddString("20");
  args.AddString("30");
  args.AddString("40");
  args.AddString("-te_srs");
  args.AddString("EPSG:4326");
  args.AddString("-wo");
  args.AddString("NUM_THREADS=2");
  args.AddString("-to");
  args.AddString("SRC_METHOD=NO_GEOTRANSFORM");
  args.AddString("-co");
  args.AddString("COMPRESS=DEFLATE");
  args.AddString("-multi");
  args.AddString("-s_coord_epoch");
  args.AddString("2020.0");
  args.AddString("-t_coord_epoch");
  args.AddString("2021.0");
  args.AddString("-ct");
  args.AddString(
      "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad");
  args.AddString("-order");
  args.AddString("2");
  args.AddString("-refine_gcps");
  args.AddString("1.5");
  args.AddString("5");
  args.AddString("-et");
  args.AddString("0.125");
  args.AddString("-wm");
  args.AddString("128");
  args.AddString("-srcnodata");
  args.AddString("-9999");
  args.AddString("-dstnodata");
  args.AddString("-9999");
  args.AddString("-tap");
  args.AddString("-wt");
  args.AddString("Float32");
  args.AddString("-ot");
  args.AddString("Int16");
  args.AddString("-of");
  args.AddString("GTiff");
  args.AddString("-srcalpha");
  args.AddString("-dstalpha");
  args.AddString("-cutline");
  args.AddString("POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))");
  args.AddString("-cutline_srs");
  args.AddString("EPSG:4326");
  args.AddString("-cwhere");
  args.AddString("id=1");
  args.AddString("-cl");
  args.AddString("layer_name");
  args.AddString("-cblend");
  args.AddString("3");
  args.AddString("-crop_to_cutline");
  args.AddString("-nomd");
  args.AddString("-cvmd");
  args.AddString("CONFLICT");
  args.AddString("-setci");
  args.AddString("-doo");
  args.AddString("OVERVIEW_LEVEL=1");
  args.AddString("-ovr");
  args.AddString("AUTO");
  args.AddString("-vshift");
  args.AddString("-b");
  args.AddString("1");
  args.AddString("-dstband");
  args.AddString("1");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);
}

TEST_F(GdalwarpLibTest, ResamplingShortcuts) {
  constexpr const char* kShortcuts[] = {
      "-rn", "-rb", "-rc", "-rcs", "-rl", "-ra", "-rrms", "-rm",
  };
  for (const char* shortcut : kShortcuts) {
    CPLStringList args;
    args.AddString(shortcut);
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }
}

TEST_F(GdalwarpLibTest, ResamplingMethodNames) {
  constexpr const char* kMethods[] = {
      "near", "bilinear", "cubic", "cubicspline", "lanczos", "average", "mode",
      "max",  "min",      "med",   "q1",          "q3",      "sum",     "rms",
  };
  for (const char* method : kMethods) {
    CPLStringList args;
    args.AddString("-r");
    args.AddString(method);
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }
}

TEST_F(GdalwarpLibTest, OptionsParsingErrors) {
  // 1. -refine_gcps negative tolerance
  {
    CPLStringList args;
    args.AddString("-refine_gcps");
    args.AddString("-1.0");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("tolerance for -refine_gcps"));
    CPLErrorReset();
  }

  // 2. -refine_gcps with single argument (refine_minimum_gcps defaults to -1)
  {
    CPLStringList args;
    args.AddString("-refine_gcps");
    args.AddString("2.0");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }

  // 3. -tr square
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("square");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }

  // 4. -tr with 0 value
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("0");
    args.AddString("10");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Wrong value for -tr"));
    CPLErrorReset();
  }

  // 5. -b and -dstband mismatched count
  {
    CPLStringList args;
    args.AddString("-b");
    args.AddString("1");
    args.AddString("-b");
    args.AddString("2");
    args.AddString("-dstband");
    args.AddString("1");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("-srcband should be specified as many times"));
    CPLErrorReset();
  }

  // 6. -b without -dstband (auto assigns 1, 2, ...)
  {
    CPLStringList args;
    args.AddString("-b");
    args.AddString("2");
    args.AddString("-b");
    args.AddString("3");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }

  // 7. Unknown argument
  {
    CPLStringList args;
    args.AddString("-this_is_an_unknown_argument");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 8. Invalid SRS in -s_srs, -t_srs, -te_srs
  {
    CPLStringList args;
    args.AddString("-s_srs");
    args.AddString("INVALID_SRS_DEFINITION");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid SRS for -s_srs"));
    CPLErrorReset();
  }
  {
    CPLStringList args;
    args.AddString("-t_srs");
    args.AddString("INVALID_SRS_DEFINITION");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid SRS for -t_srs"));
    CPLErrorReset();
  }
  {
    CPLStringList args;
    args.AddString("-te_srs");
    args.AddString("INVALID_SRS_DEFINITION");
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    EXPECT_EQ(nullptr, psOptions);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid SRS for -te_srs"));
    CPLErrorReset();
  }
}

TEST_F(GdalwarpLibTest, OverviewOptionsVariants) {
  constexpr const char* kOvrOptions[] = {
      "AUTO", "AUTO-1", "NONE", "0", "1", "2",
  };
  for (const char* ovr : kOvrOptions) {
    CPLStringList args;
    args.AddString("-ovr");
    args.AddString(ovr);
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
  }
}

TEST_F(GdalwarpLibTest, NullSourceDatasetFails) {
  constexpr char kDstPath[] = "/vsimem/gdalwarp_lib_test/null_src/out.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdalwarp_lib_test/null_src");
  };

  GDALDatasetH pahSrcDS[1] = {nullptr};
  GDALDatasetH hDstDS =
      GDALWarp(kDstPath, nullptr, 1, pahSrcDS, nullptr, nullptr);
  EXPECT_EQ(nullptr, hDstDS);
}

TEST_F(GdalwarpLibTest, CheckOptionsValidation) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/check_options";
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  GDALDriver* memDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, memDriver);
  GDALDatasetUniquePtr srcDS(
      memDriver->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, srcDS);
  double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  srcDS->SetGeoTransform(gt);
  GDALDatasetH hSrc = srcDS.get();

  // 1. Incompatible -tr and -ts
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("1");
    args.AddString("1");
    args.AddString("-ts");
    args.AddString("20");
    args.AddString("20");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    int bUsageError = 0;
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), &bUsageError);
    EXPECT_EQ(nullptr, hRet);
    EXPECT_TRUE(bUsageError);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("-tr and -ts options cannot be used"));
    CPLErrorReset();
  }

  // 2. Consistent -te, -ts, and -tr allowed
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("0");
    args.AddString("0");
    args.AddString("10");
    args.AddString("10");
    args.AddString("-ts");
    args.AddString("10");
    args.AddString("10");
    args.AddString("-tr");
    args.AddString("1");
    args.AddString("1");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hRet);
    GDALClose(hRet);
  }

  // 3. -tap without -tr
  {
    CPLStringList args;
    args.AddString("-tap");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    int bUsageError = 0;
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), &bUsageError);
    EXPECT_EQ(nullptr, hRet);
    EXPECT_TRUE(bUsageError);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("-tap option cannot be used without using -tr"));
    CPLErrorReset();
  }

  // 4. -te_srs without -te warning
  {
    auto dstDS = CreateRaster("", 10, 10, 1);
    ASSERT_NE(nullptr, dstDS);
    CPLStringList args;
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRet =
        GDALWarp("", dstDS.get(), 1, &hSrc, psOptions.get(), nullptr);
    EXPECT_EQ(dstDS.get(), hRet);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("-te_srs ignored since -te is not specified"));
    CPLErrorReset();
  }

  // 5. -te_srs ignored when no projection on input or in options
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("0");
    args.AddString("0");
    args.AddString("10");
    args.AddString("10");
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    EXPECT_EQ(nullptr, hRet);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("-te_srs ignored since none of -t_srs"));
    CPLErrorReset();
  }

  // 6. -te_srs with valid reprojection
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("2");
    args.AddString("48");
    args.AddString("3");
    args.AddString("49");
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    args.AddString("-t_srs");
    args.AddString("EPSG:3857");
    args.AddString("-s_srs");
    args.AddString("EPSG:4326");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hRet);
    GDALClose(hRet);
  }

  // 7. -of VRT with existing destination dataset
  {
    GDALDatasetUniquePtr dstDS(
        memDriver->Create("", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, dstDS);
    CPLStringList args;
    args.AddString("-of");
    args.AddString("VRT");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRet =
        GDALWarp("", dstDS.get(), 1, &hSrc, psOptions.get(), nullptr);
    EXPECT_EQ(nullptr, hRet);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("VRT output not compatible with existing dataset"));
    CPLErrorReset();
  }

  // 8. Warning on inverted -te (minx >= maxx, miny >= maxy) when quiet is false
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("10");
    args.AddString("10");
    args.AddString("0");
    args.AddString("0");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALWarpAppOptionsSetQuiet(psOptions.get(), 0);

    bool bGotWarning = false;
    CPLPushErrorHandlerEx(
        [](CPLErr eErr, CPLErrorNum, const char* pszMsg) {
          if (eErr == CE_Warning && strstr(pszMsg, "flipped")) {
            *static_cast<bool*>(CPLGetErrorHandlerUserData()) = true;
          }
        },
        &bGotWarning);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };
    GDALDatasetUniquePtr hRet(GDALDataset::FromHandle(
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr)));
    EXPECT_TRUE(bGotWarning);
  }

  // 9. Update mode warning when creation options are passed
  {
    auto dstDS = CreateRaster("", 10, 10, 1);
    ASSERT_NE(nullptr, dstDS);
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("1");
    args.AddString("1");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRet =
        GDALWarp("", dstDS.get(), 1, &hSrc, psOptions.get(), nullptr);
    EXPECT_EQ(dstDS.get(), hRet);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("All options related to creation ignored in update mode"));
    CPLErrorReset();
  }

  // 10. Unknown format returns nullptr
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("NONEXISTENT_FORMAT_FOOBAR");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hRet =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    EXPECT_EQ(nullptr, hRet);
    CPLErrorReset();
  }
}

TEST_F(GdalwarpLibTest, ExecutionMemToMem) {
  auto srcDS = CreateRaster("");
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  CPLStringList args;
  args.AddString("-s_srs");
  args.AddString("EPSG:4326");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");
  args.AddString("-r");
  args.AddString("bilinear");
  args.AddString("-of");
  args.AddString("MEM");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hDst = GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr dstDS(reinterpret_cast<GDALDataset*>(hDst));

  EXPECT_GT(dstDS->GetRasterXSize(), 0);
  EXPECT_GT(dstDS->GetRasterYSize(), 0);
  EXPECT_EQ(1, dstDS->GetRasterCount());
}

TEST_F(GdalwarpLibTest, ExecutionGTiffToFile) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/gtiff_file";
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string srcPath = std::string(kSuiteDir) + "/src.tif";
  const std::string dstPath = std::string(kSuiteDir) + "/dst.tif";

  auto srcDS = CreateRaster(srcPath, 20, 20, 2);
  ASSERT_NE(nullptr, srcDS);
  srcDS.reset();  // Close and flush to vsimem.

  GDALDatasetH hSrcDS = GDALOpen(srcPath.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrcDS);
  absl::Cleanup srcCloser = [hSrcDS] { GDALClose(hSrcDS); };

  CPLStringList args;
  args.AddString("-t_srs");
  args.AddString("EPSG:32631");
  args.AddString("-co");
  args.AddString("TILED=NO");
  args.AddString("-multi");
  args.AddString("-wo");
  args.AddString("NUM_THREADS=2");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hDst =
      GDALWarp(dstPath.c_str(), nullptr, 1, &hSrcDS, psOptions.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  GDALDatasetUniquePtr checkDS(
      reinterpret_cast<GDALDataset*>(GDALOpen(dstPath.c_str(), GA_ReadOnly)));
  ASSERT_NE(nullptr, checkDS);
  EXPECT_EQ(2, checkDS->GetRasterCount());
  EXPECT_GT(checkDS->GetRasterXSize(), 0);
  EXPECT_GT(checkDS->GetRasterYSize(), 0);
}

TEST_F(GdalwarpLibTest, ExecutionUpdateExistingDataset) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/update_existing";
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string srcPath = std::string(kSuiteDir) + "/src.tif";
  const std::string dstPath = std::string(kSuiteDir) + "/dst.tif";

  auto srcDS = CreateRaster(srcPath, 20, 20, 1);
  ASSERT_NE(nullptr, srcDS);
  srcDS.reset();

  // Create dst with same projection and extent
  auto dstDS = CreateRaster(dstPath, 20, 20, 1);
  ASSERT_NE(nullptr, dstDS);
  dstDS.reset();

  GDALDatasetH hSrcDS = GDALOpen(srcPath.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrcDS);
  absl::Cleanup srcCloser = [hSrcDS] { GDALClose(hSrcDS); };

  GDALDatasetUniquePtr dst_open(
      GDALDataset::FromHandle(GDALOpen(dstPath.c_str(), GA_Update)));
  ASSERT_NE(nullptr, dst_open);

  CPLStringList args;
  args.AddString("-r");
  args.AddString("near");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hResult = GDALWarp(dstPath.c_str(), dst_open.get(), 1, &hSrcDS,
                                  psOptions.get(), nullptr);
  EXPECT_EQ(dst_open.get(), hResult);
}

TEST_F(GdalwarpLibTest, BandSelectionAndMapping) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/band_select";
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string srcPath = std::string(kSuiteDir) + "/src.tif";
  const std::string dstPath = std::string(kSuiteDir) + "/dst.tif";

  // Create 3-band source
  auto srcDS = CreateRaster(srcPath, 20, 20, 3);
  ASSERT_NE(nullptr, srcDS);
  srcDS.reset();

  GDALDatasetH hSrc = GDALOpen(srcPath.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  absl::Cleanup srcCloser = [hSrc] { GDALClose(hSrc); };

  // Warp only band 2 to band 1 in output
  CPLStringList args;
  args.AddString("-b");
  args.AddString("2");
  args.AddString("-dstband");
  args.AddString("1");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hDst =
      GDALWarp(dstPath.c_str(), nullptr, 1, &hSrc, psOptions.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  GDALDatasetUniquePtr checkDS(
      reinterpret_cast<GDALDataset*>(GDALOpen(dstPath.c_str(), GA_ReadOnly)));
  ASSERT_NE(nullptr, checkDS);
  EXPECT_EQ(1, checkDS->GetRasterCount());
}

TEST_F(GdalwarpLibTest, AllResamplingAlgorithms) {
  auto srcDS = CreateRaster("", 16, 16, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  constexpr const char* kResampleMethods[] = {
      "near", "bilinear", "cubic", "cubicspline", "lanczos", "average", "mode",
      "max",  "min",      "med",   "q1",          "q3",      "sum",     "rms",
  };

  for (const char* method : kResampleMethods) {
    CPLStringList args;
    args.AddString("-r");
    args.AddString(method);
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions) << "Failed for resampling method: " << method;

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp failed for method: " << method;
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, ExtentAndResolutionOptions) {
  auto srcDS = CreateRaster("", 30, 20, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  // 1. -tr (target resolution)
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("0.05");
    args.AddString("0.05");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    double gt[6] = {0};
    ASSERT_EQ(CE_None, dst->GetGeoTransform(gt));
    EXPECT_NEAR(0.05, gt[1], 1e-6);
    EXPECT_NEAR(-0.05, gt[5], 1e-6);
  }

  // 2. -ts (force pixels and lines)
  {
    CPLStringList args;
    args.AddString("-ts");
    args.AddString("50");
    args.AddString("40");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_EQ(50, dst->GetRasterXSize());
    EXPECT_EQ(40, dst->GetRasterYSize());
  }

  // 3. -ts with 0 height (guessed from resolution)
  {
    CPLStringList args;
    args.AddString("-ts");
    args.AddString("60");
    args.AddString("0");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_EQ(60, dst->GetRasterXSize());
    EXPECT_GT(dst->GetRasterYSize(), 0);
  }

  // 4. -ts with 0 width (guessed from resolution)
  {
    CPLStringList args;
    args.AddString("-ts");
    args.AddString("0");
    args.AddString("50");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_GT(dst->GetRasterXSize(), 0);
    EXPECT_EQ(50, dst->GetRasterYSize());
  }

  // 5. -te (target extent)
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("10.2");
    args.AddString("48.5");
    args.AddString("11.5");
    args.AddString("49.8");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    double gt[6] = {0};
    ASSERT_EQ(CE_None, dst->GetGeoTransform(gt));
    EXPECT_NEAR(10.2, gt[0], 0.1);
    EXPECT_NEAR(49.8, gt[3], 0.1);
  }

  // 6. -tap (target aligned pixels) with -tr
  {
    CPLStringList args;
    args.AddString("-tap");
    args.AddString("-tr");
    args.AddString("0.2");
    args.AddString("0.2");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    double gt[6] = {0};
    ASSERT_EQ(CE_None, dst->GetGeoTransform(gt));
    // Coordinate modulo resolution should be near 0
    EXPECT_NEAR(0.0, std::abs(std::remainder(gt[0], 0.2)), 1e-5);
    EXPECT_NEAR(0.0, std::abs(std::remainder(gt[3], 0.2)), 1e-5);
  }
}

TEST_F(GdalwarpLibTest, NodataHandling) {
  auto srcDS = CreateRaster("", 20, 20, 2);
  ASSERT_NE(nullptr, srcDS);
  srcDS->GetRasterBand(1)->SetNoDataValue(0.0);
  srcDS->GetRasterBand(2)->SetNoDataValue(255.0);
  GDALDatasetH hSrc = srcDS.get();

  // 1. Explicit srcnodata and dstnodata
  {
    CPLStringList args;
    args.AddString("-srcnodata");
    args.AddString("0 255");
    args.AddString("-dstnodata");
    args.AddString("99 199");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_DOUBLE_EQ(99.0, dst->GetRasterBand(1)->GetNoDataValue());
    EXPECT_DOUBLE_EQ(199.0, dst->GetRasterBand(2)->GetNoDataValue());
  }

  // 2. None for srcnodata and dstnodata
  {
    CPLStringList args;
    args.AddString("-srcnodata");
    args.AddString("None");
    args.AddString("-dstnodata");
    args.AddString("None");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    int bSuccess = 0;
    dst->GetRasterBand(1)->GetNoDataValue(&bSuccess);
    EXPECT_FALSE(bSuccess);
  }

  // 3. Fallback from source nodata
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_DOUBLE_EQ(0.0, dst->GetRasterBand(1)->GetNoDataValue());
    EXPECT_DOUBLE_EQ(255.0, dst->GetRasterBand(2)->GetNoDataValue());
  }

  // 4. Clamped / rounded dstnodata warning for integer type
  {
    CPLStringList args;
    args.AddString("-ot");
    args.AddString("Byte");
    args.AddString("-dstnodata");
    args.AddString("300");  // > 255 clamps to 255
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("clamped"));
    GDALClose(hDst);
    CPLErrorReset();
  }

  // 5. Floating-point NaN nodata
  {
    auto floatDS = CreateRaster("", 10, 10, 1, GDT_Float32);
    ASSERT_NE(nullptr, floatDS);
    floatDS->GetRasterBand(1)->SetNoDataValue(
        std::numeric_limits<double>::quiet_NaN());
    GDALDatasetH hFloatSrc = floatDS.get();

    CPLStringList args;
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hFloatSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_TRUE(std::isnan(dst->GetRasterBand(1)->GetNoDataValue()));
  }
}

TEST_F(GdalwarpLibTest, AlphaBands) {
  // 1. -dstalpha adds an alpha band
  {
    auto srcDS = CreateRaster("", 20, 20, 1);
    ASSERT_NE(nullptr, srcDS);
    GDALDatasetH hSrc = srcDS.get();

    CPLStringList args;
    args.AddString("-dstalpha");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_EQ(2, dst->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, dst->GetRasterBand(2)->GetColorInterpretation());
  }

  // 2. -srcalpha treats last band as alpha
  {
    auto srcDS = CreateRaster("", 20, 20, 4);
    ASSERT_NE(nullptr, srcDS);
    GDALDatasetH hSrc = srcDS.get();

    CPLStringList args;
    args.AddString("-srcalpha");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }

  // 3. -nosrcalpha disables alpha consideration
  {
    auto srcDS = CreateRaster("", 20, 20, 4);
    ASSERT_NE(nullptr, srcDS);
    srcDS->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    GDALDatasetH hSrc = srcDS.get();

    CPLStringList args;
    args.AddString("-nosrcalpha");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, CutlineOptionsAndCrop) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/cutline_test";
  VSIMkdirRecursive(kSuiteDir, 0755);
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  auto srcDS = CreateRaster("", 50, 50, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  // 1. Cutline directly from WKT string with -crop_to_cutline
  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "POLYGON((10.5 48.5, 10.5 49.5, 11.5 49.5, 11.5 48.5, 10.5 48.5))");
    args.AddString("-crop_to_cutline");
    args.AddString("-cblend");
    args.AddString("2");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    double gt[6] = {0};
    ASSERT_EQ(CE_None, dst->GetGeoTransform(gt));
    EXPECT_NEAR(10.5, gt[0], 0.1);
    EXPECT_NEAR(49.5, gt[3], 0.1);
  }

  // 2. Cutline from MultiPolygon WKT
  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "MULTIPOLYGON(((10.5 48.5, 10.5 49.5, 11.5 49.5, 11.5 48.5, 10.5 "
        "48.5)))");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }

  // 3. Cutline from Shapefile layer with -cl and -cwhere
  {
    const std::string shpPath = std::string(kSuiteDir) + "/cutline.shp";
    GDALDriver* shpDriver =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_NE(nullptr, shpDriver);
    GDALDatasetUniquePtr shpDS(
        shpDriver->Create(shpPath.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, shpDS);

    OGRSpatialReference oSRS;
    oSRS.SetFromUserInput("EPSG:4326");
    oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* layer = shpDS->CreateLayer("cutline", &oSRS, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn fieldDefn("id", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&fieldDefn));

    // Add feature 1 (id=1)
    OGRFeature* feat1 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat1->SetField("id", 1);
    char* wkt1 = const_cast<char*>(
        "POLYGON((10.5 48.5, 10.5 49.5, 11.5 49.5, 11.5 48.5, 10.5 48.5))");
    OGRGeometry* geom1 = nullptr;
    OGRGeometryFactory::createFromWkt(&wkt1, &oSRS, &geom1);
    feat1->SetGeometryDirectly(geom1);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat1));
    OGRFeature::DestroyFeature(feat1);

    // Add feature 2 (id=2)
    OGRFeature* feat2 = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat2->SetField("id", 2);
    char* wkt2 = const_cast<char*>(
        "POLYGON((12.0 48.0, 12.0 49.0, 13.0 49.0, 13.0 48.0, 12.0 48.0))");
    OGRGeometry* geom2 = nullptr;
    OGRGeometryFactory::createFromWkt(&wkt2, &oSRS, &geom2);
    feat2->SetGeometryDirectly(geom2);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat2));
    OGRFeature::DestroyFeature(feat2);

    shpDS.reset();  // Close and flush to vsimem.

    // Warp with -cutline, -cl, -cwhere
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(shpPath.c_str());
    args.AddString("-cl");
    args.AddString("cutline");
    args.AddString("-cwhere");
    args.AddString("id=1");
    args.AddString("-crop_to_cutline");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);

    // Warp with -csql
    CPLStringList argsSql;
    argsSql.AddString("-cutline");
    argsSql.AddString(shpPath.c_str());
    argsSql.AddString("-csql");
    argsSql.AddString("SELECT * FROM cutline WHERE id = 1");
    argsSql.AddString("-of");
    argsSql.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptionsSql(
        GDALWarpAppOptionsNew(argsSql.List(), nullptr));
    ASSERT_NE(nullptr, psOptionsSql);

    GDALDatasetH hDstSql =
        GDALWarp("", nullptr, 1, &hSrc, psOptionsSql.get(), nullptr);
    ASSERT_NE(nullptr, hDstSql);
    GDALClose(hDstSql);
  }

  // 4. Invalid cutline with GDALWARP_IGNORE_BAD_CUTLINE
  {
    CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", "YES");
    absl::Cleanup configCleaner = [] {
      CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", nullptr);
    };

    // Bowtie polygon (self-intersecting)
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString("POLYGON((0 0, 1 1, 0 1, 1 0, 0 0))");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    if (hDst) GDALClose(hDst);
    CPLErrorReset();
  }

  // 5. Cutline with zero-width sliver removal
  {
    // Polygon with an antenna/sliver
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "POLYGON((10.5 48.5, 10.5 49.5, 11.5 49.5, 11.5 48.5, 10.5 48.5))");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, MetadataAndBandInfoHandling) {
  auto src1 = CreateRaster("", 20, 20, 1);
  ASSERT_NE(nullptr, src1);
  src1->SetMetadataItem("AUTHOR", "Google");
  src1->SetMetadataItem("SHARED", "Value1");
  src1->GetRasterBand(1)->SetDescription("TestBand1");
  src1->GetRasterBand(1)->SetScale(2.5);
  src1->GetRasterBand(1)->SetOffset(10.0);
  src1->GetRasterBand(1)->SetUnitType("meters");

  auto src2 = CreateRaster("", 20, 20, 1);
  ASSERT_NE(nullptr, src2);
  src2->SetMetadataItem("AUTHOR", "Google");
  src2->SetMetadataItem("SHARED", "Value2");  // Conflict!

  GDALDatasetH pahSrc[2] = {src1.get(), src2.get()};

  // 1. Single source copies band description and unit type
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, pahSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_STREQ("Google", dst->GetMetadataItem("AUTHOR"));
    EXPECT_STREQ("Value1", dst->GetMetadataItem("SHARED"));
    EXPECT_STREQ("TestBand1", dst->GetRasterBand(1)->GetDescription());
    EXPECT_STREQ("meters", dst->GetRasterBand(1)->GetUnitType());
  }

  // 2. Conflict marked with default "*"
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 2, pahSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_STREQ("Google", dst->GetMetadataItem("AUTHOR"));
    EXPECT_STREQ("*", dst->GetMetadataItem("SHARED"));
  }

  // 3. Custom conflict string with -cvmd
  {
    CPLStringList args;
    args.AddString("-cvmd");
    args.AddString("CONFLICT_RESOLVED");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 2, pahSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_STREQ("CONFLICT_RESOLVED", dst->GetMetadataItem("SHARED"));
  }

  // 4. Remove conflict with -cvmd ""
  {
    CPLStringList args;
    args.AddString("-cvmd");
    args.AddString("");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 2, pahSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_STREQ("", dst->GetMetadataItem("SHARED"));
  }

  // 5. Suppress metadata with -nomd
  {
    CPLStringList args;
    args.AddString("-nomd");
    args.AddString("-of");
    args.AddString("MEM");
    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);
    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, pahSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));
    EXPECT_EQ(nullptr, dst->GetMetadataItem("AUTHOR"));
  }
}

TEST_F(GdalwarpLibTest, ColorTableAndRatAndSetCi) {
  auto srcDS = CreateRaster("", 20, 20, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALRasterBand* band = srcDS->GetRasterBand(1);

  // Set Color Table
  GDALColorTable colorTable(GPI_RGB);
  GDALColorEntry entry1 = {255, 0, 0, 255};
  GDALColorEntry entry2 = {0, 255, 0, 255};
  colorTable.SetColorEntry(0, &entry1);
  colorTable.SetColorEntry(1, &entry2);
  band->SetColorTable(&colorTable);

  // Set RAT
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Class", GFT_String, GFU_Name);
  rat.SetRowCount(1);
  rat.SetValue(0, 0, "Forest");
  band->SetDefaultRAT(&rat);

  band->SetColorInterpretation(GCI_PaletteIndex);

  GDALDatasetH hSrc = srcDS.get();

  CPLStringList args;
  args.AddString("-setci");
  args.AddString("-of");
  args.AddString("MEM");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hDst = GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));

  EXPECT_EQ(GCI_PaletteIndex, dst->GetRasterBand(1)->GetColorInterpretation());
  EXPECT_NE(nullptr, dst->GetRasterBand(1)->GetColorTable());
  EXPECT_NE(nullptr, dst->GetRasterBand(1)->GetDefaultRAT());
}

TEST_F(GdalwarpLibTest, OverviewsHandling) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/overviews";
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string srcPath = std::string(kSuiteDir) + "/src.tif";
  auto srcDS = CreateRaster(srcPath, 64, 64, 1);
  ASSERT_NE(nullptr, srcDS);

  int anOverviewList[2] = {2, 4};
  ASSERT_EQ(CE_None, srcDS->BuildOverviews("NEAREST", 2, anOverviewList, 0,
                                           nullptr, nullptr, nullptr, nullptr));
  srcDS.reset();  // Close and flush to vsimem.

  GDALDatasetH hSrc = GDALOpen(srcPath.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  absl::Cleanup srcCloser = [hSrc] { GDALClose(hSrc); };

  constexpr const char* kOvrFlags[] = {"AUTO", "AUTO-1", "NONE", "0", "1"};
  for (const char* ovr : kOvrFlags) {
    CPLStringList args;
    args.AddString("-ovr");
    args.AddString(ovr);
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "Failed for -ovr " << ovr;
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, TransformersAndGCPs) {
  // 1. GCP-based warp with -order 1 and -tps
  {
    auto srcDS = CreateRaster("", 20, 20, 1, GDT_Byte, /*srs=*/nullptr);
    ASSERT_NE(nullptr, srcDS);

    OGRSpatialReference oSRS;
    oSRS.SetFromUserInput("EPSG:4326");
    oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    char* srs_wkt = nullptr;
    oSRS.exportToWkt(&srs_wkt);

    GDAL_GCP gcps[4];
    GDALInitGCPs(4, gcps);
    gcps[0].dfGCPPixel = 0;
    gcps[0].dfGCPLine = 0;
    gcps[0].dfGCPX = 10.0;
    gcps[0].dfGCPY = 50.0;
    gcps[1].dfGCPPixel = 20;
    gcps[1].dfGCPLine = 0;
    gcps[1].dfGCPX = 12.0;
    gcps[1].dfGCPY = 50.0;
    gcps[2].dfGCPPixel = 20;
    gcps[2].dfGCPLine = 20;
    gcps[2].dfGCPX = 12.0;
    gcps[2].dfGCPY = 48.0;
    gcps[3].dfGCPPixel = 0;
    gcps[3].dfGCPLine = 20;
    gcps[3].dfGCPX = 10.0;
    gcps[3].dfGCPY = 48.0;

    srcDS->SetGCPs(4, gcps, srs_wkt);
    GDALDeinitGCPs(4, gcps);
    CPLFree(srs_wkt);

    GDALDatasetH hSrc = srcDS.get();

    // -order 1
    {
      CPLStringList args;
      args.AddString("-order");
      args.AddString("1");
      args.AddString("-t_srs");
      args.AddString("EPSG:4326");
      args.AddString("-of");
      args.AddString("MEM");
      GDALWarpAppOptionsUniquePtr psOptions(
          GDALWarpAppOptionsNew(args.List(), nullptr));
      ASSERT_NE(nullptr, psOptions);
      GDALDatasetH hDst =
          GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
      ASSERT_NE(nullptr, hDst);
      GDALClose(hDst);
    }

    // -tps
    {
      CPLStringList args;
      args.AddString("-tps");
      args.AddString("-t_srs");
      args.AddString("EPSG:4326");
      args.AddString("-of");
      args.AddString("MEM");
      GDALWarpAppOptionsUniquePtr psOptions(
          GDALWarpAppOptionsNew(args.List(), nullptr));
      ASSERT_NE(nullptr, psOptions);
      GDALDatasetH hDst =
          GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
      ASSERT_NE(nullptr, hDst);
      GDALClose(hDst);
    }

    // -refine_gcps
    {
      CPLStringList args;
      args.AddString("-refine_gcps");
      args.AddString("1.5");
      args.AddString("4");
      args.AddString("-t_srs");
      args.AddString("EPSG:4326");
      args.AddString("-of");
      args.AddString("MEM");
      GDALWarpAppOptionsUniquePtr psOptions(
          GDALWarpAppOptionsNew(args.List(), nullptr));
      ASSERT_NE(nullptr, psOptions);
      GDALDatasetH hDst =
          GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
      ASSERT_NE(nullptr, hDst);
      GDALClose(hDst);
    }
  }

  // 2. Coordinate pipeline with -ct
  {
    auto srcDS = CreateRaster("", 20, 20, 1, GDT_Byte, "EPSG:4326");
    ASSERT_NE(nullptr, srcDS);
    GDALDatasetH hSrc = srcDS.get();

    CPLStringList args;
    args.AddString("-ct");
    args.AddString(
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad +step "
        "+proj=utm +zone=32 +ellps=WGS84");
    args.AddString("-t_srs");
    args.AddString("EPSG:32632");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }

  // 3. Epochs and Warp Memory
  {
    auto srcDS = CreateRaster("", 20, 20, 1, GDT_Byte, "EPSG:4326");
    ASSERT_NE(nullptr, srcDS);
    GDALDatasetH hSrc = srcDS.get();

    CPLStringList args;
    args.AddString("-s_coord_epoch");
    args.AddString("2020.0");
    args.AddString("-t_coord_epoch");
    args.AddString("2022.0");
    args.AddString("-wm");
    args.AddString("32");
    args.AddString("-et");
    args.AddString("0.0");
    args.AddString("-of");
    args.AddString("MEM");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "GDALWarp error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, IndirectWarpingPNG) {
  constexpr char kSuiteDir[] = "/vsimem/gdalwarp_lib_test/indirect_png";
  VSIMkdirRecursive(kSuiteDir, 0755);
  absl::Cleanup cleanup = [kSuiteDir] { VSIRmdirRecursive(kSuiteDir); };

  const std::string dstPath = std::string(kSuiteDir) + "/out.png";

  auto srcDS = CreateRaster("", 20, 20, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  // 1. Single source into PNG (CreateCopy only format)
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst =
        GDALWarp(dstPath.c_str(), nullptr, 1, &hSrc, psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);

    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(dstPath.c_str(), &sStat));
  }

  // 2. Multi-source into PNG
  {
    const std::string tmpDir = std::string(kSuiteDir) + "/tmp";
    VSIMkdirRecursive(tmpDir.c_str(), 0755);
    CPLSetConfigOption("CPL_TMPDIR", tmpDir.c_str());
    absl::Cleanup tmpCleaner = [] {
      CPLSetConfigOption("CPL_TMPDIR", nullptr);
    };

    const std::string s1Path = std::string(kSuiteDir) + "/s1.tif";
    const std::string s2Path = std::string(kSuiteDir) + "/s2.tif";
    double gt1[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
    double gt2[6] = {12.0, 0.1, 0.0, 50.0, 0.0, -0.1};
    auto s1 = CreateRaster(s1Path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    ASSERT_NE(nullptr, s1);
    auto s2 = CreateRaster(s2Path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt2);
    ASSERT_NE(nullptr, s2);
    s1.reset();
    s2.reset();

    GDALDatasetH h1 = GDALOpen(s1Path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, h1);
    absl::Cleanup c1 = [h1] { GDALClose(h1); };
    GDALDatasetH h2 = GDALOpen(s2Path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, h2);
    absl::Cleanup c2 = [h2] { GDALClose(h2); };

    GDALDatasetH pahSrc[2] = {h1, h2};

    const std::string dstPathMulti = std::string(kSuiteDir) + "/out_multi.png";
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");

    GDALWarpAppOptionsUniquePtr psOptions(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, psOptions);

    GDALDatasetH hDst = GDALWarp(dstPathMulti.c_str(), nullptr, 2, pahSrc,
                                 psOptions.get(), nullptr);
    ASSERT_NE(nullptr, hDst) << "PNG multi Error: " << CPLGetLastErrorMsg();
    GDALClose(hDst);

    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(dstPathMulti.c_str(), &sStat));
  }
}

TEST_F(GdalwarpLibTest, WarpedVRT) {
  auto srcDS = CreateRaster("", 20, 20, 1);
  ASSERT_NE(nullptr, srcDS);
  GDALDatasetH hSrc = srcDS.get();

  CPLStringList args;
  args.AddString("-of");
  args.AddString("VRT");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr psOptions(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, psOptions);

  GDALDatasetH hDst = GDALWarp("", nullptr, 1, &hSrc, psOptions.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));

  EXPECT_GT(dst->GetRasterXSize(), 0);
  EXPECT_GT(dst->GetRasterYSize(), 0);

  // Read pixels from warped VRT
  std::vector<uint8_t> buffer(dst->GetRasterXSize());
  EXPECT_EQ(CE_None, dst->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, dst->GetRasterXSize(), 1, buffer.data(),
                         dst->GetRasterXSize(), 1, GDT_Byte, 0, 0, nullptr));
}

TEST_F(GdalwarpLibTest, BlankBorderDetectionWithTapAndTr) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/blank_border/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  double gt[6] = {10.15, 0.1, 0.0, 50.15, 0.0, -0.1};
  auto src_ds = CreateRaster(src_path, 30, 30, 1, GDT_Byte, "EPSG:4326", gt);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-tap");
  args.AddString("-tr");
  args.AddString("0.1");
  args.AddString("0.1");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, MercatorClampingNearPoles) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/mercator_clamp/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  double gt[6] = {-10.0, 1.0, 0.0, 88.0, 0.0, -1.0};
  auto src_ds = CreateRaster(src_path, 20, 10, 1, GDT_Byte, "EPSG:4326", gt);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, VerticalShiftOptions) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/vshift/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  // Test 1: -vshift with 3D CRS and unit type "ft"
  {
    auto src_ds = CreateRaster(src_path, 10, 10, 1, GDT_Float32, "EPSG:4979");
    ASSERT_NE(nullptr, src_ds);
    EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetUnitType("ft"));
    src_ds.reset();

    GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrc);
    GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

    CPLStringList args;
    args.AddString("-s_srs");
    args.AddString("EPSG:4979");
    args.AddString("-t_srs");
    args.AddString("EPSG:4979");
    args.AddString("-vshift");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 2: Unit types "US survey foot", "m", and custom unit type
  for (const char* unit : {"US survey foot", "m", "custom_unit"}) {
    const std::string dst_unit_path =
        test_dir + "dst_" + std::string(unit) + ".tif";
    auto src_ds = CreateRaster(src_path, 10, 10, 1, GDT_Float32, "EPSG:4979");
    ASSERT_NE(nullptr, src_ds);
    EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetUnitType(unit));
    src_ds.reset();

    GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrc);
    GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

    CPLStringList args;
    args.AddString("-t_srs");
    args.AddString("EPSG:4979");
    args.AddString("-vshift");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_unit_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 3: -novshift option
  {
    const std::string dst_novshift = test_dir + "dst_novshift.tif";
    GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrc);
    GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

    CPLStringList args;
    args.AddString("-t_srs");
    args.AddString("EPSG:4326");
    args.AddString("-novshift");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_novshift.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, CogDriverOptions) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cog/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_cog1 = test_dir + "dst1.tif";
  const std::string dst_cog2 = test_dir + "dst2.tif";

  auto src_ds = CreateRaster(src_path, 32, 32, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test 1: Incompatible target SRS triggers failure in DealWithCOGOptions
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("COG");
    args.AddString("-t_srs");
    args.AddString("EPSG:4326");
    args.AddString("-co");
    args.AddString("TARGET_SRS=EPSG:3857");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_cog1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // Test 2: Valid COG with TARGET_SRS
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("COG");
    args.AddString("-co");
    args.AddString("TARGET_SRS=EPSG:3857");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_cog1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 3: Valid COG with TILING_SCHEME=GoogleMapsCompatible and -t_srs
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("COG");
    args.AddString("-t_srs");
    args.AddString("EPSG:3857");
    args.AddString("-co");
    args.AddString("TILING_SCHEME=GoogleMapsCompatible");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_cog2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, RpcWarpWithExtentRestriction) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/rpc/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src_rpc.tif";
  const std::string dst_path = test_dir + "dst_rpc.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  CPLStringList rpc{"LINE_OFF=10",
                    "SAMP_OFF=10",
                    "LAT_OFF=50",
                    "LONG_OFF=10",
                    "HEIGHT_OFF=0",
                    "LINE_SCALE=10",
                    "SAMP_SCALE=10",
                    "LAT_SCALE=1.0",
                    "LONG_SCALE=1.0",
                    "HEIGHT_SCALE=100.0",
                    "LINE_NUM_COEFF=0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
                    "LINE_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
                    "SAMP_NUM_COEFF=0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0",
                    "SAMP_DEN_COEFF=1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"};
  EXPECT_EQ(CE_None, src_ds->SetMetadata(rpc.List(), "RPC"));
  src_ds.reset();

  auto dst_ds = CreateRaster(dst_path, 50, 50, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, dst_ds);
  GDALDatasetH hDstDS = dst_ds.get();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-rpc");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hRet =
      GDALWarp(nullptr, hDstDS, 1, &hSrcRaw, options.get(), nullptr);
  EXPECT_EQ(hDstDS, hRet);
}

TEST_F(GdalwarpLibTest, OverviewWindowSelectionExplicitBounds) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/ovr_explicit/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 64, 64, 1);
  ASSERT_NE(nullptr, src_ds);
  int anOverviewList[2] = {2, 4};
  EXPECT_EQ(CE_None,
            src_ds->BuildOverviews("NEAR", 2, anOverviewList, 0, nullptr,
                                   nullptr, nullptr, nullptr));
  src_ds.reset();

  // Test 1: -ovr AUTO with explicit bounds and resolution
  {
    GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrc);
    GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

    CPLStringList args;
    args.AddString("-ovr");
    args.AddString("AUTO");
    args.AddString("-te");
    args.AddString("10.0");
    args.AddString("43.6");
    args.AddString("16.4");
    args.AddString("50.0");
    args.AddString("-tr");
    args.AddString("0.4");
    args.AddString("0.4");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 2: -ovr 99 (invalid level triggers warning and defaults to nOvCount -
  // 1)
  {
    const std::string dst_ovr99 = test_dir + "dst_ovr99.tif";
    GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrc);
    GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

    CPLStringList args;
    args.AddString("-ovr");
    args.AddString("99");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_ovr99.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, CutlineSliversAndDumpValidation) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_adv/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path1 = test_dir + "dst1.tif";
  const std::string dump_csv = test_dir + "cutline_dump.csv";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test 1: Cutline with LineString shapefile
  {
    const std::string cutline_shp = test_dir + "cutline_line.shp";
    GDALDriver* shp_drv =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_NE(nullptr, shp_drv);
    GDALDatasetUniquePtr shp_ds(
        shp_drv->Create(cutline_shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, shp_ds);
    OGRLayer* layer =
        shp_ds->CreateLayer("cutline_line", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(10.5, 49.5);
    line->addPoint(11.5, 48.5);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometry(line.get()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
    shp_ds.reset();

    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(cutline_shp.c_str());

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // Test 2: -crop_to_cutline with -wo CUTLINE_ALL_TOUCHED=TRUE
  {
    const std::string dst_path2 = test_dir + "dst2.tif";
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "POLYGON ((10.2 48.2, 10.8 48.2, 10.8 48.8, 10.2 48.8, 10.2 48.2))");
    args.AddString("-crop_to_cutline");
    args.AddString("-wo");
    args.AddString("CUTLINE_ALL_TOUCHED=TRUE");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 3: Cutline without SRS and source without SRS
  {
    const std::string src_no_srs = test_dir + "src_no_srs.tif";
    const std::string dst_path3 = test_dir + "dst3.tif";
    auto no_srs_ds = CreateRaster(src_no_srs, 20, 20, 1, GDT_Byte, nullptr);
    ASSERT_NE(nullptr, no_srs_ds);
    no_srs_ds.reset();

    GDALDatasetH hSrcNoSRS = GDALOpen(src_no_srs.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrcNoSRS);
    GDALDatasetUniquePtr src_no(reinterpret_cast<GDALDataset*>(hSrcNoSRS));

    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "POLYGON ((10.2 48.2, 10.8 48.2, 10.8 48.8, 10.2 48.8, 10.2 48.2))");
    args.AddString("-crop_to_cutline");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src_no.get();
    GDALDatasetH hDst = GDALWarp(dst_path3.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 4: Cutline with -s_srs override
  {
    const std::string dst_path4 = test_dir + "dst4.tif";
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(
        "POLYGON ((10.2 48.2, 10.8 48.2, 10.8 48.8, 10.2 48.8, 10.2 48.2))");
    args.AddString("-s_srs");
    args.AddString("EPSG:4326");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path4.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 5: Invalid polygon cutline with GDALWARP_IGNORE_BAD_CUTLINE and
  // GDALWARP_DUMP_WKT_TO_FILE
  {
    CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", "YES");
    CPLSetConfigOption("GDALWARP_DUMP_WKT_TO_FILE", "stderr");
    absl::Cleanup reset_config = [] {
      CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", nullptr);
      CPLSetConfigOption("GDALWARP_DUMP_WKT_TO_FILE", nullptr);
    };

    const std::string dst_path5 = test_dir + "dst5.tif";
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString("POLYGON ((10 48, 12 50, 10 50, 12 48, 10 48))");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path5.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, ComplexAndClampedNodata) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/nodata_adv/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path2 = test_dir + "src2.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path2, 20, 20, 2);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path2.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test 1: -srcnodata with imaginary part and fewer tokens than bands,
  // and -dstnodata with "none" for band 1
  {
    CPLStringList args;
    args.AddString("-srcnodata");
    args.AddString("10+5i");
    args.AddString("-dstnodata");
    args.AddString("none 20");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 2: Destination nodata clamping on GDT_Byte (e.g. -dstnodata 300)
  {
    const std::string dst_clamp = test_dir + "dst_clamp.tif";
    CPLStringList args;
    args.AddString("-dstnodata");
    args.AddString("300");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_clamp.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 3: Updating an existing dataset that has destination nodata set
  {
    const std::string dst_update = test_dir + "dst_update.tif";
    auto update_ds = CreateRaster(dst_update, 20, 20, 2);
    ASSERT_NE(nullptr, update_ds);
    EXPECT_EQ(CE_None, update_ds->GetRasterBand(1)->SetNoDataValue(42.0));
    EXPECT_EQ(CE_None, update_ds->GetRasterBand(2)->SetNoDataValue(42.0));
    GDALDatasetH hDstDS = update_ds.get();

    CPLStringList args;
    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hRet =
        GDALWarp(nullptr, hDstDS, 1, &hSrcRaw, options.get(), nullptr);
    EXPECT_EQ(hDstDS, hRet);
  }

  // Test 4: Copying nodata from source to destination without -dstnodata
  {
    const std::string src_with_nodata = test_dir + "src_nd.tif";
    const std::string dst_copied_nd = test_dir + "dst_copied_nd.tif";
    auto nd_ds = CreateRaster(src_with_nodata, 20, 20, 1);
    ASSERT_NE(nullptr, nd_ds);
    EXPECT_EQ(CE_None, nd_ds->GetRasterBand(1)->SetNoDataValue(77.0));
    nd_ds.reset();

    GDALDatasetH hSrcND = GDALOpen(src_with_nodata.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hSrcND);
    GDALDatasetUniquePtr src_guard(reinterpret_cast<GDALDataset*>(hSrcND));

    CPLStringList args;
    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src_guard.get();
    GDALDatasetH hDst = GDALWarp(dst_copied_nd.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, IndirectMaterializeTempGTiff) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/indirect_gtiff/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src1_path = test_dir + "src1.tif";
  const std::string src2_path = test_dir + "src2.tif";
  const std::string dst_png = test_dir + "dst.png";

  double gt1[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  double gt2[6] = {11.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
  auto ds2 = CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt2);
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  ds1.reset();
  ds2.reset();

  GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
  GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc1);
  ASSERT_NE(nullptr, hSrc2);
  GDALDatasetUniquePtr guard1(reinterpret_cast<GDALDataset*>(hSrc1));
  GDALDatasetUniquePtr guard2(reinterpret_cast<GDALDataset*>(hSrc2));

  GDALDatasetH pahSrc[2] = {guard1.get(), guard2.get()};

  // -of PNG with 2 sources and -t_srs EPSG:3857 forces temporary GTiff
  // materialization
  CPLStringList args;
  args.AddString("-of");
  args.AddString("PNG");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hDst =
      GDALWarp(dst_png.c_str(), nullptr, 2, pahSrc, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_png.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, CanUseBuildVRTOverlappingExtents) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/overlap_vrt/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src1_path = test_dir + "src1.tif";
  const std::string src2_path = test_dir + "src2.tif";
  const std::string dst_png = test_dir + "dst.png";

  // Overlapping extents with alpha band
  double gt1[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  double gt2[6] = {10.5, 0.1, 0.0, 50.0, 0.0, -0.1};
  auto ds1 = CreateRaster(src1_path, 20, 20, 2, GDT_Byte, "EPSG:4326", gt1);
  auto ds2 = CreateRaster(src2_path, 20, 20, 2, GDT_Byte, "EPSG:4326", gt2);
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  EXPECT_EQ(CE_None,
            ds1->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand));
  EXPECT_EQ(CE_None,
            ds2->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand));
  ds1.reset();
  ds2.reset();

  GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
  GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc1);
  ASSERT_NE(nullptr, hSrc2);
  GDALDatasetUniquePtr guard1(reinterpret_cast<GDALDataset*>(hSrc1));
  GDALDatasetUniquePtr guard2(reinterpret_cast<GDALDataset*>(hSrc2));

  GDALDatasetH pahSrc[2] = {guard1.get(), guard2.get()};

  // -of PNG without -t_srs checks CanUseBuildVRT, which detects overlap and
  // returns false
  CPLStringList args;
  args.AddString("-of");
  args.AddString("PNG");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hDst =
      GDALWarp(dst_png.c_str(), nullptr, 2, pahSrc, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_png.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, SourceAlphaAndMetadataCopying) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/alpha_md/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src_rgba.tif";
  const std::string dst_path = test_dir + "dst_rgba.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 4);
  ASSERT_NE(nullptr, src_ds);
  EXPECT_EQ(CE_None,
            src_ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand));
  src_ds->GetRasterBand(1)->SetDescription("MyBand1");
  EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetUnitType("degC"));

  CPLStringList band_md;
  band_md.AddString("CUSTOM_KEY=VALUE");
  band_md.AddString("STATISTICS_MINIMUM=10");
  EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetMetadata(band_md.List()));

  CPLStringList isis3_md;
  isis3_md.AddString("{\"key\": \"val\"}");
  EXPECT_EQ(CE_None, src_ds->SetMetadata(isis3_md.List(), "json:ISIS3"));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test 1: Warp with source alpha, json:ISIS3 metadata, and -wo
  // COPY_BAND_INFO=YES
  {
    CPLStringList args;
    args.AddString("-wo");
    args.AddString("COPY_BAND_INFO=YES");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr dst(reinterpret_cast<GDALDataset*>(hDst));

    EXPECT_STREQ("MyBand1", dst->GetRasterBand(1)->GetDescription());
    EXPECT_STREQ("degC", dst->GetRasterBand(1)->GetUnitType());
    EXPECT_STREQ("VALUE", dst->GetRasterBand(1)->GetMetadataItem("CUSTOM_KEY"));
    EXPECT_EQ(nullptr,
              dst->GetRasterBand(1)->GetMetadataItem("STATISTICS_MINIMUM"));
    EXPECT_NE(nullptr, dst->GetMetadata("json:ISIS3"));
  }

  // Test 2: Updating existing dataset with alpha band
  {
    const std::string dst_update = test_dir + "dst_update_alpha.tif";
    auto update_ds = CreateRaster(dst_update, 20, 20, 4);
    ASSERT_NE(nullptr, update_ds);
    EXPECT_EQ(CE_None, update_ds->GetRasterBand(4)->SetColorInterpretation(
                           GCI_AlphaBand));
    GDALDatasetH hDstDS = update_ds.get();

    CPLStringList args;
    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hRet =
        GDALWarp(nullptr, hDstDS, 1, &hSrcRaw, options.get(), nullptr);
    EXPECT_EQ(hDstDS, hRet);
  }

  // Test 3: Invalid -srcband
  {
    const std::string dst_err1 = test_dir + "err1.tif";
    CPLStringList args;
    args.AddString("-srcband");
    args.AddString("99");
    args.AddString("-dstband");
    args.AddString("1");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_err1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // Test 4: Invalid -dstband
  {
    const std::string dst_err2 = test_dir + "err2.tif";
    CPLStringList args;
    args.AddString("-srcband");
    args.AddString("1");
    args.AddString("-dstband");
    args.AddString("99");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_err2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }
}

TEST_F(GdalwarpLibTest, DemoteTo2DHorizontalSRS) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/demote2d/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src_3d.tif";
  const std::string dst_path = test_dir + "dst_2d.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4979");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-t_srs");
  args.AddString("EPSG:4326");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, AppOptionsSetters) {
  GDALWarpAppOptionsUniquePtr options(GDALWarpAppOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);

  GDALWarpAppOptionsSetQuiet(options.get(), 1);
  GDALWarpAppOptionsSetWarpOption(options.get(), "SAMPLE_KEY", "SAMPLE_VAL");

  auto progress_func = [](double dfComplete, const char* pszMessage,
                          void* pProgressArg) -> int {
    if (pProgressArg) {
      *static_cast<int*>(pProgressArg) = 1;
    }
    return 1;
  };
  int progress_called = 0;
  GDALWarpAppOptionsSetProgress(options.get(), progress_func, &progress_called);

  const std::string usage = GDALWarpAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
}

TEST_F(GdalwarpLibTest, CutlineReprojectionAndDensification) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_densify/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";
  const std::string cutline_shp = test_dir + "cutline_mp.shp";

  auto src_ds = CreateRaster(src_path, 100, 100, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_drv);
  GDALDatasetUniquePtr shp_ds(
      shp_drv->Create(cutline_shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, shp_ds);

  OGRSpatialReference srs3857;
  srs3857.SetFromUserInput("EPSG:3857");
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      shp_ds->CreateLayer("cutline_mp", &srs3857, wkbMultiPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  auto mp = std::make_unique<OGRMultiPolygon>();
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(1135460, 6140000);
  ring->addPoint(1202240, 6140000);
  ring->addPoint(1202240, 6241900);
  ring->addPoint(1135460, 6241900);
  ring->addPoint(1135460, 6140000);
  poly->addRingDirectly(ring.release());
  mp->addGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometry(mp.get()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  shp_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-cutline");
  args.AddString(cutline_shp.c_str());
  args.AddString("-crop_to_cutline");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, TargetExtentSRSVariants) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/te_srs_var/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string src_nosrs = test_dir + "src_nosrs.tif";
  const std::string dst_path1 = test_dir + "dst1.tif";
  const std::string dst_path2 = test_dir + "dst2.tif";

  auto ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  auto ds_nosrs = CreateRaster(src_nosrs, 20, 20, 1, GDT_Byte, nullptr);
  ASSERT_NE(nullptr, ds);
  ASSERT_NE(nullptr, ds_nosrs);
  ds.reset();
  ds_nosrs.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  GDALDatasetH hSrcNoSRS = GDALOpen(src_nosrs.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  ASSERT_NE(nullptr, hSrcNoSRS);
  GDALDatasetUniquePtr guard_src(reinterpret_cast<GDALDataset*>(hSrc));
  GDALDatasetUniquePtr guard_nosrs(reinterpret_cast<GDALDataset*>(hSrcNoSRS));

  // Test 1: -te_srs with -s_srs without -t_srs
  {
    CPLStringList args;
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    args.AddString("-s_srs");
    args.AddString("EPSG:4326");
    args.AddString("-te");
    args.AddString("10.0");
    args.AddString("48.0");
    args.AddString("12.0");
    args.AddString("50.0");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = guard_src.get();
    GDALDatasetH hDst = GDALWarp(dst_path1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 2: -te_srs without -s_srs or -t_srs (source has SRS)
  {
    CPLStringList args;
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    args.AddString("-te");
    args.AddString("10.0");
    args.AddString("48.0");
    args.AddString("12.0");
    args.AddString("50.0");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = guard_src.get();
    GDALDatasetH hDst = GDALWarp(dst_path2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 3: -te_srs with no projection anywhere -> fails
  {
    CPLStringList args;
    args.AddString("-te_srs");
    args.AddString("EPSG:4326");
    args.AddString("-te");
    args.AddString("10.0");
    args.AddString("48.0");
    args.AddString("12.0");
    args.AddString("50.0");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = guard_nosrs.get();
    GDALDatasetH hDst = GDALWarp(dst_path2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }
}

TEST_F(GdalwarpLibTest, CutlineCoordinateOperationAndSRS) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_ct/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string cutline_shp = test_dir + "cutline.shp";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_drv);
  GDALDatasetUniquePtr shp_ds(
      shp_drv->Create(cutline_shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, shp_ds);

  OGRSpatialReference srs3857;
  srs3857.SetFromUserInput("EPSG:3857");
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRLayer* layer =
      shp_ds->CreateLayer("cutline", &srs3857, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(1135460, 6140000);
  ring->addPoint(1202240, 6140000);
  ring->addPoint(1202240, 6241900);
  ring->addPoint(1135460, 6241900);
  ring->addPoint(1135460, 6140000);
  poly->addRingDirectly(ring.release());
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometry(poly.get()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  shp_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test with -ct and matching cutline & target SRS
  CPLStringList args;
  args.AddString("-cutline");
  args.AddString(cutline_shp.c_str());
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");
  args.AddString("-ct");
  args.AddString(
      "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad +step "
      "+proj=webmerc");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, DestinationDatasetInsufficientBands) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/insuf_bands/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src2.tif";
  const std::string dst_path = test_dir + "dst1.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 2);
  auto dst_ds = CreateRaster(dst_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));
  GDALDatasetH hDst = dst_ds.get();

  CPLStringList args;
  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  CPLErrorReset();
  WithQuietHandler quiet;
  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hRet =
      GDALWarp(nullptr, hDst, 1, &hSrcRaw, options.get(), nullptr);

  EXPECT_EQ(nullptr, hRet);
}

TEST_F(GdalwarpLibTest, RotatedRasterBlankBorderDetection) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/rotated_blank/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src_rot.tif";
  const std::string dst_path = test_dir + "dst_rot.tif";

  double gt[6] = {10.0, 0.08, 0.04, 50.0, -0.04, -0.08};
  auto src_ds = CreateRaster(src_path, 40, 40, 1, GDT_Byte, "EPSG:4326", gt);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-tap");
  args.AddString("-tr");
  args.AddString("0.05");
  args.AddString("0.05");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, OverwritingExistingDestinationFile) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/overwrite/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  auto dst_ds = CreateRaster(dst_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);
  src_ds.reset();
  dst_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-overwrite");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, MoreOptionsValidationVariants) {
  // 1: -refine_gcps missing arg
  {
    CPLStringList args;
    args.AddString("-refine_gcps");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));

    EXPECT_EQ(nullptr, opts);
  }

  // 2: -refine_gcps with negative tolerance
  {
    CPLStringList args;
    args.AddString("-refine_gcps");
    args.AddString("-1.5");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));

    EXPECT_EQ(nullptr, opts);
  }

  // 3: -refine_gcps with tolerance and minimum GCPs
  {
    CPLStringList args;
    args.AddString("-refine_gcps");
    args.AddString("5.0");
    args.AddString("10");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);
  }

  // 4: -tr missing args
  {
    CPLStringList args;
    args.AddString("-tr");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));

    EXPECT_EQ(nullptr, opts);
  }

  // 5: -tr 0 0
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("0");
    args.AddString("0");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));

    EXPECT_EQ(nullptr, opts);
  }

  // 6: -tr square
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("square");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);
  }

  // 7: -dstband without -srcband
  {
    CPLStringList args;
    args.AddString("-dstband");
    args.AddString("1");
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));

    EXPECT_EQ(nullptr, opts);
  }

  // 8: -srcband without -dstband (auto-populates dstband)
  {
    CPLStringList args;
    args.AddString("-srcband");
    args.AddString("1");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);
  }

  // 9: negative srcnodata and dstnodata
  {
    CPLStringList args;
    args.AddString("-srcnodata");
    args.AddString("-9999");
    args.AddString("-dstnodata");
    args.AddString("-9999");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);
  }
}

TEST_F(GdalwarpLibTest, ThematicRatHandling) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/thematic_rat/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src_rat.tif";
  const std::string dst_path1 = test_dir + "dst_bilinear.tif";
  const std::string dst_path2 = test_dir + "dst_near.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->SetTableType(GRTT_THEMATIC);
  EXPECT_EQ(CE_None, rat->CreateColumn("Val", GFT_Integer, GFU_Generic));
  rat->SetRowCount(2);
  rat->SetValue(0, 0, 1);
  rat->SetValue(1, 0, 2);
  EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetDefaultRAT(rat.get()));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Test 1: Resampling bilinear with thematic RAT (warning and discarded)
  {
    CPLStringList args;
    args.AddString("-r");
    args.AddString("bilinear");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path1.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Test 2: Resampling near with thematic RAT (copied)
  {
    CPLStringList args;
    args.AddString("-r");
    args.AddString("near");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path2.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, TooLargeRasterSizeErrors) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/too_large/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // 1: -tr with extremely small resolution causing raster dimensions > INT_MAX
  {
    CPLStringList args;
    args.AddString("-tr");
    args.AddString("1e-15");
    args.AddString("1e-15");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 2: -ts 100 0 with tiny X extent causing lines > INT_MAX (hits line 4771)
  {
    CPLStringList args;
    args.AddString("-ts");
    args.AddString("100");
    args.AddString("0");
    args.AddString("-te");
    args.AddString("0");
    args.AddString("0");
    args.AddString("1e-15");
    args.AddString("100");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 3: -ts 0 100 with tiny Y extent causing pixels > INT_MAX (hits line 4802)
  {
    CPLStringList args;
    args.AddString("-ts");
    args.AddString("0");
    args.AddString("100");
    args.AddString("-te");
    args.AddString("0");
    args.AddString("0");
    args.AddString("100");
    args.AddString("1e-15");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 4: -te with tiny resolution causing pixels/lines > INT_MAX (hits line 4819)
  {
    CPLStringList args;
    args.AddString("-te");
    args.AddString("0");
    args.AddString("0");
    args.AddString("100");
    args.AddString("100");
    args.AddString("-tr");
    args.AddString("1e-15");
    args.AddString("1e-15");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }
}

TEST_F(GdalwarpLibTest, ApproximateTransformerBranch) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/approx_trans/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 30, 30, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-et");
  args.AddString("0.125");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, CutlineSRSOption) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_srs/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-cutline");
  args.AddString(
      "POLYGON ((10.2 48.2, 10.8 48.2, 10.8 48.8, 10.2 48.8, 10.2 48.2))");
  args.AddString("-cutline_srs");
  args.AddString("EPSG:4326");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, CogWithExtentAndResampling) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cog_extent/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 32, 32, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("COG");
  args.AddString("-te");
  args.AddString("10.0");
  args.AddString("48.0");
  args.AddString("12.0");
  args.AddString("50.0");
  args.AddString("-co");
  args.AddString("RESAMPLING=BILINEAR");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, MetadataCopyingAndParserUsage) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/metadata_copy/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);

  char* papszMD[] = {const_cast<char*>("CUSTOM_KEY=CUSTOM_VALUE"), nullptr};
  EXPECT_EQ(CE_None, src_ds->SetMetadata(papszMD, nullptr));
  char* papszISIS3[] = {
      const_cast<char*>(
          "{\"IsisCube\": {\"Core\": {\"Dimensions\": {\"Samples\": 20, "
          "\"Lines\": 20}}}}"),
      nullptr};
  EXPECT_EQ(CE_None, src_ds->SetMetadata(papszISIS3, "json:ISIS3"));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("GTiff");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetUniquePtr dst(GDALDataset::FromHandle(GDALWarp(
      dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr)));
  ASSERT_NE(nullptr, dst);

  const char* pszVal = GDALGetMetadataItem(dst.get(), "CUSTOM_KEY", nullptr);
  ASSERT_NE(nullptr, pszVal);
  EXPECT_STREQ("CUSTOM_VALUE", pszVal);
  char** papszDstISIS3 = GDALGetMetadata(dst.get(), "json:ISIS3");
  EXPECT_NE(nullptr, papszDstISIS3);

  // Check GDALWarpAppGetParserUsage
  std::string usage = GDALWarpAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
}

TEST_F(GdalwarpLibTest, IndirectNodataAndComplexHandling) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/indirect_nodata/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  CPLSetConfigOption("CPL_DEBUG", "ON");
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    CPLSetConfigOption("CPL_DEBUG", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src1_path = test_dir + "src1.tif";
  const std::string src2_path = test_dir + "src2.tif";
  const std::string dst_png = test_dir + "dst.png";

  double gt1[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  double gt2[6] = {11.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
  auto ds2 = CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt2);
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  EXPECT_EQ(CE_None, ds1->GetRasterBand(1)->SetNoDataValue(42.0));
  EXPECT_EQ(CE_None, ds2->GetRasterBand(1)->SetNoDataValue(42.0));
  ds1.reset();
  ds2.reset();

  GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
  GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc1);
  ASSERT_NE(nullptr, hSrc2);
  GDALDatasetUniquePtr guard1(reinterpret_cast<GDALDataset*>(hSrc1));
  GDALDatasetUniquePtr guard2(reinterpret_cast<GDALDataset*>(hSrc2));

  GDALDatasetH pahSrc[2] = {guard1.get(), guard2.get()};

  // 1: Invalid srcnodata parsing error
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    args.AddString("-srcnodata");
    args.AddString("invalid_number");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 1, pahSrc, options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 2: Invalid dstnodata parsing error
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    args.AddString("-dstnodata");
    args.AddString("invalid_number");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 1, pahSrc, options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 3: Dstnodata rounded to integer
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    args.AddString("-dstnodata");
    args.AddString("42.6");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 1, pahSrc, options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 4: Two sources copying nodata to PNG (exercises indirect nodata copying)
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pahSrc, options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, OverviewLevelOutOfRange) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/ovr_out_of_range/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 40, 40, 1);
  ASSERT_NE(nullptr, src_ds);
  int nOverviews[1] = {2};
  EXPECT_EQ(CE_None,
            src_ds->BuildOverviews("NEAREST", 1, nOverviews, 0, nullptr,
                                   nullptr, nullptr, nullptr));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-ovr");
  args.AddString("5");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);
}

TEST_F(GdalwarpLibTest, CutlineErrorsAndMultiPolygon) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_errors/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // 1: Non-existent cutline datasource
  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString("/vsimem/nonexistent.shp");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 2: Vector cutline with MultiPolygon geometry
  const std::string cutline_shp = test_dir + "cutline.shp";
  {
    GDALDriver* shp_drv =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_NE(nullptr, shp_drv);

    GDALDatasetUniquePtr ds(
        shp_drv->Create(cutline_shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRLayer* layer =
        ds->CreateLayer("cutline", &srs, wkbMultiPolygon, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(10.2, 48.2);
    ring->addPoint(10.8, 48.2);
    ring->addPoint(10.8, 48.8);
    ring->addPoint(10.2, 48.8);
    ring->addPoint(10.2, 48.2);
    poly->addRingDirectly(ring.release());
    mp->addGeometryDirectly(poly.release());

    auto poly2 = std::make_unique<OGRPolygon>();
    auto ring2 = std::make_unique<OGRLinearRing>();
    ring2->addPoint(11.2, 48.2);
    ring2->addPoint(11.8, 48.2);
    ring2->addPoint(11.8, 48.8);
    ring2->addPoint(11.2, 48.8);
    ring2->addPoint(11.2, 48.2);
    poly2->addRingDirectly(ring2.release());
    mp->addGeometryDirectly(poly2.release());

    EXPECT_EQ(OGRERR_NONE, feat->SetGeometry(mp.get()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
    ds.reset();
  }

  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(cutline_shp.c_str());

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 3: -cwhere resulting in 0 features
  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(cutline_shp.c_str());
    args.AddString("-cwhere");
    args.AddString("1=0");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }
}

TEST_F(GdalwarpLibTest, CutlineSRSFromShapefileAndIgnoreBadCutline) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_shp_srs/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // 1: Cutline shapefile with -cutline_srs EPSG:4326 (hits lines 3547-3552)
  const std::string cutline_shp = test_dir + "cutline.shp";
  {
    GDALDriver* shp_drv =
        GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    ASSERT_NE(nullptr, shp_drv);

    GDALDatasetUniquePtr ds(
        shp_drv->Create(cutline_shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);

    OGRLayer* layer = ds->CreateLayer("cutline", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(10.2, 48.2);
    ring->addPoint(10.8, 48.2);
    ring->addPoint(10.8, 48.8);
    ring->addPoint(10.2, 48.8);
    ring->addPoint(10.2, 48.2);
    poly->addRingDirectly(ring.release());
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometry(poly.get()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
    ds.reset();
  }

  {
    CPLStringList args;
    args.AddString("-cutline");
    args.AddString(cutline_shp.c_str());
    args.AddString("-cutline_srs");
    args.AddString("EPSG:4326");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 2: GDALWARP_IGNORE_BAD_CUTLINE (hits lines 5641-5645)
  {
    CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", "YES");
    absl::Cleanup config_cleanup = [] {
      CPLSetConfigOption("GDALWARP_IGNORE_BAD_CUTLINE", nullptr);
    };

    CPLStringList args;
    args.AddString("-cutline");
    args.AddString("POLYGON ((0 89.9, 10 89.9, 10 90.0, 0 90.0, 0 89.9))");
    args.AddString("-cutline_srs");
    args.AddString("EPSG:4326");
    args.AddString("-t_srs");
    args.AddString("EPSG:3857");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp((test_dir + "dst_bad_cutline.tif").c_str(),
                                 nullptr, 1, &hSrcRaw, options.get(), nullptr);

    if (hDst) {
      GDALClose(hDst);
    }
  }
}

TEST_F(GdalwarpLibTest, GeolocationArrayMetadataAndWarp) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/geoloc/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string geoloc_path = test_dir + "geoloc.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  auto geoloc_ds =
      CreateRaster(geoloc_path, 20, 20, 2, GDT_Float32, "EPSG:4326");
  ASSERT_NE(nullptr, geoloc_ds);
  std::vector<float> x_coords(400);
  std::vector<float> y_coords(400);
  for (int y = 0; y < 20; ++y) {
    for (int x = 0; x < 20; ++x) {
      x_coords[y * 20 + x] = 10.0f + x * 0.1f;
      y_coords[y * 20 + x] = 50.0f - y * 0.1f;
    }
  }
  EXPECT_EQ(CE_None, geoloc_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 20, 20, x_coords.data(), 20, 20,
                         GDT_Float32, 0, 0, nullptr));
  EXPECT_EQ(CE_None, geoloc_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, 20, 20, y_coords.data(), 20, 20,
                         GDT_Float32, 0, 0, nullptr));
  geoloc_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-to");
  args.AddString(("GEOLOC_ARRAY=" + geoloc_path).c_str());
  args.AddString("-t_srs");
  args.AddString("EPSG:4326");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, ApproximateTransformerIndirectAndEpoch) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/approx_indirect/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.png";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("PNG");
  args.AddString("-et");
  args.AddString("0.125");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");
  args.AddString("-to");
  args.AddString("DST_METHOD=NO_GEOTRANSFORM");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, SourceImplicitAlphaPreserved) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/implicit_alpha/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 2, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  EXPECT_EQ(CE_None,
            src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("GTiff");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  EXPECT_EQ(2, GDALGetRasterCount(hDst));
  EXPECT_EQ(GCI_AlphaBand,
            GDALGetRasterColorInterpretation(GDALGetRasterBand(hDst, 2)));
  GDALClose(hDst);
}

TEST_F(GdalwarpLibTest, DirectNodataErrorsAndZeroBandDataset) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/direct_errors/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // 1: Direct mode SetupNoData failure (hits lines 3165-3171)
  {
    CPLStringList args;
    args.AddString("-srcnodata");
    args.AddString("invalid_number");

    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hSrcRaw = src.get();
    GDALDatasetH hDst = GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw,
                                 options.get(), nullptr);

    EXPECT_EQ(nullptr, hDst);
  }

  // 2: 0-band source dataset in update mode (hits lines 2700-2706)
  {
    auto dst_ds = CreateRaster(dst_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
    ASSERT_NE(nullptr, dst_ds);
    dst_ds.reset();

    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr zero_band_ds(
        mem_drv->Create("", 10, 10, 0, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, zero_band_ds);

    GDALDatasetH hDstDS = GDALOpen(dst_path.c_str(), GA_Update);
    ASSERT_NE(nullptr, hDstDS);

    CPLStringList args;
    GDALWarpAppOptionsUniquePtr options(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hZeroSrc = zero_band_ds.get();
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hResult = GDALWarp(dst_path.c_str(), hDstDS, 1, &hZeroSrc,
                                    options.get(), nullptr);

    EXPECT_EQ(nullptr, hResult);
  }
}

TEST_F(GdalwarpLibTest, RotatedRasterFourSidedBlankBorderDetection) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/rot_four_sided/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src_rot.tif";
  const std::string dst_path = test_dir + "dst_rot.tif";

  double gt[6] = {10.03, 0.08, 0.04, 50.07, -0.04, -0.08};
  auto src_ds = CreateRaster(src_path, 40, 40, 1, GDT_Byte, "EPSG:4326", gt);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-tap");
  args.AddString("-tr");
  args.AddString("0.05");
  args.AddString("0.05");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST_F(GdalwarpLibTest, OverwriteWithAuxFileUnlink) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/overwrite_unlink/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  auto dst_ds = CreateRaster(dst_path, 20, 20, 1);
  ASSERT_NE(nullptr, dst_ds);
  const std::string aux_path = dst_path + ".aux.xml";
  VSILFILE* fp = VSIFOpenL(aux_path.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  const char* xml = "<PAMDataset/>";
  VSIFWriteL(xml, 1, strlen(xml), fp);
  VSIFCloseL(fp);
  dst_ds.reset();

  // Create a source VRT referencing dst.tif
  const std::string vrt_path = test_dir + "src.vrt";
  {
    CPLStringList vrt_args;
    vrt_args.AddString("-of");
    vrt_args.AddString("VRT");
    GDALWarpAppOptionsUniquePtr vrt_opts(
        GDALWarpAppOptionsNew(vrt_args.List(), nullptr));
    ASSERT_NE(nullptr, vrt_opts);

    GDALDatasetH hDstOpened = GDALOpen(dst_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hDstOpened);
    GDALDatasetUniquePtr guard(reinterpret_cast<GDALDataset*>(hDstOpened));
    GDALDatasetH hDstOpenedRaw = guard.get();
    GDALDatasetH hVRT = GDALWarp(vrt_path.c_str(), nullptr, 1, &hDstOpenedRaw,
                                 vrt_opts.get(), nullptr);
    ASSERT_NE(nullptr, hVRT);
    GDALClose(hVRT);
  }

  // Warp from vrt_path to dst_path with -overwrite (hits lines 4436-4439)
  {
    GDALDatasetH hVrtDS = GDALOpen(vrt_path.c_str(), GA_ReadOnly);
    ASSERT_NE(nullptr, hVrtDS);
    GDALDatasetUniquePtr vrt_guard(reinterpret_cast<GDALDataset*>(hVrtDS));

    CPLStringList args;
    args.AddString("-overwrite");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);

    GDALDatasetH hVrtRaw = vrt_guard.get();
    GDALDatasetH hRes =
        GDALWarp(dst_path.c_str(), nullptr, 1, &hVrtRaw, opts.get(), nullptr);
    ASSERT_NE(nullptr, hRes);
    GDALClose(hRes);
  }
}

TEST_F(GdalwarpLibTest, CutlineTransformFailureDirectMode) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/cutline_fail_direct/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "EPSG:3857");
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  // Direct GTiff cutline transform failure without GDALWARP_IGNORE_BAD_CUTLINE
  // (hits lines 3254-3259)
  CPLStringList args;
  args.AddString("-cutline");
  args.AddString("POLYGON ((0 90, 10 90, 10 89.9, 0 89.9, 0 90))");
  args.AddString("-cutline_srs");
  args.AddString("EPSG:4326");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);
}

TEST_F(GdalwarpLibTest, UnrecognizedDriverOutputListing) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/unrecog_drv/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  auto src_ds = CreateRaster(src_path, 20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("COMPLETELY_UNRECOGNIZED_DRIVER_NAME");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  EXPECT_EQ(nullptr, hDst);
}

TEST_F(GdalwarpLibTest, CheckWithInvertProjExtentCalculation) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/invert_proj/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  double gt[6] = {-180.0, 9.0, 0.0, 89.0, 0.0, -4.45};
  auto src_ds = CreateRaster(src_path, 40, 40, 1, GDT_Byte, "EPSG:4326", gt);
  ASSERT_NE(nullptr, src_ds);
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  if (hDst) {
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, CanUseBuildVRTNegativeChecks) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/can_build_vrt_neg/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src1_path = test_dir + "src1.tif";
  const std::string src2_path = test_dir + "src2.tif";
  const std::string dst_png = test_dir + "dst.png";

  double gt1[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  double gt2_diff_res[6] = {12.0, 0.2, 0.0, 50.0, 0.0, -0.2};
  double gt2_rotated[6] = {12.0, 0.1, 0.05, 50.0, -0.05, -0.1};

  // Case 1: Different band count (hits lines 1082-1085)
  {
    auto ds1 = CreateRaster(src1_path, 20, 20, 2, GDT_Byte, "EPSG:4326", gt1);
    auto ds2 = CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    ds1.reset();
    ds2.reset();

    GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
    GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
    GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
    GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
    GDALDatasetH pah[2] = {g1.get(), g2.get()};

    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Case 2: One has SRS, one doesn't (hits lines 1087-1090)
  {
    auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    auto ds2 = CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "", gt1);
    ds1.reset();
    ds2.reset();

    GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
    GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
    GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
    GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
    GDALDatasetH pah[2] = {g1.get(), g2.get()};

    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Case 3: Different SRS (hits lines 1092-1095)
  {
    double gt2_3857[6] = {1113194.0, 10000.0, 0.0, 6446275.0, 0.0, -10000.0};
    auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    auto ds2 =
        CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:3857", gt2_3857);
    ds1.reset();
    ds2.reset();

    GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
    GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
    GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
    GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
    GDALDatasetH pah[2] = {g1.get(), g2.get()};

    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    args.AddString("-t_srs");
    args.AddString("EPSG:3857");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Case 4: Different resolution (hits lines 1097-1100)
  {
    auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    auto ds2 =
        CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt2_diff_res);
    ds1.reset();
    ds2.reset();

    GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
    GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
    GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
    GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
    GDALDatasetH pah[2] = {g1.get(), g2.get()};

    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // Case 5: Rotated geotransform (hits lines 1054-1057)
  {
    auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt1);
    auto ds2 =
        CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326", gt2_rotated);
    ds1.reset();
    ds2.reset();

    GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
    GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
    GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
    GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
    GDALDatasetH pah[2] = {g1.get(), g2.get()};

    CPLStringList args;
    args.AddString("-of");
    args.AddString("PNG");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    GDALDatasetH hDst =
        GDALWarp(dst_png.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, GeolocationMetadataDomainSRS) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/geoloc_md/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_path = test_dir + "dst.tif";

  // Create raster with GEOLOCATION domain metadata specifying SRS (hits lines
  // 408-412)
  auto src_ds = CreateRaster(src_path, 20, 20, 1, GDT_Byte, "");
  ASSERT_NE(nullptr, src_ds);
  char* papszMD[] = {const_cast<char*>("SRS=EPSG:4326"),
                     const_cast<char*>("X_DATASET=dummy.tif"),
                     const_cast<char*>("Y_DATASET=dummy.tif"), nullptr};
  EXPECT_EQ(CE_None, src_ds->SetMetadata(papszMD, "GEOLOCATION"));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-to");
  args.AddString("METHOD=GEOLOC_ARRAY");
  args.AddString("-t_srs");
  args.AddString("EPSG:3857");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  CPLErrorReset();
  WithQuietHandler quiet;
  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_path.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);

  if (hDst) {
    GDALClose(hDst);
  }
}

TEST_F(GdalwarpLibTest, VRTMultipleSourcesAndExistingDatasetWarnings) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/vrt_warnings/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  absl::Cleanup cleanup = [&test_dir] { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src1_path = test_dir + "src1.tif";
  const std::string src2_path = test_dir + "src2.tif";
  const std::string dst_vrt = test_dir + "dst.vrt";

  auto ds1 = CreateRaster(src1_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  auto ds2 = CreateRaster(src2_path, 20, 20, 1, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, ds1);
  ASSERT_NE(nullptr, ds2);
  ds1.reset();
  ds2.reset();

  GDALDatasetH hSrc1 = GDALOpen(src1_path.c_str(), GA_ReadOnly);
  GDALDatasetH hSrc2 = GDALOpen(src2_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc1);
  ASSERT_NE(nullptr, hSrc2);
  GDALDatasetUniquePtr g1(reinterpret_cast<GDALDataset*>(hSrc1));
  GDALDatasetUniquePtr g2(reinterpret_cast<GDALDataset*>(hSrc2));
  GDALDatasetH pah[2] = {g1.get(), g2.get()};

  // 1: -of VRT with 2 sources emits warning (hits lines 1523-1529)
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("VRT");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);

    GDALDatasetH hDst =
        GDALWarp(dst_vrt.c_str(), nullptr, 2, pah, opts.get(), nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 2: -of VRT with existing dataset emits error (hits lines 1514-1516)
  {
    CPLStringList args;
    args.AddString("-of");
    args.AddString("VRT");
    GDALWarpAppOptionsUniquePtr opts(
        GDALWarpAppOptionsNew(args.List(), nullptr));
    ASSERT_NE(nullptr, opts);

    GDALDatasetH hDstOpened = GDALOpen(src1_path.c_str(), GA_Update);
    ASSERT_NE(nullptr, hDstOpened);

    CPLErrorReset();
    WithQuietHandler quiet;
    GDALDatasetH hRes =
        GDALWarp(dst_vrt.c_str(), hDstOpened, 1, pah, opts.get(), nullptr);

    EXPECT_EQ(nullptr, hRes);
    GDALClose(hDstOpened);
  }
}

TEST_F(GdalwarpLibTest, FilteredMetadataAndImplicitAlphaIndirect) {
  const std::string test_dir = "/vsimem/gdalwarp_lib_test/filtered_md/";
  VSIStatBufL sStat;
  if (VSIStatL(test_dir.c_str(), &sStat) != 0) {
    VSIMkdirRecursive(test_dir.c_str(), 0755);
  }
  const std::string tmp_dir = test_dir + "tmp";
  VSIMkdirRecursive(tmp_dir.c_str(), 0755);
  CPLSetConfigOption("CPL_TMPDIR", tmp_dir.c_str());
  absl::Cleanup cleanup = [&test_dir] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

  const std::string src_path = test_dir + "src.tif";
  const std::string dst_png = test_dir + "dst.png";

  // 2 bands, band 2 is Alpha (hits lines 1738-1739 in GDALWarpIndirect when
  // !bQuiet)
  auto src_ds = CreateRaster(src_path, 20, 20, 2, GDT_Byte, "EPSG:4326");
  ASSERT_NE(nullptr, src_ds);
  EXPECT_EQ(CE_None,
            src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand));
  char* papszMD[] = {const_cast<char*>("NODATA_VALUES=255 255"),
                     const_cast<char*>("CACHE_PATH=/tmp/wms_cache"),
                     const_cast<char*>("OTHER=VALUE"), nullptr};
  EXPECT_EQ(CE_None, src_ds->SetMetadata(papszMD, nullptr));
  src_ds.reset();

  GDALDatasetH hSrc = GDALOpen(src_path.c_str(), GA_ReadOnly);
  ASSERT_NE(nullptr, hSrc);
  GDALDatasetUniquePtr src(reinterpret_cast<GDALDataset*>(hSrc));

  CPLStringList args;
  args.AddString("-of");
  args.AddString("PNG");
  args.AddString("-dstalpha");

  GDALWarpAppOptionsUniquePtr options(
      GDALWarpAppOptionsNew(args.List(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrcRaw = src.get();
  GDALDatasetH hDst =
      GDALWarp(dst_png.c_str(), nullptr, 1, &hSrcRaw, options.get(), nullptr);
  ASSERT_NE(nullptr, hDst);
  EXPECT_EQ(nullptr, GDALGetMetadataItem(hDst, "NODATA_VALUES", nullptr));
  EXPECT_EQ(nullptr, GDALGetMetadataItem(hDst, "CACHE_PATH", nullptr));
  EXPECT_STREQ("VALUE", GDALGetMetadataItem(hDst, "OTHER", nullptr));
  GDALClose(hDst);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalwarp_lib.cpp.
