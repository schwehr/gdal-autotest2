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
// Tests for GDALTranslate (apps/gdal_translate_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdal_translate_lib.cpp
//   https://gdal.org/programs/gdal_translate.html

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct TranslateOptionsDeleter {
  void operator()(GDALTranslateOptions* psOptions) const {
    GDALTranslateOptionsFree(psOptions);
  }
};
using TranslateOptionsUniquePtr =
    std::unique_ptr<GDALTranslateOptions, TranslateOptionsDeleter>;

TranslateOptionsUniquePtr ParseOptions(
    const std::vector<std::string>& args,
    GDALTranslateOptionsForBinary* psOptionsForBinary = nullptr) {
  CPLStringList csl;
  for (const auto& arg : args) {
    csl.AddString(arg.c_str());
  }
  return TranslateOptionsUniquePtr(
      GDALTranslateOptionsNew(csl.List(), psOptionsForBinary));
}

GDALDatasetUniquePtr RunTranslate(const char* dest, GDALDataset* src,
                                  const GDALTranslateOptions* options = nullptr,
                                  int* usage_error = nullptr) {
  GDALDatasetH hOut =
      GDALTranslate(dest, GDALDataset::ToHandle(src), options, usage_error);
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(hOut));
}

GDALDatasetUniquePtr CreateTestDataset(const char* path, int xsize = 20,
                                       int ysize = 20, int band_count = 1,
                                       GDALDataType dt = GDT_Byte,
                                       bool set_gt = true,
                                       bool set_srs = true) {
  GDALDriver* driver = nullptr;
  if (path == nullptr || path[0] == '\0') {
    driver = GetGDALDriverManager()->GetDriverByName("MEM");
  } else {
    driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  }
  if (!driver) return nullptr;
  GDALDatasetUniquePtr ds(
      driver->Create(path ? path : "", xsize, ysize, band_count, dt, nullptr));
  if (!ds) return nullptr;
  if (set_gt) {
    const double gt[6] = {100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
    ds->SetGeoTransform(const_cast<double*>(gt));
  }
  if (set_srs) {
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:4326");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ds->SetSpatialRef(&srs);
  }
  for (int b = 1; b <= band_count; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    std::vector<uint8_t> buffer(xsize * ysize);
    for (size_t i = 0; i < buffer.size(); ++i) {
      buffer[i] = static_cast<uint8_t>((i + b * 10) % 256);
    }
    if (dt == GDT_Byte) {
      CPL_IGNORE_RET_VAL(band->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                        buffer.data(), xsize, ysize, GDT_Byte,
                                        0, 0, nullptr));
    }
  }
  return ds;
}

class GdalTranslateLibTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_MEM();
    GDALRegister_VRT();
    GDALRegister_GTiff();
  }

  void SetUp() override {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test");
  }
  void TearDown() override {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test");
  }
};

// -----------------------------------------------------------------------------
// Options parsing tests
// -----------------------------------------------------------------------------

TEST_F(GdalTranslateLibTest, OptionsDefaultAndFreeNull) {
  TranslateOptionsUniquePtr opts(GDALTranslateOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, opts);
  GDALTranslateOptionsFree(nullptr);
}

TEST_F(GdalTranslateLibTest, OptionsBasicFlags) {
  auto opts = ParseOptions({"-ot", "Int16", "-of", "GTiff", "-q", "-strict"});
  ASSERT_NE(nullptr, opts);

  auto opts2 = ParseOptions({"-not_strict"});
  ASSERT_NE(nullptr, opts2);
}

TEST_F(GdalTranslateLibTest, OptionsBands) {
  auto opts =
      ParseOptions({"-b", "1", "-b", "2", "-b", "mask", "-b", "mask,1"});
  ASSERT_NE(nullptr, opts);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-b", "0"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-b", "invalid"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsMask) {
  auto opts_none = ParseOptions({"-mask", "none"});
  ASSERT_NE(nullptr, opts_none);

  auto opts_auto = ParseOptions({"-mask", "auto"});
  ASSERT_NE(nullptr, opts_auto);

  auto opts_1 = ParseOptions({"-mask", "1"});
  ASSERT_NE(nullptr, opts_1);

  auto opts_mask = ParseOptions({"-mask", "mask"});
  ASSERT_NE(nullptr, opts_mask);

  auto opts_mask2 = ParseOptions({"-mask", "mask,2"});
  ASSERT_NE(nullptr, opts_mask2);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-mask", "0"});
    EXPECT_EQ(nullptr, bad);
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-mask", "foo"});
    EXPECT_EQ(nullptr, bad);
  }
}

TEST_F(GdalTranslateLibTest, OptionsExpand) {
  auto gray = ParseOptions({"-expand", "gray"});
  ASSERT_NE(nullptr, gray);

  auto rgb = ParseOptions({"-expand", "rgb"});
  ASSERT_NE(nullptr, rgb);

  auto rgba = ParseOptions({"-expand", "rgba"});
  ASSERT_NE(nullptr, rgba);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-expand", "cmyk"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsOutsizeAndTargetResolution) {
  auto pix = ParseOptions({"-outsize", "50", "40"});
  ASSERT_NE(nullptr, pix);

  auto pct = ParseOptions({"-outsize", "50%", "50%"});
  ASSERT_NE(nullptr, pct);

  auto aspect_x = ParseOptions({"-outsize", "0", "40"});
  ASSERT_NE(nullptr, aspect_x);

  auto aspect_y = ParseOptions({"-outsize", "50", "0"});
  ASSERT_NE(nullptr, aspect_y);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-outsize", "0", "0"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  auto tr = ParseOptions({"-tr", "1.5", "2.5"});
  ASSERT_NE(nullptr, tr);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad_tr = ParseOptions({"-tr", "0", "0"});
    EXPECT_EQ(nullptr, bad_tr);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsOverviewLevel) {
  auto auto_ovr = ParseOptions({"-ovr", "AUTO"});
  ASSERT_NE(nullptr, auto_ovr);

  auto auto_minus = ParseOptions({"-ovr", "AUTO-1"});
  ASSERT_NE(nullptr, auto_minus);

  auto none_ovr = ParseOptions({"-ovr", "NONE"});
  ASSERT_NE(nullptr, none_ovr);

  auto int_ovr = ParseOptions({"-ovr", "2"});
  ASSERT_NE(nullptr, int_ovr);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-ovr", "invalid"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsResampling) {
  auto r = ParseOptions({"-r", "bilinear"});
  ASSERT_NE(nullptr, r);
}

TEST_F(GdalTranslateLibTest, OptionsScale) {
  auto scale_default = ParseOptions({"-scale"});
  ASSERT_NE(nullptr, scale_default);

  auto scale_2 = ParseOptions({"-scale", "0", "100"});
  ASSERT_NE(nullptr, scale_2);

  auto scale_4 = ParseOptions({"-scale", "0", "100", "0", "255"});
  ASSERT_NE(nullptr, scale_4);

  auto scale_nan = ParseOptions({"-scale", "NaN", "NaN", "0", "255"});
  ASSERT_NE(nullptr, scale_nan);

  auto scale_band =
      ParseOptions({"-scale_1", "0", "100", "0", "255", "-scale_2"});
  ASSERT_NE(nullptr, scale_band);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto mix = ParseOptions({"-scale", "-scale_1"});
    EXPECT_EQ(nullptr, mix);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto mix = ParseOptions({"-scale_1", "-scale"});
    EXPECT_EQ(nullptr, mix);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-scale_0"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-scale_65536"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-scale", "0", "abc"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-scale", "0", "100", "0", "abc"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-scale", "-unscale"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsExponent) {
  auto exp = ParseOptions({"-exponent", "2.0"});
  ASSERT_NE(nullptr, exp);

  auto exp_band = ParseOptions({"-exponent_1", "1.5", "-exponent_2", "2.5"});
  ASSERT_NE(nullptr, exp_band);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto mix = ParseOptions({"-exponent", "2.0", "-exponent_1", "1.5"});
    EXPECT_EQ(nullptr, mix);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto mix = ParseOptions({"-exponent_1", "1.5", "-exponent", "2.0"});
    EXPECT_EQ(nullptr, mix);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-exponent_0", "1.0"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-exponent_65536", "1.0"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsSubwindow) {
  auto srcwin = ParseOptions({"-srcwin", "1", "2", "10", "20"});
  ASSERT_NE(nullptr, srcwin);

  auto projwin = ParseOptions(
      {"-projwin", "10", "20", "30", "40", "-projwin_srs", "EPSG:4326"});
  ASSERT_NE(nullptr, projwin);

  auto epo = ParseOptions({"-epo"});
  ASSERT_NE(nullptr, epo);

  auto eco = ParseOptions({"-eco"});
  ASSERT_NE(nullptr, eco);
}

TEST_F(GdalTranslateLibTest, OptionsGeoreferencing) {
  auto srs = ParseOptions({"-a_srs", "EPSG:32631", "-a_coord_epoch", "2020.5"});
  ASSERT_NE(nullptr, srs);

  auto srs_none = ParseOptions({"-a_srs", "none"});
  ASSERT_NE(nullptr, srs_none);

  auto srs_null = ParseOptions({"-a_srs", "null"});
  ASSERT_NE(nullptr, srs_null);

  auto ullr = ParseOptions({"-a_ullr", "0", "10", "10", "0"});
  ASSERT_NE(nullptr, ullr);

  auto gt = ParseOptions({"-a_gt", "0", "1", "0", "10", "0", "-1"});
  ASSERT_NE(nullptr, gt);
}

TEST_F(GdalTranslateLibTest, OptionsNodataAndScaleOffset) {
  auto nodata_num = ParseOptions({"-a_nodata", "42"});
  ASSERT_NE(nullptr, nodata_num);

  auto nodata_none = ParseOptions({"-a_nodata", "none"});
  ASSERT_NE(nullptr, nodata_none);

  auto nodata_null = ParseOptions({"-a_nodata", "null"});
  ASSERT_NE(nullptr, nodata_null);

  auto so = ParseOptions({"-a_scale", "2.0", "-a_offset", "5.0"});
  ASSERT_NE(nullptr, so);
}

TEST_F(GdalTranslateLibTest, OptionsGCP) {
  auto gcp4 = ParseOptions({"-gcp", "0", "0", "100", "200"});
  ASSERT_NE(nullptr, gcp4);

  auto gcp5 = ParseOptions({"-gcp", "0", "0", "100", "200", "50"});
  ASSERT_NE(nullptr, gcp5);

  auto nogcp = ParseOptions({"-nogcp"});
  ASSERT_NE(nullptr, nogcp);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-nogcp", "-gcp", "0", "0", "100", "200"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsColorInterpretation) {
  auto ci = ParseOptions({"-colorinterp", "red,green,blue"});
  ASSERT_NE(nullptr, ci);

  auto ci_undef = ParseOptions({"-colorinterp", "undefined"});
  ASSERT_NE(nullptr, ci_undef);

  auto ci_band =
      ParseOptions({"-colorinterp_1", "red", "-colorinterp_2", "green"});
  ASSERT_NE(nullptr, ci_band);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-colorinterp_0", "red"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto bad = ParseOptions({"-colorinterp_65536", "red"});
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, OptionsStatsAndMisc) {
  auto stats = ParseOptions({"-stats"});
  ASSERT_NE(nullptr, stats);

  auto approx_stats = ParseOptions({"-approx_stats"});
  ASSERT_NE(nullptr, approx_stats);

  auto norat = ParseOptions({"-norat", "-noxmp"});
  ASSERT_NE(nullptr, norat);

  auto co_mo = ParseOptions(
      {"-co", "COMPRESS=DEFLATE", "-mo", "FOO=BAR", "-dmo", "MYDOM:KEY=VAL"});
  ASSERT_NE(nullptr, co_mo);

  auto hidden_opts =
      ParseOptions({"-limit_outsize", "1000", "--no-overwrite", "--no-clip",
                    "--no-warn-about-outside-window"});
  ASSERT_NE(nullptr, hidden_opts);
}

TEST_F(GdalTranslateLibTest, OptionsForBinary) {
  GDALTranslateOptionsForBinary bin_opts;
  auto opts = ParseOptions(
      {"-sds", "-if", "GTiff", "-oo", "PARAM=VAL", "input.tif", "output.tif"},
      &bin_opts);
  ASSERT_NE(nullptr, opts);
  EXPECT_TRUE(bin_opts.bCopySubDatasets);
  EXPECT_STREQ("input.tif", bin_opts.osSource.c_str());
  EXPECT_STREQ("output.tif", bin_opts.osDest.c_str());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALTranslateOptionsForBinary bin_opts2;
    auto opts2 = ParseOptions({"-if", "NonExistentDriver", "in.tif", "out.tif"},
                              &bin_opts2);
    ASSERT_NE(nullptr, opts2);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, ParserUsageAndSetProgress) {
  const std::string usage = GDALTranslateGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("gdal_translate"));

  auto opts = ParseOptions({});
  ASSERT_NE(nullptr, opts);
  GDALTranslateOptionsSetProgress(opts.get(), GDALDummyProgress, nullptr);
}

// -----------------------------------------------------------------------------
// GDALTranslate Validation and Error Paths
// -----------------------------------------------------------------------------

TEST_F(GdalTranslateLibTest, TranslateNullSourceOrTarget) {
  constexpr char kDest[] = "/vsimem/gdal_translate_lib_test/null_src.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/null_src");
  };

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate(kDest, nullptr, nullptr, &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate(nullptr, src.get(), nullptr, &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateSameSourceAndDest) {
  constexpr char kPath[] = "/vsimem/gdal_translate_lib_test/same_path/test.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/same_path");
  };

  GDALDatasetUniquePtr src = CreateTestDataset(kPath);
  ASSERT_NE(nullptr, src);

  WithQuietHandler quiet;
  CPLErrorReset();
  int usage_error = 0;
  auto ds = RunTranslate(kPath, src.get(), nullptr, &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(TRUE, usage_error);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GdalTranslateLibTest, TranslateInvalidSRSDefinitions) {
  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions(
        {"-projwin", "0", "10", "10", "0", "-projwin_srs", "INVALID_SRS"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/srs/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    auto opts = ParseOptions({"-a_srs", "INVALID_SRS"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/srs/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateIncompatibleOptions) {
  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-outsize", "10", "10", "-tr", "1", "1"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/incompat/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }

  {
    auto opts =
        ParseOptions({"-a_ullr", "0", "10", "10", "0", "-tr", "1", "1"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/incompat/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }

  {
    auto opts = ParseOptions({"-a_ullr", "0", "10", "10", "0", "-a_gt", "0",
                              "1", "0", "10", "0", "-1"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/incompat/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }
}

TEST_F(GdalTranslateLibTest, TranslateBandErrors) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-b", "5"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/bands/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    auto opts = ParseOptions({"-scale", "0", "10", "-scale", "0", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/bands/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }

  {
    auto opts = ParseOptions({"-scale_2", "0", "10", "0", "20"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/bands/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }

  {
    auto opts = ParseOptions(
        {"-scale", "0", "10", "-exponent", "1.5", "-exponent", "2.0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/bands/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }

  {
    auto opts = ParseOptions({"-scale_1", "0", "10", "-exponent_2", "1.5"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    int usage_error = 0;
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/bands/out.tif",
                           src.get(), opts.get(), &usage_error);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(TRUE, usage_error);
  }
}

TEST_F(GdalTranslateLibTest, TranslateZeroBandsStrict) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src(mem_drv->Create("", 10, 10, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-strict"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/zeroband/out.tif",
                         src.get(), opts.get());
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GdalTranslateLibTest, TranslateProjWinErrors) {
  {
    GDALDatasetUniquePtr src =
        CreateTestDataset("", 10, 10, 1, GDT_Byte, /*set_gt=*/false);
    ASSERT_NE(nullptr, src);
    const double gt_invalid[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    src->SetGeoTransform(const_cast<double*>(gt_invalid));
    auto opts = ParseOptions({"-projwin", "0", "10", "10", "0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/projwin/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    GDALDatasetUniquePtr src =
        CreateTestDataset("", 10, 10, 1, GDT_Byte, /*set_gt=*/false);
    ASSERT_NE(nullptr, src);
    const double gt_rot[6] = {0.0, 1.0, 0.5, 10.0, 0.5, -1.0};
    src->SetGeoTransform(const_cast<double*>(gt_rot));

    auto opts = ParseOptions({"-projwin", "0", "10", "10", "0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/projwin/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateSrcWinErrors) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-srcwin", "0", "0", "-5", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/srcwin/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    auto opts = ParseOptions({"-srcwin", "100", "100", "10", "10", "-eco"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/srcwin/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    auto opts = ParseOptions({"-srcwin", "-5", "-5", "10", "10", "-epo"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/srcwin/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateDriverErrors) {
  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-of", "NON_EXISTING_DRIVER"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/drverr/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate(
        "/vsimem/gdal_translate_lib_test/drverr/unknown_extension.xyz123",
        src.get(), nullptr);
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateNoOverwriteError) {
  constexpr char kPath[] =
      "/vsimem/gdal_translate_lib_test/no_overwrite/dest.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/no_overwrite");
  };

  GDALDatasetUniquePtr existing = CreateTestDataset(kPath);
  ASSERT_NE(nullptr, existing);
  existing.reset();

  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"--no-overwrite"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate(kPath, src.get(), opts.get());
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GdalTranslateLibTest, TranslateLimitOutSizeErrors) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-limit_outsize", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/limit/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    auto opts = ParseOptions({"-outsize", "20", "20", "-limit_outsize", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/limit/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateAppendSubdatasetUnsupported) {
  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-co", "APPEND_SUBDATASET=YES", "-of", "GTiff"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/subds/out.tif",
                         src.get(), opts.get());
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GdalTranslateLibTest, TranslateExpandErrors) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-expand", "rgb"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/expand/out.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  GDALDatasetUniquePtr src3 = CreateTestDataset("", 10, 10, 3);
  ASSERT_NE(nullptr, src3);
  {
    auto opts = ParseOptions({"-expand", "rgb"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/expand/out.tif",
                           src3.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateExponentWithoutScaleError) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-b", "1", "-exponent", "2.0"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  int usage_error = 0;
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/exp/out.tif",
                         src.get(), opts.get(), &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(TRUE, usage_error);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

// -----------------------------------------------------------------------------
// GDALTranslate Successful Functional Tests
// -----------------------------------------------------------------------------

TEST_F(GdalTranslateLibTest, TranslateCreateCopyBasic) {
  constexpr char kDest[] =
      "/vsimem/gdal_translate_lib_test/create_copy/dest.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/create_copy");
  };

  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  auto ds = RunTranslate(kDest, src.get(), nullptr);
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(20, ds->GetRasterXSize());
  EXPECT_EQ(20, ds->GetRasterYSize());
  EXPECT_EQ(1, ds->GetRasterCount());
}

TEST_F(GdalTranslateLibTest, TranslateCreateCopyWithStatsAndProgress) {
  constexpr char kDest[] =
      "/vsimem/gdal_translate_lib_test/copy_stats/dest.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/copy_stats");
  };

  GDALDatasetUniquePtr src = CreateTestDataset("");
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-stats", "-co", "TILED=YES"});
  ASSERT_NE(nullptr, opts);

  bool progress_called = false;
  auto progress_cb = [](double dfComplete, const char* /*msg*/,
                        void* pData) -> int {
    *static_cast<bool*>(pData) = true;
    return TRUE;
  };
  GDALTranslateOptionsSetProgress(opts.get(), progress_cb, &progress_called);

  auto ds = RunTranslate(kDest, src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_TRUE(progress_called);

  auto opts_approx = ParseOptions({"-approx_stats"});
  ASSERT_NE(nullptr, opts_approx);
  constexpr char kDest2[] =
      "/vsimem/gdal_translate_lib_test/copy_stats/dest2.tif";
  auto ds2 = RunTranslate(kDest2, src.get(), opts_approx.get());
  ASSERT_NE(nullptr, ds2);
}

TEST_F(GdalTranslateLibTest, TranslateOutsizePixelsAndPercent) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/outsize";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 40, 20);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-outsize", "10", "5"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/outsize/pix.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(5, ds->GetRasterYSize());
  }

  {
    auto opts = ParseOptions({"-outsize", "50%", "50%"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/outsize/pct.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(20, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }

  {
    auto opts = ParseOptions({"-outsize", "0", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/outsize/aspect_x.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(20, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }

  {
    auto opts = ParseOptions({"-outsize", "20", "0"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/outsize/aspect_y.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(20, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }
}

TEST_F(GdalTranslateLibTest, TranslateTargetResolutionAndResampling) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/tr";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20);
  ASSERT_NE(nullptr, src);

  auto opts_tr = ParseOptions({"-tr", "2.0", "2.0"});
  ASSERT_NE(nullptr, opts_tr);
  auto ds_tr = RunTranslate("/vsimem/gdal_translate_lib_test/tr/tr.tif",
                            src.get(), opts_tr.get());
  ASSERT_NE(nullptr, ds_tr);
  EXPECT_EQ(10, ds_tr->GetRasterXSize());
  EXPECT_EQ(10, ds_tr->GetRasterYSize());

  for (const std::string& alg : {"bilinear", "cubic", "nearest", "average"}) {
    auto opts = ParseOptions({"-tr", "2.0", "2.0", "-r", alg});
    ASSERT_NE(nullptr, opts);
    const std::string out_path = std::string(kDestDir) + "/r_" + alg + ".tif";
    auto ds = RunTranslate(out_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }
}

TEST_F(GdalTranslateLibTest, TranslateSrcWinAndClamping) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/srcwin_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-srcwin", "5", "5", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/srcwin_test/win.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }

  // Clamping partially outside without -epo triggers FixSrcDstWindow and
  // warning.
  {
    auto opts = ParseOptions({"-srcwin", "-2", "-2", "15", "15"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/srcwin_test/clamp.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(15, ds->GetRasterXSize());
    EXPECT_EQ(15, ds->GetRasterYSize());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // With --no-warn-about-outside-window, warning is suppressed.
  {
    auto opts = ParseOptions(
        {"-srcwin", "-2", "-2", "15", "15", "--no-warn-about-outside-window"});
    ASSERT_NE(nullptr, opts);
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/srcwin_test/nowarn.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("", CPLGetLastErrorMsg());
  }
}

TEST_F(GdalTranslateLibTest, TranslateProjWin) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/projwin_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-projwin", "105", "195", "115", "185"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/projwin_test/out.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }

  {
    auto opts = ParseOptions(
        {"-projwin", "105", "195", "115", "185", "-projwin_srs", "EPSG:4326"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/projwin_test/srs.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }
}

TEST_F(GdalTranslateLibTest, TranslateBandsReorderAndDuplicate) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/band_reorder";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 3);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-b", "3", "-b", "1", "-b", "2"});
  ASSERT_NE(nullptr, opts);
  auto ds =
      RunTranslate("/vsimem/gdal_translate_lib_test/band_reorder/reorder.tif",
                   src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(3, ds->GetRasterCount());

  auto opts_dup = ParseOptions({"-b", "1", "-b", "1"});
  ASSERT_NE(nullptr, opts_dup);
  auto ds_dup =
      RunTranslate("/vsimem/gdal_translate_lib_test/band_reorder/dup.tif",
                   src.get(), opts_dup.get());
  ASSERT_NE(nullptr, ds_dup);
  EXPECT_EQ(2, ds_dup->GetRasterCount());
}

TEST_F(GdalTranslateLibTest, TranslateBandMaskAsRegularBand) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/mask_as_band";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  ASSERT_EQ(CE_None, src->GetRasterBand(1)->CreateMaskBand(GMF_PER_DATASET));

  auto opts = ParseOptions({"-b", "mask,1", "-b", "1"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/mask_as_band/out.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(2, ds->GetRasterCount());
  EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(1)->GetColorInterpretation());
}

TEST_F(GdalTranslateLibTest, TranslateOutputDataTypes) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/ot";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1, GDT_Float32);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "10");
  src->GetRasterBand(1)->SetMetadataItem("STATISTICS_MAXIMUM", "100");

  for (const auto& [type_str, expected_dt] : {
           std::pair("Byte", GDT_Byte),
           std::pair("UInt16", GDT_UInt16),
           std::pair("Int16", GDT_Int16),
           std::pair("UInt32", GDT_UInt32),
           std::pair("Int32", GDT_Int32),
           std::pair("Float32", GDT_Float32),
       }) {
    auto opts = ParseOptions({"-ot", type_str});
    ASSERT_NE(nullptr, opts);
    const std::string out_path =
        std::string(kDestDir) + "/" + type_str + ".tif";
    auto ds = RunTranslate(out_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(expected_dt, ds->GetRasterBand(1)->GetRasterDataType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateScaleOptions) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/scale_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 2);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions({"-scale"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/scale_test/auto.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-scale", "0", "100", "0", "255"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/scale_test/explicit.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-scale_1", "0", "100", "0", "255", "-scale_2",
                              "0", "50", "0", "128"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/scale_test/per_band.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions(
        {"-scale", "0", "100", "0", "255", "-exponent", "2.0", "--no-clip"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/scale_test/exp.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  // Equal min and max is handled without crash.
  {
    auto opts = ParseOptions({"-scale", "50", "50", "0", "255"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/scale_test/equal.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTranslateLibTest, TranslateUnscaleAndSetScaleOffset) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/unscale_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetScale(2.0);
  src->GetRasterBand(1)->SetOffset(10.0);

  {
    auto opts = ParseOptions({"-unscale"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/unscale_test/unscale.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-a_scale", "3.0", "-a_offset", "5.0"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/unscale_test/set_so.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_DOUBLE_EQ(3.0, ds->GetRasterBand(1)->GetScale());
    EXPECT_DOUBLE_EQ(5.0, ds->GetRasterBand(1)->GetOffset());
  }

  // -unscale with -a_scale emits warning when not quiet.
  {
    auto opts = ParseOptions({"-unscale", "-a_scale", "2.0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/unscale_test/warn.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateNodataOptions) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/nodata_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetNoDataValue(99);

  {
    auto opts = ParseOptions({"-a_nodata", "42"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/nodata_test/set.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    int success = 0;
    EXPECT_DOUBLE_EQ(42.0, ds->GetRasterBand(1)->GetNoDataValue(&success));
    EXPECT_EQ(TRUE, success);
  }

  {
    auto opts = ParseOptions({"-a_nodata", "none"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/nodata_test/none.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    int success = 0;
    ds->GetRasterBand(1)->GetNoDataValue(&success);
    EXPECT_EQ(FALSE, success);
  }

  // Unrepresentable nodata warning
  {
    auto opts = ParseOptions({"-a_nodata", "99999", "-ot", "Byte"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/nodata_test/unrep.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateColorExpansion) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/expand_test";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  GDALColorTable ct;
  const GDALColorEntry gray_entry = {100, 100, 100, 255};
  ct.SetColorEntry(0, &gray_entry);
  const GDALColorEntry color_entry = {200, 50, 10, 255};
  ct.SetColorEntry(1, &color_entry);
  src->GetRasterBand(1)->SetColorTable(&ct);

  {
    auto opts = ParseOptions({"-expand", "rgb"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/expand_test/rgb.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
  }

  {
    auto opts = ParseOptions({"-expand", "rgba"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/expand_test/rgba.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());
  }

  // -expand gray on non-gray color table emits warning
  {
    auto opts = ParseOptions({"-expand", "gray"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/expand_test/gray.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterCount());
    EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateColorInterpretationOverride) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/colorinterp";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 3);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-colorinterp", "blue,green,red"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/colorinterp/out.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(1)->GetColorInterpretation());
  EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
  EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(3)->GetColorInterpretation());

  auto opts_band = ParseOptions({"-colorinterp_1", "alpha"});
  ASSERT_NE(nullptr, opts_band);
  auto ds_band =
      RunTranslate("/vsimem/gdal_translate_lib_test/colorinterp/band.tif",
                   src.get(), opts_band.get());
  ASSERT_NE(nullptr, ds_band);
  EXPECT_EQ(GCI_AlphaBand, ds_band->GetRasterBand(1)->GetColorInterpretation());
}

TEST_F(GdalTranslateLibTest, TranslateMaskModes) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/masks";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 2);
  ASSERT_NE(nullptr, src);
  ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));

  {
    auto opts = ParseOptions({"-mask", "auto", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/masks/auto.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-mask", "none", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/masks/none.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-mask", "1", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/masks/user1.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions({"-mask", "mask,1", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/masks/user_mask.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTranslateLibTest, TranslateOverviews) {
  constexpr char kSrcPath[] = "/vsimem/gdal_translate_lib_test/ovr/src.tif";
  absl::Cleanup cleanup = [] {
    VSIRmdirRecursive("/vsimem/gdal_translate_lib_test/ovr");
  };

  GDALDatasetUniquePtr src = CreateTestDataset(kSrcPath, 40, 40, 1);
  ASSERT_NE(nullptr, src);
  const int anOverviewList[2] = {2, 4};
  ASSERT_EQ(CE_None,
            src->BuildOverviews("NEAREST", 2, anOverviewList, 0, nullptr,
                                GDALDummyProgress, nullptr, nullptr));

  for (const std::string& ovr : {"0", "1", "AUTO", "AUTO-1", "NONE"}) {
    auto opts = ParseOptions({"-ovr", ovr, "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    const std::string out_path =
        std::string("/vsimem/gdal_translate_lib_test/ovr/out_") + ovr + ".tif";
    auto ds = RunTranslate(out_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }

  // Overviews invalid level falls back with warning
  {
    auto opts = ParseOptions({"-ovr", "99", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/ovr/fallback.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateGCPsAndGeoreferencing) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/georef";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src =
      CreateTestDataset("", 20, 20, 1, GDT_Byte, /*set_gt=*/false);
  ASSERT_NE(nullptr, src);

  GDAL_GCP gcps[2];
  GDALInitGCPs(2, gcps);
  gcps[0].dfGCPPixel = 0;
  gcps[0].dfGCPLine = 0;
  gcps[0].dfGCPX = 100;
  gcps[0].dfGCPY = 200;
  gcps[1].dfGCPPixel = 20;
  gcps[1].dfGCPLine = 20;
  gcps[1].dfGCPX = 300;
  gcps[1].dfGCPY = 400;

  OGRSpatialReference srs;
  srs.SetFromUserInput("EPSG:4326");
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  src->SetGCPs(2, gcps, &srs);
  GDALDeinitGCPs(2, gcps);

  {
    auto opts = ParseOptions({"-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/georef/gcp.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetGCPCount());
  }

  {
    auto opts = ParseOptions({"-nogcp"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/georef/nogcp.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetGCPCount());
  }

  {
    auto opts = ParseOptions(
        {"-gcp", "0", "0", "50", "60", "-gcp", "10", "10", "70", "80"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/georef/explicit_gcp.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetGCPCount());
  }

  GDALDatasetUniquePtr src_nogcp = CreateTestDataset("");
  ASSERT_NE(nullptr, src_nogcp);

  {
    auto opts = ParseOptions({"-a_ullr", "10", "50", "20", "40"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/georef/ullr.tif",
                           src_nogcp.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    double gt[6] = {};
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(50.0, gt[3]);
  }

  {
    auto opts = ParseOptions({"-a_gt", "10", "1", "0", "50", "0", "-1"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/georef/gt.tif",
                           src_nogcp.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    double gt[6] = {};
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(50.0, gt[3]);
  }

  {
    auto opts =
        ParseOptions({"-a_srs", "EPSG:32631", "-a_coord_epoch", "2022.0"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/georef/srs.tif",
                           src_nogcp.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* out_srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, out_srs);
    EXPECT_DOUBLE_EQ(2022.0, out_srs->GetCoordinateEpoch());
  }

  {
    auto opts = ParseOptions({"-a_srs", "none"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/georef/srs_none.tif",
                     src_nogcp.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetSpatialRef());
  }
}

TEST_F(GdalTranslateLibTest, TranslateMetadataPreservationAndFiltering) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/md";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 2);
  ASSERT_NE(nullptr, src);

  src->SetMetadataItem("CACHE_PATH", "/tmp/cache");
  src->SetMetadataItem("NITF_BLOCKA_FOO", "BAR");
  src->SetMetadataItem("NODATA_VALUES", "10 20");
  src->SetMetadataItem("TIFFTAG_MINSAMPLEVALUE", "0");
  src->SetMetadataItem("TIFFTAG_MAXSAMPLEVALUE", "255");
  src->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  src->SetMetadataItem("COMPRESSION", "NONE", "IMAGE_STRUCTURE");

  const char* rpc_md[] = {"SAMP_OFF=10", "LINE_OFF=10", "SAMP_SCALE=20",
                          "LINE_SCALE=20", nullptr};
  src->SetMetadata(const_cast<char**>(rpc_md), "RPC");

  const char* geoloc_md[] = {"LINE_OFFSET=0", nullptr};
  src->SetMetadata(const_cast<char**>(geoloc_md), "GEOLOCATION");

  const char* xmp_md[] = {"<xmp>test</xmp>", nullptr};
  src->SetMetadata(const_cast<char**>(xmp_md), "xml:XMP");

  // Altering bounds and scaling filters out metadata
  auto opts = ParseOptions(
      {"-of", "VRT", "-srcwin", "2", "2", "10", "10", "-scale", "0", "100", "0",
       "255", "-mo", "USER_KEY=VAL", "-dmo", "MYDOM:DKEY=DVAL", "-noxmp"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/md/filtered.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);

  EXPECT_EQ(nullptr, ds->GetMetadataItem("CACHE_PATH"));
  EXPECT_EQ(nullptr, ds->GetMetadataItem("NITF_BLOCKA_FOO"));
  EXPECT_EQ(nullptr, ds->GetMetadataItem("TIFFTAG_MINSAMPLEVALUE"));
  EXPECT_EQ(nullptr, ds->GetMetadata("xml:XMP"));
  EXPECT_STREQ("VAL", ds->GetMetadataItem("USER_KEY"));
  EXPECT_STREQ("DVAL", ds->GetMetadataItem("DKEY", "MYDOM"));
  EXPECT_NE(nullptr, ds->GetMetadata("RPC"));
}

TEST_F(GdalTranslateLibTest, TranslateRasterAttributeTable) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/rat";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Value", GFT_Integer, GFU_MinMax);
  rat.SetRowCount(2);
  rat.SetValue(0, 0, 10);
  rat.SetValue(1, 0, 20);
  src->GetRasterBand(1)->SetDefaultRAT(&rat);

  {
    auto opts = ParseOptions({"-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/rat/with_rat.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
  }

  {
    auto opts = ParseOptions({"-norat", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/rat/norat.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
  }
}

TEST_F(GdalTranslateLibTest, TranslateISIS3Metadata) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/isis3";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 3);
  ASSERT_NE(nullptr, src);

  const char* isis3_json = R"json({
    "IsisCube": {
      "BandBin": {
        "FilterName": ["750", "900", "1000"],
        "Center": {
          "value": [750, 900, 1000],
          "unit": "nm"
        }
      }
    }
  })json";
  const char* isis3_md[] = {isis3_json, nullptr};
  src->SetMetadata(const_cast<char**>(isis3_md), "json:ISIS3");

  // Reordering bands and clipping/scaling triggers
  // EditISIS3MetadataForBandChange and EditISIS3ForMetadataChanges
  auto opts = ParseOptions({"-b", "3", "-b", "1", "-srcwin", "0", "0", "10",
                            "10", "-scale", "0", "100", "0", "255"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/isis3/reordered.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);

  char** md = ds->GetMetadata("json:ISIS3");
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, md[0]);
  EXPECT_THAT(md[0], HasSubstr("OriginalBandBin"));
  EXPECT_THAT(md[0], HasSubstr("GDALHistory"));
}

TEST_F(GdalTranslateLibTest, TranslateVRTDirect) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/vrt";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-of", "VRT", "-outsize", "10", "10"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/vrt/direct.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(10, ds->GetRasterXSize());
  EXPECT_EQ(10, ds->GetRasterYSize());
}

TEST_F(GdalTranslateLibTest, TranslateCopySrcOverviewsWarning) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  auto opts =
      ParseOptions({"-outsize", "10", "10", "-co", "COPY_SRC_OVERVIEWS=YES"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/co_ovr/out.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("COPY_SRC_OVERVIEWS"));
}

TEST_F(GdalTranslateLibTest, TranslateFixSrcDstWindowEdgeCases) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/fix_window";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  // Right edge clamping: dfModifiedSrcXOff + dfModifiedSrcXSize >
  // nSrcRasterXSize
  {
    auto opts = ParseOptions({"-srcwin", "15", "0", "10", "20"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/fix_window/right.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(10, ds->GetRasterXSize());
  }

  // Bottom edge clamping: dfModifiedSrcYOff + dfModifiedSrcYSize >
  // nSrcRasterYSize
  {
    auto opts = ParseOptions({"-srcwin", "0", "15", "20", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/fix_window/bottom.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(20, ds->GetRasterXSize());
  }

  // Completely off image without -eco/-epo succeeds with warning
  {
    auto opts = ParseOptions({"-srcwin", "30", "30", "10", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/fix_window/off.tif",
                           src.get(), opts.get());
    EXPECT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // Both X and Y clamped with outsize
  {
    auto opts = ParseOptions(
        {"-outsize", "10", "10", "-srcwin", "-5", "-5", "30", "30"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/fix_window/both.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTranslateLibTest, TranslateISIS3Operations) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/isis3_ops";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 2);
  ASSERT_NE(nullptr, src);

  const char* isis3_json = R"json({
    "IsisCube": {
      "BandBin": {
        "Filter": ["A", "B"]
      }
    }
  })json";
  const char* isis3_md[] = {isis3_json, nullptr};
  src->SetMetadata(const_cast<char**>(isis3_md), "json:ISIS3");

  // Multi-operation: clip + resolution change + scaling hits ", " formatting
  // and ops list
  {
    auto opts = ParseOptions({"-srcwin", "2", "2", "10", "10", "-outsize", "5",
                              "5", "-scale", "0", "100", "0", "255"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/isis3_ops/multi.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    char** md = ds->GetMetadata("json:ISIS3");
    ASSERT_NE(nullptr, md);
    EXPECT_THAT(md[0], HasSubstr("a resolution change operation"));
  }

  // Unscaling operation
  {
    src->GetRasterBand(1)->SetScale(2.0);
    src->GetRasterBand(1)->SetOffset(1.0);
    auto opts = ParseOptions({"-unscale"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/isis3_ops/unscale.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    char** md = ds->GetMetadata("json:ISIS3");
    ASSERT_NE(nullptr, md);
    EXPECT_THAT(md[0], HasSubstr("an unscaling operation"));
  }
}

TEST_F(GdalTranslateLibTest, TranslateProjWinSrsTransform) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/projwin_srs";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  // Source in EPSG:32631, projwin in EPSG:4326
  {
    GDALDatasetUniquePtr src = CreateTestDataset(
        "", 20, 20, 1, GDT_Byte, /*set_gt=*/false, /*set_srs=*/false);
    ASSERT_NE(nullptr, src);
    const double gt[6] = {500000.0, 10.0, 0.0, 4500000.0, 0.0, -10.0};
    src->SetGeoTransform(const_cast<double*>(gt));
    OGRSpatialReference srs;
    srs.SetFromUserInput("EPSG:32631");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    src->SetSpatialRef(&srs);

    // TransformBounds transforms bounds from 4326 to 32631
    auto opts = ParseOptions({"-projwin", "2.9", "40.7", "3.1", "40.5",
                              "-projwin_srs", "EPSG:4326"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/projwin_srs/trans.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  // Source with no projection warns that -projwin_srs is ignored
  {
    GDALDatasetUniquePtr src = CreateTestDataset(
        "", 20, 20, 1, GDT_Byte, /*set_gt=*/true, /*set_srs=*/false);
    ASSERT_NE(nullptr, src);
    auto opts = ParseOptions(
        {"-projwin", "102", "198", "110", "190", "-projwin_srs", "EPSG:4326"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/projwin_srs/noproj.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("dataset has no projection"));
  }
}

TEST_F(GdalTranslateLibTest, TranslateInvalidOutsizeCalculations) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
  ASSERT_NE(nullptr, src);

  // -tr invalid target size (< 1)
  {
    auto opts = ParseOptions({"-tr", "10000.0", "10000.0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/tr.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // -tr with rotated geotransform
  {
    GDALDatasetUniquePtr src_rot =
        CreateTestDataset("", 20, 20, 1, GDT_Byte, /*set_gt=*/false);
    ASSERT_NE(nullptr, src_rot);
    const double gt_rot[6] = {0, 1, 0.5, 0, 0.5, -1};
    src_rot->SetGeoTransform(const_cast<double*>(gt_rot));
    auto opts = ParseOptions({"-tr", "1.0", "1.0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/tr_rot.tif",
                     src_rot.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // -outsize percentage width < 1
  {
    auto opts = ParseOptions({"-outsize", "0.0001%", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/w.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // -outsize percentage height < 1
  {
    auto opts = ParseOptions({"-outsize", "10", "0.0001%"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/h.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // -outsize aspect width < 1
  {
    auto opts = ParseOptions({"-outsize", "0", "0.0001%"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/aw.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // -outsize aspect height < 1
  {
    auto opts = ParseOptions({"-outsize", "0.0001%", "0"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/invalid_sz/ah.tif",
                           src.get(), opts.get());
    EXPECT_EQ(nullptr, ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GdalTranslateLibTest, TranslateOverviewLevels) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/ovr_levels";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  // Source with 0 overviews requesting level 1 warns and defaults to full res
  {
    GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 1);
    ASSERT_NE(nullptr, src);
    auto opts = ParseOptions({"-ovr", "1", "-outsize", "10", "10"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/ovr_levels/full.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Defaulting to full resolution"));
  }

  // Source with overviews specifying -ovr 0 WITHOUT -outsize and WITHOUT -tr
  {
    constexpr char kSrcPath[] =
        "/vsimem/gdal_translate_lib_test/ovr_levels/src.tif";
    GDALDatasetUniquePtr src = CreateTestDataset(kSrcPath, 40, 40, 1);
    ASSERT_NE(nullptr, src);
    const int anOverviewList[1] = {2};
    ASSERT_EQ(CE_None,
              src->BuildOverviews("NEAREST", 1, anOverviewList, 0, nullptr,
                                  GDALDummyProgress, nullptr, nullptr));
    auto opts = ParseOptions({"-ovr", "0"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/ovr_levels/auto_ovr.tif",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(20, ds->GetRasterXSize());
    EXPECT_EQ(20, ds->GetRasterYSize());
  }
}

TEST_F(GdalTranslateLibTest, TranslateMetadataPreservationDetails) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/md_details";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 20, 20, 2);
  ASSERT_NE(nullptr, src);

  src->SetMetadataItem("NODATA_VALUES", "10 20");
  const char* xmp_md[] = {"<xmp>test</xmp>", nullptr};
  src->SetMetadata(const_cast<char**>(xmp_md), "xml:XMP");
  const char* pds4_md[] = {"<pds4>test</pds4>", nullptr};
  src->SetMetadata(const_cast<char**>(pds4_md), "xml:PDS4");
  const char* vicar_md[] = {"{\"vicar\":\"test\"}", nullptr};
  src->SetMetadata(const_cast<char**>(vicar_md), "json:VICAR");

  src->GetRasterBand(1)->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
  src->GetRasterBand(1)->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                         "IMAGE_STRUCTURE");
  src->GetRasterBand(1)->SetMetadataItem("COMPRESSION", "PACKBITS",
                                         "IMAGE_STRUCTURE");
  src->GetRasterBand(1)->SetUnitType("meters");
  src->GetRasterBand(1)->SetDescription("MyBandDescription");

  // Reorder bands drops NODATA_VALUES, preserves XMP without -noxmp
  {
    auto opts = ParseOptions({"-of", "VRT", "-b", "2", "-b", "1"});
    ASSERT_NE(nullptr, opts);
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/md_details/reorder.vrt",
                     src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetMetadataItem("NODATA_VALUES"));
    EXPECT_NE(nullptr, ds->GetMetadata("xml:XMP"));
    EXPECT_STREQ("meters", ds->GetRasterBand(2)->GetUnitType());
    EXPECT_STREQ("MyBandDescription", ds->GetRasterBand(2)->GetDescription());
    EXPECT_STREQ("12", ds->GetRasterBand(2)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("SIGNEDBYTE", ds->GetRasterBand(2)->GetMetadataItem(
                                   "PIXELTYPE", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("PACKBITS", ds->GetRasterBand(2)->GetMetadataItem(
                                 "COMPRESSION", "IMAGE_STRUCTURE"));
  }

  // Spatial arrangement preserved (e.g. -ot Int16) copies RPC and GEOLOCATION
  {
    const char* rpc_md[] = {"SAMP_OFF=10", nullptr};
    src->SetMetadata(const_cast<char**>(rpc_md), "RPC");
    const char* geoloc_md[] = {"LINE_OFFSET=0", nullptr};
    src->SetMetadata(const_cast<char**>(geoloc_md), "GEOLOCATION");

    auto opts = ParseOptions({"-of", "VRT", "-ot", "Int16"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate(
        "/vsimem/gdal_translate_lib_test/md_details/spatial_ok.vrt", src.get(),
        opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetMetadata("RPC"));
    EXPECT_NE(nullptr, ds->GetMetadata("GEOLOCATION"));
  }
}

TEST_F(GdalTranslateLibTest, TranslateColorExpansion2BandsAndRangeCheck) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/expand_2b";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 2);
  ASSERT_NE(nullptr, src);

  GDALColorTable ct;
  const GDALColorEntry entry = {100, 150, 200, 255};
  ct.SetColorEntry(0, &entry);
  src->GetRasterBand(1)->SetColorTable(&ct);

  // 2 bands expanded to RGBA: 4th band is alpha from 2nd band
  {
    auto opts = ParseOptions({"-expand", "rgba"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/expand_2b/rgba.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
  }

  // 2 bands expanded to RGB
  {
    auto opts = ParseOptions({"-expand", "rgb"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/expand_2b/rgb.tif",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
  }

  // UInt16 dataset with color table entries <= 255 warns and selects Byte
  {
    GDALDatasetUniquePtr src_u16 = CreateTestDataset("", 10, 10, 1, GDT_UInt16);
    ASSERT_NE(nullptr, src_u16);
    src_u16->GetRasterBand(1)->SetColorTable(&ct);

    auto opts = ParseOptions({"-expand", "rgb"});
    ASSERT_NE(nullptr, opts);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds =
        RunTranslate("/vsimem/gdal_translate_lib_test/expand_2b/u16_byte.tif",
                     src_u16.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Byte, ds->GetRasterBand(1)->GetRasterDataType());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Using Byte output data type"));
  }
}

TEST_F(GdalTranslateLibTest, TranslateDataTypeStatsPreservation) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/dt_stats";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1, GDT_Int16);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "10");
  src->GetRasterBand(1)->SetMetadataItem("STATISTICS_MAXIMUM", "100");

  for (const std::string& dt_name : {"Int8", "UInt16", "Int16", "UInt32",
                                     "Int32", "UInt64", "Int64", "Float64"}) {
    auto opts = ParseOptions({"-of", "VRT", "-ot", dt_name});
    ASSERT_NE(nullptr, opts);
    const std::string out_path = std::string(kDestDir) + "/" + dt_name + ".vrt";
    auto ds = RunTranslate(out_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr,
              ds->GetRasterBand(1)->GetMetadataItem("STATISTICS_MINIMUM"));
  }
}

TEST_F(GdalTranslateLibTest, TranslateScaleDataTypes) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/scale_dts";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  for (const std::string& dt_name : {"Int8", "UInt16", "Int16", "UInt32",
                                     "Int32", "UInt64", "Int64", "Float32"}) {
    auto opts =
        ParseOptions({"-of", "VRT", "-scale", "0", "100", "-ot", dt_name});
    ASSERT_NE(nullptr, opts);
    const std::string out_path = std::string(kDestDir) + "/" + dt_name + ".vrt";
    auto ds = RunTranslate(out_path.c_str(), src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTranslateLibTest, TranslateVRTBlockSizeOptions) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/vrt_bs";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 32, 32, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-of", "VRT", "-outsize", "32", "32", "-co",
                            "BLOCKXSIZE=16", "-co", "BLOCKYSIZE=16"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/vrt_bs/out.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  int bx = 0, by = 0;
  ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
  EXPECT_EQ(16, bx);
  EXPECT_EQ(16, by);
}

TEST_F(GdalTranslateLibTest, TranslateSignedByteNodata) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/sb_nodata";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                         "IMAGE_STRUCTURE");

  auto opts = ParseOptions({"-of", "VRT", "-a_nodata", "-50"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/sb_nodata/out.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  int success = 0;
  EXPECT_DOUBLE_EQ(-50.0, ds->GetRasterBand(1)->GetNoDataValue(&success));
  EXPECT_EQ(TRUE, success);
}

TEST_F(GdalTranslateLibTest, TranslatePerBandMaskAndAutoMask) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/per_band_mask";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  ASSERT_EQ(CE_None, src->GetRasterBand(1)->CreateMaskBand(0));

  auto opts = ParseOptions({"-of", "VRT", "-outsize", "10", "10"});
  ASSERT_NE(nullptr, opts);
  auto ds =
      RunTranslate("/vsimem/gdal_translate_lib_test/per_band_mask/out.vrt",
                   src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetMaskBand());
}

TEST_F(GdalTranslateLibTest, TranslateVRTStats) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/vrt_stats";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-of", "VRT", "-outsize", "10", "10", "-stats"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/vrt_stats/out.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_NE(nullptr,
            ds->GetRasterBand(1)->GetMetadataItem("STATISTICS_MINIMUM"));
}

TEST_F(GdalTranslateLibTest, TranslateUnrepresentableNodataCopyWarning) {
  constexpr char kDestDir[] = "/vsimem/gdal_translate_lib_test/unrep_copy";
  absl::Cleanup cleanup = [kDestDir] { VSIRmdirRecursive(kDestDir); };

  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1, GDT_Int64);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetNoDataValueAsInt64(1000);

  auto opts =
      ParseOptions({"-of", "VRT", "-ot", "Byte", "-outsize", "10", "10"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/unrep_copy/out.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source nodata value was not copied to output band"));
}

TEST_F(GdalTranslateLibTest, TranslateUnsupportedColorInterpWarning) {
  WithQuietHandler quiet;
  CPLErrorReset();
  auto opts = ParseOptions({"-colorinterp_1", "NON_EXISTING_COLOR_NAME"});
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unsupported color interpretation"));
}

TEST_F(GdalTranslateLibTest, TranslateColorInterpMoreBandsWarning) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-colorinterp", "red,green,blue", "-b", "1"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/ci_more.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("More bands defined in -colorinterp"));
}

TEST_F(GdalTranslateLibTest, TranslateMaskBandWithColorInterp) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  ASSERT_EQ(CE_None, src->GetRasterBand(1)->CreateMaskBand(0));

  auto opts = ParseOptions({"-b", "mask,1", "-colorinterp", "gray"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/mask_ci.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(1)->GetColorInterpretation());
}

TEST_F(GdalTranslateLibTest, TranslateScaleWithNodataComplexSource) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetNoDataValue(0);

  auto opts = ParseOptions({"-of", "VRT", "-scale", "0", "100", "0", "255"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/scale_nodata.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
}

TEST_F(GdalTranslateLibTest, TranslateSignedByteNodataUnrepresentable) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  src->GetRasterBand(1)->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE",
                                         "IMAGE_STRUCTURE");

  auto opts = ParseOptions({"-of", "VRT", "-a_nodata", "99999"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/sb_unrep.vrt",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
}

TEST_F(GdalTranslateLibTest, TranslateVRTOnlyBlockXSizeOrYSize) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 32, 32, 1);
  ASSERT_NE(nullptr, src);

  {
    auto opts = ParseOptions(
        {"-of", "VRT", "-outsize", "32", "32", "-co", "BLOCKXSIZE=16"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/vrt_bx.vrt",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }

  {
    auto opts = ParseOptions(
        {"-of", "VRT", "-outsize", "32", "32", "-co", "BLOCKYSIZE=16"});
    ASSERT_NE(nullptr, opts);
    auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/vrt_by.vrt",
                           src.get(), opts.get());
    ASSERT_NE(nullptr, ds);
  }
}

TEST_F(GdalTranslateLibTest, TranslateScaleHugeValuesEqualError) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);

  auto opts = ParseOptions({"-scale", "1e300", "1e300", "0", "255"});
  ASSERT_NE(nullptr, opts);
  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/huge_scale.tif",
                         src.get(), opts.get());
  EXPECT_EQ(nullptr, ds);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("source minimum and maximum being equal"));
}

TEST_F(GdalTranslateLibTest, TranslateGCPWithOutputSRS) {
  GDALDatasetUniquePtr src =
      CreateTestDataset("", 10, 10, 1, GDT_Byte, /*set_gt=*/false);
  ASSERT_NE(nullptr, src);

  auto opts =
      ParseOptions({"-gcp", "0", "0", "100", "200", "-a_srs", "EPSG:4326"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/gcp_srs.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(1, ds->GetGCPCount());
}

TEST_F(GdalTranslateLibTest, TranslateExpandMaskBand) {
  GDALDatasetUniquePtr src = CreateTestDataset("", 10, 10, 1);
  ASSERT_NE(nullptr, src);
  ASSERT_EQ(CE_None, src->GetRasterBand(1)->CreateMaskBand(0));

  GDALColorTable ct;
  const GDALColorEntry entry = {100, 150, 200, 255};
  ct.SetColorEntry(0, &entry);
  src->GetRasterBand(1)->GetMaskBand()->SetColorTable(&ct);

  auto opts = ParseOptions({"-expand", "rgb", "-b", "mask,1"});
  ASSERT_NE(nullptr, opts);
  auto ds = RunTranslate("/vsimem/gdal_translate_lib_test/expand_mask.tif",
                         src.get(), opts.get());
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(3, ds->GetRasterCount());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdal_translate_lib.cpp.
