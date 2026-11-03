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
// Tests VRTWarpedDataset, VRTWarpedRasterBand, GDALAutoCreateWarpedVRT,
// GDALAutoCreateWarpedVRTEx, resampling, overviews, cutlines, and XML
// serialization (vrtwarped.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtwarped.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/vrtwarped_test";

// TODO(schwehr): Extract ScopedWarningCapturer to util/error_handler.h.
class ScopedWarningCapturer {
 public:
  ScopedWarningCapturer() {
    CPLErrorReset();
    CPLPushErrorHandlerEx(&ScopedWarningCapturer::Callback, this);
  }
  ~ScopedWarningCapturer() {
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  bool has_warning() const { return has_warning_; }

 private:
  static void CPL_STDCALL Callback(CPLErr err_class, int /*err_num*/,
                                   const char* /*msg*/) {
    auto* self =
        static_cast<ScopedWarningCapturer*>(CPLGetErrorHandlerUserData());
    if (self != nullptr && err_class == CE_Warning) {
      self->has_warning_ = true;
    }
  }

  bool has_warning_ = false;
};

using WarpOptionsPtr =
    std::unique_ptr<GDALWarpOptions, decltype([](GDALWarpOptions* p) {
                      GDALDestroyWarpOptions(p);
                    })>;

GDALDatasetUniquePtr OpenRaster(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(GDALOpenEx(
      filename, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
}

class VrtWarpedTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_VRT();
    GDALRegister_GTiff();
  }

  static void TearDownTestSuite() { OSRCleanup(); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  WarpOptionsPtr CreateDefaultWarpOptions(int num_bands = 1) {
    WarpOptionsPtr wo(GDALCreateWarpOptions());
    wo->nBandCount = num_bands;
    wo->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int) * num_bands));
    wo->panDstBands = static_cast<int*>(CPLMalloc(sizeof(int) * num_bands));
    for (int i = 0; i < num_bands; ++i) {
      wo->panSrcBands[i] = i + 1;
      wo->panDstBands[i] = i + 1;
    }
    return wo;
  }

  void CreateGeoTiff(const char* filename, int xsize, int ysize, int bands,
                     GDALDataType dt, const double* gt, const char* srs_wkt,
                     const double* nodata,
                     const std::vector<std::vector<double>>& band_data,
                     std::span<const int> overviews = {}) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver);

    GDALDatasetUniquePtr ds(
        driver->Create(filename, xsize, ysize, bands, dt, nullptr));
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
      if (b - 1 < static_cast<int>(band_data.size()) &&
          !band_data[b - 1].empty()) {
        std::vector<double> data = band_data[b - 1];
        EXPECT_EQ(CE_None,
                  band->RasterIO(GF_Write, 0, 0, xsize, ysize, data.data(),
                                 xsize, ysize, GDT_Float64, 0, 0));
      }
    }

    if (!overviews.empty()) {
      EXPECT_EQ(CE_None, ds->BuildOverviews(
                             "NEAREST", static_cast<int>(overviews.size()),
                             overviews.data(), 0, nullptr, nullptr, nullptr));
    }
  }

  std::string GetWGS84WKT() {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    char* wkt_raw = nullptr;
    srs.exportToWkt(&wkt_raw);
    std::unique_ptr<char, CplFreeDeleter> wkt(wkt_raw);
    return std::string(wkt.get());
  }

  std::string GetUTM31NWKT() {
    OGRSpatialReference srs;
    srs.importFromEPSG(32631);
    char* wkt_raw = nullptr;
    srs.exportToWkt(&wkt_raw);
    std::unique_ptr<char, CplFreeDeleter> wkt(wkt_raw);
    return std::string(wkt.get());
  }
};

TEST_F(VrtWarpedTest, AutoCreateWarpedVRTBasic) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_src1.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  std::vector<double> data(20 * 20);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<double>(i + 1);
  }
  constexpr double kNoData = 0.0;
  CreateGeoTiff(kFilename, 20, 20, 1, GDT_Byte, kGt, wgs84.c_str(), &kNoData,
                {data});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_Bilinear, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);
  EXPECT_GT(warped_ds->GetRasterXSize(), 0);
  EXPECT_GT(warped_ds->GetRasterYSize(), 0);
  EXPECT_EQ(1, warped_ds->GetRasterCount());

  const int out_x = warped_ds->GetRasterXSize();
  const int out_y = warped_ds->GetRasterYSize();
  std::vector<GByte> out_buf(out_x * out_y);
  EXPECT_EQ(CE_None, warped_ds->RasterIO(GF_Read, 0, 0, out_x, out_y,
                                         out_buf.data(), out_x, out_y, GDT_Byte,
                                         1, nullptr, 0, 0, 0, nullptr));

  // Cross-type destination buffer conversion via GDALCopyWords.
  std::vector<float> f_out(out_x * out_y);
  EXPECT_EQ(CE_None, warped_ds->RasterIO(
                         GF_Read, 0, 0, out_x, out_y, f_out.data(), out_x,
                         out_y, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));

  // Non-sequential pixel/line spacing.
  std::vector<GByte> nonseq_out(out_x * out_y * 2);
  int band_map[1] = {1};
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, out_x, out_y, nonseq_out.data(),
                                out_x, out_y, GDT_Byte, 1, band_map, 2,
                                2 * out_x, 0, nullptr));

  // Subsampled whole-image read uses cloned transformer path; partial window
  // read falls back to generic GDALDataset::IRasterIO.
  ASSERT_GE(out_x, 4);
  ASSERT_GE(out_y, 4);
  const int sub_x = out_x / 2;
  const int sub_y = out_y / 2;
  std::vector<GByte> sub_buf(sub_x * sub_y);
  EXPECT_EQ(CE_None, warped_ds->RasterIO(GF_Read, 0, 0, out_x, out_y,
                                         sub_buf.data(), sub_x, sub_y, GDT_Byte,
                                         1, nullptr, 0, 0, 0, nullptr));

  std::vector<GByte> part_buf(sub_x * sub_y);
  EXPECT_EQ(CE_None, warped_ds->RasterIO(
                         GF_Read, 1, 1, out_x - 2, out_y - 2, part_buf.data(),
                         sub_x, sub_y, GDT_Byte, 1, nullptr, 0, 0, 0, nullptr));

  int bx = 0;
  int by = 0;
  warped_ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
  std::vector<GByte> block_buf(bx * by);
  EXPECT_EQ(CE_None,
            warped_ds->GetRasterBand(1)->ReadBlock(0, 0, block_buf.data()));

  std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> files(
      warped_ds->GetFileList());
  ASSERT_NE(nullptr, files);
  EXPECT_GE(CSLCount(files.get()), 1);
}

TEST_F(VrtWarpedTest, AutoCreateWarpedVRTWithApproxTransformer) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_approx_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 10, 10, 1, GDT_Float32, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(10 * 10, 42.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  CPLStringList transformer_opts;
  transformer_opts.SetNameValue("SRC_METHOD", "NO_GEOTRANSFORM");
  GDALDatasetUniquePtr warped_ds(
      GDALDataset::FromHandle(GDALAutoCreateWarpedVRTEx(
          src_ds.get(), wgs84.c_str(), utm.c_str(), GRA_NearestNeighbour, 0.25,
          nullptr, transformer_opts.List())));
  ASSERT_NE(nullptr, warped_ds);

  const int xsize = warped_ds->GetRasterXSize();
  const int ysize = warped_ds->GetRasterYSize();
  std::vector<float> buf(xsize * ysize);
  EXPECT_EQ(CE_None, warped_ds->RasterIO(GF_Read, 0, 0, xsize, ysize,
                                         buf.data(), xsize, ysize, GDT_Float32,
                                         1, nullptr, 0, 0, 0, nullptr));
}

TEST_F(VrtWarpedTest, AutoCreateWarpedVRTWithDstAlphaBand) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_alpha_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 10, 10, 1, GDT_Byte, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(10 * 10, 128.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsPtr wo = CreateDefaultWarpOptions(1);
  wo->nDstAlphaBand = 2;

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, wo.get())));
  wo.reset();
  ASSERT_NE(nullptr, warped_ds);
  EXPECT_EQ(2, warped_ds->GetRasterCount());
  EXPECT_EQ(GCI_AlphaBand,
            warped_ds->GetRasterBand(2)->GetColorInterpretation());

  int bx = 0;
  int by = 0;
  warped_ds->GetRasterBand(2)->GetBlockSize(&bx, &by);
  std::vector<GByte> block(bx * by, 255);
  EXPECT_EQ(CE_None,
            warped_ds->GetRasterBand(2)->WriteBlock(0, 0, block.data()));
  EXPECT_EQ(CE_None,
            warped_ds->GetRasterBand(2)->ReadBlock(0, 0, block.data()));

  // Non-alpha band rejects WriteBlock.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              warped_ds->GetRasterBand(1)->WriteBlock(0, 0, block.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtWarpedTest, AutoCreateWarpedVRTNodataHandling) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_nodata_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  constexpr double kNoData = 255.0;
  std::vector<double> data(10 * 10, 100.0);
  data[0] = kNoData;
  CreateGeoTiff(kFilename, 10, 10, 1, GDT_Byte, kGt, wgs84.c_str(), &kNoData,
                {data});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);

  int has_nodata = FALSE;
  EXPECT_DOUBLE_EQ(255.0,
                   warped_ds->GetRasterBand(1)->GetNoDataValue(&has_nodata));
  EXPECT_EQ(TRUE, has_nodata);
}

TEST_F(VrtWarpedTest, AutoCreateWarpedVRTValidation) {
  WithQuietHandler quiet;

  CPLErrorReset();
  EXPECT_EQ(nullptr,
            GDALAutoCreateWarpedVRT(nullptr, nullptr, nullptr,
                                    GRA_NearestNeighbour, 0.0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALAutoCreateWarpedVRTEx(nullptr, nullptr, nullptr,
                                               GRA_NearestNeighbour, 0.0,
                                               nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALCreateWarpedVRT(nullptr, 10, 10, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALInitializeWarpedVRT(nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  VRTWarpedDataset ds(10, 10);
  WarpOptionsPtr wo = CreateDefaultWarpOptions(1);
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALInitializeWarpedVRT(&ds, wo.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(VrtWarpedTest, XMLSerializationAndDeserialization) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_ser_src.tif";
  constexpr char kVrtPath[] = "/vsimem/vrtwarped_test/warped_test.vrt";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 10, 10, 1, GDT_Int16, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(10 * 10, 50.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);

  auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
  ASSERT_NE(nullptr, vrt);
  CPLXMLTreeCloser tree(vrt->SerializeToXML(kVrtPath));
  ASSERT_NE(nullptr, tree.get());

  CPLXMLTreeCloser rel_tree(vrt->SerializeToXML("relative_warped.vrt"));
  EXPECT_NE(nullptr, rel_tree.get());

  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(tree.get()));
  ASSERT_NE(nullptr, xml_str);

  VsiMemTempWrapper vrt_file(kVrtPath, xml_str.get());
  GDALDatasetUniquePtr opened = OpenRaster(kVrtPath);
  ASSERT_NE(nullptr, opened);
  EXPECT_EQ(warped_ds->GetRasterXSize(), opened->GetRasterXSize());
  EXPECT_EQ(warped_ds->GetRasterYSize(), opened->GetRasterYSize());
  EXPECT_EQ(1, opened->GetRasterCount());

  const int xsize = opened->GetRasterXSize();
  const int ysize = opened->GetRasterYSize();
  std::vector<int16_t> buf(xsize * ysize);
  EXPECT_EQ(CE_None,
            opened->RasterIO(GF_Read, 0, 0, xsize, ysize, buf.data(), xsize,
                             ysize, GDT_Int16, 1, nullptr, 0, 0, 0, nullptr));
}

TEST_F(VrtWarpedTest, XMLInitErrorBranches) {
  // 1. Missing GDALWarpOptions.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <VRTRasterBand dataType='Byte' band='1' subClass='VRTWarpedRasterBand'/>
</VRTDataset>)vrt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Inconsistent dataset vs. band block size.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <BlockXSize>128</BlockXSize>
  <BlockYSize>128</BlockYSize>
  <VRTRasterBand dataType='Byte' band='1' subClass='VRTWarpedRasterBand'>
    <BlockXSize>64</BlockXSize>
    <BlockYSize>64</BlockYSize>
  </VRTRasterBand>
  <GDALWarpOptions/>
</VRTDataset>)vrt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Invalid WorkingDataType in GDALWarpOptions.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <VRTRasterBand dataType='Byte' band='1' subClass='VRTWarpedRasterBand'/>
  <GDALWarpOptions>
    <WorkingDataType>InvalidDataType</WorkingDataType>
  </GDALWarpOptions>
</VRTDataset>)vrt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Too-small WarpMemoryLimit=1 causes warper initialization failure.
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  CreateGeoTiff("/vsimem/vrtwarped_test/warp_err_src.tif", 10, 10, 1, GDT_Byte,
                kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(10 * 10, 1.0)});
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <VRTRasterBand dataType='Byte' band='1' subClass='VRTWarpedRasterBand'/>
  <GDALWarpOptions>
    <SourceDataset relativeToVRT='0'>/vsimem/vrtwarped_test/warp_err_src.tif</SourceDataset>
    <WarpMemoryLimit>1</WarpMemoryLimit>
  </GDALWarpOptions>
</VRTDataset>)vrt";
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. XML with VerticalShiftGrid.
  CreateGeoTiff("/vsimem/vrtwarped_test/vgrid.tif", 10, 10, 1, GDT_Float32, kGt,
                wgs84.c_str(), nullptr, {std::vector<double>(10 * 10, 0.5)});
  CreateGeoTiff("/vsimem/vrtwarped_test/vsrc.tif", 10, 10, 1, GDT_Float32, kGt,
                wgs84.c_str(), nullptr, {std::vector<double>(10 * 10, 10.0)});
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <VRTRasterBand dataType='Float32' band='1' subClass='VRTWarpedRasterBand'/>
  <BlockXSize>128</BlockXSize>
  <BlockYSize>128</BlockYSize>
  <GDALWarpOptions>
    <SourceDataset relativeToVRT='0'>/vsimem/vrtwarped_test/vsrc.tif</SourceDataset>
    <Transformer>
      <ApproxTransformer>
        <MaxError>0.125</MaxError>
        <BaseTransformer>
          <GenImgProjTransformer>
            <SrcGeoTransform>2,0.01,0,49,0,-0.01</SrcGeoTransform>
            <DstGeoTransform>2,0.01,0,49,0,-0.01</DstGeoTransform>
          </GenImgProjTransformer>
        </BaseTransformer>
      </ApproxTransformer>
    </Transformer>
    <BandList>
      <BandMapping src='1' dst='1'/>
    </BandList>
  </GDALWarpOptions>
  <VerticalShiftGrid inverse='1' toMeterSrc='1' toMeterDest='1'>/vsimem/vrtwarped_test/vgrid.tif</VerticalShiftGrid>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }

  // 6. XML with multiple bands, relativeToVRT="1", and DstDataset.
  {
    constexpr char kVrtPath[] = "/vsimem/vrtwarped_test/multi_band.vrt";
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='10' rasterYSize='10' subClass='VRTWarpedDataset'>
  <VRTRasterBand dataType='Float32' band='1' subClass='VRTWarpedRasterBand'/>
  <VRTRasterBand dataType='Float32' band='2' subClass='VRTWarpedRasterBand'/>
  <BlockXSize>128</BlockXSize>
  <BlockYSize>128</BlockYSize>
  <GDALWarpOptions>
    <SourceDataset relativeToVRT='1'>vsrc.tif</SourceDataset>
    <DstDataset>/vsimem/vrtwarped_test/vsrc.tif</DstDataset>
    <Transformer>
      <ApproxTransformer>
        <MaxError>0.125</MaxError>
        <BaseTransformer>
          <GenImgProjTransformer>
            <SrcGeoTransform>2,0.01,0,49,0,-0.01</SrcGeoTransform>
            <DstGeoTransform>2,0.01,0,49,0,-0.01</DstGeoTransform>
          </GenImgProjTransformer>
        </BaseTransformer>
      </ApproxTransformer>
    </Transformer>
    <BandList>
      <BandMapping src='1' dst='1'/>
      <BandMapping src='1' dst='2'/>
    </BandList>
  </GDALWarpOptions>
</VRTDataset>)vrt";
    VsiMemTempWrapper vrt_file(kVrtPath, kVrtXml);
    GDALDatasetUniquePtr multi = OpenRaster(kVrtPath);
    ASSERT_NE(nullptr, multi);
    EXPECT_EQ(2, multi->GetRasterCount());
  }
}

TEST_F(VrtWarpedTest, SrcOvrLevelMetadata) {
  VRTWarpedDataset ds(10, 10);
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SrcOvrLevel", "AUTO"));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SrcOvrLevel", "AUTO-1"));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SrcOvrLevel", "NONE"));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SrcOvrLevel", "2"));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SrcOvrLevel", nullptr));
  EXPECT_EQ(CE_None,
            ds.SetMetadataItem("SrcOvrLevel", "AUTO", "CUSTOM_DOMAIN"));
  EXPECT_STREQ("AUTO", ds.GetMetadataItem("SrcOvrLevel", "CUSTOM_DOMAIN"));
}

TEST_F(VrtWarpedTest, OverviewsAndResampling) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_ovr_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  constexpr int kOvrFactors[] = {2, 4};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 40, 40, 1, GDT_Float32, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(40 * 40, 10.0)}, kOvrFactors);

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);

  GDALRasterBand* band = warped_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_GT(band->GetOverviewCount(), 0);
  EXPECT_EQ(nullptr, band->GetOverview(-1));
  EXPECT_EQ(nullptr, band->GetOverview(99));

  GDALRasterBand* ovr = band->GetOverview(0);
  ASSERT_NE(nullptr, ovr);
  EXPECT_GT(ovr->GetXSize(), 0);
  EXPECT_GT(ovr->GetYSize(), 0);

  std::vector<float> ovr_data(ovr->GetXSize() * ovr->GetYSize());
  EXPECT_EQ(CE_None,
            ovr->RasterIO(GF_Read, 0, 0, ovr->GetXSize(), ovr->GetYSize(),
                          ovr_data.data(), ovr->GetXSize(), ovr->GetYSize(),
                          GDT_Float32, 0, 0));

  const int xsize = warped_ds->GetRasterXSize();
  const int ysize = warped_ds->GetRasterYSize();
  std::vector<float> ds_data(10 * 10);
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, xsize, ysize, ds_data.data(), 10,
                                10, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));

  {
    CPLConfigOptionSetter disable_ds_io("GDAL_VRT_WARP_USE_DATASET_RASTERIO",
                                        "NO", false);
    EXPECT_EQ(CE_None, warped_ds->RasterIO(GF_Read, 0, 0, xsize, ysize,
                                           ds_data.data(), 10, 10, GDT_Float32,
                                           1, nullptr, 0, 0, 0, nullptr));
  }

  int build_ovr[] = {2};
  EXPECT_EQ(CE_None,
            warped_ds->BuildOverviews("NEAREST", 1, build_ovr, 0, nullptr,
                                      GDALDummyProgress, nullptr));
}

TEST_F(VrtWarpedTest, AddBandAndBlockSize) {
  VRTWarpedDataset ds(100, 100, 32, 32);
  int bx = 0;
  int by = 0;
  ds.GetBlockSize(&bx, &by);
  EXPECT_EQ(32, bx);
  EXPECT_EQ(32, by);

  EXPECT_EQ(CE_None, ds.AddBand(GDT_Int16, nullptr));
  EXPECT_EQ(1, ds.GetRasterCount());
  EXPECT_EQ(GDT_Int16, ds.GetRasterBand(1)->GetRasterDataType());
  ds.GetRasterBand(1)->GetBlockSize(&bx, &by);
  EXPECT_EQ(32, bx);
  EXPECT_EQ(32, by);
}

TEST_F(VrtWarpedTest, CutlineAndCoordRescaler) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_cutline_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  constexpr int kOvrFactors[] = {2, 4};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 40, 40, 1, GDT_Byte, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(40 * 40, 100.0)}, kOvrFactors);

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  OGRPolygon polygon;
  OGRLinearRing ring;
  ring.addPoint(2.0, 49.0);
  ring.addPoint(2.2, 49.0);
  ring.addPoint(2.2, 48.8);
  ring.addPoint(2.0, 48.8);
  ring.addPoint(2.0, 49.0);
  polygon.addRing(&ring);

  WarpOptionsPtr wo = CreateDefaultWarpOptions(1);
  wo->hCutline = polygon.clone();

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, wo.get())));
  wo.reset();
  ASSERT_NE(nullptr, warped_ds);

  GDALRasterBand* band = warped_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_GT(band->GetOverviewCount(), 0);
  GDALRasterBand* ovr = band->GetOverview(0);
  ASSERT_NE(nullptr, ovr);
  EXPECT_GT(ovr->GetXSize(), 0);

  std::vector<GByte> buf(ovr->GetXSize() * ovr->GetYSize());
  EXPECT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, ovr->GetXSize(),
                                   ovr->GetYSize(), buf.data(), ovr->GetXSize(),
                                   ovr->GetYSize(), GDT_Byte, 0, 0));
}

TEST_F(VrtWarpedTest, BuildOverviewsAndOverviewTransformer) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_bldovr_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  constexpr int kOvrFactors[] = {2};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 64, 64, 1, GDT_Float32, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(64 * 64, 10.0)}, kOvrFactors);

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);

  int ovr_list[2] = {4, 8};

  // Cancelled progress callback aborts overview generation.
  auto fail_progress = [](double, const char*, void*) -> int { return 0; };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              warped_ds->BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr,
                                        fail_progress, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  EXPECT_EQ(CE_None,
            warped_ds->BuildOverviews("NEAREST", 2, ovr_list, 0, nullptr,
                                      GDALDummyProgress, nullptr));

  GDALRasterBand* band = warped_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_GE(band->GetOverviewCount(), 2);

  GDALRasterBand* ovr = band->GetOverview(band->GetOverviewCount() - 1);
  ASSERT_NE(nullptr, ovr);
  std::vector<float> buf(ovr->GetXSize() * ovr->GetYSize());
  EXPECT_EQ(CE_None, ovr->RasterIO(GF_Read, 0, 0, ovr->GetXSize(),
                                   ovr->GetYSize(), buf.data(), ovr->GetXSize(),
                                   ovr->GetYSize(), GDT_Float32, 0, 0));

  VRTWarpedDataset ds_no_warper(10, 10);
  EXPECT_EQ(CE_Failure,
            ds_no_warper.BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr,
                                        GDALDummyProgress, nullptr));

  auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
  ASSERT_NE(nullptr, vrt);
  CPLXMLTreeCloser tree(
      vrt->SerializeToXML("/vsimem/vrtwarped_test/ovr_list.vrt"));
  ASSERT_NE(nullptr, tree.get());
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "OverviewList"));
}

TEST_F(VrtWarpedTest, WarpMemoryLimitSplittingAndBufferConversion) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_mem_src.tif";
  constexpr double kGt[6] = {2.0, 0.001, 0.0, 49.0, 0.0, -0.001};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 160, 60, 1, GDT_Float64, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(160 * 60, 5.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsPtr wo = CreateDefaultWarpOptions(1);
  wo->dfWarpMemoryLimit = 100000.0;

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, wo.get())));
  wo.reset();
  ASSERT_NE(nullptr, warped_ds);

  const int w = warped_ds->GetRasterXSize();
  const int h = warped_ds->GetRasterYSize();
  std::vector<double> buf(w * h);
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, w, h, buf.data(), w, h,
                                GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));

  ASSERT_GE(h, 4);
  std::vector<double> sub_v_buf(4 * h);
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, 4, h, sub_v_buf.data(), 4, h,
                                GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));

  std::vector<float> f_buf(w * h);
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, w, h, f_buf.data(), w, h,
                                GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));

  std::vector<double> nonseq_buf(w * h * 2);
  int band_map[1] = {1};
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, w, h, nonseq_buf.data(), w, h,
                                GDT_Float64, 1, band_map, 2 * sizeof(double),
                                2 * w * sizeof(double), 0, nullptr));

  ASSERT_GE(w, 10);
  std::vector<double> sub_h_buf(w * 4);
  EXPECT_EQ(CE_None,
            warped_ds->RasterIO(GF_Read, 0, 0, w, 4, sub_h_buf.data(), w, 4,
                                GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));

  // Tall dataset (height > width).
  constexpr char kFilenameV[] = "/vsimem/vrtwarped_test/warp_mem_v_src.tif";
  CreateGeoTiff(kFilenameV, 60, 160, 1, GDT_Float64, kGt, wgs84.c_str(),
                nullptr, {std::vector<double>(60 * 160, 5.0)});

  GDALDatasetUniquePtr src_ds_v = OpenRaster(kFilenameV);
  ASSERT_NE(nullptr, src_ds_v);

  WarpOptionsPtr wo_v = CreateDefaultWarpOptions(1);
  wo_v->dfWarpMemoryLimit = 100000.0;

  GDALDatasetUniquePtr warped_v(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds_v.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, wo_v.get())));
  wo_v.reset();
  ASSERT_NE(nullptr, warped_v);

  const int w_v = warped_v->GetRasterXSize();
  const int h_v = warped_v->GetRasterYSize();
  std::vector<double> buf_v(w_v * h_v);
  EXPECT_EQ(CE_None,
            warped_v->RasterIO(GF_Read, 0, 0, w_v, h_v, buf_v.data(), w_v, h_v,
                               GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));
}

TEST_F(VrtWarpedTest, ProcessBlockPartialBlock) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_partblock_src.tif";
  constexpr char kPartVrtPath[] = "/vsimem/vrtwarped_test/partblock.vrt";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 25, 25, 1, GDT_Byte, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(25 * 25, 7.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);
  auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
  ASSERT_NE(nullptr, vrt);

  int bx = 0;
  int by = 0;
  vrt->GetBlockSize(&bx, &by);
  const int blocks_x = (vrt->GetRasterXSize() + bx - 1) / bx;
  const int blocks_y = (vrt->GetRasterYSize() + by - 1) / by;
  EXPECT_EQ(CE_None, vrt->ProcessBlock(blocks_x - 1, blocks_y - 1));

  CPLXMLTreeCloser tree(vrt->SerializeToXML(kPartVrtPath));
  ASSERT_NE(nullptr, tree.get());
  CPLSetXMLValue(tree.get(), "BlockXSize", "16");
  CPLSetXMLValue(tree.get(), "BlockYSize", "16");
  CPLSetXMLValue(tree.get(), "VRTRasterBand.BlockXSize", "16");
  CPLSetXMLValue(tree.get(), "VRTRasterBand.BlockYSize", "16");
  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(tree.get()));
  ASSERT_NE(nullptr, xml_str);

  VsiMemTempWrapper vrt_file(kPartVrtPath, xml_str.get());
  GDALDatasetUniquePtr opened = OpenRaster(kPartVrtPath);
  ASSERT_NE(nullptr, opened);
  auto* vrt_block = dynamic_cast<VRTWarpedDataset*>(opened.get());
  ASSERT_NE(nullptr, vrt_block);

  int bx2 = 0;
  int by2 = 0;
  vrt_block->GetBlockSize(&bx2, &by2);
  EXPECT_EQ(16, bx2);
  EXPECT_EQ(16, by2);
  const int blocks_x2 = (vrt_block->GetRasterXSize() + bx2 - 1) / bx2;
  const int blocks_y2 = (vrt_block->GetRasterYSize() + by2 - 1) / by2;
  EXPECT_EQ(CE_None, vrt_block->ProcessBlock(blocks_x2 - 1, blocks_y2 - 1));
}

TEST_F(VrtWarpedTest, SrcOvrLevelAndGetBestOverviewLevel) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_bestovr_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  constexpr int kOvrFactors[] = {2, 4};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  const std::vector<double> data(64 * 64, 3.0);
  CreateGeoTiff(kFilename, 64, 64, 2, GDT_Float32, kGt, wgs84.c_str(), nullptr,
                {data, data}, kOvrFactors);

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);
  auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
  ASSERT_NE(nullptr, vrt);

  GDALRasterBand* band = vrt->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  const int xsize = vrt->GetRasterXSize();
  const int ysize = vrt->GetRasterYSize();

  std::vector<float> buf(16 * 16);
  for (const char* level : {"AUTO-1", "0", "NONE"}) {
    EXPECT_EQ(CE_None, vrt->SetMetadataItem("SrcOvrLevel", level));
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, xsize, ysize, buf.data(),
                                      16, 16, GDT_Float32, 0, 0));
  }

  GDALRasterIOExtraArg extra_arg;
  INIT_RASTERIO_EXTRA_ARG(extra_arg);
  extra_arg.eResampleAlg = GRIORA_Bilinear;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, xsize, ysize, buf.data(), 16,
                                    16, GDT_Float32, 0, 0, &extra_arg));

  extra_arg.bFloatingPointWindowValidity = true;
  extra_arg.dfXOff = 0.0;
  extra_arg.dfYOff = 0.0;
  extra_arg.dfXSize = xsize;
  extra_arg.dfYSize = ysize;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, xsize, ysize, buf.data(), 16,
                                    16, GDT_Float32, 0, 0, &extra_arg));

  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, xsize - 4, ysize - 4, 4, 4,
                                    buf.data(), 4, 4, GDT_Float32, 0, 0));

  std::vector<float> buf_y1(16);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, xsize, ysize, buf_y1.data(),
                                    16, 1, GDT_Float32, 0, 0));

  std::vector<float> buf_xy(8 * 16);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, xsize, ysize, buf_xy.data(),
                                    8, 16, GDT_Float32, 0, 0));

  for (const char* level : {"AUTO-1", "2", "NONE"}) {
    EXPECT_EQ(CE_None, vrt->SetMetadataItem("SrcOvrLevel", level));
    CPLXMLTreeCloser tree(
        vrt->SerializeToXML("/vsimem/vrtwarped_test/src_ovr_level.vrt"));
    ASSERT_NE(nullptr, tree.get());
    EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "SrcOvrLevel"));
  }
}

TEST_F(VrtWarpedTest, XMLInitAdditionalElements) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_xml_src.tif";
  constexpr char kExtraVrtPath[] = "/vsimem/vrtwarped_test/xml_extra.vrt";
  constexpr char kValidGridPath[] = "/vsimem/vrtwarped_test/valid_grid.tif";
  constexpr char kValidVgridVrtPath[] = "/vsimem/vrtwarped_test/xml_vgrid.vrt";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  constexpr int kOvrFactors[] = {2};
  const std::string wgs84 = GetWGS84WKT();
  const std::string utm = GetUTM31NWKT();

  CreateGeoTiff(kFilename, 16, 16, 1, GDT_Byte, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(16 * 16, 1.0)}, kOvrFactors);

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
      GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), utm.c_str(),
                              GRA_NearestNeighbour, 0.0, nullptr)));
  ASSERT_NE(nullptr, warped_ds);
  auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
  ASSERT_NE(nullptr, vrt);

  // Non-existent VerticalShiftGrids emits a warning during open.
  {
    CPLXMLTreeCloser tree(vrt->SerializeToXML(kExtraVrtPath));
    ASSERT_NE(nullptr, tree.get());
    CPLXMLNode* vgrids =
        CPLCreateXMLNode(tree.get(), CXT_Element, "VerticalShiftGrids");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "Grids"), CXT_Text,
                     "/vsimem/vrtwarped_test/non_existent_grid.tif");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "Inverse"),
                     CXT_Text, "1");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "ToMeterSrc"),
                     CXT_Text, "1.0");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "ToMeterDest"),
                     CXT_Text, "1.0");
    CPLXMLNode* opt = CPLCreateXMLNode(vgrids, CXT_Element, "Option");
    CPLCreateXMLNode(CPLCreateXMLNode(opt, CXT_Attribute, "name"), CXT_Text,
                     "TEST_OPT");
    CPLCreateXMLNode(opt, CXT_Text, "VAL");
    CPLCreateXMLElementAndValue(tree.get(), "SrcOvrLevel", "AUTO-1");
    CPLCreateXMLElementAndValue(tree.get(), "OverviewList", "4 8");

    std::unique_ptr<char, CplFreeDeleter> xml_str(
        CPLSerializeXMLTree(tree.get()));
    ASSERT_NE(nullptr, xml_str);
    VsiMemTempWrapper vrt_file(kExtraVrtPath, xml_str.get());

    GDALDatasetUniquePtr opened;
    {
      ScopedWarningCapturer warnings;
      opened = OpenRaster(kExtraVrtPath);
      EXPECT_TRUE(warnings.has_warning());
    }
    ASSERT_NE(nullptr, opened);
  }

  // Valid VerticalShiftGrids dataset.
  {
    CreateGeoTiff(kValidGridPath, 16, 16, 1, GDT_Float32, kGt, wgs84.c_str(),
                  nullptr, {std::vector<double>(16 * 16, 2.0)});
    CPLXMLTreeCloser tree(vrt->SerializeToXML(kValidVgridVrtPath));
    ASSERT_NE(nullptr, tree.get());
    CPLXMLNode* vgrids =
        CPLCreateXMLNode(tree.get(), CXT_Element, "VerticalShiftGrids");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "Grids"), CXT_Text,
                     kValidGridPath);
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "Inverse"),
                     CXT_Text, "0");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "ToMeterSrc"),
                     CXT_Text, "1.0");
    CPLCreateXMLNode(CPLCreateXMLNode(vgrids, CXT_Attribute, "ToMeterDest"),
                     CXT_Text, "1.0");

    std::unique_ptr<char, CplFreeDeleter> xml_str(
        CPLSerializeXMLTree(tree.get()));
    ASSERT_NE(nullptr, xml_str);
    VsiMemTempWrapper vrt_file(kValidVgridVrtPath, xml_str.get());

    GDALDatasetUniquePtr opened;
    {
      ScopedWarningCapturer warnings;
      opened = OpenRaster(kValidVgridVrtPath);
      EXPECT_TRUE(warnings.has_warning());
    }
    ASSERT_NE(nullptr, opened);
  }
}

TEST_F(VrtWarpedTest, ProjectionAndCloseDependentDatasets) {
  constexpr char kFilename[] = "/vsimem/vrtwarped_test/warp_proj_src.tif";
  constexpr double kGt[6] = {2.0, 0.01, 0.0, 49.0, 0.0, -0.01};
  const std::string wgs84 = GetWGS84WKT();

  CreateGeoTiff(kFilename, 10, 10, 1, GDT_Byte, kGt, wgs84.c_str(), nullptr,
                {std::vector<double>(10 * 10, 1.0)});

  GDALDatasetUniquePtr src_ds = OpenRaster(kFilename);
  ASSERT_NE(nullptr, src_ds);

  {
    GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
        GDALAutoCreateWarpedVRT(src_ds.get(), wgs84.c_str(), nullptr,
                                GRA_NearestNeighbour, 0.0, nullptr)));
    ASSERT_NE(nullptr, warped_ds);
    auto* vrt = dynamic_cast<VRTWarpedDataset*>(warped_ds.get());
    ASSERT_NE(nullptr, vrt);
    EXPECT_NE(nullptr, vrt->GetProjectionRef());
  }

  {
    GDALDatasetUniquePtr warped_ds(GDALDataset::FromHandle(
        GDALAutoCreateWarpedVRT(src_ds.get(), nullptr, nullptr,
                                GRA_NearestNeighbour, 0.0, nullptr)));
    ASSERT_NE(nullptr, warped_ds);
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and non-8-bit LUT/density-split
// paths in VRTWarpedDataset in vrtwarped.cpp.
