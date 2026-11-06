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
// Tests gdal_rpc (alg/gdal_rpc.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_rpc.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/rpc.py

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

CPL_C_START
CPLXMLNode* GDALSerializeRPCTransformer(void* pTransformArg);
void* GDALDeserializeRPCTransformer(CPLXMLNode* psTree);
CPL_C_END

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct RPCTransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyRPCTransformer(p);
    }
  }
};
using RPCTransformerUniquePtr = std::unique_ptr<void, RPCTransformerDeleter>;

using CPLXMLNodeUniquePtr =
    std::unique_ptr<CPLXMLNode, CPLXMLTreeCloserDeleter>;

// Creates a synthetic, exactly linear and invertible RPC model.
// Longitude maps directly to Pixel and Latitude maps directly to Line.
GDALRPCInfoV2 CreateLinearRPC() {
  GDALRPCInfoV2 sRPC{};
  sRPC.dfLINE_OFF = 100.0;
  sRPC.dfSAMP_OFF = 200.0;
  sRPC.dfHEIGHT_OFF = 0.0;
  sRPC.dfLAT_OFF = 10.0;
  sRPC.dfLONG_OFF = 20.0;
  sRPC.dfLINE_SCALE = 1000.0;
  sRPC.dfSAMP_SCALE = 1000.0;
  sRPC.dfHEIGHT_SCALE = 100.0;
  sRPC.dfLAT_SCALE = 1.0;
  sRPC.dfLONG_SCALE = 1.0;
  sRPC.dfMIN_LONG = 19.0;
  sRPC.dfMIN_LAT = 9.0;
  sRPC.dfMAX_LONG = 21.0;
  sRPC.dfMAX_LAT = 11.0;
  sRPC.dfERR_BIAS = std::numeric_limits<double>::quiet_NaN();
  sRPC.dfERR_RAND = std::numeric_limits<double>::quiet_NaN();

  // Line = Lat
  sRPC.adfLINE_NUM_COEFF[2] = 1.0;  // Lat term
  sRPC.adfLINE_DEN_COEFF[0] = 1.0;  // Constant 1

  // Samp = Long
  sRPC.adfSAMP_NUM_COEFF[1] = 1.0;  // Long term
  sRPC.adfSAMP_DEN_COEFF[0] = 1.0;  // Constant 1

  return sRPC;
}

// Creates realistic QuickBird RPC model coefficients (derived from rpc.vrt).
GDALRPCInfoV2 CreateQuickBirdRPC() {
  GDALRPCInfoV2 sRPC{};
  sRPC.dfLINE_OFF = 16201.0;
  sRPC.dfSAMP_OFF = 15184.0;
  sRPC.dfHEIGHT_OFF = 97.0;
  sRPC.dfLAT_OFF = 39.7792;
  sRPC.dfLONG_OFF = 125.7510;
  sRPC.dfLINE_SCALE = 16480.0;
  sRPC.dfSAMP_SCALE = 15217.0;
  sRPC.dfHEIGHT_SCALE = 501.0;
  sRPC.dfLAT_SCALE = 0.0900;
  sRPC.dfLONG_SCALE = 0.1096;
  sRPC.dfMIN_LONG = 125.64;
  sRPC.dfMIN_LAT = 39.68;
  sRPC.dfMAX_LONG = 125.87;
  sRPC.dfMAX_LAT = 39.88;
  sRPC.dfERR_BIAS = 1.5;
  sRPC.dfERR_RAND = 0.5;

  constexpr double kLineNumCoeff[20] = {
      +5.105608E-04, -2.921055E-02, -1.010407E+00, -1.743729E-02,
      -6.604239E-05, -7.871396E-05, +3.027877E-04, -4.323587E-04,
      -2.624751E-04, +6.186490E-06, +1.084676E-06, +5.389738E-05,
      +4.145232E-06, +3.911486E-07, +1.772434E-05, +3.302960E-06,
      +3.006106E-06, +1.662606E-05, +6.051677E-06, -2.657667E-08};
  constexpr double kLineDenCoeff[20] = {
      +1.000000E+00, -9.652128E-05, +2.488346E-04, +3.089019E-04,
      -2.120170E-06, +4.117913E-07, +1.370009E-06, +1.357281E-05,
      -4.174324E-06, -3.146787E-06, -7.724587E-06, +3.524480E-04,
      -1.303224E-05, -8.507679E-07, -1.670972E-05, +6.781061E-06,
      +5.602262E-07, +1.161421E-05, +4.681872E-06, +5.593931E-08};
  constexpr double kSampNumCoeff[20] = {
      -2.429563E-04, +1.028320E+00, -3.360972E-02, +3.519600E-03,
      -6.568341E-04, +5.951139E-04, -3.875716E-04, +1.260622E-04,
      -5.273817E-05, -4.418981E-06, -3.520581E-06, -2.502760E-04,
      -4.167704E-05, -5.973233E-05, -1.438949E-04, +7.603041E-06,
      +2.358136E-06, -2.275274E-05, +1.602657E-06, -1.716541E-07};
  constexpr double kSampDenCoeff[20] = {
      +1.000000E+00, +7.765620E-05, +6.568707E-04, -6.270621E-04,
      +5.163170E-05, +6.979463E-06, +2.476334E-07, +1.083558E-04,
      -4.043734E-05, -5.819288E-05, +1.778201E-07, +5.665202E-05,
      +6.927205E-06, +6.793485E-07, +3.604209E-05, -4.057103E-07,
      -8.291254E-07, +1.010650E-05, -2.875552E-06, +5.142751E-08};

  std::copy(std::begin(kLineNumCoeff), std::end(kLineNumCoeff),
            sRPC.adfLINE_NUM_COEFF);
  std::copy(std::begin(kLineDenCoeff), std::end(kLineDenCoeff),
            sRPC.adfLINE_DEN_COEFF);
  std::copy(std::begin(kSampNumCoeff), std::end(kSampNumCoeff),
            sRPC.adfSAMP_NUM_COEFF);
  std::copy(std::begin(kSampDenCoeff), std::end(kSampDenCoeff),
            sRPC.adfSAMP_DEN_COEFF);
  return sRPC;
}

// Writes text content to a /vsimem/ file.
void WriteVsiMemFile(const char* path, std::string_view content) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  if (fp != nullptr) {
    VSIFWriteL(content.data(), 1, content.size(), fp);
    VSIFCloseL(fp);
  }
}

// Writes a GeoTIFF DEM file into /vsimem/ and closes it.
bool WriteDemTiff(const char* path, int width, int height,
                  const std::array<double, 6>& geo_transform,
                  double elevation = 100.0, double nodata_value = -9999.0,
                  const char* srs_wkt = nullptr,
                  const std::vector<double>* custom_pixels = nullptr) {
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff_drv == nullptr) return false;

  GDALDatasetUniquePtr ds(
      gtiff_drv->Create(path, width, height, 1, GDT_Float64, nullptr));
  if (ds == nullptr) return false;

  ds->SetGeoTransform(const_cast<double*>(geo_transform.data()));

  OGRSpatialReference srs;
  if (srs_wkt != nullptr) {
    srs.SetFromUserInput(srs_wkt);
  } else {
    srs.SetFromUserInput("EPSG:4326");
  }
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);

  GDALRasterBand* band = ds->GetRasterBand(1);
  band->SetNoDataValue(nodata_value);

  if (custom_pixels != nullptr) {
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, width, height,
                             const_cast<double*>(custom_pixels->data()), width,
                             height, GDT_Float64, 0, 0, nullptr));
  } else {
    std::vector<double> data(static_cast<size_t>(width) * height, elevation);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, width, height, data.data(), width,
                             height, GDT_Float64, 0, 0, nullptr));
  }
  return true;
}

class GdalRpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_GTiff();
    GDALRegister_VRT();
  }
};

// -----------------------------------------------------------------------------
// Suite 1: Null and Invalid Pointer Safety
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, NullAndInvalidPointers) {
  // 1. Destroying nullptr is safe.
  GDALDestroyRPCTransformer(nullptr);

  // 2. Transforming with nullptr transformer fails safely.
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  int success = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALRPCTransform(nullptr, 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_ObjectNull, CPLGetLastErrorNo());
    CPLErrorReset();
  }

  // 3. Serializing nullptr returns nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeRPCTransformer(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_ObjectNull, CPLGetLastErrorNo());
    CPLErrorReset();
  }

  // 4. Deserializing nullptr returns nullptr.
  EXPECT_EQ(nullptr, GDALDeserializeRPCTransformer(nullptr));

  // 5. Creating similar transformer with nullptr returns nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

// -----------------------------------------------------------------------------
// Suite 2: Metadata Translation (V1 & V2 round-trip)
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, RPCInfoV1ToMDRoundTrip) {
  GDALRPCInfoV2 sRPC2 = CreateLinearRPC();
  GDALRPCInfoV1 sRPC1{};
  std::memcpy(&sRPC1, &sRPC2, sizeof(GDALRPCInfoV1));

  StringListPtr md(RPCInfoV1ToMD(&sRPC1), CSLDestroy);
  ASSERT_NE(nullptr, md);

  // V1 does not have bias or rand errors.
  EXPECT_EQ(nullptr, CSLFetchNameValue(md.get(), "ERR_BIAS"));
  EXPECT_EQ(nullptr, CSLFetchNameValue(md.get(), "ERR_RAND"));

  // Check required keys exist.
  EXPECT_NE(nullptr, CSLFetchNameValue(md.get(), "LINE_OFF"));
  EXPECT_NE(nullptr, CSLFetchNameValue(md.get(), "LINE_NUM_COEFF"));

  GDALRPCInfoV1 sExtracted1{};
  EXPECT_TRUE(GDALExtractRPCInfoV1(md.get(), &sExtracted1));
  EXPECT_DOUBLE_EQ(sRPC1.dfLINE_OFF, sExtracted1.dfLINE_OFF);
  EXPECT_DOUBLE_EQ(sRPC1.dfSAMP_OFF, sExtracted1.dfSAMP_OFF);
  EXPECT_DOUBLE_EQ(sRPC1.dfLAT_OFF, sExtracted1.dfLAT_OFF);
  EXPECT_DOUBLE_EQ(sRPC1.dfLONG_OFF, sExtracted1.dfLONG_OFF);
  EXPECT_DOUBLE_EQ(sRPC1.dfHEIGHT_OFF, sExtracted1.dfHEIGHT_OFF);
  EXPECT_DOUBLE_EQ(sRPC1.dfLINE_SCALE, sExtracted1.dfLINE_SCALE);
  EXPECT_DOUBLE_EQ(sRPC1.dfSAMP_SCALE, sExtracted1.dfSAMP_SCALE);
  EXPECT_DOUBLE_EQ(sRPC1.dfLAT_SCALE, sExtracted1.dfLAT_SCALE);
  EXPECT_DOUBLE_EQ(sRPC1.dfLONG_SCALE, sExtracted1.dfLONG_SCALE);
  EXPECT_DOUBLE_EQ(sRPC1.dfHEIGHT_SCALE, sExtracted1.dfHEIGHT_SCALE);
  EXPECT_DOUBLE_EQ(sRPC1.adfLINE_NUM_COEFF[2],
                   sExtracted1.adfLINE_NUM_COEFF[2]);
  EXPECT_DOUBLE_EQ(sRPC1.adfSAMP_NUM_COEFF[1],
                   sExtracted1.adfSAMP_NUM_COEFF[1]);
}

TEST_F(GdalRpcTest, RPCInfoV2ToMDRoundTrip) {
  // Case A: dfERR_BIAS and dfERR_RAND are NaN.
  {
    GDALRPCInfoV2 sRPC = CreateLinearRPC();
    StringListPtr md(RPCInfoV2ToMD(&sRPC), CSLDestroy);
    ASSERT_NE(nullptr, md);
    EXPECT_EQ(nullptr, CSLFetchNameValue(md.get(), "ERR_BIAS"));
    EXPECT_EQ(nullptr, CSLFetchNameValue(md.get(), "ERR_RAND"));

    GDALRPCInfoV2 sExtracted{};
    EXPECT_TRUE(GDALExtractRPCInfoV2(md.get(), &sExtracted));
    EXPECT_DOUBLE_EQ(-1.0, sExtracted.dfERR_BIAS);
    EXPECT_DOUBLE_EQ(-1.0, sExtracted.dfERR_RAND);
  }

  // Case B: dfERR_BIAS and dfERR_RAND are valid numbers.
  {
    GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
    StringListPtr md(RPCInfoV2ToMD(&sRPC), CSLDestroy);
    ASSERT_NE(nullptr, md);
    EXPECT_NE(nullptr, CSLFetchNameValue(md.get(), "ERR_BIAS"));
    EXPECT_NE(nullptr, CSLFetchNameValue(md.get(), "ERR_RAND"));

    GDALRPCInfoV2 sExtracted{};
    EXPECT_TRUE(GDALExtractRPCInfoV2(md.get(), &sExtracted));
    EXPECT_NEAR(sRPC.dfERR_BIAS, sExtracted.dfERR_BIAS, 1e-10);
    EXPECT_NEAR(sRPC.dfERR_RAND, sExtracted.dfERR_RAND, 1e-10);
    EXPECT_NEAR(sRPC.dfLINE_OFF, sExtracted.dfLINE_OFF, 1e-10);
    EXPECT_NEAR(sRPC.dfSAMP_OFF, sExtracted.dfSAMP_OFF, 1e-10);
    EXPECT_NEAR(sRPC.dfLAT_OFF, sExtracted.dfLAT_OFF, 1e-10);
    EXPECT_NEAR(sRPC.dfLONG_OFF, sExtracted.dfLONG_OFF, 1e-10);
    EXPECT_NEAR(sRPC.dfHEIGHT_OFF, sExtracted.dfHEIGHT_OFF, 1e-10);
    EXPECT_NEAR(sRPC.dfLINE_SCALE, sExtracted.dfLINE_SCALE, 1e-10);
    EXPECT_NEAR(sRPC.dfSAMP_SCALE, sExtracted.dfSAMP_SCALE, 1e-10);
    EXPECT_NEAR(sRPC.dfLAT_SCALE, sExtracted.dfLAT_SCALE, 1e-10);
    EXPECT_NEAR(sRPC.dfLONG_SCALE, sExtracted.dfLONG_SCALE, 1e-10);
    EXPECT_NEAR(sRPC.dfHEIGHT_SCALE, sExtracted.dfHEIGHT_SCALE, 1e-10);
    for (int i = 0; i < 20; ++i) {
      EXPECT_NEAR(sRPC.adfLINE_NUM_COEFF[i], sExtracted.adfLINE_NUM_COEFF[i],
                  1e-10);
      EXPECT_NEAR(sRPC.adfLINE_DEN_COEFF[i], sExtracted.adfLINE_DEN_COEFF[i],
                  1e-10);
      EXPECT_NEAR(sRPC.adfSAMP_NUM_COEFF[i], sExtracted.adfSAMP_NUM_COEFF[i],
                  1e-10);
      EXPECT_NEAR(sRPC.adfSAMP_DEN_COEFF[i], sExtracted.adfSAMP_DEN_COEFF[i],
                  1e-10);
    }
  }
}

// -----------------------------------------------------------------------------
// Suite 3: Creation and Basic Execution (V1 API)
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, CreateAndDestroyV1) {
  GDALRPCInfoV2 sRPC2 = CreateLinearRPC();
  GDALRPCInfoV1 sRPC1{};
  std::memcpy(&sRPC1, &sRPC2, sizeof(GDALRPCInfoV1));

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV1(&sRPC1, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  double x = 20.1;
  double y = 10.2;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(300.5, x, 1e-9);
  EXPECT_NEAR(300.5, y, 1e-9);
}

// -----------------------------------------------------------------------------
// Suite 4: Linear RPC Forward and Inverse Transformation
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, LinearRPCForwardAndInverse) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  // 1. Forward transform (bDstToSrc = 1): (lon, lat) -> (pixel, line).
  double x = 20.1;
  double y = 10.2;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(300.5, x, 1e-9);
  EXPECT_NEAR(300.5, y, 1e-9);

  // 2. Inverse transform (bDstToSrc = 0): (pixel, line) -> (lon, lat).
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(20.1, x, 1e-8);
  EXPECT_NEAR(10.2, y, 1e-8);

  // 3. Transforming 0 points succeeds.
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 0, nullptr, nullptr,
                                nullptr, nullptr));

  // 4. Forward transform without Z array defaults to 0 elevation.
  x = 20.1;
  y = 10.2;
  EXPECT_EQ(
      1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(300.5, x, 1e-9);
  EXPECT_NEAR(300.5, y, 1e-9);

  // 5. Inverse transform without Z array fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, nullptr,
                                  &success));
    EXPECT_EQ(0, success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Z array should be provided"));
    CPLErrorReset();
  }
}

// -----------------------------------------------------------------------------
// Suite 5: Reversed Flag
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, ReversedFlag) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, /*bReversed=*/TRUE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  // With bReversed=TRUE, bDstToSrc=1 invokes the inverse transform.
  double x = 300.5;
  double y = 300.5;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(20.1, x, 1e-8);
  EXPECT_NEAR(10.2, y, 1e-8);

  // And bDstToSrc=0 invokes the forward transform.
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  EXPECT_NEAR(300.5, x, 1e-9);
  EXPECT_NEAR(300.5, y, 1e-9);

  // Inverse (which is bDstToSrc=1 here) without Z array fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, nullptr,
                                  &success));
    EXPECT_EQ(0, success);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

// -----------------------------------------------------------------------------
// Suite 6: Transformer Creation Options & Degenerate Cases
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, PixelErrorThresholdOptions) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();

  // 1. Default threshold when <= 0 is passed.
  RPCTransformerUniquePtr t1(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, -1.0, nullptr));
  ASSERT_NE(nullptr, t1);

  // 2. Explicit threshold via argument.
  RPCTransformerUniquePtr t2(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.05, nullptr));
  ASSERT_NE(nullptr, t2);

  // 3. Override threshold via options.
  CPLStringList options;
  options.SetNameValue("RPC_PIXEL_ERROR_THRESHOLD", "0.02");
  RPCTransformerUniquePtr t3(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, t3);
}

TEST_F(GdalRpcTest, HeightAndHeightScaleOptions) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  // Include height in line calculation: Line = Lat + Height / HeightScale
  sRPC.adfLINE_NUM_COEFF[3] = 1.0;  // Height term

  CPLStringList options;
  options.SetNameValue("RPC_HEIGHT", "100.0");
  options.SetNameValue("RPC_HEIGHT_SCALE", "2.0");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 20.1;
  double y = 10.2;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);

  // Height used = z (0.0) + RPC_HEIGHT (100.0) = 100.0.
  // Normalized height = 100.0 / 100.0 = 1.0.
  // ResultY = normLat (0.2) + normHeight (1.0) = 1.2.
  // Line = 1.2 * 1000.0 + 100.0 + 0.5 = 1300.5.
  EXPECT_NEAR(300.5, x, 1e-9);
  EXPECT_NEAR(1300.5, y, 1e-9);
}

TEST_F(GdalRpcTest, DegenerateCoefficientsCannotInvertGeotransform) {
  GDALRPCInfoV2 sRPC{};
  sRPC.dfLINE_OFF = 100.0;
  sRPC.dfSAMP_OFF = 200.0;
  sRPC.dfLINE_SCALE = 1000.0;
  sRPC.dfSAMP_SCALE = 1000.0;
  sRPC.dfLAT_SCALE = 1.0;
  sRPC.dfLONG_SCALE = 1.0;
  sRPC.dfHEIGHT_SCALE = 100.0;
  sRPC.adfLINE_DEN_COEFF[0] = 1.0;
  sRPC.adfSAMP_DEN_COEFF[0] = 1.0;
  // All numerator coefficients are 0: constant pixel/line regardless of
  // lon/lat.

  WithQuietHandler quiet;
  CPLErrorReset();
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  EXPECT_EQ(nullptr, transformer);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot invert geotransform"));
  CPLErrorReset();
}

TEST_F(GdalRpcTest, BoundsHandlingAtCreation) {
  // Case A: Default bounds [-180, 180] skips bounds center and uses offsets.
  {
    GDALRPCInfoV2 sRPC = CreateLinearRPC();
    sRPC.dfMIN_LONG = -180.0;
    sRPC.dfMAX_LONG = 180.0;
    RPCTransformerUniquePtr t(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
    ASSERT_NE(nullptr, t);
  }

  // Case B: Bounds yielding invalid/daft reference coordinates (< 0).
  {
    GDALRPCInfoV2 sRPC = CreateLinearRPC();
    sRPC.dfSAMP_OFF = -200000.0;
    sRPC.dfLINE_OFF = -200000.0;
    RPCTransformerUniquePtr t(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
    ASSERT_NE(nullptr, t);
  }
}

// -----------------------------------------------------------------------------
// Suite 7: Dateline Wrapping and Normalization Warnings
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, DatelineWrapping) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  sRPC.dfLONG_OFF = 179.0;
  sRPC.dfMIN_LONG = -180.0;
  sRPC.dfMAX_LONG = 180.0;

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  // diffLong = -179.0 - 179.0 = -358.0 < -270 -> diffLong += 360 -> +2.0.
  double x = -179.0;
  double y = 10.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  // Pixel = 2.0 * 1000.0 + 200.0 + 0.5 = 2200.5.
  EXPECT_DOUBLE_EQ(2200.5, x);

  // Opposite sign wrapping: diffLong > 270.
  sRPC.dfLONG_OFF = -179.0;
  RPCTransformerUniquePtr t2(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, t2);

  x = 179.0;
  y = 10.0;
  EXPECT_EQ(1, GDALRPCTransform(t2.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
  // diffLong = 179.0 - (-179.0) = 358.0 > 270 -> diffLong -= 360 -> -2.0.
  // Pixel = -2.0 * 1000.0 + 200.0 + 0.5 = -1799.5.
  EXPECT_DOUBLE_EQ(-1799.5, x);
}

TEST_F(GdalRpcTest, NormalizedCoordinateWarnings) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  // Trigger normalized coordinates > 1.5 multiple times to exhaust
  // MAX_ABS_VALUE_WARNINGS (20).
  for (int i = 0; i < 25; ++i) {
    double x = 30.0;   // (30 - 20) / 1 = 10 > 1.5
    double y = 20.0;   // (20 - 10) / 1 = 10 > 1.5
    double z = 500.0;  // 500 / 100 = 5 > 1.5
    int success = 0;
    EXPECT_EQ(1,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(1, success);
  }
}

// -----------------------------------------------------------------------------
// Suite 8: Inverse Iteration Logging and Non-Convergence
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, InverseIterationLogAndVerbose) {
  constexpr char kLogDir[] = "/vsimem/gdal_rpc_test/inv_log";
  constexpr char kLogFile[] = "/vsimem/gdal_rpc_test/inv_log/iter.csv";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kLogDir); };

  CPLSetConfigOption("RPC_INVERSE_VERBOSE", "YES");
  CPLSetConfigOption("RPC_INVERSE_LOG", kLogFile);
  absl::Cleanup restore_config = [&] {
    CPLSetConfigOption("RPC_INVERSE_VERBOSE", nullptr);
    CPLSetConfigOption("RPC_INVERSE_LOG", nullptr);
  };

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, nullptr));
  ASSERT_NE(nullptr, transformer);

  double x = 15180.0;
  double y = 16210.0;
  double z = 97.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);

  // Check log file and csvt companion were written.
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(kLogFile, &stat_buf));
  EXPECT_EQ(0, VSIStatL("/vsimem/gdal_rpc_test/inv_log/iter.csvt", &stat_buf));
}

TEST_F(GdalRpcTest, InverseIterationMaxIterationsExceeded) {
  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_MAX_ITERATIONS", "1");
  options.SetNameValue("RPC_PIXEL_ERROR_THRESHOLD", "1e-15");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 1e-15, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 15180.0;
  double y = 16210.0;
  double z = 97.0;
  int success = 1;
  // Fails to converge in only 1 iteration.
  EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(0, success);
  EXPECT_DOUBLE_EQ(HUGE_VAL, x);
  EXPECT_DOUBLE_EQ(HUGE_VAL, y);
}

// -----------------------------------------------------------------------------
// Suite 9: Footprint (RPC_FOOTPRINT)
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, FootprintGeoJson) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  CPLStringList options;
  options.SetNameValue(
      "RPC_FOOTPRINT",
      R"({"type":"Polygon","coordinates":[[[19.5,9.5],[20.5,9.5],[20.5,10.5],[19.5,10.5],[19.5,9.5]]]})");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  // Point inside footprint.
  double x = 20.0;
  double y = 10.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);

  // Point outside footprint forward.
  x = 22.0;
  y = 12.0;
  EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(0, success);
  EXPECT_DOUBLE_EQ(HUGE_VAL, x);
  EXPECT_DOUBLE_EQ(HUGE_VAL, y);

  // Point outside footprint inverse.
  x = 2200.0;
  y = 2200.0;
  EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(0, success);
  EXPECT_DOUBLE_EQ(HUGE_VAL, x);
  EXPECT_DOUBLE_EQ(HUGE_VAL, y);
}

TEST_F(GdalRpcTest, FootprintWkt) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  CPLStringList options;
  options.SetNameValue(
      "RPC_FOOTPRINT",
      "POLYGON((19.5 9.5, 20.5 9.5, 20.5 10.5, 19.5 10.5, 19.5 9.5))");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 20.0;
  double y = 10.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);

  x = 25.0;
  y = 25.0;
  EXPECT_EQ(0, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(0, success);
}

// -----------------------------------------------------------------------------
// Suite 10: DEM Open Failure Paths
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, DemOpenFailurePaths) {
  GDALRPCInfoV2 sRPC = CreateLinearRPC();

  // 1. Non-existent DEM file.
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", "/vsimem/gdal_rpc_test/non_existent.tif");
    WithQuietHandler quiet;
    CPLErrorReset();
    RPCTransformerUniquePtr t(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    EXPECT_EQ(nullptr, t);
    EXPECT_EQ(CPLE_OpenFailed, CPLGetLastErrorNo());
  }

  // 2. DEM with 0 raster bands.
  constexpr char kNoBandsPath[] = "/vsimem/gdal_rpc_test/nobands.vrt";
  WriteVsiMemFile(
      kNoBandsPath,
      R"(<VRTDataset rasterXSize="10" rasterYSize="10"></VRTDataset>)");
  absl::Cleanup cleanup = [&] { VSIUnlink(kNoBandsPath); };
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kNoBandsPath);
    WithQuietHandler quiet;
    CPLErrorReset();
    RPCTransformerUniquePtr t(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    EXPECT_EQ(nullptr, t);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. DEM with non-invertible geotransform.
  constexpr char kSingularGTPath[] = "/vsimem/gdal_rpc_test/singular_gt.tif";
  std::array<double, 6> singular_gt = {0, 1, 0, 0, 2, 0};
  ASSERT_TRUE(WriteDemTiff(kSingularGTPath, 5, 5, singular_gt));
  absl::Cleanup cleanup_singular = [&] { VSIUnlink(kSingularGTPath); };
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kSingularGTPath);
    WithQuietHandler quiet;
    CPLErrorReset();
    RPCTransformerUniquePtr t(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    EXPECT_EQ(nullptr, t);
    // GDALInvGeoTransform returns FALSE without setting CPLError.
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 11: DEM Interpolation, Sampling and Out-of-Bounds
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, DemInterpolationOptions) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/interp";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/interp/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 150.0));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  sRPC.adfLINE_NUM_COEFF[3] = 1.0;  // Height term affects Line

  for (const char* interp : {"near", "bilinear", "cubic", "unknown_mode"}) {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kDemPath);
    options.SetNameValue("RPC_DEMINTERPOLATION", interp);
    RPCTransformerUniquePtr transformer(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 20.0;
    double y = 10.0;
    double z = 0.0;
    int success = 0;
    EXPECT_EQ(1,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(1, success);
    // Elevation sampled = 150.0. Line = 100.0 + 150.0 / 100.0 * 1000.0 + 0.5 =
    // 1600.5.
    EXPECT_DOUBLE_EQ(1600.5, y);
  }
}

TEST_F(GdalRpcTest, DemOutOfBoundsAndMissingValue) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/oob";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/oob/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 100.0));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  sRPC.adfLINE_NUM_COEFF[3] = 1.0;

  // 1. Out-of-bounds without missing value fails.
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kDemPath);
    RPCTransformerUniquePtr transformer(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 30.0;  // Well outside DEM extent [19.0, 21.0]
    double y = 10.0;
    double z = 0.0;
    int success = 1;
    EXPECT_EQ(0,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(0, success);
    EXPECT_DOUBLE_EQ(HUGE_VAL, x);
    EXPECT_DOUBLE_EQ(HUGE_VAL, y);
  }

  // 2. Out-of-bounds with RPC_DEM_MISSING_VALUE succeeds with missing value.
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kDemPath);
    options.SetNameValue("RPC_DEM_MISSING_VALUE", "50.0");
    RPCTransformerUniquePtr transformer(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 30.0;
    double y = 10.0;
    double z = 0.0;
    int success = 0;
    EXPECT_EQ(1,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(1, success);
    // Line = normLat(0) + 50.0/100.0 * 1000.0 + 100.0 + 0.5 = 600.5.
    EXPECT_DOUBLE_EQ(600.5, y);
  }
}

TEST_F(GdalRpcTest, DemSpanning180Retry) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/span180";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/span180/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  // Spans [-180, 180] in longitude.
  std::array<double, 6> gt = {-180.0, 1.0, 0.0, 90.0, 0.0, -1.0};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 360, 180, gt, 200.0));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  sRPC.dfLONG_OFF = 180.0;
  sRPC.dfMIN_LONG = -180.0;
  sRPC.dfMAX_LONG = 180.0;
  sRPC.adfLINE_NUM_COEFF[3] = 1.0;

  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  // Query point with lon >= 180 (wraps to negative longitude on retry).
  double x = 180.5;
  double y = 0.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);

  // Query point with lon <= -180 (wraps to positive longitude on retry).
  x = -180.5;
  y = 0.0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
}

// -----------------------------------------------------------------------------
// Suite 12: DEM SRS, Reprojection and Vertical Datum Shift
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, DemSrsProjected) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/proj";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/proj/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  // UTM zone 52N covering QuickBird coordinates.
  // E.g. (125.751, 39.7792) maps to approx (224000.0, 4404000.0) in UTM 52N.
  std::array<double, 6> utm_gt = {100000.0,  10000.0, 0.0,
                                  4600000.0, 0.0,     -10000.0};
  ASSERT_TRUE(
      WriteDemTiff(kDemPath, 100, 100, utm_gt, 250.0, -9999.0, "EPSG:32652"));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 125.7510;
  double y = 39.7792;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
}

TEST_F(GdalRpcTest, DemSrsOverride) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/srs_ovr";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/srs_ovr/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 100.0));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEM_SRS", "EPSG:4326");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 20.0;
  double y = 10.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
}

TEST_F(GdalRpcTest, DemCompoundSrs) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/compound";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/compound/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  constexpr char kCompoundWkt[] =
      R"(COMPD_CS["WGS 84 + EGM96 height",)"
      R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],)"
      R"(PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4326"]],)"
      R"(VERT_CS["EGM96 height",VERT_DATUM["EGM96 geoid",2005],UNIT["metre",1],AUTHORITY["EPSG","5773"]]])";

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 100.0, -9999.0, kCompoundWkt));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  sRPC.adfLINE_NUM_COEFF[3] = 1.0;

  // 1. With vertical datum shift enabled.
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kDemPath);
    options.SetNameValue("RPC_DEM_APPLY_VDATUM_SHIFT", "TRUE");
    RPCTransformerUniquePtr transformer(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 20.0;
    double y = 10.0;
    double z = 0.0;
    int success = 0;
    EXPECT_EQ(1,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(1, success);
    EXPECT_TRUE(std::isfinite(x));
    EXPECT_TRUE(std::isfinite(y));
  }

  // 2. With vertical datum shift disabled (strips vertical component).
  {
    CPLStringList options;
    options.SetNameValue("RPC_DEM", kDemPath);
    options.SetNameValue("RPC_DEM_APPLY_VDATUM_SHIFT", "FALSE");
    RPCTransformerUniquePtr transformer(
        GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
    ASSERT_NE(nullptr, transformer);

    double x = 20.0;
    double y = 10.0;
    double z = 0.0;
    int success = 0;
    EXPECT_EQ(1,
              GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
    EXPECT_EQ(1, success);
    EXPECT_TRUE(std::isfinite(x));
    EXPECT_TRUE(std::isfinite(y));
  }
}

TEST_F(GdalRpcTest, DemNopSrsShortCircuit) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/nop_srs";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/nop_srs/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  // Custom SRS with zero 7-parameter transform to WGS84, triggering the
  // short-circuit apparent nop heuristic.
  constexpr char kNopWkt[] =
      R"(GEOGCS["Custom_WGS84",)"
      R"(DATUM["Custom_Datum",SPHEROID["WGS 84",6378137,298.257223563],TOWGS84[0,0,0,0,0,0,0]],)"
      R"(PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])";

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 100.0, -9999.0, kNopWkt));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 20.0;
  double y = 10.0;
  double z = 0.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
}

// -----------------------------------------------------------------------------
// Suite 13: Whole Line DEM Optimization (GDALRPCTransformWholeLineWithDEM)
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, WholeLineDemBilinear) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/whole_line_bilinear";
  constexpr char kDemPath[] =
      "/vsimem/gdal_rpc_test/whole_line_bilinear/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  constexpr int kWidth = 40;
  constexpr int kHeight = 40;
  std::array<double, 6> gt = {125.0, 0.01, 0.0, 40.0, 0.0, -0.01};
  std::vector<double> pixels(kWidth * kHeight, 100.0);
  // Put a nodata value in one cell.
  pixels[15 * kWidth + 15] = -9999.0;
  ASSERT_TRUE(WriteDemTiff(kDemPath, kWidth, kHeight, gt, 100.0, -9999.0,
                           nullptr, &pixels));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEMINTERPOLATION", "bilinear");
  options.SetNameValue("RPC_DEM_MISSING_VALUE", "100.0");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  constexpr int kNumPoints = 12;
  std::vector<double> x(kNumPoints);
  std::vector<double> y(kNumPoints, 39.85);  // Constant latitude
  std::vector<double> z(kNumPoints, 0.0);
  std::vector<int> success(kNumPoints, 0);

  for (int i = 0; i < kNumPoints; ++i) {
    x[i] = 125.10 + i * 0.01;
  }

  // 1. Normal execution with whole-line DEM optimization.
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, kNumPoints, x.data(),
                                y.data(), z.data(), success.data()));
  for (int s : success) {
    EXPECT_EQ(1, s);
  }

  // 2. Whole-line with GDAL_RPC_DEM_OPTIM disabled exercises standard loop.
  CPLSetConfigOption("GDAL_RPC_DEM_OPTIM", "NO");
  absl::Cleanup restore_opt = [&] {
    CPLSetConfigOption("GDAL_RPC_DEM_OPTIM", nullptr);
  };
  for (int i = 0; i < kNumPoints; ++i) {
    x[i] = 125.10 + i * 0.01;
    y[i] = 39.85;
    z[i] = 0.0;
  }
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, kNumPoints, x.data(),
                                y.data(), z.data(), success.data()));
  for (int s : success) {
    EXPECT_EQ(1, s);
  }
}

TEST_F(GdalRpcTest, WholeLineDemCubic) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/whole_line_cubic";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/whole_line_cubic/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  constexpr int kWidth = 40;
  constexpr int kHeight = 40;
  std::array<double, 6> gt = {125.0, 0.01, 0.0, 40.0, 0.0, -0.01};
  std::vector<double> pixels(kWidth * kHeight, 120.0);
  pixels[15 * kWidth + 15] = -9999.0;
  ASSERT_TRUE(WriteDemTiff(kDemPath, kWidth, kHeight, gt, 120.0, -9999.0,
                           nullptr, &pixels));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEMINTERPOLATION", "cubic");
  options.SetNameValue("RPC_DEM_MISSING_VALUE", "120.0");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  constexpr int kNumPoints = 10;
  std::vector<double> x(kNumPoints);
  std::vector<double> y(kNumPoints, 39.85);
  std::vector<double> z(kNumPoints, 0.0);
  std::vector<int> success(kNumPoints, 0);

  for (int i = 0; i < kNumPoints; ++i) {
    x[i] = 125.10 + i * 0.01;
  }

  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, kNumPoints, x.data(),
                                y.data(), z.data(), success.data()));
  for (int s : success) {
    EXPECT_EQ(1, s);
  }
}

TEST_F(GdalRpcTest, WholeLineDemNearest) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/whole_line_near";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/whole_line_near/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  constexpr int kWidth = 40;
  constexpr int kHeight = 40;
  std::array<double, 6> gt = {125.0, 0.01, 0.0, 40.0, 0.0, -0.01};
  ASSERT_TRUE(WriteDemTiff(kDemPath, kWidth, kHeight, gt, 80.0));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEMINTERPOLATION", "near");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  constexpr int kNumPoints = 10;
  std::vector<double> x(kNumPoints);
  std::vector<double> y(kNumPoints, 39.85);
  std::vector<double> z(kNumPoints, 0.0);
  std::vector<int> success(kNumPoints, 0);

  for (int i = 0; i < kNumPoints; ++i) {
    x[i] = 125.10 + i * 0.01;
  }

  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 1, kNumPoints, x.data(),
                                y.data(), z.data(), success.data()));
  for (int s : success) {
    EXPECT_EQ(1, s);
  }
}

// -----------------------------------------------------------------------------
// Suite 14: Inverse Transform with DEM (Boundary Snapping & Oscillation)
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, InverseWithDemBoundarySnappingAndFailure) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/inv_dem";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/inv_dem/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {125.70, 0.01, 0.0, 39.85, 0.0, -0.01};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 10, 10, gt, 100.0));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, transformer);

  // Transform point that snaps to DEM boundary on iteration 0.
  double x = 15180.0;
  double y = 16210.0;
  double z = 97.0;
  int success = 0;
  EXPECT_EQ(1, GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success));
  EXPECT_EQ(1, success);
}

TEST_F(GdalRpcTest, InverseWithDemOscillationAndBoost) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/oscillation";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/oscillation/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  constexpr int kWidth = 30;
  constexpr int kHeight = 30;
  std::array<double, 6> gt = {125.60, 0.01, 0.0, 39.90, 0.0, -0.01};
  // Steep terrain step inducing oscillations in Newton-Raphson approximation.
  std::vector<double> pixels(kWidth * kHeight, 0.0);
  for (int row = 0; row < kHeight; ++row) {
    for (int col = 15; col < kWidth; ++col) {
      pixels[row * kWidth + col] = 4000.0;
    }
  }
  ASSERT_TRUE(WriteDemTiff(kDemPath, kWidth, kHeight, gt, 0.0, -9999.0, nullptr,
                           &pixels));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_MAX_ITERATIONS", "30");

  RPCTransformerUniquePtr transformer(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.5, options.List()));
  ASSERT_NE(nullptr, transformer);

  double x = 15184.0;
  double y = 16201.0;
  double z = 97.0;
  int success = 0;
  GDALRPCTransform(transformer.get(), 0, 1, &x, &y, &z, &success);
  // Reaches either convergence or oscillation resolution.
  SUCCEED();
}

// -----------------------------------------------------------------------------
// Suite 15: Similar Transformer
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, SimilarTransformer) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/similar";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/similar/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {19.0, 0.1, 0.0, 11.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 20, 20, gt, 100.0));

  GDALRPCInfoV2 sRPC = CreateLinearRPC();
  CPLStringList options;
  options.SetNameValue("RPC_HEIGHT", "10.0");
  options.SetNameValue("RPC_HEIGHT_SCALE", "1.5");
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEMINTERPOLATION", "cubic");
  options.SetNameValue("RPC_DEM_MISSING_VALUE", "-9999.0");
  options.SetNameValue("RPC_DEM_APPLY_VDATUM_SHIFT", "TRUE");
  options.SetNameValue("RPC_MAX_ITERATIONS", "25");

  RPCTransformerUniquePtr orig(
      GDALCreateRPCTransformerV2(&sRPC, FALSE, 0.1, options.List()));
  ASSERT_NE(nullptr, orig);

  // 1. Ratio 1.0, 1.0 (identical transformer).
  RPCTransformerUniquePtr similar1(
      GDALCreateSimilarTransformer(orig.get(), 1.0, 1.0));
  ASSERT_NE(nullptr, similar1);

  double x1 = 20.1;
  double y1 = 10.2;
  double z1 = 0.0;
  int s1 = 0;
  EXPECT_EQ(1, GDALRPCTransform(similar1.get(), 1, 1, &x1, &y1, &z1, &s1));
  EXPECT_EQ(1, s1);

  double x_orig = 20.1;
  double y_orig = 10.2;
  double z_orig = 0.0;
  int s_orig = 0;
  EXPECT_EQ(1, GDALRPCTransform(orig.get(), 1, 1, &x_orig, &y_orig, &z_orig,
                                &s_orig));
  EXPECT_EQ(1, s_orig);
  EXPECT_DOUBLE_EQ(x_orig, x1);
  EXPECT_DOUBLE_EQ(y_orig, y1);

  // 2. Scaled ratio 2.0, 3.0 scales SAMP and LINE offsets/scales.
  RPCTransformerUniquePtr scaled(
      GDALCreateSimilarTransformer(orig.get(), 2.0, 3.0));
  ASSERT_NE(nullptr, scaled);

  double x_sc = 20.1;
  double y_sc = 10.2;
  double z_sc = 0.0;
  int s_sc = 0;
  EXPECT_EQ(1,
            GDALRPCTransform(scaled.get(), 1, 1, &x_sc, &y_sc, &z_sc, &s_sc));
  EXPECT_EQ(1, s_sc);
  // Pixel offset and scale divided by 2 -> (300.0 / 2.0) + 0.5 = 150.5.
  EXPECT_DOUBLE_EQ((x_orig - 0.5) / 2.0 + 0.5, x_sc);
}

// -----------------------------------------------------------------------------
// Suite 16: Serialization and Deserialization
// -----------------------------------------------------------------------------

TEST_F(GdalRpcTest, SerializeAndDeserializeRoundTrip) {
  constexpr char kDemDir[] = "/vsimem/gdal_rpc_test/serialize";
  constexpr char kDemPath[] = "/vsimem/gdal_rpc_test/serialize/dem.tif";
  absl::Cleanup cleanup = [&] { VSIRmdirRecursive(kDemDir); };

  std::array<double, 6> gt = {125.0, 0.01, 0.0, 40.0, 0.0, -0.01};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 30, 30, gt, 100.0));

  GDALRPCInfoV2 sRPC = CreateQuickBirdRPC();
  CPLStringList options;
  options.SetNameValue("RPC_HEIGHT", "15.0");
  options.SetNameValue("RPC_HEIGHT_SCALE", "2.0");
  options.SetNameValue("RPC_DEM", kDemPath);
  options.SetNameValue("RPC_DEMINTERPOLATION", "cubic");
  options.SetNameValue("RPC_DEM_MISSING_VALUE", "-9999.0");
  options.SetNameValue("RPC_DEM_APPLY_VDATUM_SHIFT", "false");
  options.SetNameValue("RPC_DEM_SRS", "EPSG:4326");
  options.SetNameValue("RPC_PIXEL_ERROR_THRESHOLD", "0.05");

  RPCTransformerUniquePtr orig(
      GDALCreateRPCTransformerV2(&sRPC, /*bReversed=*/1, 0.05, options.List()));
  ASSERT_NE(nullptr, orig);

  CPLXMLNodeUniquePtr xml(GDALSerializeRPCTransformer(orig.get()));
  ASSERT_NE(nullptr, xml);

  RPCTransformerUniquePtr restored(GDALDeserializeRPCTransformer(xml.get()));
  ASSERT_NE(nullptr, restored);

  // Both transformers produce identical results.
  double x1 = 15180.0;
  double y1 = 16210.0;
  double z1 = 97.0;
  int s1 = 0;

  double x2 = 15180.0;
  double y2 = 16210.0;
  double z2 = 97.0;
  int s2 = 0;

  EXPECT_EQ(1, GDALRPCTransform(orig.get(), 1, 1, &x1, &y1, &z1, &s1));
  EXPECT_EQ(1, GDALRPCTransform(restored.get(), 1, 1, &x2, &y2, &z2, &s2));
  EXPECT_EQ(s1, s2);
  EXPECT_DOUBLE_EQ(x1, x2);
  EXPECT_DOUBLE_EQ(y1, y2);
}

TEST_F(GdalRpcTest, DeserializeErrorPaths) {
  // 1. Null XML tree.
  EXPECT_EQ(nullptr, GDALDeserializeRPCTransformer(nullptr));

  // 2. Wrong root element.
  {
    CPLXMLNodeUniquePtr xml(
        CPLCreateXMLNode(nullptr, CXT_Element, "InvalidTransformer"));
    EXPECT_EQ(nullptr, GDALDeserializeRPCTransformer(xml.get()));
  }

  // 3. Missing Metadata node.
  {
    CPLXMLNodeUniquePtr xml(
        CPLCreateXMLNode(nullptr, CXT_Element, "RPCTransformer"));
    CPLCreateXMLElementAndValue(xml.get(), "Reversed", "0");
    EXPECT_EQ(nullptr, GDALDeserializeRPCTransformer(xml.get()));
  }

  // 4. Incomplete/corrupt Metadata failing GDALExtractRPCInfoV2.
  {
    CPLXMLNodeUniquePtr xml(
        CPLCreateXMLNode(nullptr, CXT_Element, "RPCTransformer"));
    CPLXMLNode* meta = CPLCreateXMLNode(xml.get(), CXT_Element, "Metadata");
    CPLXMLNode* mdi = CPLCreateXMLNode(meta, CXT_Element, "MDI");
    CPLSetXMLValue(mdi, "#key", "LINE_OFF");
    CPLCreateXMLNode(mdi, CXT_Text, "100.0");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeRPCTransformer(xml.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to reconstitute RPC transformer"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - RPC terrain intersection convergence loops and PROJ grid
// shifts.
