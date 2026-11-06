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
// Tests vector rasterization functions (alg/gdalrasterize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalrasterize.cpp

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

GDALDatasetUniquePtr CreateMemRaster(int width, int height, int bands,
                                     GDALDataType type) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_drv == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_drv->Create("", width, height, bands, type, nullptr));
}

GDALDatasetUniquePtr CreateMemVector() {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_drv == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
}

template <typename T>
std::vector<T> ReadBandPixels(GDALRasterBand* band) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  std::vector<T> buffer(static_cast<size_t>(x_size) * y_size);
  const CPLErr err =
      band->RasterIO(GF_Read, 0, 0, x_size, y_size, buffer.data(), x_size,
                     y_size, band->GetRasterDataType(), 0, 0, nullptr);
  EXPECT_EQ(CE_None, err);
  return buffer;
}

std::vector<double> ReadBandAsDouble(GDALRasterBand* band) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  std::vector<double> buffer(static_cast<size_t>(x_size) * y_size, 0.0);
  const CPLErr err =
      band->RasterIO(GF_Read, 0, 0, x_size, y_size, buffer.data(), x_size,
                     y_size, GDT_Float64, 0, 0, nullptr);
  EXPECT_EQ(CE_None, err);
  return buffer;
}

std::vector<int64_t> ReadBandAsInt64(GDALRasterBand* band) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  std::vector<int64_t> buffer(static_cast<size_t>(x_size) * y_size, 0);
  const CPLErr err =
      band->RasterIO(GF_Read, 0, 0, x_size, y_size, buffer.data(), x_size,
                     y_size, GDT_Int64, 0, 0, nullptr);
  EXPECT_EQ(CE_None, err);
  return buffer;
}

int DummyProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                  void* /*pData*/) {
  return TRUE;
}

int CancelProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                   void* pData) {
  if (pData != nullptr) {
    int* counter = static_cast<int*>(pData);
    (*counter)++;
    if (*counter >= 1) {
      return FALSE;
    }
  }
  return FALSE;
}

int TranslateTransformer(void* pTransformArg, int /*bDstToSrc*/,
                         int nPointCount, double* padfX, double* padfY,
                         double* /*padfZ*/, int* panSuccess) {
  const double offset =
      pTransformArg != nullptr ? *static_cast<double*>(pTransformArg) : 0.0;
  for (int i = 0; i < nPointCount; ++i) {
    padfX[i] += offset;
    padfY[i] += offset;
    if (panSuccess != nullptr) {
      panSuccess[i] = TRUE;
    }
  }
  return TRUE;
}

int FailingTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                       int nPointCount, double* /*padfX*/, double* /*padfY*/,
                       double* /*padfZ*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = FALSE;
    }
  }
  return FALSE;
}

OGRGeometryUniquePtr MakePolygon(double min_x, double min_y, double max_x,
                                 double max_y, bool clockwise = true) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  if (clockwise) {
    ring->addPoint(min_x, min_y);
    ring->addPoint(min_x, max_y);
    ring->addPoint(max_x, max_y);
    ring->addPoint(max_x, min_y);
    ring->addPoint(min_x, min_y);
  } else {
    ring->addPoint(min_x, min_y);
    ring->addPoint(max_x, min_y);
    ring->addPoint(max_x, max_y);
    ring->addPoint(min_x, max_y);
    ring->addPoint(min_x, min_y);
  }
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(ring.release()));
  return OGRGeometryUniquePtr(poly.release());
}

OGRGeometryUniquePtr MakePolygon3D(double min_x, double min_y, double max_x,
                                   double max_y, double z_val) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(min_x, min_y, z_val);
  ring->addPoint(min_x, max_y, z_val);
  ring->addPoint(max_x, max_y, z_val);
  ring->addPoint(max_x, min_y, z_val);
  ring->addPoint(min_x, min_y, z_val);
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(ring.release()));
  return OGRGeometryUniquePtr(poly.release());
}

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------

class GdalRasterizeTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// ---------------------------------------------------------------------------
// Options parsing tests
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, OptionsInvalidBurnValueFrom) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  double burn_values[] = {10.0};

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "INVALID");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_values,
                                    options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unrecognized value 'INVALID' for BURN_VALUE_FROM"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, OptionsInvalidMergeAlg) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  double burn_values[] = {10.0};

  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "UNKNOWN");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_values,
                                    options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unrecognized value 'UNKNOWN' for MERGE_ALG"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, OptionsInvalidOptim) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  double burn_values[] = {10.0};

  CPLStringList options;
  options.AddNameValue("OPTIM", "SUPERFAST");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_values,
                                    options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unrecognized value 'SUPERFAST' for OPTIM"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, OptionsOptimNotSupportedOnLayers) {
  GDALDatasetUniquePtr raster_ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, raster_ds);

  GDALDatasetUniquePtr vector_ds = CreateMemVector();
  ASSERT_NE(nullptr, vector_ds);
  OGRLayer* layer =
      vector_ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(point.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  int bands[] = {1};
  double burn_values[] = {50.0};

  CPLStringList options;
  options.AddNameValue("OPTIM", "RASTER");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                bands, 1, layers, nullptr, nullptr, burn_values,
                                options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Option OPTIM is not supported by this function"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, OptionsOptimNotSupportedOnLayersBuf) {
  GDALDatasetUniquePtr vector_ds = CreateMemVector();
  ASSERT_NE(nullptr, vector_ds);
  OGRLayer* layer =
      vector_ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(point.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  std::vector<GByte> buffer(16, 0);

  CPLStringList options;
  options.AddNameValue("OPTIM", "VECTOR");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALRasterizeLayersBuf(buffer.data(), 4, 4, GDT_Byte, 0, 0,
                                            1, layers, nullptr, nullptr,
                                            TranslateTransformer, nullptr, 25.0,
                                            options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Option OPTIM is not supported by this function"));
  CPLErrorReset();
}

// ---------------------------------------------------------------------------
// Argument validation and error handling
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, NullDatasetValidation) {
  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  double burn_values[] = {1.0};
  int64_t int64_burn_values[] = {1};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALRasterizeGeometries(nullptr, 1, bands, 1, geoms,
                                                nullptr, nullptr, burn_values,
                                                nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALRasterizeGeometriesInt64(
                            nullptr, 1, bands, 1, geoms, nullptr, nullptr,
                            int64_burn_values, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  OGRLayerH layers[] = {nullptr};
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayers(nullptr, 1, bands, 1, layers, nullptr, nullptr,
                                burn_values, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, ZeroCountsEarlyReturn) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  double burn_values[] = {1.0};
  int64_t int64_burn_values[] = {1};

  int progress_calls = 0;
  auto progress_cb = [](double dfComplete, const char* /*pszMessage*/,
                        void* pData) -> int {
    if (pData != nullptr) {
      *static_cast<int*>(pData) += 1;
    }
    EXPECT_DOUBLE_EQ(1.0, dfComplete);
    return TRUE;
  };

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 0, bands,
                                    1, geoms, nullptr, nullptr, burn_values,
                                    nullptr, progress_cb, &progress_calls));
  EXPECT_EQ(1, progress_calls);

  progress_calls = 0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    0, geoms, nullptr, nullptr, burn_values,
                                    nullptr, progress_cb, &progress_calls));
  EXPECT_EQ(1, progress_calls);

  progress_calls = 0;
  EXPECT_EQ(CE_None, GDALRasterizeGeometriesInt64(
                         GDALDataset::ToHandle(ds.get()), 0, bands, 1, geoms,
                         nullptr, nullptr, int64_burn_values, nullptr,
                         progress_cb, &progress_calls));
  EXPECT_EQ(1, progress_calls);

  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(ds.get()), 0, bands, 1,
                                nullptr, nullptr, nullptr, burn_values, nullptr,
                                nullptr, nullptr));

  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(ds.get()), 1, bands, 0,
                                nullptr, nullptr, nullptr, burn_values, nullptr,
                                nullptr, nullptr));

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(nullptr, 4, 4, GDT_Byte, 0, 0, 0, nullptr,
                                   nullptr, nullptr, nullptr, nullptr, 1.0,
                                   nullptr, nullptr, nullptr));
}

TEST_F(GdalRasterizeTest, InvalidBandList) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int invalid_bands[] = {99};
  double burn_values[] = {1.0};
  int64_t int64_burn_values[] = {1};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1,
                                    invalid_bands, 1, geoms, nullptr, nullptr,
                                    burn_values, nullptr, nullptr, nullptr));

  EXPECT_EQ(CE_Failure, GDALRasterizeGeometriesInt64(
                            GDALDataset::ToHandle(ds.get()), 1, invalid_bands,
                            1, geoms, nullptr, nullptr, int64_burn_values,
                            nullptr, nullptr, nullptr));

  OGRLayerH layers[] = {nullptr};
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayers(GDALDataset::ToHandle(ds.get()), 1,
                                invalid_bands, 1, layers, nullptr, nullptr,
                                burn_values, nullptr, nullptr, nullptr));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, Int64OnNonInt64RasterFails) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto point = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(point.get())};
  int bands[] = {1};
  int64_t int64_burn_values[] = {100};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, int64_burn_values, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("GDALRasterizeGeometriesInt64() only supported on "
                        "Int64 raster"));
  CPLErrorReset();
}

// ---------------------------------------------------------------------------
// GDALRasterizeLayersBuf parameter checks
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, LayersBufUnsupportedDataType) {
  std::vector<GByte> buf(16, 0);
  OGRLayerH layers[] = {nullptr};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Unknown, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   1.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unsupported data type"));
  CPLErrorReset();

  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_CInt16, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   1.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_TypeCount, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   1.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, LayersBufInvalidPixelAndLineSpace) {
  std::vector<double> buf(16, 0.0);
  OGRLayerH layers[] = {nullptr};

  WithQuietHandler quiet;
  CPLErrorReset();
  // Pixel space 4 is less than sizeof(double)=8.
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Float64, 4, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   1.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("unsupported value of nPixelSpace"));
  CPLErrorReset();

  // Line space 10 is less than nPixelSpace * nBufXSize = 8 * 4 = 32.
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Float64, 8, 10, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   1.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("unsupported value of nLineSpace"));
  CPLErrorReset();
}

// ---------------------------------------------------------------------------
// Point and MultiPoint Geometries
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, PointUserBurnValueReplaceAndAdd) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  double burn_vals[] = {40.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  // Pixel at (x=1, y=2) -> index 2*4 + 1 = 9.
  EXPECT_EQ(40, pixels[9]);
  EXPECT_EQ(0, pixels[0]);

  // Now test MERGE_ALG=ADD.
  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  burn_vals[0] = 15.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(55, pixels[9]);
}

TEST_F(GdalRasterizeTest, Point3DBurnValueFromZ) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto pt = std::make_unique<OGRPoint>(2.0, 1.0, 35.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  double burn_vals[] = {10.0};

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "Z");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  // (x=2, y=1) -> index 1*4 + 2 = 6. Value is 10 + 35 = 45.0.
  EXPECT_DOUBLE_EQ(45.0, pixels[6]);
}

TEST_F(GdalRasterizeTest, MultiPointBasicAnd3D) {
  GDALDatasetUniquePtr ds = CreateMemRaster(5, 5, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto mp = std::make_unique<OGRMultiPoint>();
  auto pt1 = std::make_unique<OGRPoint>(0.0, 0.0, 5.0);
  auto pt2 = std::make_unique<OGRPoint>(3.0, 3.0, 15.0);
  EXPECT_EQ(OGRERR_NONE, mp->addGeometryDirectly(pt1.release()));
  EXPECT_EQ(OGRERR_NONE, mp->addGeometryDirectly(pt2.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(mp.get())};
  int bands[] = {1};
  double burn_vals[] = {2.0};

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "Z");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  // (0,0) -> 2.0 + 5.0 = 7.0
  EXPECT_DOUBLE_EQ(7.0, pixels[0]);
  // (3,3) -> 2.0 + 15.0 = 17.0
  EXPECT_DOUBLE_EQ(17.0, pixels[3 * 5 + 3]);
}

// ---------------------------------------------------------------------------
// LineString and MultiLineString Geometries
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, LineStringAllTouchedVariations) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.0, 0.0);
  line->addPoint(3.0, 3.0);

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(line.get())};
  int bands[] = {1};
  double burn_vals[] = {80.0};

  // 1. ALL_TOUCHED=FALSE (default Bresenham)
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(80, pixels[0]);
  EXPECT_EQ(80, pixels[3 * 4 + 3]);

  // 2. ALL_TOUCHED=TRUE
  CPLStringList options;
  options.AddNameValue("ALL_TOUCHED", "TRUE");
  burn_vals[0] = 99.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(99, pixels[0]);
  EXPECT_EQ(99, pixels[1 * 4 + 1]);
}

TEST_F(GdalRasterizeTest, LineString3DBurnValueFromZ) {
  GDALDatasetUniquePtr ds = CreateMemRaster(5, 5, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.0, 1.0, 10.0);
  line->addPoint(4.0, 1.0, 10.0);

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(line.get())};
  int bands[] = {1};
  double burn_vals[] = {5.0};

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "Z");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  // (0,1) through (4,1) should have 5.0 + 10.0 = 15.0.
  for (int x = 0; x < 5; ++x) {
    EXPECT_DOUBLE_EQ(15.0, pixels[1 * 5 + x]);
  }
}

TEST_F(GdalRasterizeTest, LineStringAddMergeAlgSelfIntersecting) {
  GDALDatasetUniquePtr ds = CreateMemRaster(5, 5, 1, GDT_Int32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Line that visits pixel (2, 2) twice.
  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.0, 2.0);
  line->addPoint(2.0, 2.0);
  line->addPoint(4.0, 2.0);
  line->addPoint(2.0, 2.0);
  line->addPoint(2.0, 4.0);

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(line.get())};
  int bands[] = {1};
  double burn_vals[] = {25.0};

  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<GInt32> pixels = ReadBandPixels<GInt32>(band);
  // Pixel (2,2) must be added only ONCE per geometry, so value is 25, not 50.
  EXPECT_EQ(25, pixels[2 * 5 + 2]);
}

TEST_F(GdalRasterizeTest, MultiLineStringReplaceAndAdd) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Int32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto mls = std::make_unique<OGRMultiLineString>();
  auto line1 = std::make_unique<OGRLineString>();
  line1->addPoint(0.0, 1.0);
  line1->addPoint(5.0, 1.0);
  auto line2 = std::make_unique<OGRLineString>();
  line2->addPoint(0.0, 3.0);
  line2->addPoint(5.0, 3.0);
  EXPECT_EQ(OGRERR_NONE, mls->addGeometryDirectly(line1.release()));
  EXPECT_EQ(OGRERR_NONE, mls->addGeometryDirectly(line2.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(mls.get())};
  int bands[] = {1};
  double burn_vals[] = {100.0};

  // Replace mode triggers recursive gv_rasterize_one_shape for each part.
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GInt32> pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(100, pixels[1 * 6 + 2]);
  EXPECT_EQ(100, pixels[3 * 6 + 2]);
  EXPECT_EQ(0, pixels[2 * 6 + 2]);

  // Add mode triggers non-recursive path.
  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  burn_vals[0] = 50.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(150, pixels[1 * 6 + 2]);
  EXPECT_EQ(150, pixels[3 * 6 + 2]);
}

// ---------------------------------------------------------------------------
// Polygon and MultiPolygon Geometries
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, PolygonBasicReplaceAndAllTouched) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  OGRGeometryUniquePtr poly =
      MakePolygon(1.0, 1.0, 4.0, 4.0, /*clockwise=*/true);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {77.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(77, pixels[2 * 6 + 2]);
  EXPECT_EQ(0, pixels[0]);

  // With ALL_TOUCHED=TRUE.
  CPLStringList options;
  options.AddNameValue("ALL_TOUCHED", "TRUE");
  burn_vals[0] = 88.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(88, pixels[2 * 6 + 2]);
}

TEST_F(GdalRasterizeTest, PolygonAllTouchedBurnValueFromZ) {
  GDALDatasetUniquePtr ds = CreateMemRaster(5, 5, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  OGRGeometryUniquePtr poly = MakePolygon3D(1.0, 1.0, 4.0, 4.0, 20.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {15.0};

  CPLStringList options;
  options.AddNameValue("ALL_TOUCHED", "TRUE");
  options.AddNameValue("BURN_VALUE_FROM", "Z");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  // Center pixel value: 15.0 + 20.0 = 35.0.
  EXPECT_DOUBLE_EQ(35.0, pixels[2 * 5 + 2]);
}

TEST_F(GdalRasterizeTest, PolygonWithInteriorRingHole) {
  GDALDatasetUniquePtr ds = CreateMemRaster(8, 8, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto poly = std::make_unique<OGRPolygon>();
  auto ext_ring = std::make_unique<OGRLinearRing>();
  ext_ring->addPoint(0.0, 0.0);
  ext_ring->addPoint(0.0, 7.0);
  ext_ring->addPoint(7.0, 7.0);
  ext_ring->addPoint(7.0, 0.0);
  ext_ring->addPoint(0.0, 0.0);
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(ext_ring.release()));

  auto int_ring = std::make_unique<OGRLinearRing>();
  int_ring->addPoint(2.0, 2.0);
  int_ring->addPoint(5.0, 2.0);
  int_ring->addPoint(5.0, 5.0);
  int_ring->addPoint(2.0, 5.0);
  int_ring->addPoint(2.0, 2.0);
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(int_ring.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {200.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  // Exterior is burned.
  EXPECT_EQ(200, pixels[1 * 8 + 1]);
  // Interior hole is NOT burned.
  EXPECT_EQ(0, pixels[3 * 8 + 3]);
}

TEST_F(GdalRasterizeTest, PolygonCounterClockwiseAndClockwiseRingsWithZ) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Float64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Counter-clockwise polygon with Z coordinates.
  auto poly_ccw = std::make_unique<OGRPolygon>();
  auto ring_ccw = std::make_unique<OGRLinearRing>();
  ring_ccw->addPoint(1.0, 1.0, 10.0);
  ring_ccw->addPoint(4.0, 1.0, 10.0);
  ring_ccw->addPoint(4.0, 4.0, 10.0);
  ring_ccw->addPoint(1.0, 4.0, 10.0);
  ring_ccw->addPoint(1.0, 1.0, 10.0);
  EXPECT_EQ(OGRERR_NONE, poly_ccw->addRingDirectly(ring_ccw.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly_ccw.get())};
  int bands[] = {1};
  double burn_vals[] = {5.0};

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "Z");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  EXPECT_DOUBLE_EQ(15.0, pixels[2 * 6 + 2]);
}

TEST_F(GdalRasterizeTest, PolygonAddMergeAlg) {
  GDALDatasetUniquePtr ds = CreateMemRaster(5, 5, 1, GDT_Int32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  OGRGeometryUniquePtr poly = MakePolygon(1.0, 1.0, 4.0, 4.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {10.0};

  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  options.AddNameValue("ALL_TOUCHED", "TRUE");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  std::vector<GInt32> pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(10, pixels[2 * 5 + 2]);

  // Run again to verify add.
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(20, pixels[2 * 5 + 2]);
}

TEST_F(GdalRasterizeTest, PolygonSpanningOutsideRasterClamping) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Polygon extends far beyond raster boundaries: min=(-10,-10), max=(15,15).
  OGRGeometryUniquePtr poly = MakePolygon(-10.0, -10.0, 15.0, 15.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {123.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  for (GByte val : pixels) {
    EXPECT_EQ(123, val);
  }
}

TEST_F(GdalRasterizeTest, MultiPolygonReplaceAndAdd) {
  GDALDatasetUniquePtr ds = CreateMemRaster(8, 8, 1, GDT_Int32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto mp = std::make_unique<OGRMultiPolygon>();
  OGRGeometryUniquePtr p1 = MakePolygon(0.0, 0.0, 3.0, 3.0);
  OGRGeometryUniquePtr p2 = MakePolygon(4.0, 4.0, 7.0, 7.0);
  EXPECT_EQ(OGRERR_NONE, mp->addGeometryDirectly(p1.release()));
  EXPECT_EQ(OGRERR_NONE, mp->addGeometryDirectly(p2.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(mp.get())};
  int bands[] = {1};
  double burn_vals[] = {45.0};

  // Replace mode.
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GInt32> pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(45, pixels[1 * 8 + 1]);
  EXPECT_EQ(45, pixels[5 * 8 + 5]);
  EXPECT_EQ(0, pixels[0 * 8 + 7]);

  // Add mode.
  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  burn_vals[0] = 10.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(55, pixels[1 * 8 + 1]);
  EXPECT_EQ(55, pixels[5 * 8 + 5]);
}

TEST_F(GdalRasterizeTest, GeometryCollectionReplaceAndAdd) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Int32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto gc = std::make_unique<OGRGeometryCollection>();
  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.0, 4.0);
  line->addPoint(5.0, 4.0);
  EXPECT_EQ(OGRERR_NONE, gc->addGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, gc->addGeometryDirectly(line.release()));
  OGRGeometryUniquePtr poly = MakePolygon(2.0, 2.0, 4.0, 4.0);
  EXPECT_EQ(OGRERR_NONE, gc->addGeometryDirectly(poly.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(gc.get())};
  int bands[] = {1};
  double burn_vals[] = {60.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GInt32> pixels = ReadBandPixels<GInt32>(band);
  EXPECT_EQ(60, pixels[1 * 6 + 1]);
  EXPECT_EQ(60, pixels[4 * 6 + 2]);
  EXPECT_EQ(60, pixels[2 * 6 + 2]);

  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  burn_vals[0] = 40.0;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  pixels = ReadBandPixels<GInt32>(band);
  // Polygon interior is accumulated in ADD mode (60 + 40 = 100).
  EXPECT_EQ(100, pixels[2 * 6 + 2]);
  // Non-polygonal elements in a GeometryCollection in GRMA_Add mode are
  // skipped by GDALdllImageFilledPolygon and remain at previous value.
  EXPECT_EQ(60, pixels[1 * 6 + 1]);
}

TEST_F(GdalRasterizeTest, EmptyAndNullGeometries) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto empty_pt = std::make_unique<OGRPoint>();
  auto empty_line = std::make_unique<OGRLineString>();
  auto empty_poly = std::make_unique<OGRPolygon>();

  OGRGeometryH geoms[] = {nullptr, OGRGeometry::ToHandle(empty_pt.get()),
                          OGRGeometry::ToHandle(empty_line.get()),
                          OGRGeometry::ToHandle(empty_poly.get())};
  int bands[] = {1};
  double burn_vals[] = {10.0, 10.0, 10.0, 10.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    4, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  for (GByte val : pixels) {
    EXPECT_EQ(0, val);
  }
}

TEST_F(GdalRasterizeTest, NonPolygonalGeometryIgnored) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  // OGRTriangle has geometry type wkbTriangle, which is ignored with debug
  // message.
  auto triangle = std::make_unique<OGRTriangle>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(0.0, 2.0);
  ring->addPoint(2.0, 0.0);
  ring->addPoint(0.0, 0.0);
  EXPECT_EQ(OGRERR_NONE, triangle->addRingDirectly(ring.release()));

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(triangle.get())};
  int bands[] = {1};
  double burn_vals[] = {55.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));
  const auto pixels = ReadBandPixels<GByte>(ds->GetRasterBand(1));
  for (GByte p : pixels) {
    EXPECT_EQ(0, p);
  }
}

// ---------------------------------------------------------------------------
// All eleven GDAL data types in scanline and point burning
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, AllElevenDataTypesScanlineAndPoint) {
  constexpr GDALDataType kTypes[] = {
      GDT_Byte,  GDT_Int8,   GDT_Int16,   GDT_UInt16,  GDT_Int32,   GDT_UInt32,
      GDT_Int64, GDT_UInt64, GDT_Float16, GDT_Float32, GDT_Float64,
  };

  for (GDALDataType data_type : kTypes) {
    GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, data_type);
    ASSERT_NE(nullptr, ds) << "Failed to create dataset for type: "
                           << GDALGetDataTypeName(data_type);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    // 1. Point rasterization (exercises gvBurnPoint switch cases).
    auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
    OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
    int bands[] = {1};
    double burn_vals[] = {12.0};

    EXPECT_EQ(CE_None,
              GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                      1, geoms, nullptr, nullptr, burn_vals,
                                      nullptr, nullptr, nullptr));

    std::vector<double> pixels = ReadBandAsDouble(band);
    EXPECT_DOUBLE_EQ(12.0, pixels[1 * 4 + 1]);

    // 2. Polygon rasterization with REPLACE (exercises gvBurnScanline switch).
    OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 3.0, 3.0);
    OGRGeometryH poly_geoms[] = {OGRGeometry::ToHandle(poly.get())};
    burn_vals[0] = 5.0;

    EXPECT_EQ(CE_None,
              GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                      1, poly_geoms, nullptr, nullptr,
                                      burn_vals, nullptr, nullptr, nullptr));

    pixels = ReadBandAsDouble(band);
    EXPECT_DOUBLE_EQ(5.0, pixels[1 * 4 + 1]);

    // 3. Polygon rasterization with ADD.
    CPLStringList options;
    options.AddNameValue("MERGE_ALG", "ADD");
    burn_vals[0] = 7.0;

    EXPECT_EQ(CE_None, GDALRasterizeGeometries(
                           GDALDataset::ToHandle(ds.get()), 1, bands, 1,
                           poly_geoms, nullptr, nullptr, burn_vals,
                           options.List(), nullptr, nullptr));

    pixels = ReadBandAsDouble(band);
    EXPECT_DOUBLE_EQ(12.0, pixels[1 * 4 + 1]);
  }
}

// ---------------------------------------------------------------------------
// Int64 User Burn Values and Saturated Addition
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, Int64BurnValuesReplaceAndAdd) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Int64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  int64_t int64_burn_vals[] = {10000000000LL};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, int64_burn_vals, nullptr, nullptr, nullptr));

  std::vector<int64_t> pixels = ReadBandAsInt64(band);
  EXPECT_EQ(10000000000LL, pixels[1 * 4 + 1]);

  // Add merge algorithm.
  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");
  int64_burn_vals[0] = 5000LL;
  EXPECT_EQ(CE_None,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, int64_burn_vals, options.List(), nullptr, nullptr));

  pixels = ReadBandAsInt64(band);
  EXPECT_EQ(10000005000LL, pixels[1 * 4 + 1]);
}

TEST_F(GdalRasterizeTest, Int64SaturatedAdditionOverflowAndUnderflow) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Int64);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

  // 1. Pre-fill pixel (1, 1) with kMax - 10.
  std::vector<int64_t> init_buf(16, 0);
  init_buf[1 * 4 + 1] = kMax - 10;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4, init_buf.data(), 4, 4,
                                    GDT_Int64, 0, 0, nullptr));

  // Burn +50 with ADD -> overflows and saturates at kMax.
  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  int64_t burn_vals[] = {50};

  CPLStringList options;
  options.AddNameValue("MERGE_ALG", "ADD");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, burn_vals, options.List(), nullptr, nullptr));

  std::vector<int64_t> pixels = ReadBandAsInt64(band);
  EXPECT_EQ(kMax, pixels[1 * 4 + 1]);

  // 2. Pre-fill pixel (2, 2) with kMin + 10.
  init_buf[2 * 4 + 2] = kMin + 10;
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4, init_buf.data(), 4, 4,
                                    GDT_Int64, 0, 0, nullptr));

  auto pt2 = std::make_unique<OGRPoint>(2.0, 2.0);
  geoms[0] = OGRGeometry::ToHandle(pt2.get());
  burn_vals[0] = -50;

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, burn_vals, options.List(), nullptr, nullptr));

  pixels = ReadBandAsInt64(band);
  EXPECT_EQ(kMin, pixels[2 * 4 + 2]);

  // 3. Scanline saturated add on polygon with ADD and ALL_TOUCHED.
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 3.0, 3.0);
  geoms[0] = OGRGeometry::ToHandle(poly.get());
  burn_vals[0] = 100;
  options.AddNameValue("ALL_TOUCHED", "TRUE");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometriesInt64(
                GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms, nullptr,
                nullptr, burn_vals, options.List(), nullptr, nullptr));

  pixels = ReadBandAsInt64(band);
  EXPECT_EQ(kMax, pixels[1 * 4 + 1]);
  EXPECT_EQ(kMin + 100, pixels[2 * 4 + 2]);
}

// ---------------------------------------------------------------------------
// Multi-band rasterization
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, MultiBandRasterization) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 3, GDT_Float64);
  ASSERT_NE(nullptr, ds);

  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1, 2, 3};
  double burn_vals[] = {10.0, 20.0, 30.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 3, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<double> band1 = ReadBandAsDouble(ds->GetRasterBand(1));
  std::vector<double> band2 = ReadBandAsDouble(ds->GetRasterBand(2));
  std::vector<double> band3 = ReadBandAsDouble(ds->GetRasterBand(3));

  EXPECT_DOUBLE_EQ(10.0, band1[1 * 4 + 1]);
  EXPECT_DOUBLE_EQ(20.0, band2[1 * 4 + 1]);
  EXPECT_DOUBLE_EQ(30.0, band3[1 * 4 + 1]);
}

// ---------------------------------------------------------------------------
// Chunk Size variations
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, ChunkSizeVariations) {
  constexpr const char* kChunkSizes[] = {"1", "2", "100", "0"};

  for (const char* chunk_size_str : kChunkSizes) {
    GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 3.0, 3.0);
    OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
    int bands[] = {1};
    double burn_vals[] = {50.0};

    CPLStringList options;
    options.AddNameValue("CHUNKYSIZE", chunk_size_str);

    EXPECT_EQ(CE_None,
              GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                      1, geoms, nullptr, nullptr, burn_vals,
                                      options.List(), nullptr, nullptr));

    std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
    EXPECT_EQ(50, pixels[1 * 4 + 1]);
  }
}

// ---------------------------------------------------------------------------
// Optimisation Modes (OPTIM=RASTER, VECTOR, AUTO)
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, OptimModesRasterVectorAuto) {
  constexpr const char* kOptimModes[] = {"RASTER", "VECTOR", "AUTO"};

  for (const char* optim : kOptimModes) {
    GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Byte);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    OGRGeometryUniquePtr poly = MakePolygon(1.0, 1.0, 4.0, 4.0);
    OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
    int bands[] = {1};
    double burn_vals[] = {66.0};

    CPLStringList options;
    options.AddNameValue("OPTIM", optim);

    EXPECT_EQ(CE_None,
              GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                      1, geoms, nullptr, nullptr, burn_vals,
                                      options.List(), nullptr, nullptr));

    std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
    EXPECT_EQ(66, pixels[2 * 6 + 2]);
  }
}

TEST_F(GdalRasterizeTest, OptimVectorEnvelopeHandling) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  // 1. Geometry completely outside the raster envelope (e.g. 50..60).
  OGRGeometryUniquePtr poly_outside = MakePolygon(50.0, 50.0, 60.0, 60.0);
  // 2. Empty geometry.
  auto empty_geom = std::make_unique<OGRPolygon>();

  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly_outside.get()),
                          OGRGeometry::ToHandle(empty_geom.get())};
  int bands[] = {1};
  double burn_vals[] = {40.0, 40.0};

  CPLStringList options;
  options.AddNameValue("OPTIM", "VECTOR");

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    2, geoms, nullptr, nullptr, burn_vals,
                                    options.List(), nullptr, nullptr));

  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  for (GByte val : pixels) {
    EXPECT_EQ(0, val);
  }
}

TEST_F(GdalRasterizeTest, OptimVectorFailingTransformer) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 3.0, 3.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(poly.get())};
  int bands[] = {1};
  double burn_vals[] = {40.0};

  CPLStringList options;
  options.AddNameValue("OPTIM", "VECTOR");

  EXPECT_EQ(CE_None, GDALRasterizeGeometries(
                         GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms,
                         FailingTransformer, nullptr, burn_vals, options.List(),
                         nullptr, nullptr));
  const auto pixels = ReadBandPixels<GByte>(ds->GetRasterBand(1));
  for (GByte p : pixels) {
    EXPECT_EQ(0, p);
  }
}

// ---------------------------------------------------------------------------
// Transformers and Georeferencing
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, TransformerPresenceAndAbsence) {
  GDALDatasetUniquePtr ds = CreateMemRaster(6, 6, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // 1. Transformer absent, dataset has geotransform.
  double gt[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  EXPECT_EQ(CE_None, ds->SetGeoTransform(gt));

  // Point in georeferenced coordinates (104.0, 196.0) maps to pixel (2, 2).
  auto pt = std::make_unique<OGRPoint>(104.0, 196.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  double burn_vals[] = {90.0};

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(90, pixels[2 * 6 + 2]);

  // 2. Custom transformer with pTransformArg.
  double offset = 1.0;
  auto pt_origin = std::make_unique<OGRPoint>(0.0, 0.0);
  geoms[0] = OGRGeometry::ToHandle(pt_origin.get());
  burn_vals[0] = 30.0;

  EXPECT_EQ(CE_None,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, TranslateTransformer, &offset,
                                    burn_vals, nullptr, nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  // (0+1, 0+1) = (1, 1) -> index 1*6 + 1 = 7.
  EXPECT_EQ(30, pixels[1 * 6 + 1]);
}

// ---------------------------------------------------------------------------
// Progress Cancellation Tests
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, ProgressCancellation) {
  GDALDatasetUniquePtr ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, ds);

  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  OGRGeometryH geoms[] = {OGRGeometry::ToHandle(pt.get())};
  int bands[] = {1};
  double burn_vals[] = {10.0};

  int cancel_counter = 0;
  WithQuietHandler quiet;
  CPLErrorReset();

  // 1. Geometries Raster progress cancellation.
  cancel_counter = 0;
  EXPECT_EQ(CE_Failure,
            GDALRasterizeGeometries(GDALDataset::ToHandle(ds.get()), 1, bands,
                                    1, geoms, nullptr, nullptr, burn_vals,
                                    nullptr, CancelProgress, &cancel_counter));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  CPLErrorReset();

  // 2. Geometries Vector progress cancellation.
  CPLStringList vector_opts;
  vector_opts.AddNameValue("OPTIM", "VECTOR");
  cancel_counter = 0;
  EXPECT_EQ(CE_Failure, GDALRasterizeGeometries(
                            GDALDataset::ToHandle(ds.get()), 1, bands, 1, geoms,
                            nullptr, nullptr, burn_vals, vector_opts.List(),
                            CancelProgress, &cancel_counter));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 3. Layers progress cancellation.
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer = vec_ds->CreateLayer("layer1", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);
  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt_copy = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt_copy.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  cancel_counter = 0;
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayers(GDALDataset::ToHandle(ds.get()), 1, bands, 1,
                                layers, nullptr, nullptr, burn_vals, nullptr,
                                CancelProgress, &cancel_counter));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 4. LayersBuf progress cancellation.
  std::vector<GByte> buf(16, 0);
  cancel_counter = 0;
  EXPECT_EQ(CE_Failure,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 1, layers,
                                   nullptr, nullptr, nullptr, nullptr, 10.0,
                                   nullptr, CancelProgress, &cancel_counter));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// ---------------------------------------------------------------------------
// GDALRasterizeLayers Tests
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, LayersSingleAndMultiChunk) {
  // Test both single chunk (nYChunkSize == YSize) and multi chunk (nYChunkSize
  // < YSize).
  constexpr const char* kChunkConfigs[] = {"4", "1"};

  for (const char* chunk_size : kChunkConfigs) {
    GDALDatasetUniquePtr raster_ds = CreateMemRaster(4, 4, 1, GDT_Byte);
    ASSERT_NE(nullptr, raster_ds);
    GDALRasterBand* band = raster_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    GDALDatasetUniquePtr vec_ds = CreateMemVector();
    ASSERT_NE(nullptr, vec_ds);
    OGRLayer* layer = vec_ds->CreateLayer("l1", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(nullptr, layer);

    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    OGRGeometryUniquePtr poly = MakePolygon(1.0, 1.0, 3.0, 3.0);
    EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(poly.release()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

    OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
    int bands[] = {1};
    double burn_vals[] = {44.0};

    CPLStringList options;
    options.AddNameValue("CHUNKYSIZE", chunk_size);

    EXPECT_EQ(CE_None,
              GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                  bands, 1, layers, nullptr, nullptr, burn_vals,
                                  options.List(), nullptr, nullptr));

    std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
    EXPECT_EQ(44, pixels[2 * 4 + 2]);
  }
}

TEST_F(GdalRasterizeTest, LayersNullAndEmptyLayerHandling) {
  GDALDatasetUniquePtr raster_ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, raster_ds);

  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* empty_layer =
      vec_ds->CreateLayer("empty_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, empty_layer);

  OGRLayerH layers[] = {nullptr, OGRLayer::ToHandle(empty_layer)};
  int bands[] = {1};
  double burn_vals[] = {10.0, 20.0};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()),
                                         1, bands, 2, layers, nullptr, nullptr,
                                         burn_vals, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Layer element number 0 is NULL, skipping."));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, LayersAttributeBurnValue) {
  GDALDatasetUniquePtr raster_ds = CreateMemRaster(4, 4, 1, GDT_Float64);
  ASSERT_NE(nullptr, raster_ds);
  GDALRasterBand* band = raster_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer =
      vec_ds->CreateLayer("attr_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("burn_attr", OFTReal);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("burn_attr", 123.45);
  auto pt = std::make_unique<OGRPoint>(2.0, 2.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  int bands[] = {1};

  // 1. Existing attribute field.
  CPLStringList options;
  options.AddNameValue("ATTRIBUTE", "burn_attr");

  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                bands, 1, layers, nullptr, nullptr, nullptr,
                                options.List(), nullptr, nullptr));

  std::vector<double> pixels = ReadBandAsDouble(band);
  EXPECT_DOUBLE_EQ(123.45, pixels[2 * 4 + 2]);

  // 2. Missing attribute field.
  CPLStringList bad_options;
  bad_options.AddNameValue("ATTRIBUTE", "no_such_field");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                bands, 1, layers, nullptr, nullptr, nullptr,
                                bad_options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Failed to find field no_such_field on layer attr_layer"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, LayersSpatialAndAttributeAndSQLFiltering) {
  GDALDatasetUniquePtr raster_ds = CreateMemRaster(6, 6, 1, GDT_Byte);
  ASSERT_NE(nullptr, raster_ds);
  GDALRasterBand* band = raster_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer =
      vec_ds->CreateLayer("filter_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("code", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));

  // Feature 1 at (1, 1) with code 10.
  auto feat1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat1->SetField("code", 10);
  auto pt1 = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat1->SetGeometryDirectly(pt1.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat1.get()));

  // Feature 2 at (4, 4) with code 20.
  auto feat2 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat2->SetField("code", 20);
  auto pt2 = std::make_unique<OGRPoint>(4.0, 4.0);
  EXPECT_EQ(OGRERR_NONE, feat2->SetGeometryDirectly(pt2.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat2.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  int bands[] = {1};
  double burn_vals[] = {100.0};

  // 1. Spatial filter: envelope covers only (0,0) to (2,2).
  OGRGeometryUniquePtr filter_poly = MakePolygon(0.0, 0.0, 2.0, 2.0);
  layer->SetSpatialFilter(filter_poly.get());

  EXPECT_EQ(CE_None, GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()),
                                         1, bands, 1, layers, nullptr, nullptr,
                                         burn_vals, nullptr, nullptr, nullptr));

  std::vector<GByte> pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(100, pixels[1 * 6 + 1]);
  EXPECT_EQ(0, pixels[4 * 6 + 4]);

  // 2. Attribute filter: code = 20.
  layer->SetSpatialFilter(nullptr);
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("code = 20"));
  burn_vals[0] = 200.0;

  EXPECT_EQ(CE_None, GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()),
                                         1, bands, 1, layers, nullptr, nullptr,
                                         burn_vals, nullptr, nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(200, pixels[4 * 6 + 4]);

  // 3. SQL Result Set Layer.
  layer->SetAttributeFilter(nullptr);
  OGRLayer* sql_layer = vec_ds->ExecuteSQL(
      "SELECT * FROM filter_layer WHERE code = 10", nullptr, nullptr);
  ASSERT_NE(nullptr, sql_layer);
  absl::Cleanup sql_cleaner = [&vec_ds, sql_layer] {
    vec_ds->ReleaseResultSet(sql_layer);
  };

  OGRLayerH sql_layers[] = {OGRLayer::ToHandle(sql_layer)};
  burn_vals[0] = 55.0;

  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                bands, 1, sql_layers, nullptr, nullptr,
                                burn_vals, nullptr, nullptr, nullptr));

  pixels = ReadBandPixels<GByte>(band);
  EXPECT_EQ(55, pixels[1 * 6 + 1]);
}

TEST_F(GdalRasterizeTest, LayersSRSAndMissingSRSHandling) {
  GDALDatasetUniquePtr raster_ds = CreateMemRaster(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, raster_ds);

  OGRSpatialReference ds_srs;
  EXPECT_EQ(OGRERR_NONE, ds_srs.importFromEPSG(4326));
  EXPECT_EQ(CE_None, raster_ds->SetSpatialRef(&ds_srs));

  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  // Layer without SRS.
  OGRLayer* layer_no_srs =
      vec_ds->CreateLayer("no_srs", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer_no_srs);

  auto feat = std::make_unique<OGRFeature>(layer_no_srs->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, layer_no_srs->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer_no_srs)};
  int bands[] = {1};
  double burn_vals[] = {10.0};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()),
                                         1, bands, 1, layers, nullptr, nullptr,
                                         burn_vals, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Failed to fetch spatial reference on layer no_srs"));
  CPLErrorReset();

  // Layer with SRS.
  OGRSpatialReference layer_srs;
  EXPECT_EQ(OGRERR_NONE, layer_srs.importFromEPSG(4326));
  OGRLayer* layer_with_srs =
      vec_ds->CreateLayer("with_srs", &layer_srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer_with_srs);

  auto feat_srs = std::make_unique<OGRFeature>(layer_with_srs->GetLayerDefn());
  auto pt_srs = std::make_unique<OGRPoint>(2.0, 2.0);
  EXPECT_EQ(OGRERR_NONE, feat_srs->SetGeometryDirectly(pt_srs.release()));
  EXPECT_EQ(OGRERR_NONE, layer_with_srs->CreateFeature(feat_srs.get()));

  OGRLayerH layers_with_srs[] = {OGRLayer::ToHandle(layer_with_srs)};
  EXPECT_EQ(CE_None,
            GDALRasterizeLayers(GDALDataset::ToHandle(raster_ds.get()), 1,
                                bands, 1, layers_with_srs, nullptr, nullptr,
                                burn_vals, nullptr, nullptr, nullptr));
  EXPECT_EQ(10, ReadBandPixels<GByte>(raster_ds->GetRasterBand(1))[2 * 4 + 2]);
}

// ---------------------------------------------------------------------------
// GDALRasterizeLayersBuf Tests
// ---------------------------------------------------------------------------

TEST_F(GdalRasterizeTest, LayersBufBasicRasterization) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer =
      vec_ds->CreateLayer("buf_layer", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  OGRGeometryUniquePtr poly = MakePolygon(1.0, 1.0, 3.0, 3.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(poly.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  std::vector<GByte> buf(16, 0);

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 1, layers,
                                   nullptr, nullptr, nullptr, nullptr, 99.0,
                                   nullptr, nullptr, nullptr));

  EXPECT_EQ(99, buf[2 * 4 + 2]);
  EXPECT_EQ(0, buf[0]);
}

TEST_F(GdalRasterizeTest, LayersBufCustomSpacingAndDoubleType) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer =
      vec_ds->CreateLayer("buf_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  // Custom line space: width=4, pixel_space=sizeof(double)=8, line_space=64
  // (padded).
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  constexpr int kPixelSpace = sizeof(double);
  constexpr int kLineSpace = 64;
  std::vector<char> raw_buf(kLineSpace * kHeight, 0);

  EXPECT_EQ(CE_None, GDALRasterizeLayersBuf(
                         raw_buf.data(), kWidth, kHeight, GDT_Float64,
                         kPixelSpace, kLineSpace, 1, layers, nullptr, nullptr,
                         nullptr, nullptr, 77.5, nullptr, nullptr, nullptr));

  const double* pixel_ptr = reinterpret_cast<const double*>(
      raw_buf.data() + 1 * kLineSpace + 1 * kPixelSpace);
  EXPECT_DOUBLE_EQ(77.5, *pixel_ptr);
}

TEST_F(GdalRasterizeTest, LayersBufNullAndEmptyLayer) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* empty_layer =
      vec_ds->CreateLayer("empty_l", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, empty_layer);

  OGRLayerH layers[] = {nullptr, OGRLayer::ToHandle(empty_layer)};
  std::vector<GByte> buf(16, 0);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 2, layers,
                                   nullptr, nullptr, nullptr, nullptr, 10.0,
                                   nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Layer element number 0 is NULL, skipping."));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, LayersBufAttributeBurnValue) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer = vec_ds->CreateLayer("l_attr", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("val", OFTReal);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("val", 88.0);
  auto pt = std::make_unique<OGRPoint>(2.0, 2.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  std::vector<double> buf(16, 0.0);

  CPLStringList options;
  options.AddNameValue("ATTRIBUTE", "val");

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Float64, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   0.0, options.List(), nullptr, nullptr));

  EXPECT_DOUBLE_EQ(88.0, buf[2 * 4 + 2]);

  // Missing attribute field.
  CPLStringList bad_options;
  bad_options.AddNameValue("ATTRIBUTE", "non_existent");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Float64, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   0.0, bad_options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Failed to find field non_existent on layer l_attr"));
  CPLErrorReset();
}

TEST_F(GdalRasterizeTest, LayersBufSRSTransformerAndMissingSRS) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);

  // 1. Layer without SRS -> warning.
  OGRLayer* layer_no_srs =
      vec_ds->CreateLayer("no_srs_buf", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer_no_srs);
  auto feat1 = std::make_unique<OGRFeature>(layer_no_srs->GetLayerDefn());
  auto pt1 = std::make_unique<OGRPoint>(1.0, 1.0);
  EXPECT_EQ(OGRERR_NONE, feat1->SetGeometryDirectly(pt1.release()));
  EXPECT_EQ(OGRERR_NONE, layer_no_srs->CreateFeature(feat1.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer_no_srs)};
  std::vector<GByte> buf(16, 0);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 1, layers,
                                   nullptr, nullptr, nullptr, nullptr, 10.0,
                                   nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Failed to fetch spatial reference on layer no_srs_buf"));
  CPLErrorReset();

  // 2. Layer with SRS and dst geotransform.
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  OGRLayer* layer_srs = vec_ds->CreateLayer("srs_buf", &srs, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer_srs);
  auto feat2 = std::make_unique<OGRFeature>(layer_srs->GetLayerDefn());
  auto pt2 = std::make_unique<OGRPoint>(102.0, 198.0);
  EXPECT_EQ(OGRERR_NONE, feat2->SetGeometryDirectly(pt2.release()));
  EXPECT_EQ(OGRERR_NONE, layer_srs->CreateFeature(feat2.get()));

  char* dst_wkt_raw = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&dst_wkt_raw));
  std::unique_ptr<char, CplFreeDeleter> dst_wkt(dst_wkt_raw);

  double dst_gt[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  OGRLayerH layers_srs[] = {OGRLayer::ToHandle(layer_srs)};
  std::fill(buf.begin(), buf.end(), 0);

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 1,
                                   layers_srs, dst_wkt.get(), dst_gt, nullptr,
                                   nullptr, 50.0, nullptr, nullptr, nullptr));

  EXPECT_EQ(50, buf[1 * 4 + 1]);
}

TEST_F(GdalRasterizeTest, LayersBufBurnValueFromZAndMergeAlgAdd) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer = vec_ds->CreateLayer("buf_3d", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt1 = std::make_unique<OGRPoint>(1.0, 1.0, 20.0);
  EXPECT_EQ(OGRERR_NONE, feat1->SetGeometryDirectly(pt1.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat1.get()));

  auto feat2 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt2 = std::make_unique<OGRPoint>(1.0, 1.0, 30.0);
  EXPECT_EQ(OGRERR_NONE, feat2->SetGeometryDirectly(pt2.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat2.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  std::vector<double> buf(16, 0.0);

  CPLStringList options;
  options.AddNameValue("BURN_VALUE_FROM", "Z");
  options.AddNameValue("MERGE_ALG", "ADD");

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Float64, 0, 0, 1,
                                   layers, nullptr, nullptr, nullptr, nullptr,
                                   10.0, options.List(), nullptr, nullptr));

  // Feature 1: 10 + 20 = 30. Feature 2: 10 + 30 = 40. Total = 70.
  EXPECT_DOUBLE_EQ(70.0, buf[1 * 4 + 1]);
}

TEST_F(GdalRasterizeTest, LayersBufCustomTransformer) {
  GDALDatasetUniquePtr vec_ds = CreateMemVector();
  ASSERT_NE(nullptr, vec_ds);
  OGRLayer* layer =
      vec_ds->CreateLayer("buf_trans", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, layer);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  auto pt = std::make_unique<OGRPoint>(0.0, 0.0);
  EXPECT_EQ(OGRERR_NONE, feat->SetGeometryDirectly(pt.release()));
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  OGRLayerH layers[] = {OGRLayer::ToHandle(layer)};
  std::vector<GByte> buf(16, 0);
  double offset = 2.0;

  EXPECT_EQ(CE_None,
            GDALRasterizeLayersBuf(buf.data(), 4, 4, GDT_Byte, 0, 0, 1, layers,
                                   nullptr, nullptr, TranslateTransformer,
                                   &offset, 65.0, nullptr, nullptr, nullptr));

  // Shifted from (0,0) by +2 to (2,2) -> index 2*4 + 2 = 10.
  EXPECT_EQ(65, buf[2 * 4 + 2]);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and defensive
// error paths.
