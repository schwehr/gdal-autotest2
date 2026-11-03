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
// Tests VRTProcessedDataset and VRTProcessedRasterBand processing pipelines,
// custom function registration, RAM splitting, overviews, and error branches
// (vrtprocesseddataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtprocesseddataset.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/vrtprocesseddataset_test";

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

CPLErr CustomInitIdentity(const char* /*pszFuncName*/, void* /*pUserData*/,
                          CSLConstList /*papszFunctionArgs*/, int nInBands,
                          GDALDataType eInDT, double* padfInNoData,
                          int* pnOutBands, GDALDataType* peOutDT,
                          double** ppadfOutNoData, const char* /*pszVRTPath*/,
                          VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  *pnOutBands = nInBands;
  *peOutDT = eInDT;
  CPLFree(*ppadfOutNoData);
  *ppadfOutNoData = static_cast<double*>(CPLMalloc(sizeof(double) * nInBands));
  for (int i = 0; i < nInBands; ++i) {
    (*ppadfOutNoData)[i] = padfInNoData[i];
  }
  return CE_None;
}

CPLErr CustomProcessIdentity(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr /*pWorkingData*/, CSLConstList /*papszFunctionArgs*/,
    int nBufXSize, int nBufYSize, const void* pInBuffer,
    size_t /*nInBufferSize*/, GDALDataType eInDT, int nInBands,
    const double* /*padfInNoData*/, void* pOutBuffer, size_t /*nOutBufferSize*/,
    GDALDataType eOutDT, int nOutBands, const double* /*padfOutNoData*/,
    double /*dfSrcXOff*/, double /*dfSrcYOff*/, double /*dfSrcXSize*/,
    double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  if (eInDT != eOutDT) {
    return CE_Failure;
  }
  const int in_dt_size = GDALGetDataTypeSizeBytes(eInDT);
  const int out_dt_size = GDALGetDataTypeSizeBytes(eOutDT);
  const size_t total_pixels = static_cast<size_t>(nBufXSize) * nBufYSize;
  const int bands_to_copy = std::min(nInBands, nOutBands);
  for (size_t i = 0; i < total_pixels; ++i) {
    const GByte* src_px =
        static_cast<const GByte*>(pInBuffer) + i * nInBands * in_dt_size;
    GByte* dst_px =
        static_cast<GByte*>(pOutBuffer) + i * nOutBands * out_dt_size;
    std::memcpy(dst_px, src_px,
                static_cast<size_t>(bands_to_copy) * in_dt_size);
  }
  return CE_None;
}

CPLErr CustomInitExpandAndScale(const char* /*pszFuncName*/,
                                void* /*pUserData*/,
                                CSLConstList /*papszFunctionArgs*/,
                                int nInBands, GDALDataType /*eInDT*/,
                                double* /*padfInNoData*/, int* pnOutBands,
                                GDALDataType* peOutDT, double** ppadfOutNoData,
                                const char* /*pszVRTPath*/,
                                VRTPDWorkingDataPtr* ppWorkingData) {
  *pnOutBands = nInBands * 2;
  *peOutDT = GDT_Float32;
  CPLFree(*ppadfOutNoData);
  *ppadfOutNoData =
      static_cast<double*>(CPLMalloc(sizeof(double) * (*pnOutBands)));
  for (int i = 0; i < *pnOutBands; ++i) {
    (*ppadfOutNoData)[i] = -9999.0;
  }
  *ppWorkingData = CPLMalloc(sizeof(double));
  *static_cast<double*>(*ppWorkingData) = 2.0;
  return CE_None;
}

void CustomFreeExpandAndScale(const char* /*pszFuncName*/, void* /*pUserData*/,
                              VRTPDWorkingDataPtr pWorkingData) {
  CPLFree(pWorkingData);
}

CPLErr CustomProcessExpandAndScale(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr pWorkingData, CSLConstList papszFunctionArgs,
    int nBufXSize, int nBufYSize, const void* pInBuffer,
    size_t /*nInBufferSize*/, GDALDataType eInDT, int nInBands,
    const double* padfInNoData, void* pOutBuffer, size_t /*nOutBufferSize*/,
    GDALDataType eOutDT, int nOutBands, const double* padfOutNoData,
    double /*dfSrcXOff*/, double /*dfSrcYOff*/, double /*dfSrcXSize*/,
    double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  if (eInDT != GDT_Float32 || eOutDT != GDT_Float32) {
    return CE_Failure;
  }
  const double mult = *static_cast<const double*>(pWorkingData);
  const char* add_str = CSLFetchNameValue(papszFunctionArgs, "add");
  const double add = add_str ? CPLAtof(add_str) : 0.0;

  const float* src = static_cast<const float*>(pInBuffer);
  float* dst = static_cast<float*>(pOutBuffer);
  const size_t total_pixels = static_cast<size_t>(nBufXSize) * nBufYSize;
  for (size_t i = 0; i < total_pixels; ++i) {
    for (int b = 0; b < nOutBands; ++b) {
      const int src_b = b % nInBands;
      const double in_nd = padfInNoData[src_b];
      const double out_nd = padfOutNoData[b];
      const float in_val = src[i * nInBands + src_b];
      dst[i * nOutBands + b] =
          (in_val == in_nd)
              ? static_cast<float>(out_nd)
              : static_cast<float>(in_val * mult + add + b * 10.0);
    }
  }
  return CE_None;
}

CPLErr CustomInitTyped(const char* /*pszFuncName*/, void* /*pUserData*/,
                       CSLConstList /*papszFunctionArgs*/, int nInBands,
                       GDALDataType eInDT, double* padfInNoData,
                       int* pnOutBands, GDALDataType* peOutDT,
                       double** ppadfOutNoData, const char* /*pszVRTPath*/,
                       VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  *pnOutBands = nInBands;
  *peOutDT = eInDT;
  CPLFree(*ppadfOutNoData);
  *ppadfOutNoData = static_cast<double*>(CPLMalloc(sizeof(double) * nInBands));
  for (int i = 0; i < nInBands; ++i) {
    (*ppadfOutNoData)[i] = padfInNoData[i];
  }
  return CE_None;
}

CPLErr CustomProcessFail(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr /*pWorkingData*/, CSLConstList /*papszFunctionArgs*/,
    int /*nBufXSize*/, int /*nBufYSize*/, const void* /*pInBuffer*/,
    size_t /*nInBufferSize*/, GDALDataType /*eInDT*/, int /*nInBands*/,
    const double* /*padfInNoData*/, void* /*pOutBuffer*/,
    size_t /*nOutBufferSize*/, GDALDataType /*eOutDT*/, int /*nOutBands*/,
    const double* /*padfOutNoData*/, double /*dfSrcXOff*/, double /*dfSrcYOff*/,
    double /*dfSrcXSize*/, double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  CPLError(CE_Failure, CPLE_AppDefined, "Intentional process failure");
  return CE_Failure;
}

CPLErr CustomInitFail(const char* /*pszFuncName*/, void* /*pUserData*/,
                      CSLConstList /*papszFunctionArgs*/, int /*nInBands*/,
                      GDALDataType /*eInDT*/, double* /*padfInNoData*/,
                      int* /*pnOutBands*/, GDALDataType* /*peOutDT*/,
                      double** /*ppadfOutNoData*/, const char* /*pszVRTPath*/,
                      VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  CPLError(CE_Failure, CPLE_AppDefined, "Intentional init failure");
  return CE_Failure;
}

CPLErr CustomInitNullNoData(const char* /*pszFuncName*/, void* /*pUserData*/,
                            CSLConstList /*papszFunctionArgs*/, int nInBands,
                            GDALDataType eInDT, double* /*padfInNoData*/,
                            int* pnOutBands, GDALDataType* peOutDT,
                            double** ppadfOutNoData, const char* /*pszVRTPath*/,
                            VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  *pnOutBands = nInBands;
  *peOutDT = eInDT;
  CPLFree(*ppadfOutNoData);
  *ppadfOutNoData = nullptr;
  return CE_None;
}

CPLErr CustomInitRequiresFloat64(const char* /*pszFuncName*/,
                                 void* /*pUserData*/,
                                 CSLConstList /*papszFunctionArgs*/,
                                 int nInBands, GDALDataType /*eInDT*/,
                                 double* padfInNoData, int* pnOutBands,
                                 GDALDataType* peOutDT, double** ppadfOutNoData,
                                 const char* /*pszVRTPath*/,
                                 VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  *pnOutBands = nInBands;
  *peOutDT = GDT_Float64;
  CPLFree(*ppadfOutNoData);
  *ppadfOutNoData = static_cast<double*>(CPLMalloc(sizeof(double) * nInBands));
  for (int i = 0; i < nInBands; ++i) {
    (*ppadfOutNoData)[i] = padfInNoData[i];
  }
  return CE_None;
}

class VrtProcessedDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_VRT();
    GDALRegister_GTiff();

    ASSERT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                           "CustomIdentity", nullptr, nullptr, GDT_Unknown,
                           nullptr, 0, nullptr, 0, CustomInitIdentity, nullptr,
                           CustomProcessIdentity, nullptr));

    ASSERT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                           "CustomExpandAndScale", nullptr, nullptr,
                           GDT_Float32, nullptr, 0, nullptr, 0,
                           CustomInitExpandAndScale, CustomFreeExpandAndScale,
                           CustomProcessExpandAndScale, nullptr));

    constexpr GDALDataType kSupportedTypes[] = {GDT_Byte, GDT_Float32};
    constexpr int kSupportedBands[] = {1, 2, 3};
    constexpr char kMetadata[] = R"xml(
<ProcessedDatasetFunctionArgumentsList>
  <Argument name='req_arg' type='string' required='true'/>
  <Argument name='opt_arg' type='string' default='def_val'/>
  <Argument name='bool_arg' type='boolean' default='true'/>
  <Argument name='int_arg' type='integer' default='42'/>
  <Argument name='double_arg' type='double' default='3.14'/>
  <Argument name='list_arg' type='double_list' default='1.0,2.0'/>
</ProcessedDatasetFunctionArgumentsList>)xml";
    ASSERT_EQ(CE_None,
              GDALVRTRegisterProcessedDatasetFunc(
                  "CustomTyped", nullptr, kMetadata, GDT_Unknown,
                  kSupportedTypes, std::size(kSupportedTypes), kSupportedBands,
                  std::size(kSupportedBands), CustomInitTyped, nullptr,
                  CustomProcessIdentity, nullptr));

    ASSERT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                           "CustomFail", nullptr, nullptr, GDT_Unknown, nullptr,
                           0, nullptr, 0, CustomInitIdentity, nullptr,
                           CustomProcessFail, nullptr));

    ASSERT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                           "CustomInitFail", nullptr, nullptr, GDT_Unknown,
                           nullptr, 0, nullptr, 0, CustomInitFail, nullptr,
                           CustomProcessIdentity, nullptr));

    ASSERT_EQ(CE_None,
              GDALVRTRegisterProcessedDatasetFunc(
                  "CustomInitNullNoData", nullptr, nullptr, GDT_Unknown,
                  nullptr, 0, nullptr, 0, CustomInitNullNoData, nullptr,
                  CustomProcessIdentity, nullptr));

    constexpr GDALDataType kF64Types[] = {GDT_Float32, GDT_Float64};
    ASSERT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                           "CustomRequiresFloat64", nullptr, nullptr,
                           GDT_Float64, kF64Types, std::size(kF64Types),
                           nullptr, 0, CustomInitRequiresFloat64, nullptr,
                           CustomProcessIdentity, nullptr));

    constexpr char kScaleMeta[] = R"xml(
<ProcessedDatasetFunctionArgumentsList>
  <Argument name='coefficients_{band}' type='double_list' required='true'/>
</ProcessedDatasetFunctionArgumentsList>)xml";
    ASSERT_EQ(CE_None,
              GDALVRTRegisterProcessedDatasetFunc(
                  "CustomScaleOffset", nullptr, kScaleMeta, GDT_Unknown,
                  nullptr, 0, nullptr, 0, CustomInitIdentity, nullptr,
                  CustomProcessIdentity, nullptr));
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateTiff(const char* filename, int xsize, int ysize, int bands,
                  GDALDataType dt, const double* gt, const char* srs_wkt,
                  const double* nodata,
                  const std::vector<std::vector<double>>& band_data,
                  const char* interleave = nullptr,
                  std::span<const int> overviews = {}) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver);

    CPLStringList options;
    if (interleave != nullptr) {
      options.SetNameValue("INTERLEAVE", interleave);
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

TEST_F(VrtProcessedDatasetTest, RegistrationValidation) {
  struct InvalidRegistration {
    const char* name = "ValidName";
    const char* metadata = nullptr;
    GDALVRTProcessedDatasetFuncInit init_fn = CustomInitIdentity;
    GDALVRTProcessedDatasetFuncFree free_fn = nullptr;
    GDALVRTProcessedDatasetFuncProcess process_fn = CustomProcessIdentity;
    const GDALDataType* data_types = nullptr;
    size_t data_types_size = 0;
    const int* band_counts = nullptr;
    size_t band_counts_size = 0;
  };

  constexpr InvalidRegistration kCases[] = {
      {.name = nullptr},
      {.name = ""},
      {.process_fn = nullptr},
      {.name = "CustomIdentity"},  // duplicate registration
      {.metadata = "<BadXML"},
      {.metadata = "<WrongRoot/>"},
      {.metadata = "<ProcessedDatasetFunctionArgumentsList><Argument/></"
                   "ProcessedDatasetFunctionArgumentsList>"},
      {.metadata = "<ProcessedDatasetFunctionArgumentsList><Argument "
                   "name='a'/></ProcessedDatasetFunctionArgumentsList>"},
      {.metadata = "<ProcessedDatasetFunctionArgumentsList><Argument name='a' "
                   "type='constant'/></ProcessedDatasetFunctionArgumentsList>"},
      {.metadata =
           "<ProcessedDatasetFunctionArgumentsList><Argument name='bad' "
           "type='builtin'/></ProcessedDatasetFunctionArgumentsList>"},
      {.metadata = "<ProcessedDatasetFunctionArgumentsList><Argument name='a' "
                   "type='unknown'/></ProcessedDatasetFunctionArgumentsList>"},
  };

  WithQuietHandler quiet;
  for (const auto& tc : kCases) {
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALVRTRegisterProcessedDatasetFunc(
                  tc.name, nullptr, tc.metadata, GDT_Unknown, tc.data_types,
                  tc.data_types_size, tc.band_counts, tc.band_counts_size,
                  tc.init_fn, tc.free_fn, tc.process_fn, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Non-empty ProcessedDatasetFunctionArgumentsList element with no Argument
  // children and nullptr init_fn is accepted.
  EXPECT_EQ(CE_None, GDALVRTRegisterProcessedDatasetFunc(
                         "EmptyMetaValid", nullptr,
                         "<ProcessedDatasetFunctionArgumentsList/>",
                         GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
                         CustomProcessIdentity, nullptr));
}

TEST_F(VrtProcessedDatasetTest, BasicPipelineExecution) {
  constexpr char kSrcFile[] = "/vsimem/vrtprocesseddataset_test/proc_src.tif";
  constexpr double kGt[6] = {100.0, 1.0, 0.0, 1000.0, 0.0, -1.0};
  constexpr double kNoData[2] = {-9999.0, -9999.0};

  CreateTiff(kSrcFile, 8, 8, 2, GDT_Float32, kGt, nullptr, kNoData,
             {{1.0, 2.0, 3.0, 4.0}, {10.0, 20.0, 30.0, 40.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <BlockXSize>4</BlockXSize>
  <BlockYSize>4</BlockYSize>
  <Input unscale='AUTO'>
    <SourceFilename relativeToVRT='0'>/vsimem/vrtprocesseddataset_test/proc_src.tif</SourceFilename>
  </Input>
  <OutputBands count='FROM_LAST_STEP' dataType='FROM_LAST_STEP'/>
  <ProcessingSteps>
    <Step name='step1'>
      <Algorithm>CustomExpandAndScale</Algorithm>
      <Argument name='add'>5.0</Argument>
    </Step>
    <Step name='step2'>
      <Algorithm>CustomIdentity</Algorithm>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  VsiMemTempWrapper vrt_file("/vsimem/vrtprocesseddataset_test/basic.vrt",
                             kVrtXml);
  GDALDatasetUniquePtr vrt_ds = OpenRaster(
      "/vsimem/vrtprocesseddataset_test/basic.vrt", GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(8, vrt_ds->GetRasterXSize());
  EXPECT_EQ(8, vrt_ds->GetRasterYSize());
  EXPECT_EQ(4, vrt_ds->GetRasterCount());

  std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> files(
      vrt_ds->GetFileList());
  ASSERT_NE(nullptr, files);
  EXPECT_GE(CSLCount(files.get()), 1);

  EXPECT_STREQ("PIXEL",
               vrt_ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));

  // Verify band 1: (1 * 2 + 5) + 0 = 7, (2 * 2 + 5) + 0 = 9.
  std::vector<float> b1(64);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, b1.data(),
                                               8, 8, GDT_Float32, 0, 0));
  EXPECT_FLOAT_EQ(7.0f, b1[0]);
  EXPECT_FLOAT_EQ(9.0f, b1[1]);

  // Verify band 2: (10 * 2 + 5) + 10 = 35.
  std::vector<float> b2(64);
  EXPECT_EQ(CE_None,
            vrt_ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 8, 8, b2.data(),
                                               8, 8, GDT_Float32, 0, 0));
  EXPECT_FLOAT_EQ(35.0f, b2[0]);

  // Dataset-level multi-band read.
  std::vector<float> all_data(64 * 4);
  int band_map[4] = {1, 2, 3, 4};
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 8, 8, all_data.data(), 8, 8,
                             GDT_Float32, 4, band_map, 0, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(7.0f, all_data[0]);
  EXPECT_FLOAT_EQ(35.0f, all_data[64]);

  // Writes are unsupported on VRTProcessedDataset and VRTProcessedRasterBand.
  {
    ScopedErrorCapturer errors;
    EXPECT_EQ(CE_Failure,
              vrt_ds->RasterIO(GF_Write, 0, 0, 8, 8, all_data.data(), 8, 8,
                               GDT_Float32, 4, band_map, 0, 0, 0, nullptr));
    EXPECT_TRUE(errors.has_failure());
  }
  {
    ScopedErrorCapturer errors;
    EXPECT_EQ(CE_Failure,
              vrt_ds->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 8, 8, b1.data(), 8, 8, GDT_Float32, 0, 0));
    EXPECT_TRUE(errors.has_failure());
  }

  // Typed argument metadata validation with supported band count (2 bands).
  constexpr char kTypedXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input unscale='AUTO'>
    <SourceFilename relativeToVRT='0'>/vsimem/vrtprocesseddataset_test/proc_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step name='typed_step'>
      <Algorithm>CustomTyped</Algorithm>
      <Argument name='req_arg'>hello</Argument>
      <Argument name='bool_arg'>false</Argument>
      <Argument name='int_arg'>100</Argument>
      <Argument name='double_arg'>2.5</Argument>
      <Argument name='list_arg'>1.5,2.5</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
  GDALDatasetUniquePtr typed_ds = OpenRaster(kTypedXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, typed_ds);
  EXPECT_EQ(2, typed_ds->GetRasterCount());
  float first_px = 0.0f;
  EXPECT_EQ(CE_None,
            typed_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &first_px,
                                                 1, 1, GDT_Float32, 0, 0));
  EXPECT_FLOAT_EQ(1.0f, first_px);
}

TEST_F(VrtProcessedDatasetTest, InlineVRTInputAndMask) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  constexpr double kNoData[1] = {0.0};
  CreateTiff("/vsimem/vrtprocesseddataset_test/inline_base.tif", 4, 4, 1,
             GDT_Float32, kGt, nullptr, kNoData,
             {{0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input unscale='NO'>
    <VRTDataset rasterXSize='4' rasterYSize='4'>
      <VRTRasterBand dataType='Float32' band='1'>
        <NoDataValue>0.0</NoDataValue>
        <SimpleSource>
          <SourceFilename relativeToVRT='0'>/vsimem/vrtprocesseddataset_test/inline_base.tif</SourceFilename>
          <SourceBand>1</SourceBand>
        </SimpleSource>
      </VRTRasterBand>
    </VRTDataset>
  </Input>
  <ProcessingSteps>
    <Step>
      <Algorithm>CustomIdentity</Algorithm>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml, GDAL_OF_VERBOSE_ERROR);
  ASSERT_NE(nullptr, vrt_ds);
  EXPECT_EQ(GMF_NODATA, vrt_ds->GetRasterBand(1)->GetMaskFlags());
  GDALRasterBand* mask = vrt_ds->GetRasterBand(1)->GetMaskBand();
  ASSERT_NE(nullptr, mask);

  std::vector<GByte> mask_data(16);
  EXPECT_EQ(CE_None, mask->RasterIO(GF_Read, 0, 0, 4, 4, mask_data.data(), 4, 4,
                                    GDT_Byte, 0, 0));
  EXPECT_EQ(0, mask_data[0]);
  EXPECT_EQ(255, mask_data[1]);
}

TEST_F(VrtProcessedDatasetTest, Overviews) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  constexpr int kOvrFactors[] = {2};
  CreateTiff("/vsimem/vrtprocesseddataset_test/ovr_src.tif", 8, 8, 1,
             GDT_Float32, kGt, nullptr, nullptr, {{10.0}}, nullptr,
             kOvrFactors);

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input>
    <SourceFilename relativeToVRT='0'>/vsimem/vrtprocesseddataset_test/ovr_src.tif</SourceFilename>
  </Input>
  <ProcessingSteps>
    <Step>
      <Algorithm>CustomIdentity</Algorithm>
    </Step>
  </ProcessingSteps>
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
  EXPECT_EQ(4, ovr_band->GetXSize());
  EXPECT_EQ(4, ovr_band->GetYSize());

  std::vector<float> ovr_data(16);
  EXPECT_EQ(CE_None, ovr_band->RasterIO(GF_Read, 0, 0, 4, 4, ovr_data.data(), 4,
                                        4, GDT_Float32, 0, 0));
  EXPECT_GT(ovr_data[0], 0.0f);

  std::vector<float> ds_data(16);
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 8, 8, ds_data.data(), 4, 4,
                             GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));
  EXPECT_GT(ds_data[0], 0.0f);
}

TEST_F(VrtProcessedDatasetTest, ErrorBranches) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_err_src.tif", 4, 4, 1,
             GDT_Float32, kGt, nullptr, nullptr, {{10.0}});
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_int_src.tif", 2, 2, 1,
             GDT_Int16, kGt, nullptr, nullptr, {{1}});
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_4b_src.tif", 2, 2, 4,
             GDT_Float32, kGt, nullptr, nullptr, {{1}, {2}, {3}, {4}});

  constexpr std::string_view kInvalidXmlCases[] = {
      // 1. Missing Input element.
      "<VRTDataset subClass='VRTProcessedDataset'/>",
      // 2. Empty Input element.
      "<VRTDataset subClass='VRTProcessedDataset'><Input/></VRTDataset>",
      // 3. Nonexistent source file.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/nonexistent.tif</SourceFilename>"
      "</Input></VRTDataset>",
      // 4. Inconsistent declared dimensions (8x8 vs 4x4).
      "<VRTDataset rasterXSize='8' rasterYSize='8' "
      "subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input></VRTDataset>",
      // 5. BlockXSize <= 1.
      "<VRTDataset subClass='VRTProcessedDataset'><BlockXSize>1</BlockXSize>"
      "<Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input></VRTDataset>",
      // 6. BlockYSize <= 1.
      "<VRTDataset subClass='VRTProcessedDataset'><BlockYSize>0</BlockYSize>"
      "<Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input></VRTDataset>",
      // 7. Non-numeric OutputBands count.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><OutputBands count='invalid'/></VRTDataset>",
      // 8. OutputBands count == 0.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><OutputBands count='0'/></VRTDataset>",
      // 9. Invalid OutputBands dataType.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><OutputBands dataType='invalid_type'/></VRTDataset>",
      // 10. Missing ProcessingSteps.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input></VRTDataset>",
      // 11. Empty ProcessingSteps.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps/></VRTDataset>",
      // 12. Step lacking Algorithm.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step/></ProcessingSteps></VRTDataset>",
      // 13. Unregistered algorithm.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>UnknownAlg</Algorithm></Step>"
      "</ProcessingSteps></VRTDataset>",
      // 14. Unsupported input data type (Int16 for CustomTyped).
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_int_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument></Step></ProcessingSteps>"
      "</VRTDataset>",
      // 15. Unsupported input band count (4 bands for CustomTyped).
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_4b_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument></Step></ProcessingSteps>"
      "</VRTDataset>",
      // 16. Argument without name attribute.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument>val</Argument></Step></ProcessingSteps></VRTDataset>",
      // 17. Invalid boolean argument.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument><Argument name='bool_arg'>"
      "not_a_bool</Argument></Step></ProcessingSteps></VRTDataset>",
      // 18. Invalid integer argument.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument><Argument name='int_arg'>"
      "3.14</Argument></Step></ProcessingSteps></VRTDataset>",
      // 19. Invalid double argument.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument><Argument name='double_arg'>"
      "not_a_num</Argument></Step></ProcessingSteps></VRTDataset>",
      // 20. Invalid double_list argument.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm>"
      "<Argument name='req_arg'>x</Argument><Argument name='list_arg'>"
      "1.0,foo,2.0</Argument></Step></ProcessingSteps></VRTDataset>",
      // 21. Missing required argument.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm></Step>"
      "</ProcessingSteps></VRTDataset>",
      // 22. Output band count mismatch with declared VRTRasterBand count.
      "<VRTDataset subClass='VRTProcessedDataset'><Input><SourceFilename>"
      "/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename>"
      "</Input><ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm>"
      "</Step></ProcessingSteps><VRTRasterBand dataType='Float32' band='1' "
      "subClass='VRTProcessedRasterBand'/><VRTRasterBand dataType='Float32' "
      "band='2' subClass='VRTProcessedRasterBand'/></VRTDataset>",
  };

  WithQuietHandler quiet;
  for (const std::string_view xml : kInvalidXmlCases) {
    const std::string xml_str(xml);
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(xml_str.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 23. Runtime failure inside step processing function.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_err_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomFail</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    std::vector<float> data(16);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              vrt_ds->RasterIO(GF_Read, 0, 0, 4, 4, data.data(), 4, 4,
                               GDT_Float32, 1, nullptr, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtProcessedDatasetTest, RAMSplittingAndReduction) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_ram_src.tif", 16, 8, 1,
             GDT_Float32, kGt, nullptr, nullptr,
             {std::vector<double>(16 * 8, 1.0)});

  CPLConfigOptionSetter low_ram("VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE", "16",
                                false);

  // Case 1: With explicit BlockXSize/BlockYSize.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='16' rasterYSize='8' subClass='VRTProcessedDataset'>
  <BlockXSize>8</BlockXSize>
  <BlockYSize>8</BlockYSize>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_ram_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);

    int band_map[1] = {1};
    const int pixel_space = sizeof(float);
    const int line_space = pixel_space * 16;
    const int band_space = line_space * 8;

    GDALRasterIOExtraArg arg;
    INIT_RASTERIO_EXTRA_ARG(arg);
    arg.pfnProgress = GDALDummyProgress;
    std::vector<float> data(16 * 8);
    EXPECT_EQ(CE_None,
              vrt_ds->RasterIO(GF_Read, 0, 0, 16, 8, data.data(), 16, 8,
                               GDT_Float32, 1, band_map, pixel_space,
                               line_space, band_space, &arg));

    const int h_line_space = pixel_space * 8;
    const int h_band_space = h_line_space * 2;
    std::vector<float> h_data(8 * 2);
    EXPECT_EQ(CE_None,
              vrt_ds->RasterIO(GF_Read, 0, 0, 8, 2, h_data.data(), 8, 2,
                               GDT_Float32, 1, band_map, pixel_space,
                               h_line_space, h_band_space, &arg));
  }

  // Case 2: Without explicit BlockXSize/BlockYSize.
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize='16' rasterYSize='8' subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_ram_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }
}

TEST_F(VrtProcessedDatasetTest, BuiltinScaleOffsetAndXmlSerialization) {
  constexpr char kSrcFile[] =
      "/vsimem/vrtprocesseddataset_test/proc_so_src.tif";
  constexpr char kVrtPath[] = "/vsimem/vrtprocesseddataset_test/proc_so.vrt";
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff(kSrcFile, 4, 4, 1, GDT_Float32, kGt, nullptr, nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});

  {
    GDALDatasetUniquePtr src = OpenRaster(kSrcFile, GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, src);
    EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetScale(1.5));
    EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetOffset(2.5));
  }

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <VRTRasterBand dataType='Float32' band='1' subClass='VRTProcessedRasterBand'>
    <Offset>2.5</Offset>
    <Scale>1.5</Scale>
  </VRTRasterBand>
  <OutputBands count='1'/>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_so_src.tif</SourceFilename></Input>
  <ProcessingSteps>
    <Step>
      <Algorithm>CustomScaleOffset</Algorithm>
      <Argument name='coefficients_1'>3.14</Argument>
      <Argument name='unsupported_arg'>val</Argument>
    </Step>
  </ProcessingSteps>
</VRTDataset>)vrt";
  VsiMemTempWrapper vrt_file(kVrtPath, kVrtXml);

  GDALDatasetUniquePtr vrt_ds;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    vrt_ds = OpenRaster(kVrtPath);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  ASSERT_NE(nullptr, vrt_ds);

  auto* ds = dynamic_cast<VRTDataset*>(vrt_ds.get());
  ASSERT_NE(nullptr, ds);
  CPLXMLTreeCloser tree(ds->SerializeToXML(nullptr));
  ASSERT_NE(nullptr, tree.get());
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "VRTRasterBand"));
}

TEST_F(VrtProcessedDatasetTest, CustomInitFailAndNullNoData) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_init_src.tif", 4, 4, 1,
             GDT_Float32, kGt, nullptr, nullptr, {{1}});

  {
    WithQuietHandler quiet;
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_init_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomInitFail</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_init_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomInitNullNoData</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }
}

TEST_F(VrtProcessedDatasetTest, DataTypeAdaptationBetweenSteps) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/vrtprocesseddataset_test/proc_dt_src.tif", 4, 4, 1,
             GDT_Float32, kGt, nullptr, nullptr, {{1.0, 2.0, 3.0, 4.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_dt_src.tif</SourceFilename></Input>
  <ProcessingSteps>
    <Step><Algorithm>CustomIdentity</Algorithm></Step>
    <Step><Algorithm>CustomRequiresFloat64</Algorithm></Step>
  </ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);

  std::vector<double> data(16);
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 4, 4, data.data(), 4, 4,
                             GDT_Float64, 1, nullptr, 0, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(1.0, data[0]);
}

TEST_F(VrtProcessedDatasetTest, BSQInterleavedAndBlockCaching) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff(
      "/vsimem/vrtprocesseddataset_test/proc_bsq_src.tif", 4, 4, 2, GDT_Float32,
      kGt, nullptr, nullptr,
      {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
       {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160}},
      "BAND");

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <BlockXSize>4</BlockXSize>
  <BlockYSize>4</BlockYSize>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_bsq_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
  ASSERT_NE(nullptr, vrt_ds);
  ASSERT_EQ(2, vrt_ds->GetRasterCount());

  std::vector<float> b1(16);
  EXPECT_EQ(CE_None, vrt_ds->GetRasterBand(1)->ReadBlock(0, 0, b1.data()));
  EXPECT_FLOAT_EQ(1.0f, b1[0]);

  EXPECT_EQ(CE_None, vrt_ds->GetRasterBand(1)->ReadBlock(0, 0, b1.data()));

  std::vector<float> b2(16);
  EXPECT_EQ(CE_None, vrt_ds->GetRasterBand(2)->ReadBlock(0, 0, b2.data()));
  EXPECT_FLOAT_EQ(10.0f, b2[0]);

  std::vector<float> full_data(32);
  EXPECT_EQ(CE_None,
            vrt_ds->RasterIO(GF_Read, 0, 0, 4, 4, full_data.data(), 4, 4,
                             GDT_Float32, 2, nullptr, 0, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(1.0f, full_data[0]);
  EXPECT_FLOAT_EQ(10.0f, full_data[16]);
}

TEST_F(VrtProcessedDatasetTest, AdditionalBranchesAndEdgeCases) {
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};

  // 1. Offset != 0 while scale == 1.0 with unscale='AUTO'.
  {
    constexpr char kFile[] =
        "/vsimem/vrtprocesseddataset_test/proc_offset_only.tif";
    CreateTiff(kFile, 2, 2, 1, GDT_Float32, kGt, nullptr, nullptr, {{1}});
    GDALDatasetUniquePtr src = OpenRaster(kFile, GDAL_OF_UPDATE);
    ASSERT_NE(nullptr, src);
    EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetScale(1.0));
    EXPECT_EQ(CE_None, src->GetRasterBand(1)->SetOffset(5.0));
    src.reset();

    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input unscale='AUTO'><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_offset_only.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }

  // 2. unscale='YES' when scale == 1.0 and offset == 0.0.
  {
    CreateTiff("/vsimem/vrtprocesseddataset_test/proc_noscale.tif", 2, 2, 1,
               GDT_Float32, kGt, nullptr, nullptr, {{1}});
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input unscale='YES'><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_noscale.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    ASSERT_NE(nullptr, OpenRaster(kVrtXml));
  }

  // 3. Inline bandless VRT input (0 raster bands) fails.
  {
    ScopedErrorCapturer errors;
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><VRTDataset rasterXSize='2' rasterYSize='2'/></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    EXPECT_EQ(nullptr, OpenRaster(kVrtXml));
  }

  // 4. Source dataset SRS is inherited by the processed VRT dataset.
  {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
    char* wkt_raw = nullptr;
    ASSERT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt_raw));
    std::unique_ptr<char, CplFreeDeleter> wkt(wkt_raw);
    CreateTiff("/vsimem/vrtprocesseddataset_test/proc_srs_src.tif", 2, 2, 1,
               GDT_Float32, kGt, wkt.get(), nullptr, {{1}});

    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_srs_src.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_NE(nullptr, vrt_ds->GetSpatialRef());
  }

  // 5. Explicit OutputBands count and dataType.
  {
    CreateTiff("/vsimem/vrtprocesseddataset_test/proc_user_bands.tif", 2, 2, 2,
               GDT_Float32, kGt, nullptr, nullptr, {{1}, {2}});
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_user_bands.tif</SourceFilename></Input>
  <OutputBands count='2' dataType='Float32'/>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(2, vrt_ds->GetRasterCount());
  }

  // 6. Mixed band data types in input VRT emits a warning and succeeds.
  {
    constexpr char kMixedVrtPath[] =
        "/vsimem/vrtprocesseddataset_test/mixed_dt.vrt";
    constexpr char kMixedVrt[] = R"vrt(
<VRTDataset rasterXSize='2' rasterYSize='2'>
  <VRTRasterBand dataType='Byte' band='1'/>
  <VRTRasterBand dataType='Int16' band='2'/>
</VRTDataset>)vrt";
    VsiMemTempWrapper mixed_file(kMixedVrtPath, kMixedVrt);

    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/mixed_dt.vrt</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds;
    {
      ScopedErrorCapturer errors;
      vrt_ds = OpenRaster(kVrtXml);
      EXPECT_TRUE(errors.has_warning());
    }
    ASSERT_NE(nullptr, vrt_ds);
  }

  // 7. Declared VRTRasterBand dataType (Byte) differs from OutputBands
  // dataType='FROM_LAST_STEP' (Float32) -> band type is updated to Float32.
  {
    CreateTiff("/vsimem/vrtprocesseddataset_test/proc_clear_bands.tif", 2, 2, 1,
               GDT_Float32, kGt, nullptr, nullptr, {{1}});
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_clear_bands.tif</SourceFilename></Input>
  <OutputBands dataType='FROM_LAST_STEP'/>
  <VRTRasterBand dataType='Byte' band='1' subClass='VRTProcessedRasterBand'/>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);
    EXPECT_EQ(GDT_Float32, vrt_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 8. Progress callback returning FALSE aborts RasterIO.
  {
    CreateTiff("/vsimem/vrtprocesseddataset_test/proc_prog_cancel.tif", 2, 2, 1,
               GDT_Float32, kGt, nullptr, nullptr, {{1}});
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset subClass='VRTProcessedDataset'>
  <Input><SourceFilename>/vsimem/vrtprocesseddataset_test/proc_prog_cancel.tif</SourceFilename></Input>
  <ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></ProcessingSteps>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr vrt_ds = OpenRaster(kVrtXml);
    ASSERT_NE(nullptr, vrt_ds);

    GDALRasterIOExtraArg arg;
    INIT_RASTERIO_EXTRA_ARG(arg);
    arg.pfnProgress = [](double, const char*, void*) -> int { return FALSE; };
    std::vector<float> data(4);
    EXPECT_EQ(CE_Failure,
              vrt_ds->RasterIO(GF_Read, 0, 0, 2, 2, data.data(), 2, 2,
                               GDT_Float32, 1, nullptr, 0, 0, 0, &arg));
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and internal buffer reallocation
// paths in VRTProcessedDataset in vrtprocesseddataset.cpp.
