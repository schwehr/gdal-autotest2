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
// Tests for GDALBuildVRT (apps/gdalbuildvrt_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalbuildvrt_lib.cpp
//   https://gdal.org/programs/gdalbuildvrt.html

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GdalBuildVRTTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_MEM();
    GDALRegister_VRT();
    GDALRegister_GTiff();
    RegisterOGRShape();
  }

  void SetUp() override { VSIRmdirRecursive("/vsimem/buildvrt"); }

  void TearDown() override { VSIRmdirRecursive("/vsimem/buildvrt"); }

  static void CreateDataset(const std::string& filename, int xsize, int ysize,
                            int num_bands, GDALDataType type,
                            const double* gt = nullptr,
                            const char* srs_wkt = nullptr,
                            const double* nodata = nullptr,
                            double fill_val = 0.0) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    GDALDatasetUniquePtr ds(
        drv->Create(filename.c_str(), xsize, ysize, num_bands, type, nullptr));
    ASSERT_NE(nullptr, ds);
    if (gt) {
      ds->SetGeoTransform(const_cast<double*>(gt));
    }
    if (srs_wkt) {
      ds->SetProjection(srs_wkt);
    }
    for (int b = 1; b <= num_bands; ++b) {
      auto* band = ds->GetRasterBand(b);
      ASSERT_NE(nullptr, band);
      if (nodata) {
        band->SetNoDataValue(*nodata);
      }
      band->Fill(fill_val);
    }
    ds->FlushCache();
  }
};

// 1. Parser and usage tests.
TEST_F(GdalBuildVRTTest, ParserUsage) {
  const std::string usage = GDALBuildVRTGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_NE(std::string::npos, usage.find("gdalbuildvrt"));
}

// 2. Options management tests.
TEST_F(GdalBuildVRTTest, OptionsManagement) {
  // Default options with null
  auto* default_opts = GDALBuildVRTOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, default_opts);
  GDALBuildVRTOptionsFree(default_opts);

  // SetProgress
  auto* opts = GDALBuildVRTOptionsNew(nullptr, nullptr);
  ASSERT_NE(nullptr, opts);
  GDALBuildVRTOptionsSetProgress(opts, GDALDummyProgress, nullptr);
  GDALBuildVRTOptionsSetProgress(opts, GDALTermProgress, nullptr);
  GDALBuildVRTOptionsFree(opts);

  // Valid comprehensive arguments
  std::vector<std::string> args = {"-strict",
                                   "-tile_index",
                                   "MY_LOC",
                                   "-resolution",
                                   "highest",
                                   "-tr",
                                   "0.5",
                                   "0.5",
                                   "-te",
                                   "0",
                                   "0",
                                   "100",
                                   "100",
                                   "-separate",
                                   "-allow_projection_difference",
                                   "-sd",
                                   "1",
                                   "-tap",
                                   "-addalpha",
                                   "-b",
                                   "1",
                                   "-b",
                                   "2",
                                   "-hidenodata",
                                   "-srcnodata",
                                   "-9999 -9999",
                                   "-vrtnodata",
                                   "-9999 -9999",
                                   "-a_srs",
                                   "EPSG:4326",
                                   "-r",
                                   "bilinear",
                                   "-oo",
                                   "COMPRESS=NONE",
                                   "-co",
                                   "BLOCKXSIZE=128",
                                   "-write_absolute_path",
                                   "-ignore_srcmaskband",
                                   "-nodata_max_mask_threshold",
                                   "128",
                                   "-program_name",
                                   "test_prog"};
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& s : args) {
    argv.push_back(s.data());
  }
  argv.push_back(nullptr);

  auto* parsed_opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
  ASSERT_NE(nullptr, parsed_opts);
  GDALBuildVRTOptionsFree(parsed_opts);

  // Resolutions
  for (const char* res :
       {"user", "average", "highest", "lowest", "same", "common"}) {
    std::vector<std::string> r_args = {"-resolution", res};
    std::vector<char*> r_argv = {r_args[0].data(), r_args[1].data(), nullptr};
    auto* r_opts = GDALBuildVRTOptionsNew(r_argv.data(), nullptr);
    ASSERT_NE(nullptr, r_opts);
    GDALBuildVRTOptionsFree(r_opts);
  }

  // Invalid resolution
  {
    WithQuietHandler quiet;
    std::vector<std::string> bad_res = {"-resolution", "invalid_res"};
    std::vector<char*> bad_argv = {bad_res[0].data(), bad_res[1].data(),
                                   nullptr};
    EXPECT_EQ(nullptr, GDALBuildVRTOptionsNew(bad_argv.data(), nullptr));
  }

  // Invalid -a_srs
  {
    WithQuietHandler quiet;
    std::vector<std::string> bad_srs = {"-a_srs", "INVALID:PROJECTION:STRING"};
    std::vector<char*> bad_argv = {bad_srs[0].data(), bad_srs[1].data(),
                                   nullptr};
    EXPECT_EQ(nullptr, GDALBuildVRTOptionsNew(bad_argv.data(), nullptr));
  }

  // Invalid pixel function
  {
    WithQuietHandler quiet;
    std::vector<std::string> bad_pix = {"-pixel-function",
                                        "invalid_function_name"};
    std::vector<char*> bad_argv = {bad_pix[0].data(), bad_pix[1].data(),
                                   nullptr};
    EXPECT_EQ(nullptr, GDALBuildVRTOptionsNew(bad_argv.data(), nullptr));
  }

  // Pixel function arg without pixel function
  {
    WithQuietHandler quiet;
    std::vector<std::string> bad_arg = {"-pixel-function-arg", "k=v"};
    std::vector<char*> bad_argv = {bad_arg[0].data(), bad_arg[1].data(),
                                   nullptr};
    EXPECT_EQ(nullptr, GDALBuildVRTOptionsNew(bad_argv.data(), nullptr));
  }

  // Non-strict flag
  {
    std::vector<std::string> ns_args = {"-non_strict"};
    std::vector<char*> ns_argv = {ns_args[0].data(), nullptr};
    auto* ns_opts = GDALBuildVRTOptionsNew(ns_argv.data(), nullptr);
    ASSERT_NE(nullptr, ns_opts);
    GDALBuildVRTOptionsFree(ns_opts);
  }

  // OptionsForBinary
  {
    GDALBuildVRTOptionsForBinary bin_opts;
    std::vector<std::string> b_args = {"-o", "out.vrt", "-overwrite", "s1.tif",
                                       "s2.tif"};
    std::vector<char*> b_argv;
    for (auto& s : b_args) b_argv.push_back(s.data());
    b_argv.push_back(nullptr);
    auto* b_parsed = GDALBuildVRTOptionsNew(b_argv.data(), &bin_opts);
    ASSERT_NE(nullptr, b_parsed);
    EXPECT_EQ("out.vrt", bin_opts.osDstFilename);
    EXPECT_TRUE(bin_opts.bOverwrite);
    EXPECT_EQ(2, bin_opts.aosSrcFiles.size());
    GDALBuildVRTOptionsFree(b_parsed);
  }
}

// 3. Incompatible options checked in GDALBuildVRT.
TEST_F(GdalBuildVRTTest, IncompatibleOptionsInBuild) {
  int bUsageError = 0;
  WithQuietHandler quiet;

  // No input datasets
  EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/out.vrt", 0, nullptr,
                                  nullptr, nullptr, &bUsageError));
  EXPECT_TRUE(bUsageError);

  constexpr char kTile1[] = "/vsimem/buildvrt/incomp1.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  CreateDataset(kTile1, 10, 10, 1, GDT_Byte, gt);
  const char* src_files[1] = {kTile1};

  // -tr with -resolution highest
  {
    std::vector<std::string> args = {"-tr", "1", "1", "-resolution", "highest"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), args[4].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    bUsageError = 0;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bad_tr_res.vrt", 1,
                                    nullptr, src_files, opts, &bUsageError));
    EXPECT_TRUE(bUsageError);
  }

  // -tap without -tr
  {
    std::vector<std::string> args = {"-tap"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    bUsageError = 0;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bad_tap.vrt", 1, nullptr,
                                    src_files, opts, &bUsageError));
    EXPECT_TRUE(bUsageError);
  }

  // -addalpha with -separate
  {
    std::vector<std::string> args = {"-addalpha", "-separate"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    bUsageError = 0;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bad_alpha_sep.vrt", 1,
                                    nullptr, src_files, opts, &bUsageError));
    EXPECT_TRUE(bUsageError);
  }

  // -resolution user without -tr
  {
    std::vector<std::string> args = {"-resolution", "user"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    bUsageError = 0;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bad_user_res.vrt", 1,
                                    nullptr, src_files, opts, &bUsageError));
    EXPECT_TRUE(bUsageError);
  }
}

// 4. Basic Mosaic of multiple tiles.
TEST_F(GdalBuildVRTTest, BasicMosaic) {
  constexpr char kTile1[] = "/vsimem/buildvrt/tile1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/tile2.tif";
  constexpr char kVrt[] = "/vsimem/buildvrt/mosaic.vrt";

  const double gt1[6] = {0.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  const double gt2[6] = {50.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  CreateDataset(kTile1, 50, 100, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 10.0);
  CreateDataset(kTile2, 50, 100, 1, GDT_Byte, gt2, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 20.0);

  const char* src_files[2] = {kTile1, kTile2};
  int bUsageError = 0;
  GDALDatasetH hDst =
      GDALBuildVRT(kVrt, 2, nullptr, src_files, nullptr, &bUsageError);
  ASSERT_NE(nullptr, hDst);
  EXPECT_FALSE(bUsageError);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(hDst));
  EXPECT_EQ(100, out_ds->GetRasterXSize());
  EXPECT_EQ(100, out_ds->GetRasterYSize());
  EXPECT_EQ(1, out_ds->GetRasterCount());

  double out_gt[6];
  ASSERT_EQ(CE_None, out_ds->GetGeoTransform(out_gt));
  EXPECT_DOUBLE_EQ(0.0, out_gt[0]);
  EXPECT_DOUBLE_EQ(1.0, out_gt[1]);
  EXPECT_DOUBLE_EQ(0.0, out_gt[2]);
  EXPECT_DOUBLE_EQ(100.0, out_gt[3]);
  EXPECT_DOUBLE_EQ(0.0, out_gt[4]);
  EXPECT_DOUBLE_EQ(-1.0, out_gt[5]);

  // Read data across mosaic to verify sources are linked properly
  std::vector<GByte> buf(100);
  ASSERT_EQ(CE_None, out_ds->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 100, 1, buf.data(), 100, 1, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(10, buf[0]);
  EXPECT_EQ(10, buf[49]);
  EXPECT_EQ(20, buf[50]);
  EXPECT_EQ(20, buf[99]);
}

// 5. Separate bands test.
TEST_F(GdalBuildVRTTest, SeparateBands) {
  constexpr char kTile1[] = "/vsimem/buildvrt/sep1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/sep2.tif";
  constexpr char kVrt[] = "/vsimem/buildvrt/separate.vrt";

  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 1.0);
  CreateDataset(kTile2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 2.0);

  const char* src_files[2] = {kTile1, kTile2};
  std::vector<std::string> args = {"-separate"};
  std::vector<char*> argv = {args[0].data(), nullptr};
  auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
  ASSERT_NE(nullptr, opts);
  absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

  int bUsageError = 0;
  GDALDatasetH hDst =
      GDALBuildVRT(kVrt, 2, nullptr, src_files, opts, &bUsageError);
  ASSERT_NE(nullptr, hDst);
  EXPECT_FALSE(bUsageError);

  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(hDst));
  EXPECT_EQ(50, out_ds->GetRasterXSize());
  EXPECT_EQ(50, out_ds->GetRasterYSize());
  EXPECT_EQ(2, out_ds->GetRasterCount());

  std::vector<GByte> buf(50);
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 50, 1, buf.data(),
                                               50, 1, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(1, buf[0]);
  ASSERT_EQ(CE_None,
            out_ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 50, 1, buf.data(),
                                               50, 1, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(2, buf[0]);

  // Ungeoreferenced images with -separate
  constexpr char kNoGt1[] = "/vsimem/buildvrt/nogt1.tif";
  constexpr char kNoGt2[] = "/vsimem/buildvrt/nogt2.tif";
  constexpr char kNoGtDiffSize[] = "/vsimem/buildvrt/nogt_diff.tif";
  CreateDataset(kNoGt1, 40, 40, 1, GDT_Byte, nullptr, nullptr, nullptr, 5.0);
  CreateDataset(kNoGt2, 40, 40, 1, GDT_Byte, nullptr, nullptr, nullptr, 6.0);
  CreateDataset(kNoGtDiffSize, 40, 50, 1, GDT_Byte, nullptr, nullptr, nullptr,
                7.0);

  // Stacking matching ungeoreferenced images succeeds
  {
    const char* no_gt_files[2] = {kNoGt1, kNoGt2};
    GDALDatasetH hNoGt = GDALBuildVRT("/vsimem/buildvrt/nogt_sep.vrt", 2,
                                      nullptr, no_gt_files, opts, nullptr);
    ASSERT_NE(nullptr, hNoGt);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hNoGt));
    EXPECT_EQ(40, ds->GetRasterXSize());
    EXPECT_EQ(40, ds->GetRasterYSize());
    EXPECT_EQ(2, ds->GetRasterCount());
  }
  // Stacking differing dimension ungeoreferenced images fails with -strict
  {
    WithQuietHandler quiet;
    std::vector<std::string> s_args = {"-separate", "-strict"};
    std::vector<char*> s_argv = {s_args[0].data(), s_args[1].data(), nullptr};
    auto* s_opts = GDALBuildVRTOptionsNew(s_argv.data(), nullptr);
    ASSERT_NE(nullptr, s_opts);
    absl::Cleanup free_s_opts = [s_opts] { GDALBuildVRTOptionsFree(s_opts); };

    const char* diff_files[2] = {kNoGt1, kNoGtDiffSize};
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/nogt_fail.vrt", 2,
                                    nullptr, diff_files, s_opts, nullptr));

    const char* mixed_files[2] = {kTile1, kNoGt1};
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/mixed_fail.vrt", 2,
                                    nullptr, mixed_files, s_opts, nullptr));
  }

  // Ungeoreferenced image in non-separate mode fails
  {
    WithQuietHandler quiet;
    const char* no_gt_single[1] = {kNoGt1};
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/nogt_nonsep.vrt", 1,
                                    nullptr, no_gt_single, nullptr, nullptr));
  }
}

// 6. Resolution strategies and target extents.
TEST_F(GdalBuildVRTTest, ResolutionStrategies) {
  constexpr char kRes1[] = "/vsimem/buildvrt/res1.tif";
  constexpr char kRes2[] = "/vsimem/buildvrt/res2.tif";

  // Res1: 50x50 at res 1.0 (extent: 0 to 50, 50 to 100)
  const double gt1[6] = {0.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  CreateDataset(kRes1, 50, 50, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG);

  // Res2: 25x25 at res 2.0 (extent: 50 to 100, 50 to 100)
  const double gt2[6] = {50.0, 2.0, 0.0, 100.0, 0.0, -2.0};
  CreateDataset(kRes2, 25, 25, 1, GDT_Byte, gt2, SRS_WKT_WGS84_LAT_LONG);

  const char* src_files[2] = {kRes1, kRes2};

  auto test_res = [&](const std::string& strategy, double exp_we_res,
                      double exp_ns_res) {
    std::vector<std::string> args = {"-resolution", strategy};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    std::string out_path = "/vsimem/buildvrt/res_" + strategy + ".vrt";
    GDALDatasetH hDst =
        GDALBuildVRT(out_path.c_str(), 2, nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));

    double out_gt[6];
    ASSERT_EQ(CE_None, ds->GetGeoTransform(out_gt));
    EXPECT_NEAR(exp_we_res, out_gt[1], 1e-4);
    EXPECT_NEAR(exp_ns_res, out_gt[5], 1e-4);
  };

  test_res("highest", 1.0, -1.0);
  test_res("lowest", 2.0, -2.0);
  test_res("average", 1.5, -1.5);
  test_res("common", 1.0, -1.0);

  // Resolution "same" fails when resolutions differ in strict mode
  {
    WithQuietHandler quiet;
    std::vector<std::string> args = {"-resolution", "same", "-strict"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/res_same_fail.vrt", 2,
                                    nullptr, src_files, opts, nullptr));
  }

  // -tr target resolution
  {
    std::vector<std::string> args = {"-tr", "0.5", "0.5"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/tr_user.vrt", 2, nullptr,
                                     src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    double out_gt[6];
    ASSERT_EQ(CE_None, ds->GetGeoTransform(out_gt));
    EXPECT_NEAR(0.5, out_gt[1], 1e-4);
    EXPECT_NEAR(-0.5, out_gt[5], 1e-4);
  }

  // -te target extent and -tap
  {
    std::vector<std::string> args = {"-te", "0",  "0",  "100", "100",
                                     "-tr", "10", "10", "-tap"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/tap_te.vrt", 2, nullptr,
                                     src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
  }
}

// 7. NoData and Mask handling.
TEST_F(GdalBuildVRTTest, NoDataAndMaskHandling) {
  constexpr char kTile1[] = "/vsimem/buildvrt/nodata1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/nodata2.tif";

  const double gt1[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  const double gt2[6] = {50.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  const double nodata_val = 0.0;
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG,
                &nodata_val, 10.0);
  CreateDataset(kTile2, 50, 50, 1, GDT_Byte, gt2, SRS_WKT_WGS84_LAT_LONG,
                &nodata_val, 20.0);

  const char* src_files[2] = {kTile1, kTile2};

  // -srcnodata and -vrtnodata
  {
    std::vector<std::string> args = {"-srcnodata", "0", "-vrtnodata", "255"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/vrtnodata.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    int bHasNoData = 0;
    double val = ds->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(255.0, val);
  }

  // -hidenodata
  {
    std::vector<std::string> args = {"-hidenodata"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/hidenodata.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    int bHasNoData = 0;
    ds->GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    EXPECT_FALSE(bHasNoData);
  }

  // -addalpha
  {
    std::vector<std::string> args = {"-addalpha"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/addalpha.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(2, ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(2)->GetColorInterpretation());
  }

  // Mask band creation and -ignore_srcmaskband
  {
    constexpr char kMaskTile[] = "/vsimem/buildvrt/mask_tile.tif";
    CreateDataset(kMaskTile, 50, 50, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG);
    GDALDatasetH hOpen = GDALOpen(kMaskTile, GA_Update);
    ASSERT_NE(nullptr, hOpen);
    GDALDatasetUniquePtr open_ds(GDALDataset::FromHandle(hOpen));
    open_ds->CreateMaskBand(GMF_PER_DATASET);
    open_ds->FlushCache();
    open_ds.reset();

    const char* mask_src[1] = {kMaskTile};
    std::vector<std::string> args = {"-ignore_srcmaskband",
                                     "-nodata_max_mask_threshold", "10"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/mask_out.vrt", 1,
                                     nullptr, mask_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

// 8. Projection checks and -allow_projection_difference.
TEST_F(GdalBuildVRTTest, ProjectionHandling) {
  constexpr char kProj1[] = "/vsimem/buildvrt/p1.tif";
  constexpr char kProj2[] = "/vsimem/buildvrt/p2.tif";

  const double gt1[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  const double gt2[6] = {50.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kProj1, 50, 50, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  char* wkt3857 = nullptr;
  srs3857.exportToWkt(&wkt3857);
  CreateDataset(kProj2, 50, 50, 1, GDT_Byte, gt2, wkt3857);
  CPLFree(wkt3857);

  const char* src_files[2] = {kProj1, kProj2};

  // Mismatch without -allow_projection_difference fails in strict mode
  {
    WithQuietHandler quiet;
    std::vector<std::string> s_args = {"-strict"};
    std::vector<char*> s_argv = {s_args[0].data(), nullptr};
    auto* s_opts = GDALBuildVRTOptionsNew(s_argv.data(), nullptr);
    ASSERT_NE(nullptr, s_opts);
    absl::Cleanup free_s_opts = [s_opts] { GDALBuildVRTOptionsFree(s_opts); };
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/proj_mismatch.vrt", 2,
                                    nullptr, src_files, s_opts, nullptr));
  }

  // Mismatch with -allow_projection_difference succeeds
  {
    std::vector<std::string> args = {"-allow_projection_difference"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/proj_allowed.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // -a_srs projection override
  {
    std::vector<std::string> args = {"-a_srs", "EPSG:32631"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    const char* single_src[1] = {kProj1};
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/a_srs.vrt", 1, nullptr,
                                     single_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    OGRSpatialReference out_srs(ds->GetProjectionRef());
    EXPECT_STREQ("WGS 84 / UTM zone 31N", out_srs.GetName());
  }

  // Rotated geotransform fails
  {
    constexpr char kRot[] = "/vsimem/buildvrt/rot.tif";
    const double gt_rot[6] = {0.0, 1.0, 0.5, 50.0, 0.5, -1.0};
    CreateDataset(kRot, 50, 50, 1, GDT_Byte, gt_rot, SRS_WKT_WGS84_LAT_LONG);
    const char* rot_src[1] = {kRot};
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/rot_fail.vrt", 1, nullptr,
                                    rot_src, nullptr, nullptr));
  }

  // Positive NS resolution fails
  {
    constexpr char kPosNS[] = "/vsimem/buildvrt/pos_ns.tif";
    const double gt_pos[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    CreateDataset(kPosNS, 50, 50, 1, GDT_Byte, gt_pos, SRS_WKT_WGS84_LAT_LONG);
    const char* pos_src[1] = {kPosNS};
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/pos_ns_fail.vrt", 1,
                                    nullptr, pos_src, nullptr, nullptr));
  }
}

// 9. Band selection (-b).
TEST_F(GdalBuildVRTTest, BandSelection) {
  constexpr char kMultiBand[] = "/vsimem/buildvrt/multiband.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kMultiBand, 50, 50, 3, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);

  const char* src_files[1] = {kMultiBand};

  // Select band 2
  {
    std::vector<std::string> args = {"-b", "2"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/b2.vrt", 1, nullptr,
                                     src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(1, ds->GetRasterCount());
  }

  // Select bands 3 and 1
  {
    std::vector<std::string> args = {"-b", "3", "-b", "1"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/b31.vrt", 1, nullptr,
                                     src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(2, ds->GetRasterCount());
  }

  // Select invalid band
  {
    WithQuietHandler quiet;
    std::vector<std::string> args = {"-b", "99"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bad_b.vrt", 1, nullptr,
                                    src_files, opts, nullptr));
  }
}

// 10. Color tables and overviews.
TEST_F(GdalBuildVRTTest, ColorTableAndOverviews) {
  constexpr char kColorTile[] = "/vsimem/buildvrt/color_tile.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 256.0, 0.0, -1.0};
  CreateDataset(kColorTile, 256, 256, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);

  GDALDatasetH hOpen = GDALOpen(kColorTile, GA_Update);
  ASSERT_NE(nullptr, hOpen);
  GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hOpen));

  GDALColorTable ct;
  GDALColorEntry entry = {255, 0, 0, 255};
  ct.SetColorEntry(1, &entry);
  ds->GetRasterBand(1)->SetColorTable(&ct);
  ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);

  // Build overview
  int overviewList[1] = {2};
  ds->BuildOverviews("NEAREST", 1, overviewList, 0, nullptr, nullptr, nullptr,
                     nullptr);
  ds->FlushCache();
  ds.reset();

  const char* src_files[1] = {kColorTile};
  GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/color_ovr.vrt", 1, nullptr,
                                   src_files, nullptr, nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr out_ds(GDALDataset::FromHandle(hDst));

  EXPECT_EQ(GCI_PaletteIndex,
            out_ds->GetRasterBand(1)->GetColorInterpretation());
  EXPECT_NE(nullptr, out_ds->GetRasterBand(1)->GetColorTable());
  EXPECT_GT(out_ds->GetRasterBand(1)->GetOverviewCount(), 0);
}

// 11. Pixel functions.
TEST_F(GdalBuildVRTTest, PixelFunctions) {
  constexpr char kTile1[] = "/vsimem/buildvrt/pix1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/pix2.tif";

  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 10.0);
  CreateDataset(kTile2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG,
                nullptr, 20.0);

  const char* src_files[2] = {kTile1, kTile2};
  {
    std::vector<std::string> args = {"-pixel-function", "mean",
                                     "-pixel-function-arg", "k=v"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/pixfun_mean.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  {
    std::vector<std::string> args = {"-pixel-function", "inv"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};

    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/pixfun_inv.vrt", 2,
                                     nullptr, src_files, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

// 12. Subdatasets (-sd).
TEST_F(GdalBuildVRTTest, Subdatasets) {
  constexpr char kSub1[] = "/vsimem/buildvrt/sub1.tif";
  constexpr char kSub2[] = "/vsimem/buildvrt/sub2.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kSub1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  CreateDataset(kSub2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);

  const std::string multi_vrt =
      "<VRTDataset>\n"
      "  <!-- VRTPansharpenedDataset -->\n"
      "  <Group name=\"/\"/>\n"
      "  <Metadata domain=\"SUBDATASETS\">\n"
      "    <MDI key=\"SUBDATASET_1_NAME\">" +
      std::string(kSub1) +
      "</MDI>\n"
      "    <MDI key=\"SUBDATASET_1_DESC\">Subdataset 1</MDI>\n"
      "    <MDI key=\"SUBDATASET_2_NAME\">" +
      std::string(kSub2) +
      "</MDI>\n"
      "    <MDI key=\"SUBDATASET_2_DESC\">Subdataset 2</MDI>\n"
      "  </Metadata>\n"
      "</VRTDataset>\n";
  constexpr char kMultiVrt[] = "/vsimem/buildvrt/multi_sub.vrt";
  VSIFCloseL(VSIFileFromMemBuffer(
      kMultiVrt, reinterpret_cast<GByte*>(const_cast<char*>(multi_vrt.data())),
      multi_vrt.size(), FALSE));
  const char* multi_src[1] = {kMultiVrt};

  // -sd 1 selects first subdataset
  {
    std::vector<std::string> args = {"-sd", "1"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/sd1.vrt", 1, nullptr,
                                     multi_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(1, ds->GetRasterCount());
  }

  // -sd negative selects all subdatasets
  {
    std::vector<std::string> args = {"-sd", "-1"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/sd_all.vrt", 1, nullptr,
                                     multi_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

// 13. Input file list and shapefile tile index.
TEST_F(GdalBuildVRTTest, InputFileListAndShapefileTileIndex) {
  constexpr char kTile1[] = "/vsimem/buildvrt/t1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/t2.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  CreateDataset(kTile2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);

  // Text file input list
  constexpr char kListFile[] = "/vsimem/buildvrt/list.txt";
  const std::string list_content = std::string(kTile1) + "\n" + kTile2 + "\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kListFile,
      reinterpret_cast<GByte*>(const_cast<char*>(list_content.data())),
      list_content.size(), FALSE));

  GDALBuildVRTOptionsForBinary bin_opts;
  std::vector<std::string> args = {"-input_file_list", kListFile,
                                   "/vsimem/buildvrt/from_list.vrt"};
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);

  auto* opts = GDALBuildVRTOptionsNew(argv.data(), &bin_opts);
  ASSERT_NE(nullptr, opts);
  absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

  EXPECT_EQ(2, bin_opts.aosSrcFiles.size());
  GDALDatasetH hDst =
      GDALBuildVRT(bin_opts.osDstFilename.c_str(), bin_opts.aosSrcFiles.size(),
                   nullptr, bin_opts.aosSrcFiles.List(), opts, nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  // Shapefile tileindex
  constexpr char kShpTileIndex[] = "/vsimem/buildvrt/tileindex.shp";
  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_drv);
  GDALDatasetUniquePtr shp_ds(
      shp_drv->Create(kShpTileIndex, 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, shp_ds);

  OGRLayer* layer =
      shp_ds->CreateLayer("tileindex", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);
  OGRFieldDefn field_defn("LOCATION", OFTString);
  field_defn.SetWidth(254);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));

  auto add_feature = [&](const char* path) {
    OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
    feat->SetField("LOCATION", path);
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);
  };
  add_feature(kTile1);
  add_feature(kTile2);
  shp_ds.reset();

  // Build VRT from Shapefile tileindex via GDALBuildVRTOptionsNew
  GDALBuildVRTOptionsForBinary bin_opts_shp;
  std::vector<std::string> shp_args = {"-tile_index", "LOCATION",
                                       "/vsimem/buildvrt/from_shp.vrt",
                                       kShpTileIndex};
  std::vector<char*> shp_argv;
  for (auto& s : shp_args) shp_argv.push_back(s.data());
  shp_argv.push_back(nullptr);

  auto* opts_shp = GDALBuildVRTOptionsNew(shp_argv.data(), &bin_opts_shp);
  ASSERT_NE(nullptr, opts_shp);
  absl::Cleanup free_opts_shp = [opts_shp] {
    GDALBuildVRTOptionsFree(opts_shp);
  };

  EXPECT_EQ(2, bin_opts_shp.aosSrcFiles.size());
  GDALDatasetH hDstShp = GDALBuildVRT(
      bin_opts_shp.osDstFilename.c_str(), bin_opts_shp.aosSrcFiles.size(),
      nullptr, bin_opts_shp.aosSrcFiles.List(), opts_shp, nullptr);
  ASSERT_NE(nullptr, hDstShp);
  GDALClose(hDstShp);
}

// 14. Strict vs Non-strict and in-memory VRT creation.
TEST_F(GdalBuildVRTTest, StrictVsNonStrictAndInMemoryVRT) {
  constexpr char kGoodTile[] = "/vsimem/buildvrt/good.tif";
  constexpr char kBadTile[] = "/vsimem/buildvrt/nonexistent.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kGoodTile, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);

  const char* mixed_sources[2] = {kGoodTile, kBadTile};

  // With -strict (default): fails when one source cannot be opened
  {
    WithQuietHandler quiet;
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/strict_fail.vrt", 2,
                                    nullptr, mixed_sources, opts, nullptr));
  }

  // With -non_strict: skips missing file and builds with good tile
  {
    WithQuietHandler quiet;
    std::vector<std::string> args = {"-non_strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/non_strict.vrt", 2,
                                     nullptr, mixed_sources, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(50, ds->GetRasterXSize());
    EXPECT_EQ(1, ds->GetRasterCount());
  }

  // In-memory VRT creation: pszDest = nullptr
  {
    const char* single_src[1] = {kGoodTile};
    GDALDatasetH hDst =
        GDALBuildVRT(nullptr, 1, nullptr, single_src, nullptr, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(50, ds->GetRasterXSize());
    EXPECT_EQ(50, ds->GetRasterYSize());
  }

  // Passing open dataset handles directly in pahSrcDS
  {
    GDALDatasetUniquePtr open_ds(
        GDALDataset::Open(kGoodTile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, open_ds);
    GDALDatasetH handles[1] = {GDALDataset::ToHandle(open_ds.get())};
    GDALDatasetUniquePtr dst_ds(GDALDataset::FromHandle(
        GDALBuildVRT("/vsimem/buildvrt/from_handles.vrt", 1, handles, nullptr,
                     nullptr, nullptr)));
    ASSERT_NE(nullptr, dst_ds);
  }
}

// 15. Extent clipping and tile rejection in CalculateSrcDstWindow.
TEST_F(GdalBuildVRTTest, ExtentClippingAndFiltering) {
  // Target window: [25, 75] x [25, 75], resolution: 1.0 x 1.0 -> 50x50 pixels.
  constexpr char kTile1[] = "/vsimem/buildvrt/clip1.tif";
  const double gt1[6] = {0.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt1, SRS_WKT_WGS84_LAT_LONG);

  constexpr char kTile2[] = "/vsimem/buildvrt/clip2.tif";
  const double gt2[6] = {50.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  CreateDataset(kTile2, 50, 100, 1, GDT_Byte, gt2, SRS_WKT_WGS84_LAT_LONG);

  constexpr char kTileRight[] = "/vsimem/buildvrt/clip_right.tif";
  const double gt_right[6] = {100.0, 1.0, 0.0, 75.0, 0.0, -1.0};
  CreateDataset(kTileRight, 50, 50, 1, GDT_Byte, gt_right,
                SRS_WKT_WGS84_LAT_LONG);

  constexpr char kTileLeft[] = "/vsimem/buildvrt/clip_left.tif";
  const double gt_left[6] = {-60.0, 1.0, 0.0, 75.0, 0.0, -1.0};
  CreateDataset(kTileLeft, 50, 50, 1, GDT_Byte, gt_left,
                SRS_WKT_WGS84_LAT_LONG);

  constexpr char kTileBelow[] = "/vsimem/buildvrt/clip_below.tif";
  const double gt_below[6] = {25.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  CreateDataset(kTileBelow, 50, 20, 1, GDT_Byte, gt_below,
                SRS_WKT_WGS84_LAT_LONG);

  constexpr char kTileAbove[] = "/vsimem/buildvrt/clip_above.tif";
  const double gt_above[6] = {25.0, 1.0, 0.0, 100.0, 0.0, -1.0};
  CreateDataset(kTileAbove, 50, 20, 1, GDT_Byte, gt_above,
                SRS_WKT_WGS84_LAT_LONG);

  const char* src_files[6] = {kTile1,    kTile2,     kTileRight,
                              kTileLeft, kTileBelow, kTileAbove};

  std::vector<std::string> args = {"-te", "25",  "25",  "75",
                                   "75",  "-tr", "1.0", "1.0"};
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);

  auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
  ASSERT_NE(nullptr, opts);
  absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

  GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/clipped.vrt", 6, nullptr,
                                   src_files, opts, nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
  EXPECT_EQ(50, ds->GetRasterXSize());
  EXPECT_EQ(50, ds->GetRasterYSize());
}

// 16. Heterogeneous mismatches with strict error checking.
TEST_F(GdalBuildVRTTest, HeterogeneousMismatches) {
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};

  // 1. Heterogeneous band counts with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/bcount1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/bcount2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 2, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/bcount_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 2. Explicit band list out of range with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/brange1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/brange2.tif";
    CreateDataset(kDS1, 50, 50, 2, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict", "-b", "2"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/brange_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 3. Heterogeneous data type with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/dtype1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/dtype2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Float32, gt, SRS_WKT_WGS84_LAT_LONG);
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/dtype_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 4. Heterogeneous color interpretation with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/cinterp1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/cinterp2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    {
      GDALDatasetUniquePtr ds(GDALDataset::Open(kDS1, GA_Update));
      ASSERT_NE(nullptr, ds);
      ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    }
    {
      GDALDatasetUniquePtr ds(GDALDataset::Open(kDS2, GA_Update));
      ASSERT_NE(nullptr, ds);
      ds->GetRasterBand(1)->SetColorInterpretation(GCI_GreenBand);
    }
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/cinterp_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 5. Color table mismatch with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/ctable1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/ctable2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    {
      GDALDatasetUniquePtr ds(GDALDataset::Open(kDS1, GA_Update));
      ASSERT_NE(nullptr, ds);
      GDALColorTable ct;
      GDALColorEntry e1 = {255, 0, 0, 255};
      ct.SetColorEntry(0, &e1);
      ds->GetRasterBand(1)->SetColorTable(&ct);
      ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
      ds->FlushCache();
    }
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/ctable_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 6. Color table entry count mismatch with -strict (using VRT with distinct
  // counts)
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/ct_count1.vrt";
    constexpr char kDS2[] = "/vsimem/buildvrt/ct_count2.vrt";
    const std::string vrt1 =
        "<VRTDataset rasterXSize=\"50\" rasterYSize=\"50\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
        "    <ColorInterp>Palette</ColorInterp>\n"
        "    <ColorTable>\n"
        "      <Entry c1=\"255\" c2=\"0\" c3=\"0\" c4=\"255\"/>\n"
        "    </ColorTable>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    const std::string vrt2 =
        "<VRTDataset rasterXSize=\"50\" rasterYSize=\"50\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
        "    <ColorInterp>Palette</ColorInterp>\n"
        "    <ColorTable>\n"
        "      <Entry c1=\"255\" c2=\"0\" c3=\"0\" c4=\"255\"/>\n"
        "      <Entry c1=\"0\" c2=\"255\" c3=\"0\" c4=\"255\"/>\n"
        "    </ColorTable>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    VSIFCloseL(VSIFileFromMemBuffer(
        kDS1, reinterpret_cast<GByte*>(const_cast<char*>(vrt1.data())),
        vrt1.size(), FALSE));
    VSIFCloseL(VSIFileFromMemBuffer(
        kDS2, reinterpret_cast<GByte*>(const_cast<char*>(vrt2.data())),
        vrt2.size(), FALSE));

    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/ct_count_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 7. Color table palette values mismatch (warning in non-strict mode)
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/ct_warn1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/ct_warn2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    {
      GDALDatasetUniquePtr ds1(GDALDataset::Open(kDS1, GA_Update));
      ASSERT_NE(nullptr, ds1);
      GDALColorTable ct1;
      GDALColorEntry e1 = {255, 0, 0, 255};
      ct1.SetColorEntry(0, &e1);
      ds1->GetRasterBand(1)->SetColorTable(&ct1);
      ds1->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
      ds1->FlushCache();

      GDALDatasetUniquePtr ds2(GDALDataset::Open(kDS2, GA_Update));
      ASSERT_NE(nullptr, ds2);
      GDALColorTable ct2;
      GDALColorEntry e2 = {0, 255, 0, 255};
      ct2.SetColorEntry(0, &e2);
      ds2->GetRasterBand(1)->SetColorTable(&ct2);
      ds2->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
      ds2->FlushCache();
    }
    const char* src[2] = {kDS1, kDS2};
    WithQuietHandler quiet;
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/ct_warn.vrt", 2, nullptr,
                                     src, nullptr, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 8. Heterogeneous offset with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/off1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/off2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    {
      GDALDatasetUniquePtr ds1(GDALDataset::Open(kDS1, GA_Update));
      ASSERT_NE(nullptr, ds1);
      ds1->GetRasterBand(1)->SetOffset(10.0);
      ds1->FlushCache();
      GDALDatasetUniquePtr ds2(GDALDataset::Open(kDS2, GA_Update));
      ASSERT_NE(nullptr, ds2);
      ds2->GetRasterBand(1)->SetOffset(20.0);
      ds2->FlushCache();
    }
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/off_fail.vrt", 2, nullptr,
                                    src, opts, nullptr));
  }

  // 9. Heterogeneous scale with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/scale1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/scale2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    {
      GDALDatasetUniquePtr ds1(GDALDataset::Open(kDS1, GA_Update));
      ASSERT_NE(nullptr, ds1);
      ds1->GetRasterBand(1)->SetScale(2.0);
      ds1->FlushCache();
      GDALDatasetUniquePtr ds2(GDALDataset::Open(kDS2, GA_Update));
      ASSERT_NE(nullptr, ds2);
      ds2->GetRasterBand(1)->SetScale(5.0);
      ds2->FlushCache();
    }
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-strict"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/scale_fail.vrt", 2,
                                    nullptr, src, opts, nullptr));
  }

  // 10. Ungeoreferenced images with -separate and -te / -tr (warnings emitted)
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/nogt_warn1.tif";
    constexpr char kDS2[] = "/vsimem/buildvrt/nogt_warn2.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, nullptr, nullptr);
    CreateDataset(kDS2, 50, 50, 1, GDT_Byte, nullptr, nullptr);
    const char* src[2] = {kDS1, kDS2};
    std::vector<std::string> args = {"-separate", "-te", "0", "0", "10",
                                     "10",        "-tr", "1", "1"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/nogt_warn.vrt", 2,
                                     nullptr, src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 11. -separate with -b out of range with -strict
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/sep_b1.tif";
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
    const char* src[1] = {kDS1};
    std::vector<std::string> args = {"-separate", "-strict", "-b", "5"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/sep_b_fail.vrt", 1,
                                    nullptr, src, opts, nullptr));
  }

  // 12. checkNoDataValues warning: Byte data type with out-of-range NoData
  {
    constexpr char kDS1[] = "/vsimem/buildvrt/bad_nodata.tif";
    const double bad_nd = 300.0;
    CreateDataset(kDS1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG,
                  &bad_nd);
    const char* src[1] = {kDS1};
    WithQuietHandler quiet;
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/bad_nd.vrt", 1, nullptr,
                                     src, nullptr, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

// 17. Separate mode advanced features.
TEST_F(GdalBuildVRTTest, SeparateAdvancedFeatures) {
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  constexpr char kTile1[] = "/vsimem/buildvrt/sep_adv1.tif";
  constexpr char kTile2[] = "/vsimem/buildvrt/sep_adv2.tif";
  CreateDataset(kTile1, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  CreateDataset(kTile2, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  {
    GDALDatasetUniquePtr ds1(GDALDataset::Open(kTile1, GA_Update));
    ASSERT_NE(nullptr, ds1);
    ds1->GetRasterBand(1)->SetOffset(5.0);
    ds1->GetRasterBand(1)->SetScale(1.5);
  }

  // -separate with -srcnodata, -vrtnodata, -r, -hidenodata
  const char* src[2] = {kTile1, kTile2};
  std::vector<std::string> args = {"-separate",  "-srcnodata", "0",
                                   "-vrtnodata", "255",        "-r",
                                   "cubic",      "-hidenodata"};
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);

  auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
  ASSERT_NE(nullptr, opts);
  absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

  GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/sep_adv.vrt", 2, nullptr,
                                   src, opts, nullptr);
  ASSERT_NE(nullptr, hDst);
  GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
  EXPECT_EQ(2, ds->GetRasterCount());
  int has_nd = true;
  ds->GetRasterBand(1)->GetNoDataValue(&has_nd);
  EXPECT_FALSE(has_nd);

  // Passing in-memory MEM dataset handles directly to pahSrcDS with -separate
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem1(mem_drv->Create("", 50, 50, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem1);
  mem1->SetGeoTransform(const_cast<double*>(gt));
  GDALDatasetUniquePtr mem2(mem_drv->Create("", 50, 50, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem2);
  mem2->SetGeoTransform(const_cast<double*>(gt));

  GDALDatasetH mem_handles[2] = {GDALDataset::ToHandle(mem1.get()),
                                 GDALDataset::ToHandle(mem2.get())};
  std::vector<std::string> mem_args = {"-separate"};
  std::vector<char*> mem_argv = {mem_args[0].data(), nullptr};
  auto* mem_opts = GDALBuildVRTOptionsNew(mem_argv.data(), nullptr);
  ASSERT_NE(nullptr, mem_opts);
  absl::Cleanup free_mem_opts = [mem_opts] {
    GDALBuildVRTOptionsFree(mem_opts);
  };

  GDALDatasetH hDstMem = GDALBuildVRT("/vsimem/buildvrt/sep_mem.vrt", 2,
                                      mem_handles, nullptr, mem_opts, nullptr);
  ASSERT_NE(nullptr, hDstMem);
  GDALDatasetUniquePtr ds_mem(GDALDataset::FromHandle(hDstMem));
  EXPECT_EQ(2, ds_mem->GetRasterCount());
}

// 18. Mask and alpha band features.
TEST_F(GdalBuildVRTTest, MaskAndAlphaFeatures) {
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};

  // 1. Input datasets where 4th band is already GCI_AlphaBand
  constexpr char kRgba1[] = "/vsimem/buildvrt/rgba1.tif";
  constexpr char kRgba2[] = "/vsimem/buildvrt/rgba2.tif";
  CreateDataset(kRgba1, 50, 50, 4, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  CreateDataset(kRgba2, 50, 50, 4, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  {
    GDALDatasetUniquePtr ds1(GDALDataset::Open(kRgba1, GA_Update));
    ASSERT_NE(nullptr, ds1);
    ds1->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    GDALDatasetUniquePtr ds2(GDALDataset::Open(kRgba2, GA_Update));
    ASSERT_NE(nullptr, ds2);
    ds2->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
  }

  // -addalpha with pre-existing alpha band: does not add a 5th band
  const char* rgba_src[2] = {kRgba1, kRgba2};
  {
    std::vector<std::string> args = {"-addalpha"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/rgba_alpha.vrt", 2,
                                     nullptr, rgba_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(4, ds->GetRasterCount());
  }

  // 2. -nodata_max_mask_threshold and -ignore_srcmaskband
  {
    std::vector<std::string> args = {"-nodata_max_mask_threshold", "128.0",
                                     "-ignore_srcmaskband", "-vrtnodata", "0"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/mask_thresh.vrt", 2,
                                     nullptr, rgba_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 3. Dataset mask band with -addalpha
  constexpr char kMaskTile[] = "/vsimem/buildvrt/tile_with_mask.tif";
  CreateDataset(kMaskTile, 50, 50, 3, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  {
    GDALDatasetUniquePtr ds(GDALDataset::Open(kMaskTile, GA_Update));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
  }
  const char* mask_src[1] = {kMaskTile};
  {
    std::vector<std::string> args = {"-addalpha"};
    std::vector<char*> argv = {args[0].data(), nullptr};
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };

    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/mask_addalpha.vrt", 1,
                                     nullptr, mask_src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(hDst));
    EXPECT_EQ(4, ds->GetRasterCount());
  }
}

// 19. Shapefile tile index error conditions.
TEST_F(GdalBuildVRTTest, ShapefileTileIndexErrors) {
  // 1. Non-existent shapefile in GDALBuildVRTOptionsNew with binary options
  {
    GDALBuildVRTOptionsForBinary bin_opts;
    std::vector<std::string> args = {
        "/vsimem/buildvrt/out.vrt",
        "/vsimem/buildvrt/nonexistent_tileindex.shp"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    WithQuietHandler quiet;
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), &bin_opts);
    EXPECT_EQ(nullptr, opts);
  }

  // Create shapefile with field "LOCATION"
  constexpr char kShpPath[] = "/vsimem/buildvrt/err_ti.shp";
  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_drv);
  {
    GDALDatasetUniquePtr ds(
        shp_drv->Create(kShpPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("err_ti", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, layer);
    OGRFieldDefn fdefn("LOCATION", OFTString);
    fdefn.SetWidth(254);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&fdefn));
    // Notice: 0 features added -> empty tile index!
  }

  // 2. Empty shapefile tile index (warns and skips)
  {
    GDALBuildVRTOptionsForBinary bin_opts;
    std::vector<std::string> args = {"/vsimem/buildvrt/out_empty.vrt",
                                     kShpPath};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    WithQuietHandler quiet;
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), &bin_opts);
    ASSERT_NE(nullptr, opts);
    GDALBuildVRTOptionsFree(opts);
    EXPECT_EQ(0, bin_opts.aosSrcFiles.size());
  }

  // 3. Shapefile with missing specified tile index field
  {
    GDALBuildVRTOptionsForBinary bin_opts;
    std::vector<std::string> args = {"-tile_index", "NONEXISTENT_FIELD",
                                     "/vsimem/buildvrt/out_bad_field.vrt",
                                     kShpPath};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    WithQuietHandler quiet;
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), &bin_opts);
    EXPECT_EQ(nullptr, opts);
  }
}

// 20. Invalid build parameters and nodata none.
TEST_F(GdalBuildVRTTest, InvalidBuildParameters) {
  constexpr char kTile[] = "/vsimem/buildvrt/param_tile.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 50.0, 0.0, -1.0};
  CreateDataset(kTile, 50, 50, 1, GDT_Byte, gt, SRS_WKT_WGS84_LAT_LONG);
  const char* src[1] = {kTile};

  // 1. Invalid user extent: minX >= maxX
  {
    std::vector<std::string> args = {"-te", "100", "0", "50", "50"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/inv_te.vrt", 1, nullptr,
                                    src, opts, nullptr));
  }

  // 2. Invalid user resolution: we_res <= 0
  {
    std::vector<std::string> args = {"-tr", "0", "1"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/inv_tr.vrt", 1, nullptr,
                                    src, opts, nullptr));
  }

  // 3. Invalid -srcnodata
  {
    std::vector<std::string> args = {"-srcnodata", "not_a_number"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/inv_srcnd.vrt", 1,
                                    nullptr, src, opts, nullptr));
  }

  // 4. -srcnodata "none" (valid, disables src nodata)
  {
    std::vector<std::string> args = {"-srcnodata", "none"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/srcnd_none.vrt", 1,
                                     nullptr, src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }

  // 5. Invalid -vrtnodata
  {
    std::vector<std::string> args = {"-vrtnodata", "not_a_number"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, GDALBuildVRT("/vsimem/buildvrt/inv_vrtnd.vrt", 1,
                                    nullptr, src, opts, nullptr));
  }

  // 6. -vrtnodata "none" (valid, disables vrt nodata)
  {
    std::vector<std::string> args = {"-vrtnodata", "none"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    auto* opts = GDALBuildVRTOptionsNew(argv.data(), nullptr);
    ASSERT_NE(nullptr, opts);
    absl::Cleanup free_opts = [opts] { GDALBuildVRTOptionsFree(opts); };
    GDALDatasetH hDst = GDALBuildVRT("/vsimem/buildvrt/vrtnd_none.vrt", 1,
                                     nullptr, src, opts, nullptr);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalbuildvrt_lib.cpp.
