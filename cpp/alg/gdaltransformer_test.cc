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
// Tests gdaltransformer (alg/gdaltransformer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdaltransformer.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/alg/warp.py

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom Deleters for GDAL transformer types.
struct GenImgProjTransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyGenImgProjTransformer(p);
    }
  }
};
using GenImgProjTransformerUniquePtr =
    std::unique_ptr<void, GenImgProjTransformerDeleter>;

struct ReprojectionTransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyReprojectionTransformer(p);
    }
  }
};
using ReprojectionTransformerUniquePtr =
    std::unique_ptr<void, ReprojectionTransformerDeleter>;

struct ApproxTransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyApproxTransformer(p);
    }
  }
};
using ApproxTransformerUniquePtr =
    std::unique_ptr<void, ApproxTransformerDeleter>;

struct TransformerDeleter {
  void operator()(void* p) const {
    if (p != nullptr) {
      GDALDestroyTransformer(p);
    }
  }
};
using TransformerUniquePtr = std::unique_ptr<void, TransformerDeleter>;

using CPLXMLNodeUniquePtr =
    std::unique_ptr<CPLXMLNode, CPLXMLTreeCloserDeleter>;

// Helper to create synthetic linear RPC metadata.
CPLStringList CreateLinearRPCMetadata() {
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
  sRPC.dfERR_BIAS = -1.0;
  sRPC.dfERR_RAND = -1.0;

  // Line = Lat
  sRPC.adfLINE_NUM_COEFF[2] = 1.0;
  sRPC.adfLINE_DEN_COEFF[0] = 1.0;

  // Samp = Long
  sRPC.adfSAMP_NUM_COEFF[1] = 1.0;
  sRPC.adfSAMP_DEN_COEFF[0] = 1.0;

  char** papszMD = RPCInfoV2ToMD(&sRPC);
  return CPLStringList(papszMD, /*bTakeOwnership=*/TRUE);
}

// Writes a 1-band GeoTIFF DEM file into /vsimem/ for RPC DEM testing.
bool WriteDemTiff(const char* path, int width, int height,
                  const double* padfGeoTransform, double elevation = 100.0) {
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff_drv == nullptr) return false;

  GDALDatasetUniquePtr ds(
      gtiff_drv->Create(path, width, height, 1, GDT_Float64, nullptr));
  if (ds == nullptr) return false;

  ds->SetGeoTransform(const_cast<double*>(padfGeoTransform));
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);

  GDALRasterBand* band = ds->GetRasterBand(1);
  std::vector<double> data(static_cast<size_t>(width) * height, elevation);
  return band->RasterIO(GF_Write, 0, 0, width, height, data.data(), width,
                        height, GDT_Float64, 0, 0, nullptr) == CE_None;
}

// Writes a 2-band GeoTIFF geolocation dataset into /vsimem/.
bool WriteGeolocTiff(const char* path, int width, int height) {
  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff_drv == nullptr) return false;

  GDALDatasetUniquePtr ds(
      gtiff_drv->Create(path, width, height, 2, GDT_Float64, nullptr));
  if (ds == nullptr) return false;

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);

  std::vector<double> x_coords(static_cast<size_t>(width) * height);
  std::vector<double> y_coords(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      x_coords[static_cast<size_t>(y) * width + x] = 10.0 + x * 0.1;
      y_coords[static_cast<size_t>(y) * width + x] = 40.0 + y * 0.1;
    }
  }

  GDALRasterBand* b1 = ds->GetRasterBand(1);
  GDALRasterBand* b2 = ds->GetRasterBand(2);
  if (b1->RasterIO(GF_Write, 0, 0, width, height, x_coords.data(), width,
                   height, GDT_Float64, 0, 0, nullptr) != CE_None) {
    return false;
  }
  return b2->RasterIO(GF_Write, 0, 0, width, height, y_coords.data(), width,
                      height, GDT_Float64, 0, 0, nullptr) == CE_None;
}

// Dummy transformer function for testing custom deserializers.
int DummyCustomTransform(void* /*pTransformArg*/, int /*bDstToSrc*/,
                         int nPointCount, double* x, double* y, double* /*z*/,
                         int* panSuccess) {
  for (int i = 0; i < nPointCount; ++i) {
    x[i] += 10.0;
    y[i] += 20.0;
    if (panSuccess) panSuccess[i] = TRUE;
  }
  return TRUE;
}

// Custom deserializer function for testing deserializer registry.
void* DummyCustomDeserialize(CPLXMLNode* /*psTree*/) {
  auto* psInfo = new GDALTransformerInfo();
  std::memcpy(psInfo->abySignature, GDAL_GTI2_SIGNATURE,
              strlen(GDAL_GTI2_SIGNATURE));
  psInfo->pszClassName = "DummyCustomTransform";
  psInfo->pfnTransform = DummyCustomTransform;
  psInfo->pfnCleanup = [](void* p) {
    delete static_cast<GDALTransformerInfo*>(p);
  };
  psInfo->pfnSerialize = nullptr;
  psInfo->pfnCreateSimilar = nullptr;
  return psInfo;
}

// Always-failing transformer function for error-propagation testing.
int FailingTransform(void* /*pTransformArg*/, int /*bDstToSrc*/,
                     int nPointCount, double* /*x*/, double* /*y*/,
                     double* /*z*/, int* panSuccess) {
  if (panSuccess) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = FALSE;
    }
  }
  return FALSE;
}

// Partially-failing transformer function where odd indices fail.
int PartiallyFailingTransform(void* /*pTransformArg*/, int /*bDstToSrc*/,
                              int nPointCount, double* x, double* y,
                              double* /*z*/, int* panSuccess) {
  for (int i = 0; i < nPointCount; ++i) {
    if (i % 2 == 0) {
      x[i] += 1.0;
      y[i] += 1.0;
      if (panSuccess) panSuccess[i] = TRUE;
    } else {
      if (panSuccess) panSuccess[i] = FALSE;
    }
  }
  return TRUE;
}

class GdalTransformerTest : public ::testing::Test {
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

TEST_F(GdalTransformerTest, NullAndInvalidPointers) {
  // 1. Destroying nullptr is safe across all transformer types.
  GDALDestroyGenImgProjTransformer(nullptr);
  GDALDestroyReprojectionTransformer(nullptr);
  GDALDestroyApproxTransformer(nullptr);
  GDALDestroyTransformer(nullptr);

  // 2. Setting / Getting geotransform on nullptr handles safely without crash.
  double padfGT[6] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  {
    WithQuietHandler quiet;
    GDALSetTransformerDstGeoTransform(nullptr, padfGT);
    GDALGetTransformerDstGeoTransform(nullptr, padfGT);
    GDALSetGenImgProjTransformerDstGeoTransform(nullptr, padfGT);

    // 3. Cloning or creating similar with nullptr returns nullptr.
    EXPECT_EQ(nullptr, GDALCloneTransformer(nullptr));
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
  }

  // 4. Transforming with nullptr using GDALUseTransformer fails safely.
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  int success = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE,
              GDALUseTransformer(nullptr, FALSE, 1, &x, &y, &z, &success));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. SuggestedWarpOutput on nullptr dataset fails safely.
  int nPixels = 0;
  int nLines = 0;
  double adfExtent[4] = {};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALSuggestedWarpOutput(nullptr, DummyCustomTransform, nullptr,
                                      padfGT, &nPixels, &nLines));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSuggestedWarpOutput2(
                              nullptr, DummyCustomTransform, nullptr, padfGT,
                              &nPixels, &nLines, adfExtent, 0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Non-GTI2 pointers passed to generic transformer routines emit
  // CE_Failure.
  alignas(GDALTransformerInfo) char dummy[sizeof(GDALTransformerInfo)] = {};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDestroyTransformer(dummy);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALUseTransformer(dummy, FALSE, 1, &x, &y, &z, &success));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCloneTransformer(dummy));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(dummy, 1.0, 1.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeTransformer(DummyCustomTransform, dummy));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALSetTransformerDstGeoTransform(dummy, padfGT);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALGetTransformerDstGeoTransform(dummy, padfGT);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 7. GDALIsTransformer with nullptr or non-GTI2 returns false.
  EXPECT_FALSE(GDALIsTransformer(nullptr, GDAL_GEN_IMG_TRANSFORMER_CLASS_NAME));
  EXPECT_FALSE(GDALIsTransformer(dummy, GDAL_GEN_IMG_TRANSFORMER_CLASS_NAME));
}

// -----------------------------------------------------------------------------
// Suite 2: GenImgProj Option List Validation
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjOptionList) {
  const char* pszList = GDALGetGenImgProjTranformerOptionList();
  ASSERT_NE(nullptr, pszList);
  EXPECT_THAT(pszList, HasSubstr("<OptionList>"));
  EXPECT_THAT(pszList, HasSubstr("SRC_SRS"));
  EXPECT_THAT(pszList, HasSubstr("DST_SRS"));
  EXPECT_THAT(pszList, HasSubstr("SRC_METHOD"));
  EXPECT_THAT(pszList, HasSubstr("DST_METHOD"));
  EXPECT_THAT(pszList, HasSubstr("RPC_HEIGHT"));
  EXPECT_THAT(pszList, HasSubstr("COORDINATE_OPERATION"));
  EXPECT_THAT(pszList, HasSubstr("ALLOW_BALLPARK"));
  EXPECT_THAT(pszList, HasSubstr("ONLY_BEST"));
  EXPECT_THAT(pszList, HasSubstr("GEOLOC_ARRAY"));
}

// -----------------------------------------------------------------------------
// Suite 3: GenImgProj with GeoTransform (North-up, Sheared, and Singular)
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GeoTransformOnly) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  src_ds->SetSpatialRef(&srs);
  double adfSrcGT[6] = {1000.0, 10.0, 0.0, 2000.0, 0.0, -10.0};
  src_ds->SetGeoTransform(adfSrcGT);

  GDALDatasetUniquePtr dst_ds(
      mem_driver->Create("", 200, 200, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, dst_ds);
  dst_ds->SetSpatialRef(&srs);
  double adfDstGT[6] = {1000.0, 5.0, 0.0, 2000.0, 0.0, -5.0};
  dst_ds->SetGeoTransform(adfDstGT);

  // 1. Create transformer with GDALCreateGenImgProjTransformer (v1)
  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer(
      src_ds.get(), nullptr, dst_ds.get(), nullptr,
      /*bGCPUseOK=*/FALSE, /*dfGCPErrorThreshold=*/0.0, /*nOrder=*/0));
  ASSERT_NE(nullptr, pTrans);

  // 2. Transform forward (Src pixel/line -> Dst pixel/line)
  double x[3] = {0.0, 50.0, 100.0};
  double y[3] = {0.0, 50.0, 100.0};
  double z[3] = {0.0, 0.0, 0.0};
  int success[3] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 3, x, y, z, success));
  for (int s : success) EXPECT_EQ(TRUE, s);
  EXPECT_DOUBLE_EQ(0.0, x[0]);
  EXPECT_DOUBLE_EQ(0.0, y[0]);
  EXPECT_DOUBLE_EQ(100.0, x[1]);
  EXPECT_DOUBLE_EQ(100.0, y[1]);
  EXPECT_DOUBLE_EQ(200.0, x[2]);
  EXPECT_DOUBLE_EQ(200.0, y[2]);

  // 3. Transform inverse (Dst pixel/line -> Src pixel/line)
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), TRUE, 3, x, y, z, success));
  for (int s : success) EXPECT_EQ(TRUE, s);
  EXPECT_DOUBLE_EQ(0.0, x[0]);
  EXPECT_DOUBLE_EQ(0.0, y[0]);
  EXPECT_DOUBLE_EQ(50.0, x[1]);
  EXPECT_DOUBLE_EQ(50.0, y[1]);
  EXPECT_DOUBLE_EQ(100.0, x[2]);
  EXPECT_DOUBLE_EQ(100.0, y[2]);

  // 4. Test GDALSetGenImgProjTransformerDstGeoTransform and
  // GDALGetTransformerDstGeoTransform
  double adfNewDstGT[6] = {500.0, 2.5, 0.0, 1000.0, 0.0, -2.5};
  GDALSetGenImgProjTransformerDstGeoTransform(pTrans.get(), adfNewDstGT);
  double adfFetchedGT[6] = {};
  GDALGetTransformerDstGeoTransform(pTrans.get(), adfFetchedGT);
  for (int i = 0; i < 6; ++i) {
    EXPECT_DOUBLE_EQ(adfNewDstGT[i], adfFetchedGT[i]);
  }

  // 5. Test setting singular destination geotransform emits CE_Failure
  double adfSingularGT[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALSetGenImgProjTransformerDstGeoTransform(pTrans.get(), adfSingularGT);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 7. Test Sheared/Rotated Geotransform
  double adfShearedGT[6] = {1000.0, 10.0, 2.0, 2000.0, 3.0, -10.0};
  src_ds->SetGeoTransform(adfShearedGT);
  GenImgProjTransformerUniquePtr pShearedTrans(
      GDALCreateGenImgProjTransformer2(src_ds.get(), dst_ds.get(), nullptr));
  ASSERT_NE(nullptr, pShearedTrans);
  double sx[1] = {10.0};
  double sy[1] = {20.0};
  double sz[1] = {0.0};
  int ssuccess[1] = {};
  EXPECT_EQ(TRUE, GDALGenImgProjTransform(pShearedTrans.get(), FALSE, 1, sx, sy,
                                          sz, ssuccess));
  EXPECT_EQ(TRUE, ssuccess[0]);

  // 8. Test Singular source geotransform emits CE_Failure on transformer
  // creation
  src_ds->SetGeoTransform(adfSingularGT);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer2(src_ds.get(),
                                                        dst_ds.get(), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 4: GenImgProj NO_GEOTRANSFORM Option
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjNoGeoTransform) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Dataset without geotransform and without GCPs.
  GDALDatasetUniquePtr unref_ds(
      mem_driver->Create("", 50, 50, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, unref_ds);

  // 1. Without NO_GEOTRANSFORM option, creation must fail.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer2(unref_ds.get(), nullptr,
                                                        nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. With SRC_METHOD=NO_GEOTRANSFORM, creation succeeds and uses identity.
  CPLStringList aosOptions;
  aosOptions.SetNameValue("SRC_METHOD", "NO_GEOTRANSFORM");
  aosOptions.SetNameValue("DST_METHOD", "NO_GEOTRANSFORM");
  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
      unref_ds.get(), nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pTrans);

  double x[1] = {25.0};
  double y[1] = {15.0};
  double z[1] = {0.0};
  int success[1] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_DOUBLE_EQ(25.0, x[0]);
  EXPECT_DOUBLE_EQ(15.0, y[0]);

  // 3. Passing nullptr datasets with NO_GEOTRANSFORM also succeeds as identity.
  GenImgProjTransformerUniquePtr pNullDS(
      GDALCreateGenImgProjTransformer2(nullptr, nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pNullDS);
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pNullDS.get(), FALSE, 1, x, y, z, success));
  EXPECT_DOUBLE_EQ(25.0, x[0]);
  EXPECT_DOUBLE_EQ(15.0, y[0]);
}

// -----------------------------------------------------------------------------
// Suite 5: GenImgProj with Coordinate Reprojection & Comprehensive Options
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjReprojection) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs4326, srs3857;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  src_ds->SetSpatialRef(&srs4326);
  double adfSrcGT[6] = {2.0, 0.01, 0.0, 48.0, 0.0, -0.01};
  src_ds->SetGeoTransform(adfSrcGT);

  // 1. Reproject to EPSG:3857 via DST_SRS option.
  CPLStringList aosOptions;
  aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
  aosOptions.SetNameValue("AREA_OF_INTEREST", "2.0,47.0,3.0,49.0");
  aosOptions.SetNameValue("ALLOW_BALLPARK", "YES");
  aosOptions.SetNameValue("ONLY_BEST", "NO");
  aosOptions.SetNameValue("COORDINATE_EPOCH", "2020.0");
  aosOptions.SetNameValue("SRC_COORDINATE_EPOCH", "2015.0");
  aosOptions.SetNameValue("DST_COORDINATE_EPOCH", "2025.0");
  aosOptions.SetNameValue("PROMOTE_TO_3D", "YES");
  aosOptions.SetNameValue("INSERT_CENTER_LONG", "YES");

  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
      src_ds.get(), nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pTrans);

  double x[1] = {50.0};
  double y[1] = {50.0};
  double z[1] = {100.0};
  int success[1] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_GT(x[0], 200000.0);
  EXPECT_GT(y[0], 5000000.0);
  EXPECT_DOUBLE_EQ(100.0, z[0]);

  // Reverse transform back to source pixel/line
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), TRUE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_NEAR(50.0, x[0], 1e-4);
  EXPECT_NEAR(50.0, y[0], 1e-4);

  // 2. Test GDALRefreshGenImgProjTransformer with CHECK_WITH_INVERT_PROJ
  // toggled
  CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "YES");
  absl::Cleanup opt_cleanup = [] {
    CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr);
  };
  GDALRefreshGenImgProjTransformer(pTrans.get());
  CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "NO");
  GDALRefreshGenImgProjTransformer(pTrans.get());

  // 3. Test Axis Mapping Strategy options
  {
    CPLStringList aosAxisOpts;
    aosAxisOpts.SetNameValue("SRC_SRS", "EPSG:4326");
    aosAxisOpts.SetNameValue("DST_SRS", "EPSG:3857");
    aosAxisOpts.SetNameValue("SRC_SRS_AXIS_MAPPING_STRATEGY",
                             "AUTHORITY_COMPLIANT");
    GenImgProjTransformerUniquePtr pAxisTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosAxisOpts.List()));
    EXPECT_NE(nullptr, pAxisTrans);
  }
  {
    CPLStringList aosAxisOpts;
    aosAxisOpts.SetNameValue("SRC_SRS", "EPSG:4326");
    aosAxisOpts.SetNameValue("DST_SRS", "EPSG:3857");
    aosAxisOpts.SetNameValue("SRC_SRS_AXIS_MAPPING_STRATEGY",
                             "INVALID_STRATEGY");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                                        aosAxisOpts.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // 4. Test DATA_AXIS_TO_SRS_AXIS_MAPPING option
  {
    CPLStringList aosMappingOpts;
    aosMappingOpts.SetNameValue("SRC_SRS", "EPSG:4326");
    aosMappingOpts.SetNameValue("DST_SRS", "EPSG:3857");
    aosMappingOpts.SetNameValue("SRC_SRS_DATA_AXIS_TO_SRS_AXIS_MAPPING", "2,1");
    GenImgProjTransformerUniquePtr pMappingTrans(
        GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                         aosMappingOpts.List()));
    EXPECT_NE(nullptr, pMappingTrans);
  }

  // 5. Test STRIP_VERT_CS with Compound CRS
  {
    WithQuietHandler quiet;
    CPLStringList aosVertOpts;
    aosVertOpts.SetNameValue("SRC_SRS", "EPSG:4326+5773");
    aosVertOpts.SetNameValue("DST_SRS", "EPSG:3857");
    aosVertOpts.SetNameValue("STRIP_VERT_CS", "YES");
    GenImgProjTransformerUniquePtr pVertTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosVertOpts.List()));
    EXPECT_NE(nullptr, pVertTrans);
  }

  // 6. Test COORDINATE_OPERATION override
  {
    CPLStringList aosCoordOpOpts;
    aosCoordOpOpts.SetNameValue("DST_SRS", "EPSG:3857");
    aosCoordOpOpts.SetNameValue(
        "COORDINATE_OPERATION",
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad +step "
        "+proj=webmerc +lat_0=0 +lon_0=0 +ellps=WGS84");
    GenImgProjTransformerUniquePtr pCoordOpTrans(
        GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                         aosCoordOpOpts.List()));
    EXPECT_NE(nullptr, pCoordOpTrans);
  }

  // 7. Test TARGET_EXTENT crossing antimeridian for InsertCenterLong
  {
    CPLStringList aosTargetExtentOpts;
    aosTargetExtentOpts.SetNameValue("DST_SRS", "EPSG:4326");
    aosTargetExtentOpts.SetNameValue("TARGET_EXTENT", "-179,-80,181,80");
    aosTargetExtentOpts.SetNameValue("INSERT_CENTER_LONG", "YES");
    GenImgProjTransformerUniquePtr pTETrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosTargetExtentOpts.List()));
    EXPECT_NE(nullptr, pTETrans);
  }

  // 8. Test Invalid SRC_SRS emits CE_Failure
  {
    CPLStringList aosBadOpts;
    aosBadOpts.SetNameValue("SRC_SRS", "INVALID_COORDINATE_SYSTEM_DEF");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                                        aosBadOpts.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 6: GenImgProj Approximation Options
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjApproxOptions) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  src_ds->SetSpatialRef(&srs4326);
  double adfSrcGT[6] = {2.0, 0.01, 0.0, 48.0, 0.0, -0.01};
  src_ds->SetGeoTransform(adfSrcGT);

  CPLStringList aosOptions;
  aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
  aosOptions.SetNameValue("SRC_APPROX_ERROR_IN_SRS_UNIT", "0.001");
  aosOptions.SetNameValue("SRC_APPROX_ERROR_IN_PIXEL", "0.1");
  aosOptions.SetNameValue("DST_APPROX_ERROR_IN_SRS_UNIT", "1.0");
  aosOptions.SetNameValue("DST_APPROX_ERROR_IN_PIXEL", "0.1");
  aosOptions.SetNameValue("REPROJECTION_APPROX_ERROR_IN_DST_SRS_UNIT", "1.0");
  aosOptions.SetNameValue("REPROJECTION_APPROX_ERROR_IN_SRC_SRS_UNIT", "0.001");

  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
      src_ds.get(), nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pTrans);

  double x[1] = {50.0};
  double y[1] = {50.0};
  double z[1] = {0.0};
  int success[1] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_GT(x[0], 200000.0);
  EXPECT_GT(y[0], 5000000.0);
}

// -----------------------------------------------------------------------------
// Suite 7: GenImgProj with GCPs (Polynomial Orders, Refinement & Antimeridian)
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjGCPPolynomial) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  // Setup 8 GCPs spanning antimeridian ([-175, 175])
  GDAL_GCP asGCPs[8];
  GDALInitGCPs(8, asGCPs);
  absl::Cleanup gcp_cleanup = [&asGCPs] { GDALDeinitGCPs(8, asGCPs); };

  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = -175.0;
  asGCPs[0].dfGCPY = 10.0;

  asGCPs[1].dfGCPPixel = 100.0;
  asGCPs[1].dfGCPLine = 0.0;
  asGCPs[1].dfGCPX = 175.0;
  asGCPs[1].dfGCPY = 10.0;

  asGCPs[2].dfGCPPixel = 0.0;
  asGCPs[2].dfGCPLine = 100.0;
  asGCPs[2].dfGCPX = -175.0;
  asGCPs[2].dfGCPY = 20.0;

  asGCPs[3].dfGCPPixel = 100.0;
  asGCPs[3].dfGCPLine = 100.0;
  asGCPs[3].dfGCPX = 175.0;
  asGCPs[3].dfGCPY = 20.0;

  asGCPs[4].dfGCPPixel = 50.0;
  asGCPs[4].dfGCPLine = 50.0;
  asGCPs[4].dfGCPX = 180.0;
  asGCPs[4].dfGCPY = 15.0;

  asGCPs[5].dfGCPPixel = 25.0;
  asGCPs[5].dfGCPLine = 75.0;
  asGCPs[5].dfGCPX = -177.5;
  asGCPs[5].dfGCPY = 17.5;

  asGCPs[6].dfGCPPixel = 75.0;
  asGCPs[6].dfGCPLine = 25.0;
  asGCPs[6].dfGCPX = 177.5;
  asGCPs[6].dfGCPY = 12.5;

  asGCPs[7].dfGCPPixel = 50.0;
  asGCPs[7].dfGCPLine = 0.0;
  asGCPs[7].dfGCPX = 180.0;
  asGCPs[7].dfGCPY = 10.0;

  src_ds->SetGCPs(8, asGCPs, &srs);

  // 1. Order 1 polynomial with auto antimeridian unwrapping
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "GCP_POLYNOMIAL");
    aosOptions.SetNameValue("MAX_GCP_ORDER", "1");
    aosOptions.SetNameValue("GCP_ANTIMERIDIAN_UNWRAP", "AUTO");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double x[1] = {50.0};
    double y[1] = {50.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z,
                                            success));
    EXPECT_EQ(TRUE, success[0]);
  }

  // 2. Order 2 polynomial with GCP refinement options
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "GCP_POLYNOMIAL");
    aosOptions.SetNameValue("MAX_GCP_ORDER", "2");
    aosOptions.SetNameValue("REFINE_TOLERANCE", "5.0");
    aosOptions.SetNameValue("REFINE_MINIMUM_GCPS", "6");
    aosOptions.SetNameValue("GCP_ANTIMERIDIAN_UNWRAP", "YES");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);
  }

  // 3. GCP_ANTIMERIDIAN_UNWRAP=NO
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "GCP_POLYNOMIAL");
    aosOptions.SetNameValue("GCP_ANTIMERIDIAN_UNWRAP", "NO");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);
  }

  // 4. GCPS_OK=NO must reject GCPs and fail since no geotransform exists
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("GCPS_OK", "NO");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                                        aosOptions.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 8: GenImgProj with Thin Plate Spline (TPS)
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjThinPlateSpline) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDAL_GCP asGCPs[5];
  GDALInitGCPs(5, asGCPs);
  absl::Cleanup gcp_cleanup = [&asGCPs] { GDALDeinitGCPs(5, asGCPs); };

  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 10.0;
  asGCPs[0].dfGCPY = 20.0;

  asGCPs[1].dfGCPPixel = 100.0;
  asGCPs[1].dfGCPLine = 0.0;
  asGCPs[1].dfGCPX = 11.0;
  asGCPs[1].dfGCPY = 20.0;

  asGCPs[2].dfGCPPixel = 0.0;
  asGCPs[2].dfGCPLine = 100.0;
  asGCPs[2].dfGCPX = 10.0;
  asGCPs[2].dfGCPY = 21.0;

  asGCPs[3].dfGCPPixel = 100.0;
  asGCPs[3].dfGCPLine = 100.0;
  asGCPs[3].dfGCPX = 11.0;
  asGCPs[3].dfGCPY = 21.0;

  asGCPs[4].dfGCPPixel = 50.0;
  asGCPs[4].dfGCPLine = 50.0;
  asGCPs[4].dfGCPX = 10.5;
  asGCPs[4].dfGCPY = 20.5;

  src_ds->SetGCPs(5, asGCPs, &srs);

  CPLStringList aosOptions;
  aosOptions.SetNameValue("SRC_METHOD", "GCP_TPS");
  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
      src_ds.get(), nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pTrans);

  // TPS transformer does NOT have fast clone
  EXPECT_FALSE(GDALTransformHasFastClone(pTrans.get()));

  double x[1] = {50.0};
  double y[1] = {50.0};
  double z[1] = {0.0};
  int success[1] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_NEAR(10.5, x[0], 1e-4);
  EXPECT_NEAR(20.5, y[0], 1e-4);
}

// -----------------------------------------------------------------------------
// Suite 9: GenImgProj with Homography
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjHomography) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDAL_GCP asGCPs[4];
  GDALInitGCPs(4, asGCPs);
  absl::Cleanup gcp_cleanup = [&asGCPs] { GDALDeinitGCPs(4, asGCPs); };

  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 10.0;
  asGCPs[0].dfGCPY = 20.0;

  asGCPs[1].dfGCPPixel = 100.0;
  asGCPs[1].dfGCPLine = 0.0;
  asGCPs[1].dfGCPX = 11.0;
  asGCPs[1].dfGCPY = 20.0;

  asGCPs[2].dfGCPPixel = 0.0;
  asGCPs[2].dfGCPLine = 100.0;
  asGCPs[2].dfGCPX = 10.0;
  asGCPs[2].dfGCPY = 21.0;

  asGCPs[3].dfGCPPixel = 100.0;
  asGCPs[3].dfGCPLine = 100.0;
  asGCPs[3].dfGCPX = 11.0;
  asGCPs[3].dfGCPY = 21.0;

  src_ds->SetGCPs(4, asGCPs, &srs);

  CPLStringList aosOptions;
  aosOptions.SetNameValue("SRC_METHOD", "GCP_HOMOGRAPHY");
  GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
      src_ds.get(), nullptr, aosOptions.List()));
  ASSERT_NE(nullptr, pTrans);

  double x[1] = {50.0};
  double y[1] = {50.0};
  double z[1] = {0.0};
  int success[1] = {};
  EXPECT_EQ(TRUE,
            GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z, success));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_NEAR(10.5, x[0], 1e-4);
  EXPECT_NEAR(20.5, y[0], 1e-4);
}

// -----------------------------------------------------------------------------
// Suite 10: GenImgProj with RPC Model
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjRPC) {
  constexpr char kVsiDir[] = "/vsimem/gdaltransformer_test/rpc";
  constexpr char kDemPath[] = "/vsimem/gdaltransformer_test/rpc/dem.tif";
  absl::Cleanup dir_cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };

  double adfDemGT[6] = {18.0, 0.1, 0.0, 22.0, 0.0, -0.1};
  ASSERT_TRUE(WriteDemTiff(kDemPath, 40, 40, adfDemGT, 50.0));

  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 1000, 1000, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  CPLStringList aosRPCMD = CreateLinearRPCMetadata();
  aosRPCMD.SetNameValue("HEIGHT_DEFAULT", "25.0");
  src_ds->SetMetadata(aosRPCMD.List(), "RPC");

  // 1. Basic RPC forward and reverse
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "RPC");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    EXPECT_TRUE(GDALTransformHasFastClone(pTrans.get()));

    double x[1] = {200.0};
    double y[1] = {100.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z,
                                            success));
    EXPECT_EQ(TRUE, success[0]);
    EXPECT_NEAR(20.0, x[0], 1e-3);
    EXPECT_NEAR(10.0, y[0], 1e-3);
  }

  // 2. RPC with DEM, RPC_HEIGHT, and DEM interpolation options
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "RPC");
    aosOptions.SetNameValue("RPC_DEM", kDemPath);
    aosOptions.SetNameValue("RPC_DEMINTERPOLATION", "BILINEAR");
    aosOptions.SetNameValue("RPC_DEM_MISSING_VALUE", "0.0");
    aosOptions.SetNameValue("RPC_PIXEL_ERROR_THRESHOLD", "0.05");
    aosOptions.SetNameValue("RPC_MAX_ITERATIONS", "15");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);
  }
}

// -----------------------------------------------------------------------------
// Suite 11: GenImgProj with Geolocation Array
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjGeoLoc) {
  constexpr char kVsiDir[] = "/vsimem/gdaltransformer_test/geoloc";
  constexpr char kGeolocPath[] =
      "/vsimem/gdaltransformer_test/geoloc/geoloc.tif";
  absl::Cleanup dir_cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };
  CPLSetConfigOption("CPL_TMPDIR", kVsiDir);
  absl::Cleanup tmpdir_cleanup = [] {
    CPLSetConfigOption("CPL_TMPDIR", nullptr);
  };

  ASSERT_TRUE(WriteGeolocTiff(kGeolocPath, 10, 10));

  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  // 1. Using SRC_GEOLOC_ARRAY option
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_GEOLOC_ARRAY", kGeolocPath);
    aosOptions.SetNameValue("GEOLOC_BACKMAP_OVERSAMPLE_FACTOR", "1.5");
    aosOptions.SetNameValue("GEOLOC_USE_TEMP_DATASETS", "NO");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double x[1] = {50.0};
    double y[1] = {50.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z,
                                            success));
    EXPECT_EQ(TRUE, success[0]);
    EXPECT_NEAR(10.5, x[0], 0.2);
    EXPECT_NEAR(40.5, y[0], 0.2);
  }

  // 2. Both GEOLOCATION metadata domain and GEOLOC_ARRAY option set emits
  // warning
  {
    CPLStringList aosGeolocMD =
        GDALCreateGeolocationMetadata(src_ds.get(), kGeolocPath, true);
    src_ds->SetMetadata(aosGeolocMD.List(), "GEOLOCATION");

    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_GEOLOC_ARRAY", kGeolocPath);
    aosOptions.SetNameValue("GEOLOC_USE_TEMP_DATASETS", "NO");
    std::string captured_warnings;
    CPLPushErrorHandlerEx(
        [](CPLErr eErrClass, CPLErrorNum /*nError*/, const char* pszErrorMsg) {
          if (eErrClass == CE_Warning && pszErrorMsg != nullptr) {
            auto* out = static_cast<std::string*>(CPLGetErrorHandlerUserData());
            out->append(pszErrorMsg);
          }
        },
        &captured_warnings);
    absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };
    CPLErrorReset();
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    EXPECT_NE(nullptr, pTrans);
    EXPECT_THAT(captured_warnings,
                HasSubstr("Both GEOLOCATION metadata domain"));
  }
}

// -----------------------------------------------------------------------------
// Suite 12: GenImgProj Versions 3 and 4
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjVersions3And4) {
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  char* pszWKT = nullptr;
  srs4326.exportToWkt(&pszWKT);
  absl::Cleanup wkt_cleanup = [pszWKT] { CPLFree(pszWKT); };

  double adfSrcGT[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  double adfDstGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};

  // 1. GDALCreateGenImgProjTransformer3 nominal
  {
    GenImgProjTransformerUniquePtr pTrans(
        GDALCreateGenImgProjTransformer3(pszWKT, adfSrcGT, pszWKT, adfDstGT));
    ASSERT_NE(nullptr, pTrans);

    double x[1] = {10.0};
    double y[1] = {20.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, GDALGenImgProjTransform(pTrans.get(), FALSE, 1, x, y, z,
                                            success));
    EXPECT_EQ(TRUE, success[0]);
  }

  // 2. GDALCreateGenImgProjTransformer3 with nullptr WKTs
  {
    GenImgProjTransformerUniquePtr pTrans(
        GDALCreateGenImgProjTransformer3(nullptr, adfSrcGT, nullptr, adfDstGT));
    EXPECT_NE(nullptr, pTrans);
  }

  // 3. GDALCreateGenImgProjTransformer3 with invalid WKT emits CE_Failure
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer3("INVALID_WKT", adfSrcGT,
                                                        nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer3(
                           nullptr, nullptr, "INVALID_WKT", adfDstGT));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. GDALCreateGenImgProjTransformer4 nominal
  {
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer4(
        OGRSpatialReference::ToHandle(&srs4326), adfSrcGT,
        OGRSpatialReference::ToHandle(&srs4326), adfDstGT, nullptr));
    ASSERT_NE(nullptr, pTrans);
  }

  // 5. GDALCreateGenImgProjTransformer4 with singular geotransform emits
  // CE_Failure
  double adfSingular[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGenImgProjTransformer4(
                  OGRSpatialReference::ToHandle(&srs4326), adfSingular,
                  OGRSpatialReference::ToHandle(&srs4326), adfDstGT, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGenImgProjTransformer4(
                           OGRSpatialReference::ToHandle(&srs4326), adfSrcGT,
                           OGRSpatialReference::ToHandle(&srs4326), adfSingular,
                           nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 13: LonLatToDest Coordinate Transforms
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjLonLatToDest) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs4326, srs32631;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs32631.importFromEPSG(32631);
  srs32631.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Geographic source CRS (EPSG:4326) to destination pixel/line
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    src_ds->SetSpatialRef(&srs4326);
    double adfSrcGT[6] = {2.0, 0.01, 0.0, 48.0, 0.0, -0.01};
    src_ds->SetGeoTransform(adfSrcGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    // GDALTransformLonLatToDestGenImgProjTransformer takes a geographic fast
    // path (gdaltransformer.cpp:3203-3246) returning (lon, lat) unchanged when
    // the source CRS is geographic.
    double lon = 2.5;
    double lat = 48.5;
    EXPECT_EQ(TRUE, GDALTransformLonLatToDestGenImgProjTransformer(pTrans.get(),
                                                                   &lon, &lat));
    EXPECT_DOUBLE_EQ(2.5, lon);
    EXPECT_DOUBLE_EQ(48.5, lat);

    // Test through ApproxTransformer wrapper
    ApproxTransformerUniquePtr pApprox(GDALCreateApproxTransformer(
        GDALGenImgProjTransform, pTrans.get(), 0.0));
    ASSERT_NE(nullptr, pApprox);
    lon = 2.5;
    lat = 48.5;
    EXPECT_EQ(TRUE, GDALTransformLonLatToDestApproxTransformer(pApprox.get(),
                                                               &lon, &lat));
  }

  // 2. Projected source CRS (EPSG:32631) to destination pixel/line
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    src_ds->SetSpatialRef(&srs32631);
    double adfSrcGT[6] = {500000.0, 10.0, 0.0, 4500000.0, 0.0, -10.0};
    src_ds->SetGeoTransform(adfSrcGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:4326");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double lon = 3.0;
    double lat = 40.5;
    EXPECT_EQ(TRUE, GDALTransformLonLatToDestGenImgProjTransformer(pTrans.get(),
                                                                   &lon, &lat));
    EXPECT_NEAR(3.0, lon, 1e-4);
    EXPECT_NEAR(40.5, lat, 1e-4);
  }

  // 3. GenImgProj without reprojection returns false
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    double adfSrcGT[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    src_ds->SetGeoTransform(adfSrcGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "NO_GEOTRANSFORM");
    aosOptions.SetNameValue("DST_METHOD", "NO_GEOTRANSFORM");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double lon = 10.0;
    double lat = 20.0;
    EXPECT_EQ(FALSE, GDALTransformLonLatToDestGenImgProjTransformer(
                         pTrans.get(), &lon, &lat));
  }
}

// -----------------------------------------------------------------------------
// Suite 14: Reprojection Transformer
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, ReprojectionTransformer) {
  OGRSpatialReference srs4326, srs3857;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  char* pszSrcWKT = nullptr;
  srs4326.exportToWkt(&pszSrcWKT);
  std::unique_ptr<char, CplFreeDeleter> src_cleanup(pszSrcWKT);

  char* pszDstWKT = nullptr;
  srs3857.exportToWkt(&pszDstWKT);
  std::unique_ptr<char, CplFreeDeleter> dst_cleanup(pszDstWKT);

  // 1. Nominal forward and reverse reprojection
  {
    ReprojectionTransformerUniquePtr pTrans(
        GDALCreateReprojectionTransformer(pszSrcWKT, pszDstWKT));
    ASSERT_NE(nullptr, pTrans);

    double x[1] = {2.0};
    double y[1] = {48.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, GDALReprojectionTransform(pTrans.get(), FALSE, 1, x, y, z,
                                              success));
    EXPECT_EQ(TRUE, success[0]);
    EXPECT_GT(x[0], 200000.0);
    EXPECT_GT(y[0], 5000000.0);

    // Inverse
    EXPECT_EQ(TRUE, GDALReprojectionTransform(pTrans.get(), TRUE, 1, x, y, z,
                                              success));
    EXPECT_EQ(TRUE, success[0]);
    EXPECT_NEAR(2.0, x[0], 1e-6);
    EXPECT_NEAR(48.0, y[0], 1e-6);
  }

  // 2. Invalid WKT emits CE_Failure
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateReprojectionTransformer("INVALID_WKT", pszDstWKT));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateReprojectionTransformer(pszSrcWKT, "INVALID_WKT"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. GDALCreateReprojectionTransformerEx with options
  {
    CPLStringList aosOptions;
    aosOptions.SetNameValue("AREA_OF_INTEREST", "2.0,47.0,3.0,49.0");
    aosOptions.SetNameValue("CENTER_LONG", "2.5");
    aosOptions.SetNameValue("ALLOW_BALLPARK", "NO");
    aosOptions.SetNameValue("ONLY_BEST", "YES");
    aosOptions.SetNameValue("COORDINATE_EPOCH", "2022.0");

    ReprojectionTransformerUniquePtr pTrans(GDALCreateReprojectionTransformerEx(
        OGRSpatialReference::ToHandle(&srs4326),
        OGRSpatialReference::ToHandle(&srs3857), aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    // 4. Test poReverseTransform == nullptr failure path
    auto* psInfo = static_cast<GDALReprojectionTransformInfo*>(pTrans.get());
    OGRCoordinateTransformation* poSavedRev = psInfo->poReverseTransform;
    psInfo->poReverseTransform = nullptr;
    double rx[1] = {200000.0};
    double ry[1] = {5000000.0};
    double rz[1] = {0.0};
    int rsuccess[1] = {TRUE};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(FALSE, GDALReprojectionTransform(pTrans.get(), TRUE, 1, rx, ry,
                                                 rz, rsuccess));
      EXPECT_EQ(FALSE, rsuccess[0]);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    psInfo->poReverseTransform = poSavedRev;
  }
}

// -----------------------------------------------------------------------------
// Suite 15: Approx Transformer
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, ApproxTransformer) {
  OGRSpatialReference srs4326, srs3857;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  ReprojectionTransformerUniquePtr pReproj(GDALCreateReprojectionTransformerEx(
      OGRSpatialReference::ToHandle(&srs4326),
      OGRSpatialReference::ToHandle(&srs3857), nullptr));
  ASSERT_NE(nullptr, pReproj);

  // 1. ApproxTransformer with dfMaxError = 0.25 and ownership transferred
  ApproxTransformerUniquePtr pApprox(GDALCreateApproxTransformer(
      GDALReprojectionTransform, pReproj.get(), 0.25));
  ASSERT_NE(nullptr, pApprox);
  GDALApproxTransformerOwnsSubtransformer(pApprox.get(), TRUE);
  pReproj.release();

  // 2. Exact evaluation path (<= 5 points)
  double x5[5] = {10.0, 10.1, 10.2, 10.3, 10.4};
  double y5[5] = {20.0, 20.1, 20.2, 20.3, 20.4};
  double z5[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
  int success5[5] = {};
  EXPECT_EQ(1,
            GDALApproxTransform(pApprox.get(), FALSE, 5, x5, y5, z5, success5));
  for (int s : success5) EXPECT_EQ(1, s);

  // 3. Adaptive subdivision path (> 5 points)
  constexpr int kNumPoints = 50;
  std::vector<double> x(kNumPoints), y(kNumPoints), z(kNumPoints, 0.0);
  std::vector<int> success(kNumPoints, 0);
  for (int i = 0; i < kNumPoints; ++i) {
    x[i] = 10.0 + i * 0.1;
    y[i] = 20.0 + i * 0.1;
  }
  EXPECT_EQ(1, GDALApproxTransform(pApprox.get(), FALSE, kNumPoints, x.data(),
                                   y.data(), z.data(), success.data()));
  for (int s : success) EXPECT_EQ(1, s);

  // 4. Reverse transform
  EXPECT_EQ(1, GDALApproxTransform(pApprox.get(), TRUE, kNumPoints, x.data(),
                                   y.data(), z.data(), success.data()));
  for (int i = 0; i < kNumPoints; ++i) {
    EXPECT_NEAR(10.0 + i * 0.1, x[i], 1e-1);
    EXPECT_NEAR(20.0 + i * 0.1, y[i], 1e-1);
  }

  // 5. Test GDALRefreshApproxTransformer
  GDALRefreshApproxTransformer(pApprox.get());

  // 6. Test subtransformer failure propagation
  ApproxTransformerUniquePtr pFailingApprox(
      GDALCreateApproxTransformer(PartiallyFailingTransform, nullptr, 0.25));
  ASSERT_NE(nullptr, pFailingApprox);
  std::vector<double> fx(10, 5.0), fy(10, 5.0), fz(10, 0.0);
  std::vector<int> fsuccess(10, 0);
  GDALApproxTransform(pFailingApprox.get(), FALSE, 10, fx.data(), fy.data(),
                      fz.data(), fsuccess.data());
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(i % 2 == 0 ? 1 : 0, fsuccess[i]);
  }
}

// -----------------------------------------------------------------------------
// Suite 16: Transformer Serialization, Deserializer Registry & Error Handling
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, TransformerSerializationAndRegistry) {
  OGRSpatialReference srs4326, srs3857;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. ReprojectionTransformer serialization round-trip
  {
    ReprojectionTransformerUniquePtr pReproj(
        GDALCreateReprojectionTransformerEx(
            OGRSpatialReference::ToHandle(&srs4326),
            OGRSpatialReference::ToHandle(&srs3857), nullptr));
    ASSERT_NE(nullptr, pReproj);

    CPLXMLNodeUniquePtr xml(
        GDALSerializeTransformer(GDALReprojectionTransform, pReproj.get()));
    ASSERT_NE(nullptr, xml);
    EXPECT_STREQ("ReprojectionTransformer", xml->pszValue);

    GDALTransformerFunc pfnFunc = nullptr;
    void* pDeserialized = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xml.get(), &pfnFunc, &pDeserialized));
    ASSERT_NE(nullptr, pDeserialized);
    TransformerUniquePtr cleanup(pDeserialized);
    EXPECT_EQ(GDALReprojectionTransform, pfnFunc);

    double x[1] = {2.0};
    double y[1] = {48.0};
    double z[1] = {0.0};
    int success[1] = {};
    EXPECT_EQ(TRUE, pfnFunc(pDeserialized, FALSE, 1, x, y, z, success));
    EXPECT_EQ(TRUE, success[0]);
    EXPECT_GT(x[0], 200000.0);
  }

  // 2. ApproxTransformer serialization round-trip (Equal and Unequal Errors)
  {
    ReprojectionTransformerUniquePtr pReproj(
        GDALCreateReprojectionTransformerEx(
            OGRSpatialReference::ToHandle(&srs4326),
            OGRSpatialReference::ToHandle(&srs3857), nullptr));
    ASSERT_NE(nullptr, pReproj);

    ApproxTransformerUniquePtr pApprox(GDALCreateApproxTransformer(
        GDALReprojectionTransform, pReproj.get(), 0.5));
    ASSERT_NE(nullptr, pApprox);
    GDALApproxTransformerOwnsSubtransformer(pApprox.get(), TRUE);
    pReproj.release();

    CPLXMLNodeUniquePtr xml(
        GDALSerializeTransformer(GDALApproxTransform, pApprox.get()));
    ASSERT_NE(nullptr, xml);
    EXPECT_STREQ("ApproxTransformer", xml->pszValue);
    EXPECT_NE(nullptr, CPLGetXMLNode(xml.get(), "MaxError"));

    GDALTransformerFunc pfnFunc = nullptr;
    void* pDeserialized = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xml.get(), &pfnFunc, &pDeserialized));
    ASSERT_NE(nullptr, pDeserialized);
    TransformerUniquePtr cleanup(pDeserialized);
    EXPECT_EQ(GDALApproxTransform, pfnFunc);
  }

  // 3. GenImgProjTransformer serialization round-trip
  {
    double adfSrcGT[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    double adfDstGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
    GenImgProjTransformerUniquePtr pGen(GDALCreateGenImgProjTransformer4(
        OGRSpatialReference::ToHandle(&srs4326), adfSrcGT,
        OGRSpatialReference::ToHandle(&srs3857), adfDstGT, nullptr));
    ASSERT_NE(nullptr, pGen);

    CPLXMLNodeUniquePtr xml(
        GDALSerializeTransformer(GDALGenImgProjTransform, pGen.get()));
    ASSERT_NE(nullptr, xml);
    EXPECT_STREQ("GenImgProjTransformer", xml->pszValue);

    GDALTransformerFunc pfnFunc = nullptr;
    void* pDeserialized = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xml.get(), &pfnFunc, &pDeserialized));
    ASSERT_NE(nullptr, pDeserialized);
    TransformerUniquePtr cleanup(pDeserialized);
    EXPECT_EQ(GDALGenImgProjTransform, pfnFunc);
  }

  // 4. Custom Deserializer Registration & Unregistration
  {
    void* hReg = GDALRegisterTransformDeserializer(
        "DummyCustomTransform", DummyCustomTransform, DummyCustomDeserialize);
    ASSERT_NE(nullptr, hReg);
    absl::Cleanup reg_cleanup = [hReg] {
      GDALUnregisterTransformDeserializer(hReg);
      GDALCleanupTransformDeserializerMutex();
    };

    CPLXMLNode* psTree =
        CPLCreateXMLNode(nullptr, CXT_Element, "DummyCustomTransform");
    absl::Cleanup xml_cleanup = [psTree] { CPLDestroyXMLNode(psTree); };

    GDALTransformerFunc pfnFunc = nullptr;
    void* pArg = nullptr;
    EXPECT_EQ(CE_None, GDALDeserializeTransformer(psTree, &pfnFunc, &pArg));
    EXPECT_EQ(DummyCustomTransform, pfnFunc);
    ASSERT_NE(nullptr, pArg);
    TransformerUniquePtr cleanup(pArg);
  }

  // 5. Deserialization error handling (Malformed XML, Unknown Element)
  {
    GDALTransformerFunc pfnFunc = nullptr;
    void* pArg = nullptr;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALDeserializeTransformer(nullptr, &pfnFunc, &pArg));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLXMLNode* psBadTree =
        CPLCreateXMLNode(nullptr, CXT_Element, "UnrecognizedTransformerType");
    absl::Cleanup bad_cleanup = [psBadTree] { CPLDestroyXMLNode(psBadTree); };
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDeserializeTransformer(psBadTree, &pfnFunc, &pArg));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Deserializing ApproxTransformer with invalid BaseTransformer emits
  // CE_Failure
  {
    CPLXMLNode* psApproxXML =
        CPLCreateXMLNode(nullptr, CXT_Element, "ApproxTransformer");
    absl::Cleanup approx_cleanup = [psApproxXML] {
      CPLDestroyXMLNode(psApproxXML);
    };
    CPLCreateXMLElementAndValue(psApproxXML, "MaxError", "0.25");
    GDALTransformerFunc pfnFunc = nullptr;
    void* pArg = nullptr;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDeserializeTransformer(psApproxXML, &pfnFunc, &pArg));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 7. Deserializing GenImgProjTransformer with singular GeoTransform emits
  // CE_Failure
  {
    CPLXMLNode* psGenXML =
        CPLCreateXMLNode(nullptr, CXT_Element, "GenImgProjTransformer");
    absl::Cleanup gen_cleanup = [psGenXML] { CPLDestroyXMLNode(psGenXML); };
    CPLCreateXMLElementAndValue(psGenXML, "SrcGeoTransform", "0,0,0,0,0,0");
    GDALTransformerFunc pfnFunc = nullptr;
    void* pArg = nullptr;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALDeserializeTransformer(psGenXML, &pfnFunc, &pArg));
    if (pArg != nullptr) {
      GDALDestroyTransformer(pArg);
    }
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 17: Transformer Cloning and Similar Transformer Creation
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, TransformerCloneAndSimilar) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  double adfSrcGT[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
  double adfDstGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};

  GenImgProjTransformerUniquePtr pGen(GDALCreateGenImgProjTransformer4(
      OGRSpatialReference::ToHandle(&srs), adfSrcGT,
      OGRSpatialReference::ToHandle(&srs), adfDstGT, nullptr));
  ASSERT_NE(nullptr, pGen);

  // 1. GDALCloneTransformer on GenImgProjTransformer
  TransformerUniquePtr pCloned(GDALCloneTransformer(pGen.get()));
  ASSERT_NE(nullptr, pCloned);

  double x1[1] = {5.0}, y1[1] = {5.0}, z1[1] = {0.0};
  double x2[1] = {5.0}, y2[1] = {5.0}, z2[1] = {0.0};
  int success1[1] = {}, success2[1] = {};
  GDALGenImgProjTransform(pGen.get(), FALSE, 1, x1, y1, z1, success1);
  GDALGenImgProjTransform(pCloned.get(), FALSE, 1, x2, y2, z2, success2);
  EXPECT_DOUBLE_EQ(x1[0], x2[0]);
  EXPECT_DOUBLE_EQ(y1[0], y2[0]);

  // 2. GDALCreateSimilarTransformer with scaling ratios
  TransformerUniquePtr pSimilar(
      GDALCreateSimilarTransformer(pGen.get(), 2.0, 3.0));
  ASSERT_NE(nullptr, pSimilar);

  // 3. CreateSimilar on ReprojectionTransformer (which lacks pfnCreateSimilar)
  // emits CE_Failure
  ReprojectionTransformerUniquePtr pReproj(GDALCreateReprojectionTransformerEx(
      OGRSpatialReference::ToHandle(&srs), OGRSpatialReference::ToHandle(&srs),
      nullptr));
  ASSERT_NE(nullptr, pReproj);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(pReproj.get(), 2.0, 2.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. GDALCloneTransformer on ApproxTransformer
  ApproxTransformerUniquePtr pApprox(
      GDALCreateApproxTransformer(GDALGenImgProjTransform, pGen.get(), 0.1));
  ASSERT_NE(nullptr, pApprox);
  TransformerUniquePtr pClonedApprox(GDALCloneTransformer(pApprox.get()));
  ASSERT_NE(nullptr, pClonedApprox);

  // 5. GDALCreateSimilarTransformer on ApproxTransformer
  TransformerUniquePtr pSimilarApprox(
      GDALCreateSimilarTransformer(pApprox.get(), 2.0, 2.0));
  ASSERT_NE(nullptr, pSimilarApprox);
}

// -----------------------------------------------------------------------------
// Suite 18: Classification & Introspection Helpers
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, TransformerClassificationHelpers) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Translation on pixel boundaries
  // Identical pixel size, no rotation, integer pixel offset (4 / 2 = 2 pixels)
  double adfSrcGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  double adfDstGT[6] = {104.0, 2.0, 0.0, 196.0, 0.0, -2.0};
  GenImgProjTransformerUniquePtr pTransOnBoundaries(
      GDALCreateGenImgProjTransformer4(
          OGRSpatialReference::ToHandle(&srs), adfSrcGT,
          OGRSpatialReference::ToHandle(&srs), adfDstGT, nullptr));
  ASSERT_NE(nullptr, pTransOnBoundaries);
  EXPECT_TRUE(GDALTransformIsTranslationOnPixelBoundaries(
      GDALGenImgProjTransform, pTransOnBoundaries.get()));
  EXPECT_TRUE(GDALTransformIsAffineNoRotation(GDALGenImgProjTransform,
                                              pTransOnBoundaries.get()));
  EXPECT_TRUE(GDALTransformHasFastClone(pTransOnBoundaries.get()));

  // Wrapping in ApproxTransformer retains the classification
  ApproxTransformerUniquePtr pApproxWrap(GDALCreateApproxTransformer(
      GDALGenImgProjTransform, pTransOnBoundaries.get(), 0.0));
  EXPECT_TRUE(GDALTransformIsTranslationOnPixelBoundaries(GDALApproxTransform,
                                                          pApproxWrap.get()));
  EXPECT_TRUE(
      GDALTransformIsAffineNoRotation(GDALApproxTransform, pApproxWrap.get()));
  EXPECT_TRUE(GDALTransformHasFastClone(pApproxWrap.get()));

  // 2. Fractional translation (not on pixel boundaries)
  double adfDstFracGT[6] = {101.5, 2.0, 0.0, 196.0, 0.0, -2.0};
  GenImgProjTransformerUniquePtr pTransFrac(GDALCreateGenImgProjTransformer4(
      OGRSpatialReference::ToHandle(&srs), adfSrcGT,
      OGRSpatialReference::ToHandle(&srs), adfDstFracGT, nullptr));
  ASSERT_NE(nullptr, pTransFrac);
  EXPECT_FALSE(GDALTransformIsTranslationOnPixelBoundaries(
      GDALGenImgProjTransform, pTransFrac.get()));
  EXPECT_TRUE(GDALTransformIsAffineNoRotation(GDALGenImgProjTransform,
                                              pTransFrac.get()));

  // 3. Sheared geotransform (not unrotated)
  double adfDstShearedGT[6] = {100.0, 2.0, 0.5, 200.0, 0.25, -2.0};
  GenImgProjTransformerUniquePtr pTransSheared(GDALCreateGenImgProjTransformer4(
      OGRSpatialReference::ToHandle(&srs), adfSrcGT,
      OGRSpatialReference::ToHandle(&srs), adfDstShearedGT, nullptr));
  ASSERT_NE(nullptr, pTransSheared);
  EXPECT_FALSE(GDALTransformIsTranslationOnPixelBoundaries(
      GDALGenImgProjTransform, pTransSheared.get()));
  EXPECT_FALSE(GDALTransformIsAffineNoRotation(GDALGenImgProjTransform,
                                               pTransSheared.get()));

  // 4. GDALIsTransformer verification across classes
  EXPECT_TRUE(GDALIsTransformer(pTransOnBoundaries.get(),
                                GDAL_GEN_IMG_TRANSFORMER_CLASS_NAME));
  EXPECT_FALSE(GDALIsTransformer(pTransOnBoundaries.get(),
                                 GDAL_APPROX_TRANSFORMER_CLASS_NAME));

  ReprojectionTransformerUniquePtr pReproj(GDALCreateReprojectionTransformerEx(
      OGRSpatialReference::ToHandle(&srs), OGRSpatialReference::ToHandle(&srs),
      nullptr));
  EXPECT_TRUE(GDALIsTransformer(pReproj.get(),
                                GDAL_REPROJECTION_TRANSFORMER_CLASS_NAME));
}

// -----------------------------------------------------------------------------
// Suite 19: Suggested Warp Output
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, SuggestedWarpOutput) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs4326, srs3857, srs3413;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3413.importFromEPSG(3413);  // NSIDC Polar Stereographic North
  srs3413.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. Same CRS shortcut (North-up vs South-up)
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 50, 1, GDT_Byte, nullptr));
    src_ds->SetSpatialRef(&srs4326);
    double adfGT[6] = {10.0, 0.5, 0.0, 20.0, 0.0, -0.5};
    src_ds->SetGeoTransform(adfGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:4326");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double adfGTOut[6] = {};
    int nPixels = 0;
    int nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput2(
                           src_ds.get(), GDALGenImgProjTransform, pTrans.get(),
                           adfGTOut, &nPixels, &nLines, adfExtent, 0));
    EXPECT_EQ(100, nPixels);
    EXPECT_EQ(50, nLines);
    EXPECT_DOUBLE_EQ(10.0, adfExtent[0]);
    EXPECT_DOUBLE_EQ(-5.0, adfExtent[1]);
    EXPECT_DOUBLE_EQ(60.0, adfExtent[2]);
    EXPECT_DOUBLE_EQ(20.0, adfExtent[3]);

    // South-up input (adfGT[5] > 0)
    double adfSouthUpGT[6] = {10.0, 0.5, 0.0, 20.0, 0.0, 0.5};
    src_ds->SetGeoTransform(adfSouthUpGT);
    GenImgProjTransformerUniquePtr pSouthUpTrans(
        GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr,
                                         aosOptions.List()));
    ASSERT_NE(nullptr, pSouthUpTrans);
    EXPECT_EQ(CE_None,
              GDALSuggestedWarpOutput2(src_ds.get(), GDALGenImgProjTransform,
                                       pSouthUpTrans.get(), adfGTOut, &nPixels,
                                       &nLines, adfExtent, 0));
    EXPECT_EQ(100, nPixels);
    EXPECT_EQ(50, nLines);
    EXPECT_DOUBLE_EQ(-0.5, adfGTOut[5]);
  }

  // 2. Standard Reprojection: EPSG:4326 to EPSG:3857
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    src_ds->SetSpatialRef(&srs4326);
    double adfGT[6] = {2.0, 0.01, 0.0, 48.0, 0.0, -0.01};
    src_ds->SetGeoTransform(adfGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double adfGTOut[6] = {};
    int nPixels = 0;
    int nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput(
                           src_ds.get(), GDALGenImgProjTransform, pTrans.get(),
                           adfGTOut, &nPixels, &nLines));
    EXPECT_GT(nPixels, 0);
    EXPECT_GT(nLines, 0);

    EXPECT_EQ(CE_None,
              GDALSuggestedWarpOutput2(
                  src_ds.get(), GDALGenImgProjTransform, pTrans.get(), adfGTOut,
                  &nPixels, &nLines, adfExtent,
                  GDAL_SWO_ROUND_UP_SIZE | GDAL_SWO_FORCE_SQUARE_PIXEL));
    EXPECT_GT(nPixels, 0);
    EXPECT_GT(nLines, 0);
  }

  // 3. Polar Stereographic containing the North Pole
  {
    GDALDatasetUniquePtr polar_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    polar_ds->SetSpatialRef(&srs3413);
    double adfPolarGT[6] = {-500000.0, 10000.0, 0.0, 500000.0, 0.0, -10000.0};
    polar_ds->SetGeoTransform(adfPolarGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:4326");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        polar_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double adfGTOut[6] = {};
    int nPixels = 0;
    int nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None,
              GDALSuggestedWarpOutput2(polar_ds.get(), GDALGenImgProjTransform,
                                       pTrans.get(), adfGTOut, &nPixels,
                                       &nLines, adfExtent, 0));
    EXPECT_NEAR(-180.0, adfExtent[0], 1e-3);
    EXPECT_NEAR(180.0, adfExtent[2], 1e-3);
    EXPECT_NEAR(90.0, adfExtent[3], 1e-3);
  }

  // 4. Antimeridian Crossing Dataset
  {
    GDALDatasetUniquePtr pacific_ds(
        mem_driver->Create("", 100, 50, 1, GDT_Byte, nullptr));
    pacific_ds->SetSpatialRef(&srs4326);
    double adfPacificGT[6] = {175.0, 0.1, 0.0, 10.0, 0.0, -0.1};
    pacific_ds->SetGeoTransform(adfPacificGT);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:4326");
    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        pacific_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    double adfGTOut[6] = {};
    int nPixels = 0;
    int nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput2(
                           pacific_ds.get(), GDALGenImgProjTransform,
                           pTrans.get(), adfGTOut, &nPixels, &nLines, adfExtent,
                           GDAL_SWO_ROUND_UP_SIZE));
    EXPECT_GT(nPixels, 0);
    EXPECT_GT(nLines, 0);
  }

  // 5. Transformation points failing completely emits CE_Failure
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 50, 50, 1, GDT_Byte, nullptr));
    double adfGT[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    src_ds->SetGeoTransform(adfGT);

    double adfGTOut[6] = {};
    int nPixels = 0;
    int nLines = 0;
    double adfExtent[4] = {};
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALSuggestedWarpOutput2(
                              src_ds.get(), FailingTransform, nullptr, adfGTOut,
                              &nPixels, &nLines, adfExtent, 0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

// -----------------------------------------------------------------------------
// Suite 20: Mathematical Helpers & Area of Interest
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GeoTransformMathAndAreaOfInterest) {
  // 1. GDALApplyGeoTransform and GDALInvGeoTransform round-trip
  double adfGT[6] = {100.0, 2.0, 0.5, 200.0, 0.25, -1.5};
  double geo_x = 0.0;
  double geo_y = 0.0;
  GDALApplyGeoTransform(adfGT, 10.0, 20.0, &geo_x, &geo_y);
  EXPECT_DOUBLE_EQ(100.0 + 10.0 * 2.0 + 20.0 * 0.5, geo_x);
  EXPECT_DOUBLE_EQ(200.0 + 10.0 * 0.25 - 20.0 * 1.5, geo_y);

  double adfInvGT[6] = {};
  EXPECT_EQ(TRUE, GDALInvGeoTransform(adfGT, adfInvGT));
  double pixel = 0.0;
  double line = 0.0;
  GDALApplyGeoTransform(adfInvGT, geo_x, geo_y, &pixel, &line);
  EXPECT_NEAR(10.0, pixel, 1e-9);
  EXPECT_NEAR(20.0, line, 1e-9);

  // 2. Singular geotransform inversion returns FALSE
  double adfSingular[6] = {0.0, 1.0, 2.0, 0.0, 2.0, 4.0};
  EXPECT_EQ(FALSE, GDALInvGeoTransform(adfSingular, adfInvGT));

  // 3. GDALComputeAreaOfInterest with geographic SRS
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  double dfWest = 0.0, dfSouth = 0.0, dfEast = 0.0, dfNorth = 0.0;
  double adfGeoGT[6] = {10.0, 0.1, 0.0, 50.0, 0.0, -0.1};
  EXPECT_TRUE(GDALComputeAreaOfInterest(&srs4326, adfGeoGT, 100, 100, dfWest,
                                        dfSouth, dfEast, dfNorth));
  EXPECT_NEAR(10.0, dfWest, 1e-5);
  EXPECT_NEAR(40.0, dfSouth, 1e-5);
  EXPECT_NEAR(20.0, dfEast, 1e-5);
  EXPECT_NEAR(50.0, dfNorth, 1e-5);

  // Overload 2 with bounding box
  EXPECT_TRUE(GDALComputeAreaOfInterest(&srs4326, 10.0, 40.0, 20.0, 50.0,
                                        dfWest, dfSouth, dfEast, dfNorth));
  EXPECT_NEAR(10.0, dfWest, 1e-5);
  EXPECT_NEAR(40.0, dfSouth, 1e-5);
  EXPECT_NEAR(20.0, dfEast, 1e-5);
  EXPECT_NEAR(50.0, dfNorth, 1e-5);

  // Projected SRS
  OGRSpatialReference srs32631;
  srs32631.importFromEPSG(32631);
  srs32631.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  double adfUtmGT[6] = {500000.0, 10.0, 0.0, 4500000.0, 0.0, -10.0};
  EXPECT_TRUE(GDALComputeAreaOfInterest(&srs32631, adfUtmGT, 100, 100, dfWest,
                                        dfSouth, dfEast, dfNorth));
  EXPECT_LT(dfWest, dfEast);
  EXPECT_LT(dfSouth, dfNorth);

  // Empty SRS returns false
  OGRSpatialReference srs_empty;
  EXPECT_FALSE(GDALComputeAreaOfInterest(&srs_empty, adfGeoGT, 100, 100, dfWest,
                                         dfSouth, dfEast, dfNorth));
  EXPECT_FALSE(GDALComputeAreaOfInterest(&srs_empty, 10.0, 40.0, 20.0, 50.0,
                                         dfWest, dfSouth, dfEast, dfNorth));
}

// -----------------------------------------------------------------------------
// Suite 21: Approx Transformer Scanline Subdivision & Recursive Approximation
// -----------------------------------------------------------------------------

struct CurvedScanlineState {
  double quad_coeff = 0.01;
  double fail_at_x = -999.0;
};

int CurvedScanlineTransform(void* pArg, int bDstToSrc, int nPoints, double* x,
                            double* y, double* z, int* panSuccess) {
  auto* state = static_cast<CurvedScanlineState*>(pArg);
  int all_success = TRUE;
  for (int i = 0; i < nPoints; ++i) {
    if (state != nullptr && std::abs(x[i] - state->fail_at_x) < 1e-5) {
      if (panSuccess != nullptr) panSuccess[i] = FALSE;
      all_success = FALSE;
      continue;
    }
    if (panSuccess != nullptr) panSuccess[i] = TRUE;
    const double coeff = state != nullptr ? state->quad_coeff : 0.01;
    if (bDstToSrc) {
      x[i] = x[i] - 0.5 * coeff * x[i] * x[i];
      y[i] = y[i] - 0.5 * coeff * x[i] * x[i];
    } else {
      x[i] = x[i] + coeff * x[i] * x[i];
      y[i] = y[i] + coeff * x[i] * x[i];
      if (z != nullptr) z[i] += 0.5 * x[i];
    }
  }
  return all_success;
}

TEST_F(GdalTransformerTest, ApproxTransformerScanlineSubdivision) {
  CurvedScanlineState state{};

  // 1. Full recursive subdivision + linear interpolation leaf (L4151-4197,
  // L4239-4346)
  {
    ApproxTransformerUniquePtr approx(GDALCreateApproxTransformer(
        CurvedScanlineTransform, &state, /*dfMaxError=*/0.2));
    ASSERT_NE(nullptr, approx);

    constexpr int kPoints = 65;
    std::vector<double> x(kPoints), y(kPoints, 45.0), z(kPoints, 1.0);
    std::vector<int> success(kPoints, FALSE);
    for (int i = 0; i < kPoints; ++i) {
      x[i] = static_cast<double>(i);
    }
    EXPECT_EQ(TRUE, GDALApproxTransform(approx.get(), FALSE, kPoints, x.data(),
                                        y.data(), z.data(), success.data()));
    for (int s : success) EXPECT_EQ(TRUE, s);
    EXPECT_NEAR(0.0, x[0], 1e-6);
    EXPECT_NEAR(64.0 + 0.01 * 64.0 * 64.0, x[64], 1.0);

    // Reverse transform
    EXPECT_EQ(TRUE, GDALApproxTransform(approx.get(), TRUE, kPoints, x.data(),
                                        y.data(), z.data(), success.data()));
    for (int s : success) EXPECT_EQ(TRUE, s);

    // Large error threshold -> goes straight to linear approximation loop
    ApproxTransformerUniquePtr approx_loose(GDALCreateApproxTransformer(
        CurvedScanlineTransform, &state, /*dfMaxError=*/1000.0));
    ASSERT_NE(nullptr, approx_loose);
    constexpr int kFew = 17;
    std::vector<double> x_few(kFew), y_few(kFew, 45.0), z_few(kFew, 0.0);
    std::vector<int> s_few(kFew, FALSE);
    for (int i = 0; i < kFew; ++i) x_few[i] = i * 2.0;
    EXPECT_EQ(TRUE,
              GDALApproxTransform(approx_loose.get(), FALSE, kFew, x_few.data(),
                                  y_few.data(), z_few.data(), s_few.data()));
    for (int s : s_few) EXPECT_EQ(TRUE, s);
  }

  // 2. Half-1 base transform fallback (bUseBaseTransformForHalf1 == true,
  // !bUseBaseTransformForHalf2)
  {
    ApproxTransformerUniquePtr approx(
        GDALCreateApproxTransformer(CurvedScanlineTransform, &state, 0.001));
    ASSERT_NE(nullptr, approx);
    constexpr int kPts10 = 10;
    std::vector<double> x(kPts10), y(kPts10, 45.0), z(kPts10, 0.0);
    std::vector<int> success(kPts10, FALSE);
    for (int i = 0; i < kPts10; ++i) x[i] = i * 4.0;
    EXPECT_EQ(TRUE, GDALApproxTransform(approx.get(), FALSE, kPts10, x.data(),
                                        y.data(), z.data(), success.data()));
    for (int s : success) EXPECT_EQ(TRUE, s);
  }

  // 3. Half-2 base transform fallback (!bUseBaseTransformForHalf1,
  // bUseBaseTransformForHalf2 == true)
  {
    ApproxTransformerUniquePtr approx(
        GDALCreateApproxTransformer(CurvedScanlineTransform, &state, 0.001));
    ASSERT_NE(nullptr, approx);
    constexpr int kPts21 = 21;
    std::vector<double> x(kPts21), y(kPts21, 45.0), z(kPts21, 0.0);
    std::vector<int> success(kPts21, FALSE);
    for (int i = 0; i < kPts21; ++i) x[i] = i * 2.0;
    // Set midpoint of right half to differ in Y so bUseBaseTransformForHalf2 is
    // true
    y[15] = 46.0;
    EXPECT_EQ(TRUE, GDALApproxTransform(approx.get(), FALSE, kPts21, x.data(),
                                        y.data(), z.data(), success.data()));
    for (int s : success) EXPECT_EQ(TRUE, s);
  }

  // 4. Quarter-midpoint failure fallback inside GDALApproxTransformInternal
  {
    ApproxTransformerUniquePtr approx(
        GDALCreateApproxTransformer(CurvedScanlineTransform, &state, 0.001));
    ASSERT_NE(nullptr, approx);
    constexpr int kPts21 = 21;
    std::vector<double> x(kPts21), y(kPts21, 45.0), z(kPts21, 0.0);
    std::vector<int> success(kPts21, FALSE);
    for (int i = 0; i < kPts21; ++i) x[i] = i * 2.0;
    state.fail_at_x = 8.0;  // x[(10-1)/2] = x[4] = 8.0 fails in xMiddle
    EXPECT_FALSE(GDALApproxTransform(approx.get(), FALSE, kPts21, x.data(),
                                     y.data(), z.data(), success.data()));
    EXPECT_EQ(FALSE, success[4]);
    state.fail_at_x = -999.0;
  }

  // 5. Initial SME failure fallback in GDALApproxTransform
  {
    ApproxTransformerUniquePtr approx(
        GDALCreateApproxTransformer(CurvedScanlineTransform, &state, 0.001));
    ASSERT_NE(nullptr, approx);
    constexpr int kPts21 = 21;
    std::vector<double> x(kPts21), y(kPts21, 45.0), z(kPts21, 0.0);
    std::vector<int> success(kPts21, FALSE);
    for (int i = 0; i < kPts21; ++i) x[i] = i * 2.0;
    state.fail_at_x = 20.0;  // x[nMiddle] = x[10] = 20.0 fails in initial SME
    EXPECT_FALSE(GDALApproxTransform(approx.get(), FALSE, kPts21, x.data(),
                                     y.data(), z.data(), success.data()));
    EXPECT_EQ(FALSE, success[10]);
    state.fail_at_x = -999.0;
  }

  // 6. Unequal forward/reverse error XML round-trip &
  // GDALRefreshApproxTransformer
  {
    OGRSpatialReference srs4326, srs3857;
    srs4326.importFromEPSG(4326);
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    double adfGT[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    GenImgProjTransformerUniquePtr pGen(GDALCreateGenImgProjTransformer4(
        OGRSpatialReference::ToHandle(&srs4326), adfGT,
        OGRSpatialReference::ToHandle(&srs3857), adfGT, nullptr));
    ASSERT_NE(nullptr, pGen);

    ApproxTransformerUniquePtr approx(GDALCreateApproxTransformer(
        GDALGenImgProjTransform, pGen.get(), 0.125));
    ASSERT_NE(nullptr, approx);
    GDALApproxTransformerOwnsSubtransformer(approx.get(), TRUE);
    pGen.release();

    CPLXMLNodeUniquePtr xml(
        GDALSerializeTransformer(GDALApproxTransform, approx.get()));
    ASSERT_NE(nullptr, xml);
    CPLXMLNode* psMaxError = CPLGetXMLNode(xml.get(), "MaxError");
    ASSERT_NE(nullptr, psMaxError);
    CPLRemoveXMLChild(xml.get(), psMaxError);
    CPLDestroyXMLNode(psMaxError);
    CPLCreateXMLElementAndValue(xml.get(), "MaxErrorForward", "0.125");
    CPLCreateXMLElementAndValue(xml.get(), "MaxErrorReverse", "0.375");
    EXPECT_NE(nullptr, CPLGetXMLNode(xml.get(), "MaxErrorForward"));
    EXPECT_NE(nullptr, CPLGetXMLNode(xml.get(), "MaxErrorReverse"));

    GDALTransformerFunc pfnFunc = nullptr;
    void* pDeserialized = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xml.get(), &pfnFunc, &pDeserialized));
    ASSERT_NE(nullptr, pDeserialized);
    TransformerUniquePtr cleanup(pDeserialized);
    EXPECT_EQ(GDALApproxTransform, pfnFunc);

    CPLConfigOptionSetter setter("CHECK_WITH_INVERT_PROJ", "YES", false);
    GDALRefreshApproxTransformer(approx.get());
  }
}

// -----------------------------------------------------------------------------
// Suite 22: Suggested Warp Output Special Branches & Discontinuities
// -----------------------------------------------------------------------------

int AntimeridianEdgeTransform(void* /*pTransformArg*/, int bDstToSrc,
                              int nPointCount, double* x, double* y,
                              double* /*z*/, int* panSuccess) {
  for (int i = 0; i < nPointCount; ++i) {
    if (panSuccess != nullptr) panSuccess[i] = TRUE;
    if (!bDstToSrc) {
      const double lon = 170.0 + 0.1 * x[i];
      if (std::abs(lon - 180.0) < 1e-9) {
        x[i] = -180.0;
      } else {
        x[i] = lon;
      }
      y[i] = 20.0 - 0.1 * y[i];
    } else {
      const double lon = (std::abs(x[i] - (-180.0)) < 1e-7) ? 180.0 : x[i];
      x[i] = (lon - 170.0) / 0.1;
      y[i] = (20.0 - y[i]) / 0.1;
    }
  }
  return TRUE;
}

int DichotomicSearchTransform(void* /*pTransformArg*/, int bDstToSrc,
                              int nPointCount, double* x, double* y,
                              double* /*z*/, int* panSuccess) {
  for (int i = 0; i < nPointCount; ++i) {
    if (!bDstToSrc) {
      if (x[i] < 25.0) {
        if (panSuccess != nullptr) panSuccess[i] = FALSE;
      } else {
        if (panSuccess != nullptr) panSuccess[i] = TRUE;
        x[i] = x[i] - 50.0;
        y[i] = 50.0 - y[i];
      }
    } else {
      if (panSuccess != nullptr) panSuccess[i] = TRUE;
      x[i] = x[i] + 50.0;
      y[i] = 50.0 - y[i];
    }
  }
  return TRUE;
}

int HugeDimensionsTransform(void* pTransformArg, int bDstToSrc, int nPointCount,
                            double* x, double* y, double* /*z*/,
                            int* panSuccess) {
  const bool overflow_both = pTransformArg != nullptr;
  const double scale = overflow_both ? 1.0e-312 : 0.1;
  for (int i = 0; i < nPointCount; ++i) {
    if (panSuccess != nullptr) panSuccess[i] = TRUE;
    if (!bDstToSrc) {
      if (std::abs(x[i] - 5.0) < 0.4 && std::abs(y[i] - 0.0) < 0.4) {
        x[i] = overflow_both ? 100.0 : 1.0e10;
        y[i] = overflow_both ? 200.0 : 2.0e10;
      } else {
        x[i] = scale * x[i];
        y[i] = -scale * y[i];
      }
    } else {
      if (x[i] > 50.0) {
        x[i] = 5.0;
        y[i] = 0.0;
      } else {
        x[i] = x[i] / scale;
        y[i] = -y[i] / scale;
      }
    }
  }
  return TRUE;
}

TEST_F(GdalTransformerTest, SuggestedWarpOutputSpecialBranches) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // 1. Geolocation fast-path in GDALSuggestedWarpOutput2 (L860-926)
  {
    constexpr char kVsiDir[] = "/vsimem/gdaltransformer_test/swo_geoloc";
    constexpr char kGeolocPath[] =
        "/vsimem/gdaltransformer_test/swo_geoloc/geoloc.tif";
    absl::Cleanup dir_cleanup = [kVsiDir] { VSIRmdirRecursive(kVsiDir); };
    CPLSetConfigOption("CPL_TMPDIR", kVsiDir);
    absl::Cleanup tmpdir_cleanup = [] {
      CPLSetConfigOption("CPL_TMPDIR", nullptr);
    };

    ASSERT_TRUE(WriteGeolocTiff(kGeolocPath, 10, 10));

    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src_ds);

    // 1a. Without reprojection
    {
      CPLStringList aosOptions;
      aosOptions.SetNameValue("SRC_GEOLOC_ARRAY", kGeolocPath);
      aosOptions.SetNameValue("GEOLOC_USE_TEMP_DATASETS", "NO");
      GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
          src_ds.get(), nullptr, aosOptions.List()));
      ASSERT_NE(nullptr, pTrans);

      double adfGTOut[6] = {};
      int nPixels = 0, nLines = 0;
      double adfExtent[4] = {};
      EXPECT_EQ(CE_None,
                GDALSuggestedWarpOutput2(src_ds.get(), GDALGenImgProjTransform,
                                         pTrans.get(), adfGTOut, &nPixels,
                                         &nLines, adfExtent, 0));
      EXPECT_GT(nPixels, 0);
      EXPECT_GT(nLines, 0);
    }

    // 1b. With reprojection to EPSG:3857
    {
      CPLStringList aosOptions;
      aosOptions.SetNameValue("SRC_GEOLOC_ARRAY", kGeolocPath);
      aosOptions.SetNameValue("GEOLOC_USE_TEMP_DATASETS", "NO");
      aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
      GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
          src_ds.get(), nullptr, aosOptions.List()));
      ASSERT_NE(nullptr, pTrans);

      double adfGTOut[6] = {};
      int nPixels = 0, nLines = 0;
      double adfExtent[4] = {};
      EXPECT_EQ(CE_None,
                GDALSuggestedWarpOutput2(src_ds.get(), GDALGenImgProjTransform,
                                         pTrans.get(), adfGTOut, &nPixels,
                                         &nLines, adfExtent, 0));
      EXPECT_GT(nPixels, 0);
      EXPECT_GT(nLines, 0);
    }
  }

  // 2. Orthographic polar projection with CHECK_WITH_INVERT_PROJ=YES
  // (L958-1030)
  {
    OGRSpatialReference srs_ortho_n;
    srs_ortho_n.importFromProj4(
        "+proj=ortho +lat_0=90 +lon_0=10 +datum=WGS84 +units=m +no_defs");
    srs_ortho_n.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    GDALDatasetUniquePtr north_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    north_ds->SetSpatialRef(&srs_ortho_n);
    double adfGT[6] = {-500000.0, 10000.0, 0.0, 500000.0, 0.0, -10000.0};
    north_ds->SetGeoTransform(adfGT);

    CPLConfigOptionSetter setter("CHECK_WITH_INVERT_PROJ", "YES", false);
    CPLStringList aosOptions;
    aosOptions.SetNameValue("DST_SRS", "EPSG:4326");
    GenImgProjTransformerUniquePtr pTransN(GDALCreateGenImgProjTransformer2(
        north_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTransN);

    double adfGTOut[6] = {};
    int nPixels = 0, nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None,
              GDALSuggestedWarpOutput2(north_ds.get(), GDALGenImgProjTransform,
                                       pTransN.get(), adfGTOut, &nPixels,
                                       &nLines, adfExtent, 0));
    EXPECT_LT(adfExtent[0], -80.0);
    EXPECT_GT(adfExtent[2], 100.0);
    EXPECT_NEAR(90.0, adfExtent[3], 1e-1);

    // South Pole polar stereographic (covers non-orthographic pole branch
    // L1006-1012)
    OGRSpatialReference srs_polar_s;
    srs_polar_s.importFromProj4(
        "+proj=stere +lat_0=-90 +lat_ts=-71 +lon_0=0 +datum=WGS84 +units=m "
        "+no_defs");
    srs_polar_s.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    GDALDatasetUniquePtr south_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    south_ds->SetSpatialRef(&srs_polar_s);
    south_ds->SetGeoTransform(adfGT);
    GenImgProjTransformerUniquePtr pTransS(GDALCreateGenImgProjTransformer2(
        south_ds.get(), nullptr, aosOptions.List()));
    ASSERT_NE(nullptr, pTransS);
    EXPECT_EQ(CE_None,
              GDALSuggestedWarpOutput2(south_ds.get(), GDALGenImgProjTransform,
                                       pTransS.get(), adfGTOut, &nPixels,
                                       &nLines, adfExtent, 0));
    EXPECT_NEAR(-180.0, adfExtent[0], 1e-1);
    EXPECT_NEAR(-88.59, adfExtent[1], 0.2);
    EXPECT_NEAR(180.0, adfExtent[2], 1e-1);
  }

  // 3. Antimeridian -180.0 -> +180.0 discontinuity fixup (L551-607)
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 50, 1, GDT_Byte, nullptr));
    double adfGTOut[6] = {};
    int nPixels = 0, nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput2(
                           src_ds.get(), AntimeridianEdgeTransform, nullptr,
                           adfGTOut, &nPixels, &nLines, adfExtent, 0));
    EXPECT_GT(nPixels, 0);
    EXPECT_GT(nLines, 0);
  }

  // 4. Partial failure + dichotomic search (L677-810)
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 50, 50, 1, GDT_Byte, nullptr));
    double adfGTOut[6] = {};
    int nPixels = 0, nLines = 0;
    double adfExtent[4] = {};
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput2(
                           src_ds.get(), DichotomicSearchTransform, nullptr,
                           adfGTOut, &nPixels, &nLines, adfExtent, 0));
    EXPECT_GT(nPixels, 0);
    EXPECT_GT(nLines, 0);
  }

  // 5. Huge output dimensions clamping and error (L1107-1131)
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 10, 10, 1, GDT_Byte, nullptr));
    double adfGTOut[6] = {};
    int nPixels = 0, nLines = 0;
    double adfExtent[4] = {};

    // 5a. Clamping to INT_MAX - 1 (L1113-1118)
    EXPECT_EQ(CE_None, GDALSuggestedWarpOutput2(
                           src_ds.get(), HugeDimensionsTransform, nullptr,
                           adfGTOut, &nPixels, &nLines, adfExtent, 0));
    EXPECT_EQ(std::numeric_limits<int>::max() - 1, nLines);

    // 5b. Overflowing both dimensions -> CE_Failure (L1120-1131)
    int overflow_flag = 1;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALSuggestedWarpOutput2(src_ds.get(), HugeDimensionsTransform,
                                       &overflow_flag, adfGTOut, &nPixels,
                                       &nLines, adfExtent, 0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Computed dimensions are too big"));
  }
}

// -----------------------------------------------------------------------------
// Suite 23: GenImgProj Options, Deserialization & Output Geotransform
// -----------------------------------------------------------------------------

TEST_F(GdalTransformerTest, GenImgProjOptionsCloneAndDstGeoTransform) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  OGRSpatialReference srs4326, srs3857;
  srs4326.importFromEPSG(4326);
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. GDALCreateSimilarTransformer with rotated geotransform (L1315-1331)
  {
    double adfRotGT[6] = {10.0, 1.0, 0.2, 20.0, 0.1, -1.0};
    GenImgProjTransformerUniquePtr pRot(GDALCreateGenImgProjTransformer4(
        OGRSpatialReference::ToHandle(&srs4326), adfRotGT,
        OGRSpatialReference::ToHandle(&srs4326), adfRotGT, nullptr));
    ASSERT_NE(nullptr, pRot);

    TransformerUniquePtr pSimilar(
        GDALCreateSimilarTransformer(pRot.get(), 2.0, 2.0));
    EXPECT_NE(nullptr, pSimilar);

    // Singular ratio -> cannot invert geotransform
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(pRot.get(), 0.0, 0.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. GDALCreateGenImgProjTransformer2 approximation, epoch, and pipeline
  // options (L2605-2787)
  {
    GDALDatasetUniquePtr src_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src_ds);

    GDAL_GCP asGCPs[4];
    GDALInitGCPs(4, asGCPs);
    absl::Cleanup gcp_cleanup = [&asGCPs] { GDALDeinitGCPs(4, asGCPs); };
    asGCPs[0].dfGCPPixel = 0.0;
    asGCPs[0].dfGCPLine = 0.0;
    asGCPs[0].dfGCPX = 2.0;
    asGCPs[0].dfGCPY = 48.0;
    asGCPs[1].dfGCPPixel = 100.0;
    asGCPs[1].dfGCPLine = 0.0;
    asGCPs[1].dfGCPX = 3.0;
    asGCPs[1].dfGCPY = 48.0;
    asGCPs[2].dfGCPPixel = 0.0;
    asGCPs[2].dfGCPLine = 100.0;
    asGCPs[2].dfGCPX = 2.0;
    asGCPs[2].dfGCPY = 49.0;
    asGCPs[3].dfGCPPixel = 100.0;
    asGCPs[3].dfGCPLine = 100.0;
    asGCPs[3].dfGCPX = 3.0;
    asGCPs[3].dfGCPY = 49.0;
    src_ds->SetGCPs(4, asGCPs, &srs4326);

    GDALDatasetUniquePtr dst_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, dst_ds);
    dst_ds->SetGCPs(4, asGCPs, &srs4326);

    CPLStringList aosOptions;
    aosOptions.SetNameValue("SRC_METHOD", "GCP_POLYNOMIAL");
    aosOptions.SetNameValue("DST_METHOD", "GCP_POLYNOMIAL");
    aosOptions.SetNameValue("SRC_APPROX_ERROR_IN_SRS_UNIT", "0.01");
    aosOptions.SetNameValue("SRC_APPROX_ERROR_IN_PIXEL", "0.1");
    aosOptions.SetNameValue("DST_SRS", "EPSG:3857");
    aosOptions.SetNameValue("REPROJECTION_APPROX_ERROR_IN_DST_SRS_UNIT",
                            "10.0");
    aosOptions.SetNameValue("REPROJECTION_APPROX_ERROR_IN_SRC_SRS_UNIT",
                            "0.001");
    aosOptions.SetNameValue("PROMOTE_TO_3D", "YES");
    aosOptions.SetNameValue("SRC_COORDINATE_EPOCH", "2020.0");
    aosOptions.SetNameValue("DST_COORDINATE_EPOCH", "2025.0");

    GenImgProjTransformerUniquePtr pTrans(GDALCreateGenImgProjTransformer2(
        src_ds.get(), dst_ds.get(), aosOptions.List()));
    ASSERT_NE(nullptr, pTrans);

    // Serialize and deserialize round-trip (SrcApproxTransformer +
    // DstGCPTransformer)
    CPLXMLNodeUniquePtr xml(
        GDALSerializeTransformer(GDALGenImgProjTransform, pTrans.get()));
    ASSERT_NE(nullptr, xml);
    GDALTransformerFunc pfnFunc = nullptr;
    void* pDeserialized = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xml.get(), &pfnFunc, &pDeserialized));
    ASSERT_NE(nullptr, pDeserialized);
    TransformerUniquePtr cleanup(pDeserialized);

    // Custom coordinate operation pipeline
    CPLStringList aosCO;
    aosCO.SetNameValue(
        "COORDINATE_OPERATION",
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad");
    GenImgProjTransformerUniquePtr pTransCO(
        GDALCreateGenImgProjTransformer2(src_ds.get(), nullptr, aosCO.List()));
    EXPECT_NE(nullptr, pTransCO);

    // Authority-compliant SRS without traditional GIS axis order triggers
    // std::swap(*pdfX, *pdfY)
    OGRSpatialReference srs_auth;
    srs_auth.importFromEPSG(4326);
    char* pszWktAuth = nullptr;
    srs_auth.exportToWkt(&pszWktAuth);
    std::unique_ptr<char, CplFreeDeleter> auth_cleanup(pszWktAuth);
    CPLStringList aosAuth;
    aosAuth.SetNameValue("SRC_SRS", pszWktAuth);
    aosAuth.SetNameValue("DST_SRS", "EPSG:3857");
    double adfAuthGT[6] = {2.0, 0.01, 0.0, 48.0, 0.0, -0.01};
    GDALDatasetUniquePtr auth_ds(
        mem_driver->Create("", 100, 100, 1, GDT_Byte, nullptr));
    auth_ds->SetGeoTransform(adfAuthGT);
    GenImgProjTransformerUniquePtr pTransAuth(GDALCreateGenImgProjTransformer2(
        auth_ds.get(), nullptr, aosAuth.List()));
    ASSERT_NE(nullptr, pTransAuth);
    double lon = 2.5, lat = 48.5;
    EXPECT_EQ(TRUE, GDALTransformLonLatToDestGenImgProjTransformer(
                        pTransAuth.get(), &lon, &lat));
  }

  // 3. Direct GDALDeserializeTransformer dispatch & GDALCloneTransformer XML
  // fallback (L4728-4757, L4861-4882)
  {
    GDAL_GCP asGCPs[4];
    GDALInitGCPs(4, asGCPs);
    absl::Cleanup gcp_cleanup = [&asGCPs] { GDALDeinitGCPs(4, asGCPs); };
    for (int i = 0; i < 4; ++i) {
      asGCPs[i].dfGCPPixel = (i % 2) * 100.0;
      asGCPs[i].dfGCPLine = (i / 2) * 100.0;
      asGCPs[i].dfGCPX = 10.0 + (i % 2) * 1.0;
      asGCPs[i].dfGCPY = 20.0 + (i / 2) * 1.0;
    }

    // 3a. GCPTransformer serialize, deserialize, and XML clone
    TransformerUniquePtr pGCPTrans(
        GDALCreateGCPTransformer(4, asGCPs, 1, FALSE));
    ASSERT_NE(nullptr, pGCPTrans);
    CPLXMLNodeUniquePtr xmlGCP(
        GDALSerializeTransformer(GDALGCPTransform, pGCPTrans.get()));
    ASSERT_NE(nullptr, xmlGCP);

    GDALTransformerFunc pfnGCP = nullptr;
    void* pDeserGCPRaw = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xmlGCP.get(), &pfnGCP, &pDeserGCPRaw));
    ASSERT_NE(nullptr, pDeserGCPRaw);
    TransformerUniquePtr pDeserGCP(pDeserGCPRaw);
    EXPECT_EQ(GDALGCPTransform, pfnGCP);

    TransformerUniquePtr pClonedGCP(GDALCloneTransformer(pGCPTrans.get()));
    ASSERT_NE(nullptr, pClonedGCP);

    // 3b. TPSTransformer serialize and deserialize
    TransformerUniquePtr pTPSTrans(GDALCreateTPSTransformer(4, asGCPs, FALSE));
    ASSERT_NE(nullptr, pTPSTrans);
    CPLXMLNodeUniquePtr xmlTPS(
        GDALSerializeTransformer(GDALTPSTransform, pTPSTrans.get()));
    ASSERT_NE(nullptr, xmlTPS);
    GDALTransformerFunc pfnTPS = nullptr;
    void* pDeserTPSRaw = nullptr;
    EXPECT_EQ(CE_None,
              GDALDeserializeTransformer(xmlTPS.get(), &pfnTPS, &pDeserTPSRaw));
    ASSERT_NE(nullptr, pDeserTPSRaw);
    TransformerUniquePtr pDeserTPS(pDeserTPSRaw);
    EXPECT_EQ(GDALTPSTransform, pfnTPS);

    // 3c. HomographyTransformer serialize and deserialize
    double adfH[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    TransformerUniquePtr pHomoTrans(GDALCreateHomographyTransformer(adfH));
    ASSERT_NE(nullptr, pHomoTrans);
    CPLXMLNodeUniquePtr xmlHomo(
        GDALSerializeTransformer(GDALHomographyTransform, pHomoTrans.get()));
    ASSERT_NE(nullptr, xmlHomo);
    GDALTransformerFunc pfnHomo = nullptr;
    void* pDeserHomoRaw = nullptr;
    EXPECT_EQ(CE_None, GDALDeserializeTransformer(xmlHomo.get(), &pfnHomo,
                                                  &pDeserHomoRaw));
    ASSERT_NE(nullptr, pDeserHomoRaw);
    TransformerUniquePtr pDeserHomo(pDeserHomoRaw);
    EXPECT_EQ(GDALHomographyTransform, pfnHomo);

    // 3d. Clone non-serializable custom transformer emits error
    void* pCustom = DummyCustomDeserialize(nullptr);
    ASSERT_NE(nullptr, pCustom);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, GDALCloneTransformer(pCustom));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    GDALDestroyTransformer(pCustom);
  }

  // 4. GDALSetTransformerDstGeoTransform & GDALGetTransformerDstGeoTransform
  // (L4917-5022)
  {
    double adfSrcGT[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    double adfDstGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
    GenImgProjTransformerUniquePtr pGen(GDALCreateGenImgProjTransformer4(
        OGRSpatialReference::ToHandle(&srs4326), adfSrcGT,
        OGRSpatialReference::ToHandle(&srs3857), adfDstGT, nullptr));
    ASSERT_NE(nullptr, pGen);

    double adfNewGT[6] = {500.0, 5.0, 0.0, 600.0, 0.0, -5.0};
    GDALSetTransformerDstGeoTransform(pGen.get(), adfNewGT);

    double adfReadGT[6] = {};
    GDALGetTransformerDstGeoTransform(pGen.get(), adfReadGT);
    EXPECT_DOUBLE_EQ(500.0, adfReadGT[0]);
    EXPECT_DOUBLE_EQ(5.0, adfReadGT[1]);

    // Wrapped in ApproxTransformer
    ApproxTransformerUniquePtr approx(
        GDALCreateApproxTransformer(GDALGenImgProjTransform, pGen.get(), 0.1));
    ASSERT_NE(nullptr, approx);
    double adfNewGT2[6] = {700.0, 10.0, 0.0, 800.0, 0.0, -10.0};
    GDALSetTransformerDstGeoTransform(approx.get(), adfNewGT2);
    double adfReadGT2[6] = {};
    GDALGetTransformerDstGeoTransform(approx.get(), adfReadGT2);
    EXPECT_DOUBLE_EQ(700.0, adfReadGT2[0]);
    EXPECT_DOUBLE_EQ(10.0, adfReadGT2[1]);

    // On ReprojectionTransformer (not GenImgProj)
    ReprojectionTransformerUniquePtr pReproj(
        GDALCreateReprojectionTransformerEx(
            OGRSpatialReference::ToHandle(&srs4326),
            OGRSpatialReference::ToHandle(&srs3857), nullptr));
    ASSERT_NE(nullptr, pReproj);
    GDALSetTransformerDstGeoTransform(pReproj.get(), adfNewGT2);

    // On nullptr or non-GTI2 object under WithQuietHandler
    alignas(GDALTransformerInfo) char dummy[sizeof(GDALTransformerInfo)] = {};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALSetTransformerDstGeoTransform(dummy, adfNewGT2);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Transformer XML deserialization invalid nodes and
// non-invertible edge cases.
