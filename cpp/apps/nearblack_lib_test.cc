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
// Tests for Nearblack (apps/nearblack_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/nearblack_lib.cpp
//   https://gdal.org/programs/nearblack.html

// clang-format off
// nearblack_lib.h requires types declared in gdal.h.
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/apps/nearblack_lib.h"
// clang-format on

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct DatasetDeleter {
  void operator()(GDALDatasetH ds) const {
    if (ds != nullptr) {
      GDALClose(ds);
    }
  }
};
using DatasetUniquePtr =
    std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, DatasetDeleter>;

struct OptionsDeleter {
  void operator()(GDALNearblackOptions* opts) const {
    if (opts != nullptr) {
      GDALNearblackOptionsFree(opts);
    }
  }
};
using OptionsUniquePtr = std::unique_ptr<GDALNearblackOptions, OptionsDeleter>;

class NearblackLibTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }

  void SetUp() override { VSIRmdirRecursive("/vsimem/nearblack_lib_test"); }
  void TearDown() override { VSIRmdirRecursive("/vsimem/nearblack_lib_test"); }
};

TEST_F(NearblackLibTest, OptionsNewDefault) {
  OptionsUniquePtr options(GDALNearblackOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);
}

TEST_F(NearblackLibTest, OptionsNewAllFlags) {
  std::vector<const char*> argv = {
      "-of",      "GTiff", "-co",       "TILED=YES", "-white",
      "-nb",      "3",     "-near",     "10",        "-setalpha",
      "-setmask", "-alg",  "twopasses", "-q",        nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);
}

TEST_F(NearblackLibTest, OptionsNewWithColorsAndBinaryOptions) {
  std::vector<const char*> argv = {
      "-color",    "0,0,0", "-color",          "10,20,30",       "-alg",
      "floodfill", "-o",    "/vsimem/out.tif", "/vsimem/in.tif", nullptr,
  };
  GDALNearblackOptionsForBinary options_for_binary;
  OptionsUniquePtr options(GDALNearblackOptionsNew(
      const_cast<char**>(argv.data()), &options_for_binary));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("/vsimem/out.tif", options_for_binary.osOutFile);
  EXPECT_EQ("/vsimem/in.tif", options_for_binary.osInFile);
}

TEST_F(NearblackLibTest, OptionsNewNegativeColor) {
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv = {
      "-color",
      "-1,-2,-3",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  EXPECT_EQ(nullptr, options);
}

TEST_F(NearblackLibTest, OptionsNewInvalidColorNonInteger) {
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv = {
      "-color",
      "invalid",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  EXPECT_EQ(nullptr, options);
}

TEST_F(NearblackLibTest, OptionsNewInconsistentColorBandCount) {
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv = {
      "-color", "0,0,0", "-color", "0,0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  EXPECT_EQ(nullptr, options);
}

TEST_F(NearblackLibTest, OptionsNewUnknownFlag) {
  WithQuietHandler quiet_handler;
  std::vector<const char*> argv = {
      "-unsupported_flag",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  EXPECT_EQ(nullptr, options);
}

TEST_F(NearblackLibTest, ParserUsage) {
  const std::string usage = GDALNearblackGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_NE(std::string::npos, usage.find("nearblack"));
}

TEST_F(NearblackLibTest, SetProgress) {
  OptionsUniquePtr options(GDALNearblackOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);
  int dummy = 42;
  GDALNearblackOptionsSetProgress(
      options.get(), [](double, const char*, void*) -> int { return 1; },
      &dummy);
  GDALNearblackOptionsSetProgress(options.get(), nullptr, nullptr);
}

TEST_F(NearblackLibTest, InvalidDatasetArguments) {
  WithQuietHandler quiet_handler;
  int usage_error = FALSE;

  GDALDatasetH ds =
      GDALNearblack(nullptr, nullptr, nullptr, nullptr, &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_TRUE(usage_error);

  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  usage_error = FALSE;
  ds = GDALNearblack(nullptr, nullptr, src.get(), nullptr, &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_TRUE(usage_error);

  usage_error = FALSE;
  ds =
      GDALNearblack("/vsimem/out.tif", nullptr, nullptr, nullptr, &usage_error);
  EXPECT_EQ(nullptr, ds);
  EXPECT_TRUE(usage_error);
}

TEST_F(NearblackLibTest, TwoPassesBasicBlackBorder) {
  const std::string test_dir = "/vsimem/nearblack_test/basic";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 12, 12, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  double geo_transform[6] = {100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
  GDALSetGeoTransform(src_ds.get(), geo_transform);

  // 12x12 image: border of value 5 (near black <= 15), center from (3,3) to
  // (8,8) is 100.
  std::vector<uint8_t> data(144, 5);
  for (int y = 3; y <= 8; ++y) {
    for (int x = 3; x <= 8; ++x) {
      data[y * 12 + x] = 100;
    }
  }
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 12, 12, data.data(),
                                  12, 12, GDT_Byte, 0, 0));

  DatasetUniquePtr dst_ds(
      GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, dst_ds);

  double dst_gt[6] = {};
  EXPECT_EQ(CE_None, GDALGetGeoTransform(dst_ds.get(), dst_gt));
  EXPECT_DOUBLE_EQ(100.0, dst_gt[0]);

  std::vector<uint8_t> out_data(144, 255);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 12, 12,
                                  out_data.data(), 12, 12, GDT_Byte, 0, 0));

  // Border should be replaced with 0.
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(0, out_data[11]);
  EXPECT_EQ(0, out_data[132]);
  EXPECT_EQ(0, out_data[143]);
  // Center should remain 100.
  EXPECT_EQ(100, out_data[5 * 12 + 5]);
  EXPECT_EQ(100, out_data[6 * 12 + 6]);
}

TEST_F(NearblackLibTest, WhiteBorder) {
  const std::string test_dir = "/vsimem/nearblack_test/white";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> data(25, 250);  // near white <= 255
  data[2 * 5 + 2] = 50;                // center pixel
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 5, 5, data.data(),
                                  5, 5, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-white",
      "-nb",
      "0",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(25, 0);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 5, 5,
                                  out_data.data(), 5, 5, GDT_Byte, 0, 0));

  EXPECT_EQ(255, out_data[0]);
  EXPECT_EQ(255, out_data[4]);
  EXPECT_EQ(50, out_data[2 * 5 + 2]);
}

TEST_F(NearblackLibTest, CustomColorsMultipleAndNearDistance) {
  const std::string test_dir = "/vsimem/nearblack_test/custom_colors";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  // Border has 120 and 200, center is 50.
  std::vector<uint8_t> data = {
      120, 120, 120, 120, 120, 200, 50,  50,  50,  200, 200, 50,  50,
      50,  200, 200, 50,  50,  50,  200, 120, 120, 120, 120, 120,
  };
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 5, 5, data.data(),
                                  5, 5, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-color", "125", "-color", "195", "-near", "10", "-nb", "1", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(25, 0);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 5, 5,
                                  out_data.data(), 5, 5, GDT_Byte, 0, 0));

  // Border replaced with 0.
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(0, out_data[5]);
  // Center unchanged.
  EXPECT_EQ(50, out_data[2 * 5 + 2]);
}

TEST_F(NearblackLibTest, InPlaceModificationAndExistingDstDS) {
  const std::string test_dir = "/vsimem/nearblack_test/inplace";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string file_path = test_dir + "/inplace.tif";
  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, file_path.c_str(), 4, 4, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> data(16, 5);
    data[1 * 4 + 1] = 80;
    GDALRasterBandH band = GDALGetRasterBand(ds.get(), 1);
    ASSERT_EQ(CE_None, GDALRasterIO(band, GF_Write, 0, 0, 4, 4, data.data(), 4,
                                    4, GDT_Byte, 0, 0));
  }

  DatasetUniquePtr update_ds(GDALOpen(file_path.c_str(), GA_Update));
  ASSERT_NE(nullptr, update_ds);

  WithQuietHandler quiet_handler;
  std::vector<const char*> argv = {
      "-co", "TILED=YES", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH result = GDALNearblack(nullptr, update_ds.get(), update_ds.get(),
                                      options.get(), nullptr);
  EXPECT_EQ(update_ds.get(), result);

  std::vector<uint8_t> out_data(16, 0);
  GDALRasterBandH band = GDALGetRasterBand(update_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(band, GF_Read, 0, 0, 4, 4, out_data.data(), 4,
                                  4, GDT_Byte, 0, 0));
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(80, out_data[1 * 4 + 1]);
}

TEST_F(NearblackLibTest, DimensionMismatchFails) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 5, 5, 1, GDT_Byte, nullptr));
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "dst", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  ASSERT_NE(nullptr, dst);

  WithQuietHandler quiet_handler;
  GDALDatasetH result =
      GDALNearblack(nullptr, dst.get(), src.get(), nullptr, nullptr);
  EXPECT_EQ(nullptr, result);
}

TEST_F(NearblackLibTest, BandCountMismatchFails) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 4, 4, 1, GDT_Byte, nullptr));
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "dst", 4, 4, 3, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  ASSERT_NE(nullptr, dst);

  WithQuietHandler quiet_handler;
  GDALDatasetH result =
      GDALNearblack(nullptr, dst.get(), src.get(), nullptr, nullptr);
  EXPECT_EQ(nullptr, result);
}

TEST_F(NearblackLibTest, ColorBandCountMismatchFails) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  std::vector<const char*> argv = {
      "-color",
      "10,20,30",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  WithQuietHandler quiet_handler;
  GDALDatasetH result = GDALNearblack("/vsimem/out.tif", nullptr, src.get(),
                                      options.get(), nullptr);
  EXPECT_EQ(nullptr, result);
}

TEST_F(NearblackLibTest, SetAlphaCreateNewFile) {
  const std::string test_dir = "/vsimem/nearblack_test/setalpha";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 3 bands (RGB).
  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 4, 4, 3, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<const char*> argv = {
      "-setalpha",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(4, GDALGetRasterCount(dst_ds.get()));
  EXPECT_EQ(GCI_AlphaBand, GDALGetRasterColorInterpretation(
                               GDALGetRasterBand(dst_ds.get(), 4)));
}

TEST_F(NearblackLibTest, SetAlphaExistingDstWithoutAlphaFails) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 4, 4, 1, GDT_Byte, nullptr));
  DatasetUniquePtr dst(
      GDALCreate(mem_driver, "dst", 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  ASSERT_NE(nullptr, dst);
  // Last band of dst is GCI_Undefined, not GCI_AlphaBand.

  std::vector<const char*> argv = {
      "-setalpha",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  WithQuietHandler quiet_handler;
  GDALDatasetH result =
      GDALNearblack(nullptr, dst.get(), src.get(), options.get(), nullptr);
  EXPECT_EQ(nullptr, result);
}

TEST_F(NearblackLibTest, SetAlphaSourceWithAlphaBand) {
  const std::string test_dir = "/vsimem/nearblack_test/src_alpha";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 2 bands, band 2 is Alpha.
  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALSetRasterColorInterpretation(GDALGetRasterBand(src_ds.get(), 2),
                                   GCI_AlphaBand);

  std::vector<const char*> argv = {
      "-setalpha",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(2, GDALGetRasterCount(dst_ds.get()));
}

TEST_F(NearblackLibTest, SetMaskOption) {
  const std::string test_dir = "/vsimem/nearblack_test/setmask";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> data(16, 5);
  data[1 * 4 + 1] = 90;
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 4, 4, data.data(),
                                  4, 4, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-setmask",
      "-nb",
      "0",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  GDALRasterBandH mask_band =
      GDALGetMaskBand(GDALGetRasterBand(dst_ds.get(), 1));
  ASSERT_NE(nullptr, mask_band);

  std::vector<uint8_t> mask_data(16, 0);
  ASSERT_EQ(CE_None, GDALRasterIO(mask_band, GF_Read, 0, 0, 4, 4,
                                  mask_data.data(), 4, 4, GDT_Byte, 0, 0));
  // Border has mask 0, center has mask 255.
  EXPECT_EQ(0, mask_data[0]);
  EXPECT_EQ(255, mask_data[1 * 4 + 1]);
}

TEST_F(NearblackLibTest, SetMaskWithAlphaBandInSrcAndDst) {
  const std::string test_dir = "/vsimem/nearblack_test/mask_alpha";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALSetRasterColorInterpretation(GDALGetRasterBand(src_ds.get(), 2),
                                   GCI_AlphaBand);

  std::vector<const char*> argv = {
      "-setmask",
      "-nb",
      "0",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(1, GDALGetRasterCount(dst_ds.get()));
}

TEST_F(NearblackLibTest, ProgressCancellation) {
  const std::string test_dir = "/vsimem/nearblack_test/cancel";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 6, 6, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  OptionsUniquePtr options(GDALNearblackOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);

  auto cancel_callback = [](double progress, const char*, void*) -> int {
    if (progress > 0.0) {
      return 0;  // cancel
    }
    return 1;
  };
  GDALNearblackOptionsSetProgress(options.get(), cancel_callback, nullptr);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  EXPECT_EQ(nullptr, dst_ds);
}

TEST_F(NearblackLibTest, NonByteAndColorTableWarnings) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(mem_driver, "src_float", 4, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALColorTableH color_table = GDALCreateColorTable(GPI_RGB);
  ASSERT_NE(nullptr, color_table);
  GDALSetRasterColorTable(GDALGetRasterBand(src_ds.get(), 1), color_table);
  GDALDestroyColorTable(color_table);

  CPLErrorReset();
  WithQuietHandler quiet_handler;
  DatasetUniquePtr dst_ds(
      GDALNearblack("/vsimem/nearblack_lib_test/out_warn.tif", nullptr,
                    src_ds.get(), nullptr, nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("color table"));
}

TEST_F(NearblackLibTest, DriverResolutionFailures) {
  GDALDriverH mem_driver = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  DatasetUniquePtr src(
      GDALCreate(mem_driver, "src", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  WithQuietHandler quiet_handler;

  // Failed file creation (invalid directory path).
  GDALDatasetH ds1 = GDALNearblack("/invalid_dir_does_not_exist/out.tif",
                                   nullptr, src.get(), nullptr, nullptr);
  EXPECT_EQ(nullptr, ds1);

  // Non-existing format driver.
  std::vector<const char*> argv = {
      "-of",
      "NON_EXISTING_DRIVER",
      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH ds2 = GDALNearblack("/vsimem/out.tif", nullptr, src.get(),
                                   options.get(), nullptr);
  EXPECT_EQ(nullptr, ds2);
}

TEST_F(NearblackLibTest, EdgePixelHandling) {
  const std::string test_dir = "/vsimem/nearblack_test/edges";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 5, 5, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  // Line 0 has valid value at col 0, testing iLineFromTopOrBottom == 0 branch.
  // Center row has non-black at iStart = 0, testing i == iStart branch.
  std::vector<uint8_t> data = {
      100, 5, 5, 5, 5,   5, 5, 100, 5, 5, 100, 100, 100,
      5,   5, 5, 5, 100, 5, 5, 5,   5, 5, 5,   100,
  };
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 5, 5, data.data(),
                                  5, 5, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-nb", "2", "-near", "10", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
}

TEST_F(NearblackLibTest, ExistingDstWithAlphaAndMask) {
  const std::string test_dir = "/vsimem/nearblack_test/existing_alpha_mask";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // Src and Dst: 4 bands, band 4 is alpha.
  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  4, 4, 4, GDT_Byte, nullptr));
  DatasetUniquePtr dst(GDALCreate(gtiff_driver, (test_dir + "/dst.tif").c_str(),
                                  4, 4, 4, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  ASSERT_NE(nullptr, dst);
  GDALSetRasterColorInterpretation(GDALGetRasterBand(src.get(), 4),
                                   GCI_AlphaBand);
  GDALSetRasterColorInterpretation(GDALGetRasterBand(dst.get(), 4),
                                   GCI_AlphaBand);

  std::vector<const char*> argv = {
      "-setalpha", "-setmask", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH res =
      GDALNearblack(nullptr, dst.get(), src.get(), options.get(), nullptr);
  EXPECT_EQ(dst.get(), res);
}

TEST_F(NearblackLibTest, AlgFloodFillInvokedFromNearblack) {
  const std::string test_dir = "/vsimem/nearblack_test/floodfill_alg";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst(GDALNearblack((test_dir + "/dst.tif").c_str(), nullptr,
                                     src.get(), options.get(), nullptr));
  ASSERT_NE(nullptr, dst);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/nearblack_lib.cpp.
