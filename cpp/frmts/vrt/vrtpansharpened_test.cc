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
// Tests VRTPansharpenedDataset and VRTPansharpenedRasterBand XML parsing,
// pansharpening options, caching, spatial extent adjustment, SRS checks,
// overviews, and error branches (vrtpansharpened.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtpansharpened.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtpansharpened_test";

// TODO(schwehr): Extract ScopedErrorCapturer to util/error_handler.h.
class ScopedErrorCapturer {
 public:
  ScopedErrorCapturer() {
    CPLErrorReset();
    CPLPushErrorHandlerEx(&ScopedErrorCapturer::Callback, this);
  }
  ~ScopedErrorCapturer() {
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  bool has_failure() const { return has_failure_; }
  bool has_warning() const { return has_warning_; }

 private:
  static void CPL_STDCALL Callback(CPLErr err_class, int /*err_num*/,
                                   const char* /*msg*/) {
    auto* self =
        static_cast<ScopedErrorCapturer*>(CPLGetErrorHandlerUserData());
    if (self == nullptr) return;
    if (err_class == CE_Failure) {
      self->has_failure_ = true;
    } else if (err_class == CE_Warning) {
      self->has_warning_ = true;
    }
  }

  bool has_failure_ = false;
  bool has_warning_ = false;
};

GDALDatasetUniquePtr OpenRaster(const char* filename,
                                unsigned int extra_flags = 0) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(GDALOpenEx(
      filename, GDAL_OF_RASTER | extra_flags, nullptr, nullptr, nullptr)));
}

class VrtPansharpenedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateTiff(const char* filename, int xsize, int ysize, int bands,
                  GDALDataType dt, const double* gt, const char* srs_wkt,
                  const double* nodata,
                  const std::vector<std::vector<double>>& band_data,
                  int nbits = 0, std::span<const int> overviews = {}) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver);

    CPLStringList options;
    if (nbits > 0 && nbits != 8 && nbits != 16 && nbits != 32) {
      options.SetNameValue("NBITS", std::format("{}", nbits).c_str());
    }

    GDALDatasetUniquePtr ds(
        driver->Create(filename, xsize, ysize, bands, dt, options.List()));
    ASSERT_NE(nullptr, ds);

    if (gt != nullptr) {
      EXPECT_EQ(CE_None, ds->SetGeoTransform(GDALGeoTransform(gt)));
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      EXPECT_EQ(OGRERR_NONE, srs.importFromWkt(srs_wkt));
      EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    }

    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      ASSERT_NE(nullptr, band);
      if (nodata != nullptr) {
        EXPECT_EQ(CE_None, band->SetNoDataValue(nodata[b - 1]));
      }
      if (nbits > 0) {
        band->SetMetadataItem("NBITS", std::format("{}", nbits).c_str(),
                              "IMAGE_STRUCTURE");
      }
      if (b - 1 < static_cast<int>(band_data.size()) &&
          !band_data[b - 1].empty()) {
        const size_t total_pixels = static_cast<size_t>(xsize) * ysize;
        std::vector<double> write_buf(total_pixels, band_data[b - 1][0]);
        const size_t to_copy = std::min(total_pixels, band_data[b - 1].size());
        std::copy_n(band_data[b - 1].begin(), to_copy, write_buf.begin());
        EXPECT_EQ(CE_None,
                  band->RasterIO(GF_Write, 0, 0, xsize, ysize, write_buf.data(),
                                 xsize, ysize, GDT_Float64, 0, 0));
      }
    }

    if (!overviews.empty()) {
      EXPECT_EQ(CE_None, ds->BuildOverviews(
                             "NEAREST", static_cast<int>(overviews.size()),
                             overviews.data(), 0, nullptr, nullptr, nullptr));
    }
  }
};

TEST_F(VrtPansharpenedTest, GDALCreatePansharpenedVRTValidation) {
  WithQuietHandler quiet;

  // 1. Null XML string.
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALCreatePansharpenedVRT(
                         nullptr, reinterpret_cast<GDALRasterBandH>(1), 1,
                         reinterpret_cast<GDALRasterBandH*>(1)));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 2. Null panchromatic band.
  CPLErrorReset();
  EXPECT_EQ(nullptr,
            GDALCreatePansharpenedVRT("<VRTDataset/>", nullptr, 1,
                                      reinterpret_cast<GDALRasterBandH*>(1)));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 3. Null spectral bands array.
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALCreatePansharpenedVRT(
                         "<VRTDataset/>", reinterpret_cast<GDALRasterBandH>(1),
                         1, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 4. Malformed XML.
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALCreatePansharpenedVRT(
                         "<InvalidXML", reinterpret_cast<GDALRasterBandH>(1), 1,
                         reinterpret_cast<GDALRasterBandH*>(1)));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(VrtPansharpenedTest, GDALCreatePansharpenedVRTSuccessAndOptions) {
  constexpr char kPanFile[] = "/vsimem/vrtpansharpened_test/api_pan.tif";
  constexpr char kSpecFile[] = "/vsimem/vrtpansharpened_test/api_spec.tif";
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  std::vector<double> pan_data(64);
  for (int i = 0; i < 64; ++i) {
    pan_data[i] = 100.0 + i;
  }
  CreateTiff(kPanFile, 8, 8, 1, GDT_Float32, kPanGt, nullptr, nullptr,
             {pan_data});

  std::vector<double> spec_b1(16), spec_b2(16);
  for (int i = 0; i < 16; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff(kSpecFile, 4, 4, 2, GDT_Float32, kSpecGt, nullptr, nullptr,
             {spec_b1, spec_b2});

  GDALDatasetUniquePtr pan_ds = OpenRaster(kPanFile);
  ASSERT_NE(nullptr, pan_ds);
  GDALDatasetUniquePtr spec_ds = OpenRaster(kSpecFile);
  ASSERT_NE(nullptr, spec_ds);

  GDALRasterBandH pan_band = pan_ds->GetRasterBand(1);
  GDALRasterBandH spec_bands[2] = {spec_ds->GetRasterBand(1),
                                   spec_ds->GetRasterBand(2)};

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpectralBand dstBand='1'/>
    <SpectralBand dstBand='2'/>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds(GDALDataset::FromHandle(
      GDALCreatePansharpenedVRT(kVrtXml, pan_band, 2, spec_bands)));
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(8, vrt_ds->GetRasterXSize());
  EXPECT_EQ(8, vrt_ds->GetRasterYSize());
  EXPECT_EQ(2, vrt_ds->GetRasterCount());

  std::vector<float> buf(64);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(),
                                               8, 8, GDT_Float32, 0, 0));
  EXPECT_GT(buf[0], 0.0f);

  // Mismatched SpectralBand count relative to pahInputSpectralBands must fail.
  {
    WithQuietHandler quiet;
    constexpr char kTooManyBandsXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpectralBand dstBand='1'/>
    <SpectralBand dstBand='2'/>
    <SpectralBand dstBand='3'/>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreatePansharpenedVRT(kTooManyBandsXml, pan_band, 2,
                                                 spec_bands));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    constexpr char kTooFewBandsXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpectralBand dstBand='1'/>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreatePansharpenedVRT(kTooFewBandsXml, pan_band, 2,
                                                 spec_bands));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtPansharpenedTest, BasicPansharpening) {
  constexpr char kPanFile[] = "/vsimem/vrtpansharpened_test/basic_pan.tif";
  constexpr char kSpecFile[] = "/vsimem/vrtpansharpened_test/basic_spec.tif";
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  std::vector<double> pan_data(64);
  for (int i = 0; i < 64; ++i) {
    pan_data[i] = 100.0 + i;
  }
  CreateTiff(kPanFile, 8, 8, 1, GDT_Float32, kPanGt, nullptr, nullptr,
             {pan_data});

  std::vector<double> spec_r(16), spec_g(16), spec_b(16);
  for (int i = 0; i < 16; ++i) {
    spec_r[i] = 10.0 + i;
    spec_g[i] = 20.0 + i;
    spec_b[i] = 30.0 + i;
  }
  CreateTiff(kSpecFile, 4, 4, 3, GDT_Float32, kSpecGt, nullptr, nullptr,
             {spec_r, spec_g, spec_b});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='8' rasterYSize='8' subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/basic_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/basic_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/basic_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='3'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/basic_spec.tif</SourceFilename>
      <SourceBand>3</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(8, vrt_ds->GetRasterXSize());
  EXPECT_EQ(8, vrt_ds->GetRasterYSize());
  EXPECT_EQ(3, vrt_ds->GetRasterCount());

  auto* pan_band =
      dynamic_cast<VRTPansharpenedRasterBand*>(vrt_ds->GetRasterBand(1));
  ASSERT_NE(nullptr, pan_band);
  int bx = 0;
  int by = 0;
  cpl::down_cast<VRTPansharpenedDataset*>(vrt_ds.get())->GetBlockSize(&bx, &by);
  EXPECT_EQ(512, bx);
  EXPECT_EQ(512, by);

  EXPECT_STREQ("PIXEL",
               vrt_ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));

  std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> files(
      vrt_ds->GetFileList());
  ASSERT_NE(nullptr, files);
  EXPECT_GE(CSLCount(files.get()), 2);

  // AddBand is unsupported on VRTPansharpenedDataset.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, vrt_ds->AddBand(GDT_Float32, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  std::vector<float> all_data(8 * 8 * 3);
  int band_map[3] = {1, 2, 3};
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 8, 8, all_data.data(), 8, 8,
                             GDT_Float32, 3, band_map, 3 * sizeof(float),
                             8 * 3 * sizeof(float), sizeof(float), nullptr));
  EXPECT_GT(all_data[0], 0.0f);

  std::vector<float> sub_data(4 * 4 * 3);
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 2, 2, 4, 4, sub_data.data(), 4, 4,
                             GDT_Float32, 3, band_map, 3 * sizeof(float),
                             4 * 3 * sizeof(float), sizeof(float), nullptr));

  // Read-only pansharpened dataset and band reject GF_Write.
  EXPECT_EQ(CE_Failure,
            vrt_ds->RasterIO(GF_Write, 0, 0, 8, 8, all_data.data(), 8, 8,
                             GDT_Float32, 3, band_map, 0, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, vrt_ds->GetRasterBand(1)->RasterIO(
                            GF_Write, 0, 0, 8, 8, all_data.data(), 8, 8,
                            GDT_Float32, 0, 0));

  std::vector<float> band1(64);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 8, 8, band1.data(), 8, 8, GDT_Float32, 0, 0));
  EXPECT_GT(band1[0], 0.0f);
}

TEST_F(VrtPansharpenedTest, OptionsAndCustomization) {
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  CreateTiff("/vsimem/vrtpansharpened_test/opt_pan.tif", 8, 8, 1, GDT_UInt16,
             kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/opt_spec.tif", 4, 4, 3, GDT_UInt16,
             kSpecGt, nullptr, nullptr, {{10.0}, {20.0}, {30.0}});

  constexpr char kVrtPath[] = "/vsimem/vrtpansharpened_test/test_opt.vrt";
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <BlockXSize>16</BlockXSize>
  <BlockYSize>16</BlockYSize>
  <PansharpeningOptions>
    <Algorithm>WeightedBrovey</Algorithm>
    <AlgorithmOptions>
      <Weights>0.4,0.3,0.3</Weights>
    </AlgorithmOptions>
    <Resampling>Bilinear</Resampling>
    <NumThreads>ALL_CPUS</NumThreads>
    <BitDepth>12</BitDepth>
    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>
    <NoData>None</NoData>
    <PanchroBand>
      <SourceFilename relativeToVRT='1'>opt_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <OpenOptions>
        <OOI key='NUM_THREADS'>1</OOI>
      </OpenOptions>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <OpenOptions>
        <OOI key='NUM_THREADS'>1</OOI>
      </OpenOptions>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='3'>
      <SourceFilename relativeToVRT='1'>opt_spec.tif</SourceFilename>
      <SourceBand>3</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
  VsiMemTempWrapper vrt_file(kVrtPath, kVrtXml);

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtPath, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);

  int bx = 0;
  int by = 0;
  cpl::down_cast<VRTPansharpenedDataset*>(vrt_ds.get())->GetBlockSize(&bx, &by);
  EXPECT_EQ(16, bx);
  EXPECT_EQ(16, by);

  EXPECT_STREQ("12", vrt_ds->GetRasterBand(1)->GetMetadataItem(
                         "NBITS", "IMAGE_STRUCTURE"));

  char** xml_md = vrt_ds->GetMetadata("xml:VRT");
  ASSERT_NE(nullptr, xml_md);
  ASSERT_NE(nullptr, xml_md[0]);
  const std::string_view serialized(xml_md[0]);
  EXPECT_THAT(serialized, HasSubstr("<Algorithm>WeightedBrovey</Algorithm>"));
  EXPECT_THAT(serialized, HasSubstr("<NumThreads>ALL_CPUS</NumThreads>"));
  EXPECT_THAT(serialized, HasSubstr("<BitDepth>12</BitDepth>"));
  EXPECT_THAT(serialized, HasSubstr("<NoData>None</NoData>"));
  EXPECT_THAT(serialized, HasSubstr("relativeToVRT=\"1\""));

  constexpr char kRoundtripVrtPath[] =
      "/vsimem/vrtpansharpened_test/test_opt2.vrt";
  VsiMemTempWrapper roundtrip_file(kRoundtripVrtPath, std::string(serialized));
  GDALDatasetUniquePtr vrt_ds2 = OpenRaster(kRoundtripVrtPath);
  ASSERT_NE(nullptr, vrt_ds2);
}

TEST_F(VrtPansharpenedTest, OptionsNumThreadsNumericAndBitDepthInherited) {
  constexpr char kPanFile[] = "/vsimem/vrtpansharpened_test/threads_pan.tif";
  constexpr char kSpecFile[] = "/vsimem/vrtpansharpened_test/threads_spec.tif";
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  CreateTiff(kPanFile, 4, 4, 1, GDT_UInt16, kPanGt, nullptr, nullptr,
             {{100.0}});
  CreateTiff(kSpecFile, 2, 2, 2, GDT_UInt16, kSpecGt, nullptr, nullptr,
             {{10.0}, {20.0}}, 11);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <NumThreads>2</NumThreads>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/threads_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/threads_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/threads_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_STREQ("11", vrt_ds->GetRasterBand(1)->GetMetadataItem(
                         "NBITS", "IMAGE_STRUCTURE"));

  char** xml_md = vrt_ds->GetMetadata("xml:VRT");
  ASSERT_NE(nullptr, xml_md);
  ASSERT_NE(nullptr, xml_md[0]);
  EXPECT_THAT(xml_md[0], HasSubstr("<NumThreads>2</NumThreads>"));
}

TEST_F(VrtPansharpenedTest, NoDataHandling) {
  constexpr char kPanFile[] = "/vsimem/vrtpansharpened_test/nd_pan.tif";
  constexpr char kSpecFile[] = "/vsimem/vrtpansharpened_test/nd_spec.tif";
  constexpr char kPanNoneFile[] =
      "/vsimem/vrtpansharpened_test/nd_pan_none.tif";
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};
  constexpr double kNoData[2] = {-999.0, -999.0};

  CreateTiff(kPanFile, 4, 4, 1, GDT_Float32, kPanGt, nullptr, kNoData,
             {{100.0}});
  CreateTiff(kSpecFile, 2, 2, 2, GDT_Float32, kSpecGt, nullptr, kNoData,
             {{10.0}, {20.0}});
  CreateTiff(kPanNoneFile, 4, 4, 1, GDT_Float32, kPanGt, nullptr, nullptr,
             {{100.0}});

  // 1. Matching nodata on pan and spectral propagates to output.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    int has_nodata = FALSE;
    EXPECT_DOUBLE_EQ(-999.0,
                     vrt_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
    EXPECT_EQ(TRUE, has_nodata);
  }

  // 2. Explicit NoData=None disables output nodata.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <NoData>None</NoData>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    int has_nodata = TRUE;
    vrt_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
    EXPECT_EQ(FALSE, has_nodata);
  }

  // 3. Pan without nodata produces no output nodata.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_pan_none.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/nd_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    int has_nodata = TRUE;
    vrt_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata);
    EXPECT_EQ(FALSE, has_nodata);
  }
}

TEST_F(VrtPansharpenedTest, SpatialExtentAdjustmentUnion) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {4.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/vrtpansharpened_test/adj_pan.tif", 8, 8, 1, GDT_Float32,
             kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/adj_spec.tif", 4, 4, 2, GDT_Float32,
             kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(12, vrt_ds->GetRasterXSize());
  EXPECT_EQ(8, vrt_ds->GetRasterYSize());

  std::vector<float> data(12 * 8);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 0, 0, 12, 8, data.data(), 12, 8, GDT_Float32, 0, 0));
}

TEST_F(VrtPansharpenedTest, SpatialExtentAdjustmentIntersection) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {4.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/vrtpansharpened_test/adj_int_pan.tif", 8, 8, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/adj_int_spec.tif", 4, 4, 2,
             GDT_Float32, kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>Intersection</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_int_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_int_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_int_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(4, vrt_ds->GetRasterXSize());
  EXPECT_EQ(8, vrt_ds->GetRasterYSize());

  std::vector<float> data(4 * 8);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 8, data.data(),
                                               4, 8, GDT_Float32, 0, 0));
}

TEST_F(VrtPansharpenedTest, SpatialExtentAdjustmentNoneAndNoneWithoutWarning) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {0.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/vrtpansharpened_test/adj_none_pan.tif", 8, 8, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/adj_none_spec.tif", 4, 4, 2,
             GDT_Float32, kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});

  // 1. None.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>None</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }

  // 2. NoneWithoutWarning.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>NoneWithoutWarning</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }

  // 3. Unsupported adjustment name falls back to Union after emitting
  // CE_Failure.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>InvalidAdjustment</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_none_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds;
    {
      ScopedErrorCapturer errors;
      vrt_ds = OpenRaster(kVrtXml);
      EXPECT_TRUE(errors.has_failure());
    }
    ASSERT_NE(nullptr, vrt_ds);
  }
}

TEST_F(VrtPansharpenedTest, SpatialExtentAdjustmentFailures) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  constexpr double kSpecRotGt[6] = {2.0, 2.0, 0.5, 8.0, 0.5, -2.0};
  constexpr double kSpecDisjointGt[6] = {100.0, 2.0, 0.0, 8.0, 0.0, -2.0};
  CreateTiff("/vsimem/vrtpansharpened_test/adj_fail_pan.tif", 8, 8, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/adj_fail_rot.tif", 4, 4, 2,
             GDT_Float32, kSpecRotGt, nullptr, nullptr, {{10.0}, {20.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/adj_fail_disjoint.tif", 4, 4, 2,
             GDT_Float32, kSpecDisjointGt, nullptr, nullptr, {{10.0}, {20.0}});

  WithQuietHandler quiet;

  // 1. Rotational geotransform terms are rejected by Union adjustment.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>Union</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_rot.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_rot.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Non-overlapping datasets are rejected by Intersection adjustment.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>Intersection</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_disjoint.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/adj_fail_disjoint.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtPansharpenedTest, ProjectionHandling) {
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};

  OGRSpatialReference srs4326;
  OGRSpatialReference srs3857;
  ASSERT_EQ(OGRERR_NONE, srs4326.SetWellKnownGeogCS("WGS84"));
  ASSERT_EQ(OGRERR_NONE, srs3857.importFromEPSG(3857));
  char* wkt4326_raw = nullptr;
  char* wkt3857_raw = nullptr;
  ASSERT_EQ(OGRERR_NONE, srs4326.exportToWkt(&wkt4326_raw));
  std::unique_ptr<char, CplFreeDeleter> wkt4326(wkt4326_raw);
  ASSERT_EQ(OGRERR_NONE, srs3857.exportToWkt(&wkt3857_raw));
  std::unique_ptr<char, CplFreeDeleter> wkt3857(wkt3857_raw);

  CreateTiff("/vsimem/vrtpansharpened_test/srs_pan_4326.tif", 4, 4, 1,
             GDT_Float32, kPanGt, wkt4326.get(), nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/srs_pan_none.tif", 4, 4, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/srs_spec_3857.tif", 2, 2, 2,
             GDT_Float32, kSpecGt, wkt3857.get(), nullptr, {{10.0}, {20.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/srs_spec_none.tif", 2, 2, 2,
             GDT_Float32, kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});

  constexpr std::string_view kProjectionWarningCases[] = {
      // 1. Mismatched SRS between pan and spectral.
      R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_pan_4326.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_3857.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_3857.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt",
      // 2. Spectral band lacks SRS while pan has SRS.
      R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_pan_4326.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_none.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_none.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt",
      // 3. Pan band lacks SRS while spectral has SRS.
      R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_pan_none.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_3857.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/srs_spec_3857.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt",
  };

  for (const std::string_view vrt_xml : kProjectionWarningCases) {
    const std::string xml_str(vrt_xml);
    GDALDatasetUniquePtr vrt_ds;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      vrt_ds = OpenRaster(xml_str.c_str());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    }
    ASSERT_NE(nullptr, vrt_ds);
  }
}

TEST_F(VrtPansharpenedTest, CachingAndBlockIO) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {0.0, 2.0, 0.0, 10.0, 0.0, -2.0};

  std::vector<double> pan_data(100);
  for (int i = 0; i < 100; ++i) {
    pan_data[i] = 100.0 + i;
  }
  CreateTiff("/vsimem/vrtpansharpened_test/block_pan.tif", 10, 10, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {pan_data});

  std::vector<double> spec_b1(25), spec_b2(25);
  for (int i = 0; i < 25; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff("/vsimem/vrtpansharpened_test/block_spec.tif", 5, 5, 2,
             GDT_Float32, kSpecGt, nullptr, nullptr, {spec_b1, spec_b2});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <BlockXSize>8</BlockXSize>
  <BlockYSize>8</BlockYSize>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/block_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/block_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/block_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);

  // 1. Corner block (1, 1) exercises partial-block zero-padding (2x2 in 8x8).
  std::vector<float> block_buf(8 * 8);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->ReadBlock(1, 1, block_buf.data()));
  EXPECT_GT(block_buf[0], 0.0f);

  // 2. Scanline read caches all output bands for subsequent band reads.
  std::vector<float> line_b1(10);
  std::vector<float> line_b2(10);
  EXPECT_EQ(CE_None, vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 10, 1,
                                                        line_b1.data(), 10, 1,
                                                        GDT_Float32, 0, 0));
  EXPECT_EQ(CE_None, vrt_ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 10, 1,
                                                        line_b2.data(), 10, 1,
                                                        GDT_Float32, 0, 0));
  EXPECT_GT(line_b1[0], 0.0f);
  EXPECT_GT(line_b2[0], 0.0f);

  // 3. Sub-window caching across bands.
  std::vector<float> sub_b1(4);
  std::vector<float> sub_b2(4);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(
                GF_Read, 2, 2, 2, 2, sub_b1.data(), 2, 2, GDT_Float32, 0, 0));
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(2)->RasterIO(
                GF_Read, 2, 2, 2, 2, sub_b2.data(), 2, 2, GDT_Float32, 0, 0));
}

TEST_F(VrtPansharpenedTest, Overviews) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {0.0, 2.0, 0.0, 16.0, 0.0, -2.0};
  constexpr int kOvrFactors[] = {2};

  std::vector<double> pan_data(256);
  for (int i = 0; i < 256; ++i) {
    pan_data[i] = 100.0 + i;
  }
  CreateTiff("/vsimem/vrtpansharpened_test/ovr_pan.tif", 16, 16, 1, GDT_Float32,
             kPanGt, nullptr, nullptr, {pan_data}, 0, kOvrFactors);

  std::vector<double> spec_b1(64), spec_b2(64);
  for (int i = 0; i < 64; ++i) {
    spec_b1[i] = 10.0 + i;
    spec_b2[i] = 20.0 + i;
  }
  CreateTiff("/vsimem/vrtpansharpened_test/ovr_spec.tif", 8, 8, 2, GDT_Float32,
             kSpecGt, nullptr, nullptr, {spec_b1, spec_b2}, 0, kOvrFactors);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/ovr_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/ovr_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
    <SpectralBand dstBand='2'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/ovr_spec.tif</SourceFilename>
      <SourceBand>2</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);

  GDALRasterBand* band = vrt_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(1, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetOverview(-1));
  EXPECT_EQ(nullptr, band->GetOverview(99));

  GDALRasterBand* ovr_band = band->GetOverview(0);
  ASSERT_NE(nullptr, ovr_band);
  EXPECT_EQ(8, ovr_band->GetXSize());
  EXPECT_EQ(8, ovr_band->GetYSize());

  std::vector<float> ovr_data(64);
  EXPECT_EQ(CE_None, ovr_band->RasterIO(GF_Read, 0, 0, 8, 8, ovr_data.data(), 8,
                                        8, GDT_Float32, 0, 0));
  EXPECT_GT(ovr_data[0], 0.0f);

  std::vector<float> ds_data(8 * 8 * 2);
  int band_map[2] = {1, 2};
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 16, 16, ds_data.data(), 8, 8,
                             GDT_Float32, 2, band_map, 0, 0, 0, nullptr));
  EXPECT_GT(ds_data[0], 0.0f);

  std::vector<float> ds_band_data(8 * 8);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 16, 16, ds_band_data.data(),
                                    8, 8, GDT_Float32, 0, 0));
  EXPECT_GT(ds_band_data[0], 0.0f);
}

TEST_F(VrtPansharpenedTest, OverviewsWithNonPansharpenBand) {
  constexpr double kPanGt[6] = {0.0, 1.0, 0.0, 16.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {0.0, 2.0, 0.0, 16.0, 0.0, -2.0};
  constexpr int kOvrFactors[] = {2};
  CreateTiff("/vsimem/vrtpansharpened_test/ovr_np_pan.tif", 16, 16, 1,
             GDT_Float32, kPanGt, nullptr, nullptr, {{100.0}}, 0, kOvrFactors);
  CreateTiff("/vsimem/vrtpansharpened_test/ovr_np_spec.tif", 8, 8, 1,
             GDT_Float32, kSpecGt, nullptr, nullptr, {{10.0}}, 0, kOvrFactors);

  // Presence of a non-pansharpened band suppresses dataset virtual overviews.
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/ovr_np_pan.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/ovr_np_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
  <VRTRasterBand dataType='Float32' band='1' subClass='VRTPansharpenedRasterBand'/>
  <VRTRasterBand dataType='Float32' band='2'/>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(0, vrt_ds->GetRasterBand(1)->GetOverviewCount());
}

TEST_F(VrtPansharpenedTest, ErrorBranches) {
  constexpr double kPanGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kSpecGt[6] = {100.0, 2.0, 0.0, 1000.0, 0.0, -2.0};
  constexpr double kSpecInvertedGt[6] = {100.0, 2.0, 0.0, -1000.0, 0.0, 2.0};
  CreateTiff("/vsimem/vrtpansharpened_test/err_pan.tif", 4, 4, 1, GDT_Float32,
             kPanGt, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/err_spec.tif", 2, 2, 2, GDT_Float32,
             kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/err_pan_nogt.tif", 4, 4, 1,
             GDT_Float32, nullptr, nullptr, nullptr, {{100.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/err_spec_nogt.tif", 2, 2, 2,
             GDT_Float32, nullptr, nullptr, nullptr, {{10.0}, {20.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/err_spec_inverted.tif", 2, 2, 2,
             GDT_Float32, kSpecInvertedGt, nullptr, nullptr, {{10.0}, {20.0}});
  CreateTiff("/vsimem/vrtpansharpened_test/err_spec_large.tif", 8, 8, 2,
             GDT_Float32, kSpecGt, nullptr, nullptr, {{10.0}, {20.0}});

  constexpr std::string_view kInvalidXmlCases[] = {
      // 1. Missing PansharpeningOptions.
      "<VRTDataset subClass='VRTPansharpenedDataset'/>",
      // 2. Deprecated MSShiftX / MSShiftY.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<MSShiftX>1.0</MSShiftX></PansharpeningOptions></VRTDataset>",
      // 3. Missing PanchroBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<SpectralBand dstBand='1'><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>"
      "</SpectralBand></PansharpeningOptions></VRTDataset>",
      // 4. Empty PanchroBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand/></PansharpeningOptions></VRTDataset>",
      // 5. Nonexistent PanchroBand file.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/"
      "nonexistent.tif"
      "</SourceFilename></PanchroBand></PansharpeningOptions></VRTDataset>",
      // 6. Invalid SourceBand on PanchroBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename><SourceBand>99</SourceBand></PanchroBand>"
      "</PansharpeningOptions></VRTDataset>",
      // 7. Unsupported Algorithm.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<Algorithm>UnsupportedAlg</Algorithm><PanchroBand><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_pan.tif</SourceFilename></PanchroBand>"
      "</PansharpeningOptions></VRTDataset>",
      // 8. Missing geotransform on panchro dataset.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_pan_nogt.tif</SourceFilename>"
      "</PanchroBand><SpectralBand dstBand='1'><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>"
      "</SpectralBand></PansharpeningOptions></VRTDataset>",
      // 9. Missing SourceFilename on SpectralBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'/>"
      "</PansharpeningOptions></VRTDataset>",
      // 10. Nonexistent file on SpectralBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/nonexistent.tif"
      "</SourceFilename></SpectralBand></PansharpeningOptions></VRTDataset>",
      // 11. Missing geotransform on SpectralBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec_nogt.tif"
      "</SourceFilename></SpectralBand></PansharpeningOptions></VRTDataset>",
      // 12. Inverted vertical orientation between spectral and pan.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec_inverted.tif"
      "</SourceFilename></SpectralBand></PansharpeningOptions></VRTDataset>",
      // 13. Inconsistent declared dimensions with panchro dataset (8x8 vs 4x4).
      "<VRTDataset rasterXSize='8' rasterYSize='8' "
      "subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename></SpectralBand></PansharpeningOptions></VRTDataset>",
      // 14. Invalid dstBand <= 0.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='0'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename></SpectralBand></PansharpeningOptions></VRTDataset>",
      // 15. Invalid SourceBand on spectral dataset.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>99</SourceBand></SpectralBand>"
      "</PansharpeningOptions></VRTDataset>",
      // 16. Duplicate dstBand mapping.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>1</SourceBand></SpectralBand>"
      "<SpectralBand dstBand='1'><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>"
      "<SourceBand>2</SourceBand></SpectralBand></PansharpeningOptions>"
      "</VRTDataset>",
      // 17. No spectral bands defined.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand></PansharpeningOptions></VRTDataset>",
      // 18. Hole in dstBand numbering (dstBand 1 and 3 without 2).
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>1</SourceBand></SpectralBand>"
      "<SpectralBand dstBand='3'><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>"
      "<SourceBand>2</SourceBand></SpectralBand></PansharpeningOptions>"
      "</VRTDataset>",
      // 19. Mismatched Weights count (1 weight for 2 bands).
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<AlgorithmOptions><Weights>0.5</Weights></AlgorithmOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>1</SourceBand></SpectralBand>"
      "<SpectralBand dstBand='2'><SourceFilename>"
      "/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>"
      "<SourceBand>2</SourceBand></SpectralBand></PansharpeningOptions>"
      "</VRTDataset>",
      // 20. Explicit VRTPansharpenedRasterBand without matching SpectralBand.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='1'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>1</SourceBand></SpectralBand>"
      "</PansharpeningOptions><VRTRasterBand dataType='Float32' band='1' "
      "subClass='VRTPansharpenedRasterBand'/><VRTRasterBand "
      "dataType='Float32' band='2' subClass='VRTPansharpenedRasterBand'/>"
      "</VRTDataset>",
      // 21. Explicit dstBand > declared VRTRasterBand count.
      "<VRTDataset subClass='VRTPansharpenedDataset'><PansharpeningOptions>"
      "<PanchroBand><SourceFilename>/vsimem/vrtpansharpened_test/err_pan.tif"
      "</SourceFilename></PanchroBand><SpectralBand dstBand='3'>"
      "<SourceFilename>/vsimem/vrtpansharpened_test/err_spec.tif"
      "</SourceFilename><SourceBand>1</SourceBand></SpectralBand>"
      "</PansharpeningOptions><VRTRasterBand dataType='Float32' band='1' "
      "subClass='VRTPansharpenedRasterBand'/></VRTDataset>",
  };

  {
    WithQuietHandler quiet;
    for (const std::string_view xml : kInvalidXmlCases) {
      const std::string xml_str(xml);
      CPLErrorReset();
      EXPECT_EQ(nullptr, OpenRaster(xml_str.c_str()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }

  // 22. Spectral band dimensions larger than panchro band emits a warning.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <SpatialExtentAdjustment>NoneWithoutWarning</SpatialExtentAdjustment>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/err_pan.tif</SourceFilename>
    </PanchroBand>
    <SpectralBand dstBand='1'>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/err_spec_large.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds;
    {
      ScopedErrorCapturer errors;
      vrt_ds = OpenRaster(kVrtXml);
      EXPECT_TRUE(errors.has_warning());
    }
    ASSERT_NE(nullptr, vrt_ds);
  }

  // 23. Unmapped output band (omitted dstBand) emits a warning and succeeds.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTPansharpenedDataset'>
  <PansharpeningOptions>
    <PanchroBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/err_pan.tif</SourceFilename>
    </PanchroBand>
    <SpectralBand>
      <SourceFilename relativeToVRT='0'>/vsimem/vrtpansharpened_test/err_spec.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SpectralBand>
  </PansharpeningOptions>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds;
    {
      ScopedErrorCapturer errors;
      vrt_ds = OpenRaster(kVrtXml);
      EXPECT_TRUE(errors.has_warning());
    }
    ASSERT_NE(nullptr, vrt_ds);
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and multi-band buffer layout
// edge cases in VRTPansharpenedDataset in vrtpansharpened.cpp.
