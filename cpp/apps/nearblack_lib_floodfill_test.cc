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
// Tests for Nearblack floodfill algorithm (apps/nearblack_lib_floodfill.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/nearblack_lib_floodfill.cpp

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

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

class NearblackLibFloodfillTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }
};

TEST_F(NearblackLibFloodfillTest, Basic8BitFloodFillAndPondPreserved) {
  const std::string test_dir = "/vsimem/nearblack_ff/basic8";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  // 8x8 image:
  // Border is 5 (near black <= 15).
  // Interior (x in 1..6, y in 1..6) is 100.
  // At (4, 4) there is an isolated black pixel (0).
  std::vector<uint8_t> data(64, 5);
  for (int y = 1; y <= 6; ++y) {
    for (int x = 1; x <= 6; ++x) {
      data[y * 8 + x] = 100;
    }
  }
  data[4 * 8 + 4] = 0;  // isolated black pond inside interior

  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 8, 8, data.data(),
                                  8, 8, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(64, 255);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 8, 8,
                                  out_data.data(), 8, 8, GDT_Byte, 0, 0));

  // Border was flooded to 0.
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(0, out_data[7]);
  EXPECT_EQ(0, out_data[56]);
  EXPECT_EQ(0, out_data[63]);

  // Interior 100 remains 100.
  EXPECT_EQ(100, out_data[1 * 8 + 1]);
  EXPECT_EQ(100, out_data[2 * 8 + 2]);

  // The isolated pond at (4, 4) was NOT connected to border, so it remains 0
  // (not flooded).
  EXPECT_EQ(0, out_data[4 * 8 + 4]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillWithTwoPassesPrePass) {
  const std::string test_dir = "/vsimem/nearblack_ff/nb_twopasses";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> data(100, 5);
  for (int y = 3; y <= 7; ++y) {
    for (int x = 3; x <= 7; ++x) {
      data[y * 10 + x] = 150;
    }
  }
  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 10, 10, data.data(),
                                  10, 10, GDT_Byte, 0, 0));

  // -nb 2 triggers two-pass algorithm first, then flood fill pass.
  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "2", "-near", "10", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(100, 0);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 10, 10,
                                  out_data.data(), 10, 10, GDT_Byte, 0, 0));
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(150, out_data[5 * 10 + 5]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillWhiteBorder) {
  const std::string test_dir = "/vsimem/nearblack_ff/white";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 6, 6, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> data(36, 250);  // near white <= 255
  data[2 * 6 + 2] = 40;
  data[3 * 6 + 3] = 40;

  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 6, 6, data.data(),
                                  6, 6, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-white", "-nb", "0", "-near", "10", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(36, 0);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 6, 6,
                                  out_data.data(), 6, 6, GDT_Byte, 0, 0));
  // White border replaced with 255.
  EXPECT_EQ(255, out_data[0]);
  EXPECT_EQ(255, out_data[5]);
  EXPECT_EQ(40, out_data[2 * 6 + 2]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillCustomAndMultipleColors) {
  const std::string test_dir = "/vsimem/nearblack_ff/colors";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 6, 6, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> data(36, 122);  // near 120
  for (int y = 2; y <= 3; ++y) {
    for (int x = 2; x <= 3; ++x) {
      data[y * 6 + x] = 200;
    }
  }

  GDALRasterBandH src_band = GDALGetRasterBand(src_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(src_band, GF_Write, 0, 0, 6, 6, data.data(),
                                  6, 6, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-alg",  "floodfill", "-color", "120", "-color", "10",
      "-near", "5",         "-nb",    "0",   nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> out_data(36, 0);
  GDALRasterBandH dst_band = GDALGetRasterBand(dst_ds.get(), 1);
  ASSERT_EQ(CE_None, GDALRasterIO(dst_band, GF_Read, 0, 0, 6, 6,
                                  out_data.data(), 6, 6, GDT_Byte, 0, 0));
  EXPECT_EQ(0, out_data[0]);
  EXPECT_EQ(200, out_data[2 * 6 + 2]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillMultiBandRGB) {
  const std::string test_dir = "/vsimem/nearblack_ff/rgb";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 6, 6, 3, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> r(36, 5), g(36, 5), b(36, 5);
  r[2 * 6 + 2] = 100;
  g[2 * 6 + 2] = 150;
  b[2 * 6 + 2] = 200;

  ASSERT_EQ(CE_None, GDALRasterIO(GDALGetRasterBand(src_ds.get(), 1), GF_Write,
                                  0, 0, 6, 6, r.data(), 6, 6, GDT_Byte, 0, 0));
  ASSERT_EQ(CE_None, GDALRasterIO(GDALGetRasterBand(src_ds.get(), 2), GF_Write,
                                  0, 0, 6, 6, g.data(), 6, 6, GDT_Byte, 0, 0));
  ASSERT_EQ(CE_None, GDALRasterIO(GDALGetRasterBand(src_ds.get(), 3), GF_Write,
                                  0, 0, 6, 6, b.data(), 6, 6, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-color", "0,0,0", "-near",
      "10",   "-nb",       "0",      nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(3, GDALGetRasterCount(dst_ds.get()));

  std::vector<uint8_t> out_r(36, 255);
  ASSERT_EQ(CE_None,
            GDALRasterIO(GDALGetRasterBand(dst_ds.get(), 1), GF_Read, 0, 0, 6,
                         6, out_r.data(), 6, 6, GDT_Byte, 0, 0));
  EXPECT_EQ(0, out_r[0]);
  EXPECT_EQ(100, out_r[2 * 6 + 2]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillSetAlphaAndSetMask) {
  const std::string test_dir = "/vsimem/nearblack_ff/alpha_mask";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  const std::string src_path = test_dir + "/src.tif";
  const std::string dst_path = test_dir + "/dst.tif";

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src_ds(
      GDALCreate(gtiff_driver, src_path.c_str(), 6, 6, 3, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  std::vector<uint8_t> band_data(36, 5);
  band_data[2 * 6 + 2] = 80;
  for (int i = 1; i <= 3; ++i) {
    ASSERT_EQ(CE_None,
              GDALRasterIO(GDALGetRasterBand(src_ds.get(), i), GF_Write, 0, 0,
                           6, 6, band_data.data(), 6, 6, GDT_Byte, 0, 0));
  }

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-setalpha", "-setmask", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst_ds(GDALNearblack(dst_path.c_str(), nullptr, src_ds.get(),
                                        options.get(), nullptr));
  ASSERT_NE(nullptr, dst_ds);
  EXPECT_EQ(4, GDALGetRasterCount(dst_ds.get()));

  // Alpha band (band 4)
  GDALRasterBandH alpha_band = GDALGetRasterBand(dst_ds.get(), 4);
  EXPECT_EQ(GCI_AlphaBand, GDALGetRasterColorInterpretation(alpha_band));
  std::vector<uint8_t> alpha_data(36, 128);
  ASSERT_EQ(CE_None, GDALRasterIO(alpha_band, GF_Read, 0, 0, 6, 6,
                                  alpha_data.data(), 6, 6, GDT_Byte, 0, 0));
  EXPECT_EQ(0, alpha_data[0]);
  EXPECT_EQ(255, alpha_data[2 * 6 + 2]);

  // Mask band
  GDALRasterBandH mask_band =
      GDALGetMaskBand(GDALGetRasterBand(dst_ds.get(), 1));
  ASSERT_NE(nullptr, mask_band);
  std::vector<uint8_t> mask_data(36, 128);
  ASSERT_EQ(CE_None, GDALRasterIO(mask_band, GF_Read, 0, 0, 6, 6,
                                  mask_data.data(), 6, 6, GDT_Byte, 0, 0));
  EXPECT_EQ(0, mask_data[0]);
  EXPECT_EQ(255, mask_data[2 * 6 + 2]);
}

TEST_F(NearblackLibFloodfillTest, FloodFillUInt16AndFloat) {
  const std::string test_dir = "/vsimem/nearblack_ff/uint16_float";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // UInt16 dataset
  {
    const std::string u16_src = test_dir + "/src_u16.tif";
    const std::string u16_dst = test_dir + "/dst_u16.tif";
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, u16_src.c_str(), 4, 4, 1,
                                   GDT_UInt16, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint16_t> data(16, 5);
    data[1 * 4 + 1] = 500;
    ASSERT_EQ(CE_None,
              GDALRasterIO(GDALGetRasterBand(ds.get(), 1), GF_Write, 0, 0, 4, 4,
                           data.data(), 4, 4, GDT_UInt16, 0, 0));

    WithQuietHandler quiet_handler;
    std::vector<const char*> argv = {
        "-alg", "floodfill", "-nb", "0", nullptr,
    };
    OptionsUniquePtr options(
        GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, options);
    DatasetUniquePtr dst(GDALNearblack(u16_dst.c_str(), nullptr, ds.get(),
                                       options.get(), nullptr));
    ASSERT_NE(nullptr, dst);
  }

  // Float32 dataset
  {
    const std::string f32_src = test_dir + "/src_f32.tif";
    const std::string f32_dst = test_dir + "/dst_f32.tif";
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, f32_src.c_str(), 4, 4, 1,
                                   GDT_Float32, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<float> data(16, 5.0f);
    data[1 * 4 + 1] = 200.0f;
    ASSERT_EQ(CE_None,
              GDALRasterIO(GDALGetRasterBand(ds.get(), 1), GF_Write, 0, 0, 4, 4,
                           data.data(), 4, 4, GDT_Float32, 0, 0));

    WithQuietHandler quiet_handler;
    std::vector<const char*> argv = {
        "-alg", "floodfill", "-nb", "0", nullptr,
    };
    OptionsUniquePtr options(
        GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, options);
    DatasetUniquePtr dst(GDALNearblack(f32_dst.c_str(), nullptr, ds.get(),
                                       options.get(), nullptr));
    ASSERT_NE(nullptr, dst);
  }
}

TEST_F(NearblackLibFloodfillTest, FloodFillInPlaceAndSeparateExistingDst) {
  const std::string test_dir = "/vsimem/nearblack_ff/inplace_sep";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // In-place
  const std::string inplace_path = test_dir + "/inplace.tif";
  {
    DatasetUniquePtr ds(GDALCreate(gtiff_driver, inplace_path.c_str(), 5, 5, 1,
                                   GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> data(25, 5);
    data[2 * 5 + 2] = 80;
    ASSERT_EQ(CE_None,
              GDALRasterIO(GDALGetRasterBand(ds.get(), 1), GF_Write, 0, 0, 5, 5,
                           data.data(), 5, 5, GDT_Byte, 0, 0));
  }
  DatasetUniquePtr update_ds(GDALOpen(inplace_path.c_str(), GA_Update));
  ASSERT_NE(nullptr, update_ds);

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH res = GDALNearblack(nullptr, update_ds.get(), update_ds.get(),
                                   options.get(), nullptr);
  EXPECT_EQ(update_ds.get(), res);

  // Separate existing destination
  const std::string sep_dst_path = test_dir + "/sep_dst.tif";
  DatasetUniquePtr sep_dst(GDALCreate(gtiff_driver, sep_dst_path.c_str(), 5, 5,
                                      1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, sep_dst);

  GDALDatasetH res2 = GDALNearblack(nullptr, sep_dst.get(), update_ds.get(),
                                    options.get(), nullptr);
  EXPECT_EQ(sep_dst.get(), res2);
}

TEST_F(NearblackLibFloodfillTest, TempDriversGTiffAndMemAndInvalid) {
  const std::string test_dir = "/vsimem/nearblack_ff/temp_drivers";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() {
    CPLSetConfigOption("GDAL_TEMP_DRIVER_NAME", nullptr);
    VSIRmdirRecursive(test_dir.c_str());
  };

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

  // 1. Force GTiff as temp driver
  CPLSetConfigOption("GDAL_TEMP_DRIVER_NAME", "GTiff");
  DatasetUniquePtr dst1(GDALNearblack((test_dir + "/dst1.tif").c_str(), nullptr,
                                      src.get(), options.get(), nullptr));
  ASSERT_NE(nullptr, dst1);

  // 2. Force MEM as temp driver
  CPLSetConfigOption("GDAL_TEMP_DRIVER_NAME", "MEM");
  DatasetUniquePtr dst2(GDALNearblack((test_dir + "/dst2.tif").c_str(), nullptr,
                                      src.get(), options.get(), nullptr));
  ASSERT_NE(nullptr, dst2);

  // 3. Force non-existing temp driver -> failure
  WithQuietHandler quiet_handler;
  CPLSetConfigOption("GDAL_TEMP_DRIVER_NAME", "NON_EXISTING_DRIVER");
  DatasetUniquePtr dst3(GDALNearblack((test_dir + "/dst3.tif").c_str(), nullptr,
                                      src.get(), options.get(), nullptr));
  EXPECT_EQ(nullptr, dst3);

  CPLSetConfigOption("GDAL_TEMP_DRIVER_NAME", nullptr);
}

TEST_F(NearblackLibFloodfillTest, ProgressCancellation) {
  const std::string test_dir = "/vsimem/nearblack_ff/cancel";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  auto cancel_callback = [](double progress, const char*, void*) -> int {
    return (progress > 0.0) ? 0 : 1;
  };
  GDALNearblackOptionsSetProgress(options.get(), cancel_callback, nullptr);

  DatasetUniquePtr dst(GDALNearblack((test_dir + "/dst.tif").c_str(), nullptr,
                                     src.get(), options.get(), nullptr));
  EXPECT_EQ(nullptr, dst);
}

TEST_F(NearblackLibFloodfillTest, ComplexGeometryLabyrinth) {
  const std::string test_dir = "/vsimem/nearblack_ff/labyrinth";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 12x12 labyrinth that forces floodfill to branch, move up/down, left/right
  const int nSize = 12;
  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  nSize, nSize, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  // Fill with 100 (non-black)
  std::vector<uint8_t> data(nSize * nSize, 100);

  // Carve black paths from border inwards in multiple directions:
  // Top border: (0,0) to (5,0)
  for (int x = 0; x <= 5; ++x) data[0 * nSize + x] = 0;
  // Path down at x=5
  for (int y = 0; y <= 8; ++y) data[y * nSize + 5] = 0;
  // Path left at y=8
  for (int x = 2; x <= 5; ++x) data[8 * nSize + x] = 0;
  // Path up at x=2
  for (int y = 3; y <= 8; ++y) data[y * nSize + 2] = 0;

  // Bottom border: (6, 11) to (11, 11)
  for (int x = 6; x < nSize; ++x) data[11 * nSize + x] = 0;
  // Path up at x=6
  for (int y = 5; y <= 11; ++y) data[y * nSize + 6] = 0;

  ASSERT_EQ(CE_None,
            GDALRasterIO(GDALGetRasterBand(src.get(), 1), GF_Write, 0, 0, nSize,
                         nSize, data.data(), nSize, nSize, GDT_Byte, 0, 0));

  std::vector<const char*> argv = {
      "-alg", "floodfill", "-nb", "0", nullptr,
  };
  OptionsUniquePtr options(
      GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
  ASSERT_NE(nullptr, options);

  DatasetUniquePtr dst(GDALNearblack((test_dir + "/dst.tif").c_str(), nullptr,
                                     src.get(), options.get(), nullptr));
  ASSERT_NE(nullptr, dst);

  std::vector<uint8_t> out(nSize * nSize, 255);
  ASSERT_EQ(CE_None,
            GDALRasterIO(GDALGetRasterBand(dst.get(), 1), GF_Read, 0, 0, nSize,
                         nSize, out.data(), nSize, nSize, GDT_Byte, 0, 0));

  // The carved labyrinth path from border should be flooded to 0.
  EXPECT_EQ(0, out[0 * nSize + 0]);
  EXPECT_EQ(0, out[5 * nSize + 5]);
  EXPECT_EQ(0, out[8 * nSize + 3]);
  EXPECT_EQ(0, out[4 * nSize + 2]);
  // An uncarved cell remains 100
  EXPECT_EQ(100, out[6 * nSize + 1]);
}

TEST_F(NearblackLibFloodfillTest, FillFromSideAndBottomBorders) {
  const std::string test_dir = "/vsimem/nearblack_ff/sides_bottom";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  // 6x6 image where top line (y=0) is entirely white (100)
  // Left border (x=0, y=1..5) is black (0)
  // Right border (x=5, y=1..5) is black (0)
  // Bottom border (y=5, x=0..5) is black (0)
  // Interior is 100
  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  6, 6, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  std::vector<uint8_t> data(36, 100);
  for (int y = 1; y < 6; ++y) {
    data[y * 6 + 0] = 0;  // left side
    data[y * 6 + 5] = 0;  // right side
  }
  for (int x = 0; x < 6; ++x) {
    data[5 * 6 + x] = 0;  // bottom
  }

  ASSERT_EQ(CE_None, GDALRasterIO(GDALGetRasterBand(src.get(), 1), GF_Write, 0,
                                  0, 6, 6, data.data(), 6, 6, GDT_Byte, 0, 0));

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

TEST_F(NearblackLibFloodfillTest, ProgressCancelAtFinalAndInTwoPasses) {
  const std::string test_dir = "/vsimem/nearblack_ff/cancel_stages";
  VSIMkdir(test_dir.c_str(), 0755);
  absl::Cleanup cleanup = [&]() { VSIRmdirRecursive(test_dir.c_str()); };

  GDALDriverH gtiff_driver = GDALGetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);

  DatasetUniquePtr src(GDALCreate(gtiff_driver, (test_dir + "/src.tif").c_str(),
                                  4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);

  // 1. Cancel at progress == 1.0 in flood fill
  {
    std::vector<const char*> argv = {
        "-alg", "floodfill", "-nb", "0", nullptr,
    };
    OptionsUniquePtr options(
        GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, options);
    auto cancel_at_1 = [](double progress, const char*, void*) -> int {
      return (progress >= 1.0) ? 0 : 1;
    };
    GDALNearblackOptionsSetProgress(options.get(), cancel_at_1, nullptr);
    DatasetUniquePtr dst(GDALNearblack((test_dir + "/dst1.tif").c_str(),
                                       nullptr, src.get(), options.get(),
                                       nullptr));
    EXPECT_EQ(nullptr, dst);
  }

  // 2. Cancel in TwoPasses pre-pass when -nb > 0
  {
    std::vector<const char*> argv = {
        "-alg", "floodfill", "-nb", "2", nullptr,
    };
    OptionsUniquePtr options(
        GDALNearblackOptionsNew(const_cast<char**>(argv.data()), nullptr));
    ASSERT_NE(nullptr, options);
    auto cancel_early = [](double progress, const char*, void*) -> int {
      return (progress > 0.1) ? 0 : 1;
    };
    GDALNearblackOptionsSetProgress(options.get(), cancel_early, nullptr);
    DatasetUniquePtr dst(GDALNearblack((test_dir + "/dst2.tif").c_str(),
                                       nullptr, src.get(), options.get(),
                                       nullptr));
    EXPECT_EQ(nullptr, dst);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/nearblack_lib_floodfill.cpp.
