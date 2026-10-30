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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

// Custom Test Functions to register with GDALVRTRegisterProcessedDatasetFunc

// 1. CustomIdentity: No pfnInit, simple passthrough in pfnProcess
CPLErr CustomIdentityProcess(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr /*pWorkingData*/, CSLConstList /*papszFunctionArgs*/,
    int /*nBufXSize*/, int /*nBufYSize*/, const void* pInBuffer,
    size_t nInBufferSize, GDALDataType /*eInDT*/, int /*nInBands*/,
    const double* /*padfInNoData*/, void* pOutBuffer, size_t nOutBufferSize,
    GDALDataType /*eOutDT*/, int /*nOutBands*/, const double* /*padfOutNoData*/,
    double /*dfSrcXOff*/, double /*dfSrcYOff*/, double /*dfSrcXSize*/,
    double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  size_t nToCopy = std::min(nInBufferSize, nOutBufferSize);
  std::memcpy(pOutBuffer, pInBuffer, nToCopy);
  return CE_None;
}

// 2. CustomTyped: With metadata, pfnInit, pfnFree, pfnProcess
struct CustomTypedData {
  double multiplier = 1.0;
  double addition = 0.0;
};

CPLErr CustomTypedInit(const char* /*pszFuncName*/, void* /*pUserData*/,
                       CSLConstList papszFunctionArgs, int nInBands,
                       GDALDataType eInDT, double* /*padfInNoData*/,
                       int* pnOutBands, GDALDataType* peOutDT,
                       double** ppadfOutNoData, const char* /*pszVRTPath*/,
                       VRTPDWorkingDataPtr* ppWorkingData) {
  auto* data = new CustomTypedData();
  const char* pszMul = CSLFetchNameValue(papszFunctionArgs, "multiplier");
  if (pszMul) data->multiplier = CPLAtof(pszMul);
  const char* pszAdd = CSLFetchNameValue(papszFunctionArgs, "addition");
  if (pszAdd) data->addition = CPLAtof(pszAdd);

  *peOutDT = eInDT;
  if (*pnOutBands == 0) {
    *pnOutBands = nInBands;
  }
  if (*ppadfOutNoData == nullptr) {
    *ppadfOutNoData =
        static_cast<double*>(CPLMalloc(sizeof(double) * (*pnOutBands)));
    for (int i = 0; i < *pnOutBands; ++i) {
      (*ppadfOutNoData)[i] = -999.0;
    }
  }

  *ppWorkingData = data;
  return CE_None;
}

void CustomTypedFree(const char* /*pszFuncName*/, void* /*pUserData*/,
                     VRTPDWorkingDataPtr pWorkingData) {
  delete static_cast<CustomTypedData*>(pWorkingData);
}

CPLErr CustomTypedProcess(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr pWorkingData, CSLConstList /*papszFunctionArgs*/,
    int nBufXSize, int nBufYSize, const void* pInBuffer,
    size_t /*nInBufferSize*/, GDALDataType eInDT, int nInBands,
    const double* /*padfInNoData*/, void* pOutBuffer, size_t /*nOutBufferSize*/,
    GDALDataType eOutDT, int nOutBands, const double* /*padfOutNoData*/,
    double /*dfSrcXOff*/, double /*dfSrcYOff*/, double /*dfSrcXSize*/,
    double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  auto* data = static_cast<CustomTypedData*>(pWorkingData);
  double mul = data ? data->multiplier : 1.0;
  double add = data ? data->addition : 0.0;

  const size_t nPixels = static_cast<size_t>(nBufXSize) * nBufYSize;
  if (eInDT == GDT_Float32 && eOutDT == GDT_Float32) {
    const float* pIn = static_cast<const float*>(pInBuffer);
    float* pOut = static_cast<float*>(pOutBuffer);
    int nBands = std::min(nInBands, nOutBands);
    for (size_t i = 0; i < nPixels * nBands; ++i) {
      pOut[i] = static_cast<float>(pIn[i] * mul + add);
    }
  } else if (eInDT == GDT_Float64 && eOutDT == GDT_Float64) {
    const double* pIn = static_cast<const double*>(pInBuffer);
    double* pOut = static_cast<double*>(pOutBuffer);
    int nBands = std::min(nInBands, nOutBands);
    for (size_t i = 0; i < nPixels * nBands; ++i) {
      pOut[i] = pIn[i] * mul + add;
    }
  }
  return CE_None;
}

// 3. CustomFail: Always returns CE_Failure in process
CPLErr CustomFailProcess(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr /*pWorkingData*/, CSLConstList /*papszFunctionArgs*/,
    int /*nBufXSize*/, int /*nBufYSize*/, const void* /*pInBuffer*/,
    size_t /*nInBufferSize*/, GDALDataType /*eInDT*/, int /*nInBands*/,
    const double* /*padfInNoData*/, void* /*pOutBuffer*/,
    size_t /*nOutBufferSize*/, GDALDataType /*eOutDT*/, int /*nOutBands*/,
    const double* /*padfOutNoData*/, double /*dfSrcXOff*/, double /*dfSrcYOff*/,
    double /*dfSrcXSize*/, double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  return CE_Failure;
}

// 4. CustomTypeChanger: converts Float32 to Float64
CPLErr CustomTypeChangerInit(const char* /*pszFuncName*/, void* /*pUserData*/,
                             CSLConstList /*papszFunctionArgs*/, int nInBands,
                             GDALDataType /*eInDT*/, double* /*padfInNoData*/,
                             int* pnOutBands, GDALDataType* peOutDT,
                             double** ppadfOutNoData,
                             const char* /*pszVRTPath*/,
                             VRTPDWorkingDataPtr* /*ppWorkingData*/) {
  *peOutDT = GDT_Float64;
  if (*pnOutBands == 0) {
    *pnOutBands = nInBands;
  }
  if (*ppadfOutNoData == nullptr) {
    *ppadfOutNoData =
        static_cast<double*>(CPLMalloc(sizeof(double) * (*pnOutBands)));
    for (int i = 0; i < *pnOutBands; ++i) {
      (*ppadfOutNoData)[i] = -999.0;
    }
  }
  return CE_None;
}

CPLErr CustomTypeChangerProcess(
    const char* /*pszFuncName*/, void* /*pUserData*/,
    VRTPDWorkingDataPtr /*pWorkingData*/, CSLConstList /*papszFunctionArgs*/,
    int nBufXSize, int nBufYSize, const void* pInBuffer,
    size_t /*nInBufferSize*/, GDALDataType /*eInDT*/, int nInBands,
    const double* /*padfInNoData*/, void* pOutBuffer, size_t /*nOutBufferSize*/,
    GDALDataType /*eOutDT*/, int nOutBands, const double* /*padfOutNoData*/,
    double /*dfSrcXOff*/, double /*dfSrcYOff*/, double /*dfSrcXSize*/,
    double /*dfSrcYSize*/, const double /*adfSrcGT*/[6],
    const char* /*pszVRTPath*/, CSLConstList /*papszExtra*/) {
  const float* pIn = static_cast<const float*>(pInBuffer);
  double* pOut = static_cast<double*>(pOutBuffer);
  const size_t nPixels = static_cast<size_t>(nBufXSize) * nBufYSize;
  int nBands = std::min(nInBands, nOutBands);
  for (size_t i = 0; i < nPixels * nBands; ++i) {
    pOut[i] = pIn[i] * 2.0;
  }
  return CE_None;
}

class VRTProcessedDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();

    // Register CustomIdentity
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomIdentity", nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr, 0,
        nullptr, nullptr, CustomIdentityProcess, nullptr);

    // Register CustomTyped
    const char* pszTypedMetadata =
        "<ProcessedDatasetFunctionArgumentsList>\n"
        "  <Argument name='const_arg' type='constant' value='42'/>\n"
        "  <Argument name='nodata' type='builtin'/>\n"
        "  <Argument name='bool_arg' type='boolean'/>\n"
        "  <Argument name='int_arg' type='integer'/>\n"
        "  <Argument name='double_arg' type='double'/>\n"
        "  <Argument name='list_arg' type='double_list'/>\n"
        "  <Argument name='req_arg' type='string' required='true'/>\n"
        "  <Argument name='multiplier' type='double'/>\n"
        "  <Argument name='addition' type='double'/>\n"
        "</ProcessedDatasetFunctionArgumentsList>\n";

    const GDALDataType aeSupportedDT[2] = {GDT_Float32, GDT_Float64};
    const int anSupportedBands[3] = {1, 2, 3};
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomTyped", nullptr, pszTypedMetadata, GDT_Unknown, aeSupportedDT, 2,
        anSupportedBands, 3, CustomTypedInit, CustomTypedFree,
        CustomTypedProcess, nullptr);

    // Register CustomFail
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomFail", nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr, 0,
        nullptr, nullptr, CustomFailProcess, nullptr);

    // Register CustomTypeChanger
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomTypeChanger", nullptr, nullptr, GDT_Float32, nullptr, 0, nullptr,
        0, CustomTypeChangerInit, nullptr, CustomTypeChangerProcess, nullptr);

    // Register CustomScaleOffset
    const char* pszScaleOffsetMetadata =
        "<ProcessedDatasetFunctionArgumentsList>\n"
        "  <Argument name='offset_{band}' type='builtin'/>\n"
        "  <Argument name='scale_{band}' type='builtin'/>\n"
        "  <Argument name='coefficients_{band}' type='double'/>\n"
        "</ProcessedDatasetFunctionArgumentsList>\n";
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomScaleOffset", nullptr, pszScaleOffsetMetadata, GDT_Unknown,
        nullptr, 0, nullptr, 0, nullptr, nullptr, CustomIdentityProcess,
        nullptr);

    // Register CustomInitFail
    auto customInitFailInit =
        [](const char*, void*, CSLConstList, int, GDALDataType, double*, int*,
           GDALDataType*, double**, const char*,
           VRTPDWorkingDataPtr*) -> CPLErr { return CE_Failure; };
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomInitFail", nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr, 0,
        customInitFailInit, nullptr, CustomIdentityProcess, nullptr);

    // Register CustomInitNullNoData
    auto customInitNullNoData = [](const char*, void*, CSLConstList,
                                   int nInBands, GDALDataType eInDT, double*,
                                   int* pnOutBands, GDALDataType* peOutDT,
                                   double** ppadfOutNoData, const char*,
                                   VRTPDWorkingDataPtr*) -> CPLErr {
      *pnOutBands = nInBands;
      *peOutDT = eInDT;
      if (ppadfOutNoData && *ppadfOutNoData) {
        CPLFree(*ppadfOutNoData);
        *ppadfOutNoData = nullptr;
      }
      return CE_None;
    };
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomInitNullNoData", nullptr, nullptr, GDT_Unknown, nullptr, 0,
        nullptr, 0, customInitNullNoData, nullptr, CustomIdentityProcess,
        nullptr);

    // Register CustomRequiresFloat64
    GDALVRTRegisterProcessedDatasetFunc(
        "CustomRequiresFloat64", nullptr, nullptr, GDT_Float64, nullptr, 0,
        nullptr, 0, nullptr, nullptr, CustomIdentityProcess, nullptr);
  }

  void TearDown() override {
    for (const auto& filename : to_unlink_) {
      VSIUnlink(filename.c_str());
      VSIUnlink((filename + ".ovr").c_str());
    }
    to_unlink_.clear();
  }

  void TrackFile(const std::string& filename) {
    to_unlink_.push_back(filename);
  }

  void CreateTiff(const std::string& filename, int xsize, int ysize, int bands,
                  GDALDataType dt, const double* gt, const char* srs_wkt,
                  const double* nodata,
                  const std::vector<std::vector<double>>& band_data,
                  const char* interleave = nullptr,
                  const std::vector<int>& overviews = {}) {
    TrackFile(filename);
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);

    char** papszOptions = nullptr;
    if (interleave) {
      papszOptions = CSLSetNameValue(papszOptions, "INTERLEAVE", interleave);
    }

    auto poDS = std::unique_ptr<GDALDataset>(poDriver->Create(
        filename.c_str(), xsize, ysize, bands, dt, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(poDS, nullptr);

    if (gt != nullptr) {
      EXPECT_EQ(poDS->SetGeoTransform(const_cast<double*>(gt)), CE_None);
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      srs.importFromWkt(srs_wkt);
      EXPECT_EQ(poDS->SetSpatialRef(&srs), CE_None);
    }

    for (int b = 1; b <= bands; ++b) {
      auto* poBand = poDS->GetRasterBand(b);
      if (nodata != nullptr) {
        EXPECT_EQ(poBand->SetNoDataValue(nodata[b - 1]), CE_None);
      }
      if (b - 1 < static_cast<int>(band_data.size())) {
        const auto& data = band_data[b - 1];
        if (!data.empty()) {
          const size_t total_pixels = static_cast<size_t>(xsize) * ysize;
          const double* write_ptr = data.data();
          std::vector<double> padded_data;
          if (data.size() < total_pixels) {
            padded_data.assign(total_pixels, data[0]);
            std::copy(data.begin(), data.end(), padded_data.begin());
            write_ptr = padded_data.data();
          }
          EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                     const_cast<double*>(write_ptr), xsize,
                                     ysize, GDT_Float64, 0, 0),
                    CE_None);
        }
      }
    }

    if (!overviews.empty()) {
      EXPECT_EQ(
          poDS->BuildOverviews("NEAREST", static_cast<int>(overviews.size()),
                               const_cast<int*>(overviews.data()), 0, nullptr,
                               nullptr, nullptr),
          CE_None);
    }
  }

 private:
  std::vector<std::string> to_unlink_;
};

// =========================================================================
// GDALVRTRegisterProcessedDatasetFunc Validation
// =========================================================================

TEST_F(VRTProcessedDatasetTest, RegistrationValidation) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Null or empty func name
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                nullptr, nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr, 0,
                nullptr, nullptr, CustomIdentityProcess, nullptr),
            CE_Failure);
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "", nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr, 0,
                nullptr, nullptr, CustomIdentityProcess, nullptr),
            CE_Failure);

  // 2. Already registered function name
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "CustomIdentity", nullptr, nullptr, GDT_Unknown, nullptr, 0,
                nullptr, 0, nullptr, nullptr, CustomIdentityProcess, nullptr),
            CE_Failure);

  // 3. Null pfnProcess
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc1", nullptr, nullptr, GDT_Unknown, nullptr, 0, nullptr,
                0, nullptr, nullptr, nullptr, nullptr),
            CE_Failure);

  // 4. Invalid XML metadata
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc2", nullptr, "<InvalidXML", GDT_Unknown, nullptr, 0,
                nullptr, 0, nullptr, nullptr, CustomIdentityProcess, nullptr),
            CE_Failure);

  // 5. Wrong root element in XML metadata
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc3", nullptr, "<WrongRoot/>", GDT_Unknown, nullptr, 0,
                nullptr, 0, nullptr, nullptr, CustomIdentityProcess, nullptr),
            CE_Failure);

  // 6. Missing Argument.name
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc4", nullptr,
                "<ProcessedDatasetFunctionArgumentsList><Argument "
                "type='string'/></ProcessedDatasetFunctionArgumentsList>",
                GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
                CustomIdentityProcess, nullptr),
            CE_Failure);

  // 7. Missing Argument.type
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc5", nullptr,
                "<ProcessedDatasetFunctionArgumentsList><Argument "
                "name='foo'/></ProcessedDatasetFunctionArgumentsList>",
                GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
                CustomIdentityProcess, nullptr),
            CE_Failure);

  // 8. type="constant" missing value
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc6", nullptr,
                "<ProcessedDatasetFunctionArgumentsList><Argument name='foo' "
                "type='constant'/></ProcessedDatasetFunctionArgumentsList>",
                GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
                CustomIdentityProcess, nullptr),
            CE_Failure);

  // 9. type="builtin" unsupported name
  EXPECT_EQ(
      GDALVRTRegisterProcessedDatasetFunc(
          "NewFunc7", nullptr,
          "<ProcessedDatasetFunctionArgumentsList><Argument name='unsupported' "
          "type='builtin'/></ProcessedDatasetFunctionArgumentsList>",
          GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
          CustomIdentityProcess, nullptr),
      CE_Failure);

  // 10. Unsupported type attribute
  EXPECT_EQ(GDALVRTRegisterProcessedDatasetFunc(
                "NewFunc8", nullptr,
                "<ProcessedDatasetFunctionArgumentsList><Argument name='foo' "
                "type='unknown_type'/></ProcessedDatasetFunctionArgumentsList>",
                GDT_Unknown, nullptr, 0, nullptr, 0, nullptr, nullptr,
                CustomIdentityProcess, nullptr),
            CE_Failure);

  CPLPopErrorHandler();
}

// =========================================================================
// Basic Processing and Custom Algorithms
// =========================================================================

TEST_F(VRTProcessedDatasetTest, BasicProcessingAndArguments) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  const double nodata[1] = {-999.0};
  CreateTiff("/vsimem/proc_src.tif", 4, 4, 1, GDT_Float32, gt, nullptr, nodata,
             {{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0,
               13.0, 14.0, 15.0, 16.0}});

  // Test: CustomTyped algorithm with all argument types validated
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='4' rasterYSize='4' "
      "subClass='VRTProcessedDataset'>\n"
      "  <BlockXSize>2</BlockXSize>\n"
      "  <BlockYSize>2</BlockYSize>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/proc_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='step1'>\n"
      "      <Algorithm>CustomTyped</Algorithm>\n"
      "      <Argument name='bool_arg'>true</Argument>\n"
      "      <Argument name='int_arg'>10</Argument>\n"
      "      <Argument name='double_arg'>3.14</Argument>\n"
      "      <Argument name='list_arg'>1.0,2.0,3.0</Argument>\n"
      "      <Argument name='req_arg'>hello</Argument>\n"
      "      <Argument name='multiplier'>2.0</Argument>\n"
      "      <Argument name='addition'>5.0</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  EXPECT_EQ(poVRT->GetRasterXSize(), 4);
  EXPECT_EQ(poVRT->GetRasterYSize(), 4);
  EXPECT_EQ(poVRT->GetRasterCount(), 1);

  // GetBlockSize
  int bx = 0, by = 0;
  static_cast<VRTProcessedDataset*>(poVRT.get())->GetBlockSize(&bx, &by);
  EXPECT_EQ(bx, 2);
  EXPECT_EQ(by, 2);

  // ReadBlock on band 1
  std::vector<float> block(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->ReadBlock(0, 0, block.data()), CE_None);
  // Pixel (0, 0) was 1.0 -> 1.0 * 2.0 + 5.0 = 7.0
  EXPECT_FLOAT_EQ(block[0], 7.0f);
  // Pixel (1, 0) was 2.0 -> 2.0 * 2.0 + 5.0 = 9.0
  EXPECT_FLOAT_EQ(block[1], 9.0f);

  // Read full dataset with RasterIO
  std::vector<float> full(16);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, full.data(), 4, 4, GDT_Float32,
                            1, nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(full[0], 7.0f);
  EXPECT_FLOAT_EQ(full[15], 16.0f * 2.0f + 5.0f);

  // Serialize to XML
  char** papszMD = poVRT->GetMetadata("xml:VRT");
  ASSERT_NE(papszMD, nullptr);
  ASSERT_NE(papszMD[0], nullptr);
  const std::string serialized = papszMD[0];
  EXPECT_NE(serialized.find("<Algorithm>CustomTyped</Algorithm>"),
            std::string::npos);
  EXPECT_NE(serialized.find("subClass=\"VRTProcessedRasterBand\""),
            std::string::npos);

  // Re-open from serialized XML
  auto poVRT2 = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(serialized.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT2, nullptr);
}

TEST_F(VRTProcessedDatasetTest, CustomIdentityNoInit) {
  // Test algorithm without pfnInit
  const double gt[6] = {0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_noinit.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             nullptr, {{10.0, 20.0, 30.0, 40.0}});

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/proc_noinit.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='identity'>\n"
      "      <Algorithm>CustomIdentity</Algorithm>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  std::vector<float> data(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, data.data(),
                                              2, 2, GDT_Float32, 0, 0),
            CE_None);
  EXPECT_FLOAT_EQ(data[0], 10.0f);
  EXPECT_FLOAT_EQ(data[3], 40.0f);
}

// =========================================================================
// Inlined VRTDataset in Input and Chaining with Datatype Change
// =========================================================================

TEST_F(VRTProcessedDatasetTest, InlinedVRTDatasetAndTypeConversion) {
  const double gt[6] = {0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_inlined_src.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
             nullptr, {{10.0, 20.0, 30.0, 40.0}});

  // Step 1: CustomTypeChanger (Float32 in -> Float64 out)
  // Step 2: CustomTyped (Float64 in -> Float64 out)
  const std::string vrt_xml =
      "<VRTDataset rasterXSize='2' rasterYSize='2' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <VRTDataset rasterXSize='2' rasterYSize='2'>\n"
      "      <VRTRasterBand dataType='Float32' band='1'>\n"
      "        <SimpleSource>\n"
      "          <SourceFilename "
      "relativeToVRT='0'>/vsimem/proc_inlined_src.tif</SourceFilename>\n"
      "          <SourceBand>1</SourceBand>\n"
      "          <SrcRect xOff='0' yOff='0' xSize='2' ySize='2'/>\n"
      "          <DstRect xOff='0' yOff='0' xSize='2' ySize='2'/>\n"
      "        </SimpleSource>\n"
      "      </VRTRasterBand>\n"
      "    </VRTDataset>\n"
      "  </Input>\n"
      "  <OutputBands count='FROM_LAST_STEP' dataType='FROM_LAST_STEP'/>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='changer'>\n"
      "      <Algorithm>CustomTypeChanger</Algorithm>\n"
      "    </Step>\n"
      "    <Step name='typed'>\n"
      "      <Algorithm>CustomTyped</Algorithm>\n"
      "      <Argument name='req_arg'>ok</Argument>\n"
      "      <Argument name='multiplier'>3.0</Argument>\n"
      "      <Argument name='addition'>1.0</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);
  EXPECT_EQ(poVRT->GetRasterBand(1)->GetRasterDataType(), GDT_Float64);

  std::vector<double> out(4);
  EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                              2, 2, GDT_Float64, 0, 0),
            CE_None);
  // 10 -> Changer (10 * 2 = 20) -> Typed (20 * 3 + 1 = 61)
  EXPECT_DOUBLE_EQ(out[0], 61.0);
  // 40 -> Changer (40 * 2 = 80) -> Typed (80 * 3 + 1 = 241)
  EXPECT_DOUBLE_EQ(out[3], 241.0);
}

// =========================================================================
// BSQ Multi-band Source and RAM-splitting in IRasterIO
// =========================================================================

TEST_F(VRTProcessedDatasetTest, MultiBandBSQAndRAMSplitting) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  // 2 bands with INTERLEAVE=BAND (BSQ)
  CreateTiff(
      "/vsimem/proc_bsq.tif", 4, 4, 2, GDT_Float32, gt, nullptr, nullptr,
      {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
       {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160}},
      "BAND");

  const std::string vrt_xml =
      "<VRTDataset rasterXSize='4' rasterYSize='4' "
      "subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/proc_bsq.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='identity'>\n"
      "      <Algorithm>CustomIdentity</Algorithm>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);
  EXPECT_EQ(poVRT->GetRasterCount(), 2);
  EXPECT_STREQ(poVRT->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"),
               "PIXEL");

  // Read full dataset in BIP mode
  std::vector<float> bip_data(4 * 4 * 2);
  int panBandMap[2] = {1, 2};
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, bip_data.data(), 4, 4,
                            GDT_Float32, 2, panBandMap, 2 * sizeof(float),
                            4 * 2 * sizeof(float), sizeof(float), nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(bip_data[0], 1.0f);
  EXPECT_FLOAT_EQ(bip_data[1], 10.0f);

  // Read full dataset in BSQ mode
  std::vector<float> bsq_data(4 * 4 * 2);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, bsq_data.data(), 4, 4,
                            GDT_Float32, 2, panBandMap, sizeof(float),
                            4 * sizeof(float), 16 * sizeof(float), nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(bsq_data[0], 1.0f);
  EXPECT_FLOAT_EQ(bsq_data[16], 10.0f);

  // Test RAM splitting by setting VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE to 16
  // bytes
  CPLSetConfigOption("VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE", "16B");
  std::vector<float> split_data(4 * 4 * 2);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, split_data.data(), 4, 4,
                            GDT_Float32, 2, panBandMap, 2 * sizeof(float),
                            4 * 2 * sizeof(float), sizeof(float), nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(split_data[0], 1.0f);
  EXPECT_FLOAT_EQ(split_data[1], 10.0f);
  CPLSetConfigOption("VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE", nullptr);
}

// =========================================================================
// Unscale tests (AUTO, YES, NO)
// =========================================================================

TEST_F(VRTProcessedDatasetTest, UnscaleOptions) {
  const double gt[6] = {0.0, 1.0, 0.0, 2.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_unscale_src.tif", 2, 2, 1, GDT_UInt16, gt, nullptr,
             nullptr, {{100, 200, 300, 400}});

  // Set scale=0.1 and offset=10 on source
  {
    auto poSrc = std::unique_ptr<GDALDataset>(
        GDALDataset::Open("/vsimem/proc_unscale_src.tif", GA_Update));
    ASSERT_NE(poSrc, nullptr);
    poSrc->GetRasterBand(1)->SetScale(0.1);
    poSrc->GetRasterBand(1)->SetOffset(10.0);
  }

  // 1. unscale="AUTO" (detected and unscaled)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input unscale='AUTO'>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/proc_unscale_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='identity'>\n"
        "      <Algorithm>CustomIdentity</Algorithm>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<double> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_Float64, 0, 0),
              CE_None);
    // 100 * 0.1 + 10 = 20
    EXPECT_DOUBLE_EQ(out[0], 20.0);
  }

  // 2. unscale="NO" (remains raw unscaled values)
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input unscale='NO'>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/proc_unscale_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='identity'>\n"
        "      <Algorithm>CustomIdentity</Algorithm>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<uint16_t> out(4);
    EXPECT_EQ(poVRT->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, out.data(),
                                                2, 2, GDT_UInt16, 0, 0),
              CE_None);
    EXPECT_EQ(out[0], 100);
  }

  // 3. Invalid unscale value
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='2' rasterYSize='2' "
        "subClass='VRTProcessedDataset'>\n"
        "  <Input unscale='INVALID'>\n"
        "    <SourceFilename "
        "relativeToVRT='0'>/vsimem/proc_unscale_src.tif</SourceFilename>\n"
        "  </Input>\n"
        "  <ProcessingSteps>\n"
        "    <Step name='identity'>\n"
        "      <Algorithm>CustomIdentity</Algorithm>\n"
        "    </Step>\n"
        "  </ProcessingSteps>\n"
        "</VRTDataset>\n";

    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  CPLPopErrorHandler();
}

// =========================================================================
// Overviews
// =========================================================================

TEST_F(VRTProcessedDatasetTest, Overviews) {
  const double gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_ovr_src.tif", 8, 8, 1, GDT_Float32, gt, nullptr,
             nullptr, {{100.0}}, nullptr, {2});

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTProcessedDataset'>\n"
      "  <Input>\n"
      "    <SourceFilename "
      "relativeToVRT='0'>/vsimem/proc_ovr_src.tif</SourceFilename>\n"
      "  </Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step name='identity'>\n"
      "      <Algorithm>CustomIdentity</Algorithm>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poVRT, nullptr);

  auto* poBand = poVRT->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  // Overview count should be 1
  EXPECT_EQ(poBand->GetOverviewCount(), 1);
  EXPECT_EQ(poBand->GetOverview(-1), nullptr);
  EXPECT_EQ(poBand->GetOverview(99), nullptr);

  auto* poOvrBand = poBand->GetOverview(0);
  ASSERT_NE(poOvrBand, nullptr);
  EXPECT_EQ(poOvrBand->GetXSize(), 4);
  EXPECT_EQ(poOvrBand->GetYSize(), 4);

  std::vector<float> ovr_data(16);
  EXPECT_EQ(poOvrBand->RasterIO(GF_Read, 0, 0, 4, 4, ovr_data.data(), 4, 4,
                                GDT_Float32, 0, 0),
            CE_None);
  EXPECT_GT(ovr_data[0], 0.0f);

  // Downsampled read on dataset triggering TryOverviewRasterIO
  std::vector<float> ds_data(16);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 8, 8, ds_data.data(), 4, 4,
                            GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_GT(ds_data[0], 0.0f);
}

// =========================================================================
// Error Branches in XMLInit, ParseStep, ProcessRegion
// =========================================================================

TEST_F(VRTProcessedDatasetTest, ErrorBranches) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_err_src.tif", 4, 4, 1, GDT_Float32, gt, nullptr,
             nullptr, {{10.0}});

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Missing Input element
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'/>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 2. Input lacks SourceFilename or VRTDataset
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'><Input/></VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 3. Nonexistent source file
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/nonexistent.tif</SourceFilename></"
        "Input>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 4. Inconsistent declared dimensions
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='8' rasterYSize='8' "
        "subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 5. Invalid BlockXSize <= 1
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  <BlockXSize>1</BlockXSize>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 6. Invalid BlockYSize <= 1
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  <BlockYSize>0</BlockYSize>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 7. OutputBands count='invalid'
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <OutputBands count='invalid'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 8. OutputBands count='0'
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <OutputBands count='0'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 9. OutputBands dataType='invalid'
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <OutputBands dataType='invalid_type'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 10. Missing ProcessingSteps
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 11. Empty ProcessingSteps (no Step)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 12. Step lacking Algorithm
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step/></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 13. Step with unregistered algorithm
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>UnknownAlg</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 14. Step with unsupported input data type
  {
    CreateTiff("/vsimem/proc_int_src.tif", 2, 2, 1, GDT_Int16, gt, nullptr,
               nullptr, {{1}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_int_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 15. Step with unsupported input band count (4 bands, CustomTyped only
  // supports 1..3)
  {
    CreateTiff("/vsimem/proc_4b_src.tif", 2, 2, 4, GDT_Float32, gt, nullptr,
               nullptr, {{1}, {2}, {3}, {4}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_4b_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 16. Argument without name attribute
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomTyped</"
        "Algorithm><Argument>val</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 17. Argument with non-boolean value
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument><Argument "
        "name='bool_arg'>not_a_bool</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 18. Argument with non-integer value
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument><Argument "
        "name='int_arg'>3.14</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 19. Argument with non-double value
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument><Argument "
        "name='double_arg'>not_a_num</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 20. Argument with invalid double_list
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  <ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm><Argument "
        "name='req_arg'>x</Argument><Argument "
        "name='list_arg'>1.0,foo,2.0</Argument></Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 21. Missing required argument ('req_arg')
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomTyped</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 22. Inconsistent output band count with VRTRasterBand count
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "  <VRTRasterBand dataType='Float32' band='1' "
        "subClass='VRTProcessedRasterBand'/>\n"
        "  <VRTRasterBand dataType='Float32' band='2' "
        "subClass='VRTProcessedRasterBand'/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  // 23. Process failure (CustomFail)
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_err_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomFail</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    std::vector<float> data(16);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, data.data(), 4, 4,
                              GDT_Float32, 1, nullptr, 0, 0, 0, nullptr),
              CE_Failure);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// RAM splitting and block size reduction
// =========================================================================

TEST_F(VRTProcessedDatasetTest, RAMSplittingAndReduction) {
  const double gt[6] = {0.0, 1.0, 0.0, 8.0, 0.0, -1.0};
  std::vector<double> src_data(16 * 8, 1.0);
  CreateTiff("/vsimem/proc_ram_src.tif", 16, 8, 1, GDT_Float32, gt, nullptr,
             nullptr, {src_data});

  // Set low RAM usage limit before opening dataset
  CPLSetConfigOption("VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE", "16");

  // Case 1: with explicit BlockXSize/BlockYSize
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='16' rasterYSize='8' "
        "subClass='VRTProcessedDataset'>\n"
        "  <BlockXSize>8</BlockXSize>\n"
        "  <BlockYSize>8</BlockYSize>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_ram_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);

    int anBandMap[1] = {1};
    const int nPixelSpace = sizeof(float);
    const int nLineSpace = nPixelSpace * 16;
    const int nBandSpace = nLineSpace * 8;

    // Vertical split with progress
    GDALRasterIOExtraArg sArg;
    INIT_RASTERIO_EXTRA_ARG(sArg);
    sArg.pfnProgress = GDALDummyProgress;
    std::vector<float> data(16 * 8);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 16, 8, data.data(), 16, 8,
                              GDT_Float32, 1, anBandMap, nPixelSpace,
                              nLineSpace, nBandSpace, &sArg),
              CE_None);

    // Horizontal split with progress
    const int hPixelSpace = sizeof(float);
    const int hLineSpace = hPixelSpace * 8;
    const int hBandSpace = hLineSpace * 2;
    std::vector<float> h_data(8 * 2);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 8, 2, h_data.data(), 8, 2,
                              GDT_Float32, 1, anBandMap, hPixelSpace,
                              hLineSpace, hBandSpace, &sArg),
              CE_None);
  }

  // Case 2: without explicit BlockXSize/BlockYSize
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize='16' rasterYSize='8' "
        "subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_ram_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";

    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  CPLSetConfigOption("VRT_PROCESSED_DATASET_ALLOWED_RAM_USAGE", nullptr);
}

// =========================================================================
// Builtin Scale/Offset and XML Serialization
// =========================================================================

TEST_F(VRTProcessedDatasetTest, BuiltinScaleOffsetAndXmlSerialization) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_so_src.tif", 4, 4, 1, GDT_Float32, gt, nullptr,
             nullptr,
             {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}});

  // Set scale and offset on source
  {
    auto poSrc = std::unique_ptr<GDALDataset>(
        GDALDataset::Open("/vsimem/proc_so_src.tif", GA_Update));
    ASSERT_NE(poSrc, nullptr);
    poSrc->GetRasterBand(1)->SetScale(1.5);
    poSrc->GetRasterBand(1)->SetOffset(2.5);
  }

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTProcessedDataset'><VRTRasterBand "
      "dataType='Float32' band='1' subClass='VRTProcessedRasterBand'>\n"
      "    <Offset>2.5</Offset>\n"
      "    <Scale>1.5</Scale>\n"
      "  </VRTRasterBand>\n"
      "  <OutputBands count='1'/>\n"
      "  "
      "<Input><SourceFilename>/vsimem/proc_so_src.tif</SourceFilename></"
      "Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step>\n"
      "      <Algorithm>CustomScaleOffset</Algorithm>\n"
      "      <Argument name='coefficients_1'>3.14</Argument>\n"
      "      <Argument name='unsupported_arg'>val</Argument>\n"
      "    </Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  TrackFile("/vsimem/proc_so.vrt");
  {
    VSILFILE* fp = VSIFOpenL("/vsimem/proc_so.vrt", "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(vrt_xml.data(), 1, vrt_xml.size(), fp);
    VSIFCloseL(fp);
  }

  // Open with filename to test pszVRTPathIn
  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open("/vsimem/proc_so.vrt", GDAL_OF_RASTER));
  CPLPopErrorHandler();
  ASSERT_NE(poVRT, nullptr);

  // SerializeToXML should succeed
  auto* poVRTDS = dynamic_cast<VRTDataset*>(poVRT.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto* psTree = poVRTDS->SerializeToXML(nullptr);
  ASSERT_NE(psTree, nullptr);
  EXPECT_NE(CPLGetXMLNode(psTree, "VRTRasterBand"), nullptr);
  CPLDestroyXMLNode(psTree);
}

// =========================================================================
// CustomInitFail and CustomInitNullNoData
// =========================================================================

TEST_F(VRTProcessedDatasetTest, CustomInitFailAndNullNoData) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_init_src.tif", 4, 4, 1, GDT_Float32, gt, nullptr,
             nullptr, {{1}});

  // 1. CustomInitFail
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_init_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomInitFail</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  CPLPopErrorHandler();

  // 2. CustomInitNullNoData
  {
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_init_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomInitNullNoData</Algorithm></"
        "Step></ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }
}

// =========================================================================
// Data type adaptation between steps
// =========================================================================

TEST_F(VRTProcessedDatasetTest, DataTypeAdaptationBetweenSteps) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff("/vsimem/proc_dt_src.tif", 4, 4, 1, GDT_Float32, gt, nullptr,
             nullptr, {{1.0, 2.0, 3.0, 4.0}});

  // Step 1 outputs Float32, Step 2 requires Float64 -> data type adaptation
  const std::string vrt_xml =
      "<VRTDataset subClass='VRTProcessedDataset'>\n"
      "  "
      "<Input><SourceFilename>/vsimem/proc_dt_src.tif</SourceFilename></"
      "Input>\n"
      "  <ProcessingSteps>\n"
      "    <Step><Algorithm>CustomIdentity</Algorithm></Step>\n"
      "    <Step><Algorithm>CustomRequiresFloat64</Algorithm></Step>\n"
      "  </ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT, nullptr);

  std::vector<double> data(16);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, data.data(), 4, 4, GDT_Float64,
                            1, nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(data[0], 1.0);
}

// =========================================================================
// BSQ interleaved multi-band and block caching
// =========================================================================

TEST_F(VRTProcessedDatasetTest, BSQInterleavedAndBlockCaching) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  CreateTiff(
      "/vsimem/proc_bsq_src.tif", 4, 4, 2, GDT_Float32, gt, nullptr, nullptr,
      {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
       {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160}},
      "BAND");

  const std::string vrt_xml =
      "<VRTDataset subClass='VRTProcessedDataset'>\n"
      "  <BlockXSize>4</BlockXSize>\n"
      "  <BlockYSize>4</BlockYSize>\n"
      "  "
      "<Input><SourceFilename>/vsimem/proc_bsq_src.tif</SourceFilename></"
      "Input>\n"
      "  "
      "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
      "ProcessingSteps>\n"
      "</VRTDataset>\n";

  auto poVRT = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poVRT, nullptr);
  ASSERT_EQ(poVRT->GetRasterCount(), 2);

  // 1. ReadBlock on Band 1 caches Band 2
  std::vector<float> b1(16);
  EXPECT_EQ(poVRT->GetRasterBand(1)->ReadBlock(0, 0, b1.data()), CE_None);
  EXPECT_FLOAT_EQ(b1[0], 1.0f);

  // 2. ReadBlock on Band 1 again should hit cache on other bands
  EXPECT_EQ(poVRT->GetRasterBand(1)->ReadBlock(0, 0, b1.data()), CE_None);

  // 3. ReadBlock on Band 2 should use cached block
  std::vector<float> b2(16);
  EXPECT_EQ(poVRT->GetRasterBand(2)->ReadBlock(0, 0, b2.data()), CE_None);
  EXPECT_FLOAT_EQ(b2[0], 10.0f);

  // 4. RasterIO BSQ through GDALTranspose2D
  std::vector<float> full_data(32);
  EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 4, 4, full_data.data(), 4, 4,
                            GDT_Float32, 2, nullptr, 0, 0, 0, nullptr),
            CE_None);
  EXPECT_FLOAT_EQ(full_data[0], 1.0f);
  EXPECT_FLOAT_EQ(full_data[16], 10.0f);
}

// =========================================================================
// Additional branches and edge cases
// =========================================================================

TEST_F(VRTProcessedDatasetTest, AdditionalBranchesAndEdgeCases) {
  const double gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};

  // 1. HasScaleOffset: offset != 0 while scale == 1.0
  {
    CreateTiff("/vsimem/proc_offset_only.tif", 2, 2, 1, GDT_Float32, gt,
               nullptr, nullptr, {{1}});
    auto poSrc = std::unique_ptr<GDALDataset>(
        GDALDataset::Open("/vsimem/proc_offset_only.tif", GA_Update));
    ASSERT_NE(poSrc, nullptr);
    poSrc->GetRasterBand(1)->SetScale(1.0);
    poSrc->GetRasterBand(1)->SetOffset(5.0);
    poSrc.reset();

    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  <Input "
        "unscale='AUTO'><SourceFilename>/vsimem/proc_offset_only.tif</"
        "SourceFilename></Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 2. unscale='YES' when scale=1.0 and offset=0.0
  {
    CreateTiff("/vsimem/proc_noscale.tif", 2, 2, 1, GDT_Float32, gt, nullptr,
               nullptr, {{1}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  <Input "
        "unscale='YES'><SourceFilename>/vsimem/proc_noscale.tif</"
        "SourceFilename></Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
  }

  // 3. Source dataset with 0 raster bands (inline band-less VRT input)
  {
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  <Input><VRTDataset rasterXSize='2' rasterYSize='2'/></Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
    CPLPopErrorHandler();
  }

  // 4. Source dataset with SRS inherited by VRT
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    char* pszWKT = nullptr;
    srs.exportToWkt(&pszWKT);
    CreateTiff("/vsimem/proc_srs_src.tif", 2, 2, 1, GDT_Float32, gt, pszWKT,
               nullptr, {{1}});
    CPLFree(pszWKT);

    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_srs_src.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_NE(poVRT->GetSpatialRef(), nullptr);
  }

  // 5. OutputBands user-provided count and dataType
  {
    CreateTiff("/vsimem/proc_user_bands.tif", 2, 2, 2, GDT_Float32, gt, nullptr,
               nullptr, {{1}, {2}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_user_bands.tif</SourceFilename></"
        "Input>\n"
        "  <OutputBands count='2' dataType='Float32'/>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_EQ(poVRT->GetRasterCount(), 2);
  }

  // 6. Source dataset with mixed band types (triggers warning)
  {
    TrackFile("/vsimem/mixed_dt.vrt");
    const std::string mixed_vrt =
        "<VRTDataset rasterXSize='2' rasterYSize='2'>\n"
        "  <VRTRasterBand dataType='Byte' band='1'/>\n"
        "  <VRTRasterBand dataType='Int16' band='2'/>\n"
        "</VRTDataset>\n";
    VSILFILE* fp = VSIFOpenL("/vsimem/mixed_dt.vrt", "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(mixed_vrt.data(), 1, mixed_vrt.size(), fp);
    VSIFCloseL(fp);

    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/mixed_dt.vrt</SourceFilename></Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    CPLPopErrorHandler();
    ASSERT_NE(poVRT, nullptr);
  }

  // 7. ClearBands: declared VRTRasterBand dataType (Byte) differs from step
  // output (Float32)
  {
    CreateTiff("/vsimem/proc_clear_bands.tif", 2, 2, 1, GDT_Float32, gt,
               nullptr, nullptr, {{1}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_clear_bands.tif</SourceFilename></"
        "Input>\n"
        "  <OutputBands dataType='FROM_LAST_STEP'/>\n"
        "  <VRTRasterBand dataType='Byte' band='1' "
        "subClass='VRTProcessedRasterBand'/>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);
    EXPECT_EQ(poVRT->GetRasterBand(1)->GetRasterDataType(), GDT_Float32);
  }

  // 8. Progress callback that cancels (returns FALSE)
  {
    CreateTiff("/vsimem/proc_prog_cancel.tif", 2, 2, 1, GDT_Float32, gt,
               nullptr, nullptr, {{1}});
    const std::string vrt_xml =
        "<VRTDataset subClass='VRTProcessedDataset'>\n"
        "  "
        "<Input><SourceFilename>/vsimem/proc_prog_cancel.tif</SourceFilename></"
        "Input>\n"
        "  "
        "<ProcessingSteps><Step><Algorithm>CustomIdentity</Algorithm></Step></"
        "ProcessingSteps>\n"
        "</VRTDataset>\n";
    auto poVRT = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poVRT, nullptr);

    GDALRasterIOExtraArg sArg;
    INIT_RASTERIO_EXTRA_ARG(sArg);
    sArg.pfnProgress = [](double, const char*, void*) -> int { return FALSE; };
    std::vector<float> data(4);
    EXPECT_EQ(poVRT->RasterIO(GF_Read, 0, 0, 2, 2, data.data(), 2, 2,
                              GDT_Float32, 1, nullptr, 0, 0, 0, &sArg),
              CE_Failure);
  }
}

}  // namespace
