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
// Tests zonal statistics computation (alg/zonal.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/zonal.cpp
//   https://gdal.org/api/gdal_alg.html

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
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
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::DoubleNear;
using ::testing::HasSubstr;

// -----------------------------------------------------------------------------
// Test Helpers & Mock Classes
// -----------------------------------------------------------------------------

GDALDatasetUniquePtr CreateMemRaster(int width, int height, int bands = 1,
                                     GDALDataType type = GDT_Float64,
                                     const double* gt_values = nullptr) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_drv == nullptr) {
    return nullptr;
  }
  GDALDatasetUniquePtr ds(
      mem_drv->Create("", width, height, bands, type, nullptr));
  if (ds == nullptr) {
    return nullptr;
  }
  if (gt_values != nullptr) {
    double gt[6] = {gt_values[0], gt_values[1], gt_values[2],
                    gt_values[3], gt_values[4], gt_values[5]};
    ds->SetGeoTransform(gt);
  } else {
    double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(height), 0.0, -1.0};
    ds->SetGeoTransform(gt);
  }
  return ds;
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
void WriteBandData(GDALRasterBand* band, const std::vector<T>& data) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  ASSERT_EQ(static_cast<size_t>(x_size) * y_size, data.size());
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, x_size, y_size,
                                    const_cast<T*>(data.data()), x_size, y_size,
                                    band->GetRasterDataType(), 0, 0, nullptr));
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

OGRGeometryUniquePtr MakePolygonWithHole(double min_x, double min_y,
                                         double max_x, double max_y,
                                         double hole_min_x, double hole_min_y,
                                         double hole_max_x, double hole_max_y) {
  auto poly = std::make_unique<OGRPolygon>();
  auto ext_ring = std::make_unique<OGRLinearRing>();
  ext_ring->addPoint(min_x, min_y);
  ext_ring->addPoint(min_x, max_y);
  ext_ring->addPoint(max_x, max_y);
  ext_ring->addPoint(max_x, min_y);
  ext_ring->addPoint(min_x, min_y);
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(ext_ring.release()));

  auto int_ring = std::make_unique<OGRLinearRing>();
  int_ring->addPoint(hole_min_x, hole_min_y);
  int_ring->addPoint(hole_max_x, hole_min_y);
  int_ring->addPoint(hole_max_x, hole_max_y);
  int_ring->addPoint(hole_min_x, hole_max_y);
  int_ring->addPoint(hole_min_x, hole_min_y);
  EXPECT_EQ(OGRERR_NONE, poly->addRingDirectly(int_ring.release()));

  return OGRGeometryUniquePtr(poly.release());
}

OGRGeometryUniquePtr MakeMultiPolygon(std::vector<OGRGeometryUniquePtr> polys) {
  auto mp = std::make_unique<OGRMultiPolygon>();
  for (auto& p : polys) {
    EXPECT_EQ(OGRERR_NONE, mp->addGeometryDirectly(p.release()));
  }
  return OGRGeometryUniquePtr(mp.release());
}

int TrackingProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                     void* pData) {
  if (pData != nullptr) {
    auto* call_count = static_cast<int*>(pData);
    (*call_count)++;
  }
  return TRUE;
}

class CapturingErrorHandler {
 public:
  CapturingErrorHandler() { CPLPushErrorHandlerEx(&Handler, this); }
  ~CapturingErrorHandler() { CPLPopErrorHandler(); }
  const std::string& messages() const { return messages_; }
  void clear() { messages_.clear(); }

 private:
  static void CPL_STDCALL Handler(CPLErr /*eErr*/, CPLErrorNum /*nErrNo*/,
                                  const char* pszMsg) {
    auto* self =
        static_cast<CapturingErrorHandler*>(CPLGetErrorHandlerUserData());
    if (self != nullptr && pszMsg != nullptr) {
      self->messages_ += pszMsg;
      self->messages_ += '\n';
    }
  }

  std::string messages_;
};

// -----------------------------------------------------------------------------
// Custom Mock Datasets for Hard-to-Reach Error Paths
// -----------------------------------------------------------------------------

class FailCreateLayerDataset final : public GDALDataset {
 public:
  FailCreateLayerDataset() = default;

 protected:
  OGRLayer* ICreateLayer(const char*, const OGRGeomFieldDefn*,
                         CSLConstList) override {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated CreateLayer failure");
    return nullptr;
  }
};

class FailCreateFieldLayer final : public OGRLayer {
 public:
  FailCreateFieldLayer() {
    feature_defn_ = new OGRFeatureDefn("stats");
    feature_defn_->Reference();
  }

  ~FailCreateFieldLayer() override {
    if (feature_defn_ != nullptr) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }
  OGRErr ICreateFeature(OGRFeature*) override { return OGRERR_NONE; }

  OGRErr CreateField(const OGRFieldDefn*, int) override {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated CreateField failure");
    return OGRERR_FAILURE;
  }

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

class FailCreateFieldDataset final : public GDALDataset {
 public:
  FailCreateFieldDataset() = default;
  int GetLayerCount() const override { return layer_ ? 1 : 0; }
  const OGRLayer* GetLayer(int i) const override {
    return i == 0 ? layer_.get() : nullptr;
  }

 protected:
  OGRLayer* ICreateLayer(const char*, const OGRGeomFieldDefn*,
                         CSLConstList) override {
    layer_ = std::make_unique<FailCreateFieldLayer>();
    return layer_.get();
  }

 private:
  std::unique_ptr<FailCreateFieldLayer> layer_;
};

class FailCreateFeatureLayer final : public OGRLayer {
 public:
  FailCreateFeatureLayer() {
    feature_defn_ = new OGRFeatureDefn("stats");
    feature_defn_->Reference();
  }

  ~FailCreateFeatureLayer() override {
    if (feature_defn_ != nullptr) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }

  int TestCapability(const char* cap) const override {
    if (EQUAL(cap, OLCSequentialWrite)) {
      return TRUE;
    }
    return FALSE;
  }

  OGRErr ICreateFeature(OGRFeature*) override {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated CreateFeature failure");
    return OGRERR_FAILURE;
  }

  OGRErr CreateField(const OGRFieldDefn* poField, int) override {
    feature_defn_->AddFieldDefn(poField);
    return OGRERR_NONE;
  }

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

class FailCreateFeatureDataset final : public GDALDataset {
 public:
  FailCreateFeatureDataset() = default;
  int GetLayerCount() const override { return layer_ ? 1 : 0; }
  const OGRLayer* GetLayer(int i) const override {
    return i == 0 ? layer_.get() : nullptr;
  }

 protected:
  OGRLayer* ICreateLayer(const char*, const OGRGeomFieldDefn*,
                         CSLConstList) override {
    layer_ = std::make_unique<FailCreateFeatureLayer>();
    return layer_.get();
  }

 private:
  std::unique_ptr<FailCreateFeatureLayer> layer_;
};

class FailReadRasterBand final : public GDALRasterBand {
 public:
  FailReadRasterBand(GDALDataset* ds, int x_size, int y_size) {
    poDS = ds;
    nBand = 1;
    eDataType = GDT_Float64;
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    nBlockXSize = x_size;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void*) override { return CE_Failure; }
};

class FailReadDataset final : public GDALDataset {
 public:
  FailReadDataset(int x_size, int y_size) {
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    SetBand(1, new FailReadRasterBand(this, x_size, y_size));
    double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(y_size), 0.0, -1.0};
    SetGeoTransform(gt);
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt) const override {
    gt = gt_;
    return CE_None;
  }

  CPLErr SetGeoTransform(const GDALGeoTransform& gt) override {
    gt_ = gt;
    return CE_None;
  }

  using GDALDataset::GetGeoTransform;
  using GDALDataset::SetGeoTransform;

 private:
  GDALGeoTransform gt_{};
};

// -----------------------------------------------------------------------------
// Test Fixture
// -----------------------------------------------------------------------------

class ZonalTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALRegister_MEM();
    GDALRegister_VRT();
  }
};

// -----------------------------------------------------------------------------
// Suite 1: Pointer & Option Validation Tests
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, ValidatePointers) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(nullptr, nullptr, GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));

  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr, nullptr,
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));

  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()), nullptr, nullptr,
                           nullptr, nullptr));
}

TEST_F(ZonalTest, OptionInvalidBands) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  {
    CPLStringList options;
    options.AddNameValue("BANDS", "0");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band: 0"));
  }
  {
    CPLStringList options;
    options.AddNameValue("BANDS", "-1");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band: -1"));
  }
  {
    CPLStringList options;
    options.AddNameValue("BANDS", "abc");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid band: abc"));
  }
}

TEST_F(ZonalTest, OptionInvalidPixelIntersection) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("PIXEL_INTERSECTION", "INVALID_MODE");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Unexpected value of PIXEL_INTERSECTION: INVALID_MODE"));
}

TEST_F(ZonalTest, OptionInvalidRasterChunkSizeBytes) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  {
    CPLStringList options;
    options.AddNameValue("RASTER_CHUNK_SIZE_BYTES", "not_a_number");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid memory size: not_a_number"));
  }
  {
    CPLStringList options;
    options.AddNameValue("RASTER_CHUNK_SIZE_BYTES",
                         "999999999999999999999999999999999999999999999999");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid memory size"));
  }
}

TEST_F(ZonalTest, OptionInvalidStrategy) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("STRATEGY", "BOGUS_STRATEGY");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unexpected value of STRATEGY: BOGUS_STRATEGY"));
}

TEST_F(ZonalTest, OptionInvalidWeightsAndZonesBand) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  {
    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "0");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid weights band: 0"));
  }
  {
    CPLStringList options;
    options.AddNameValue("ZONES_BAND", "0");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid zones band: 0"));
  }
}

TEST_F(ZonalTest, OptionUnexpectedKey) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("UNRECOGNIZED_OPTION", "VAL");

  CapturingErrorHandler capturing;
  CPLErrorReset();
  // Upstream quirk: GDALZonalStatsOptions::Init logs an error on unrecognized
  // option but does not abort, returning CE_None.
  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));
  EXPECT_THAT(capturing.messages(),
              HasSubstr("Unexpected zonal stats option: UNRECOGNIZED_OPTION"));
}

// -----------------------------------------------------------------------------
// Suite 2: Init() Validation & Checks
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, SrcDatasetNoBands) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src(mem_drv->Create("", 4, 4, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src);
  double gt[6] = {0, 1, 0, 4, 0, -1};
  src->SetGeoTransform(gt);

  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("GDALRasterZonalStats: input dataset has no bands"));
}

TEST_F(ZonalTest, SrcDatasetInvalidBand) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4, 1);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("BANDS", "5");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("GDALRasterZonalStats: Invalid band number: 5"));
}

TEST_F(ZonalTest, SrcDatasetUnsupportedType) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4, 1, GDT_CFloat64);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source data type CFloat64 is not supported"));
}

TEST_F(ZonalTest, WeightsDatasetInvalidBandAndUnsupportedType) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  {
    GDALDatasetUniquePtr weights = CreateMemRaster(4, 4, 1);
    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "3");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                         GDALDataset::ToHandle(weights.get()),
                                         GDALDataset::ToHandle(zones.get()),
                                         GDALDataset::ToHandle(out.get()),
                                         options.List(), nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALRasterZonalStats: invalid weights band"));
  }
  {
    GDALDatasetUniquePtr weights = CreateMemRaster(4, 4, 1, GDT_CFloat64);
    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                         GDALDataset::ToHandle(weights.get()),
                                         GDALDataset::ToHandle(zones.get()),
                                         GDALDataset::ToHandle(out.get()),
                                         options.List(), nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Weights data type CFloat64 is not supported"));
  }
}

TEST_F(ZonalTest, StatInvalid) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("STATS", "nonexistent_stat");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Invalid stat: nonexistent_stat"));
}

TEST_F(ZonalTest, WeightedStatWithoutWeights) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  constexpr const char* kWeightedStats[] = {
      "weighted_mean", "weighted_sum",      "weights",
      "weighted_frac", "weighted_variance", "weighted_stdev"};

  WithQuietHandler quiet;
  for (const char* stat : kWeightedStats) {
    CPLStringList options;
    options.AddNameValue("STATS", stat);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("requires weights but none were provided"));
  }
}

TEST_F(ZonalTest, SrcDatasetNoGeoTransform) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src(mem_drv->Create("", 4, 4, 1, GDT_Float64, nullptr));
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Dataset has no geotransform"));
}

TEST_F(ZonalTest, SrcDatasetNonInvertibleGeoTransform) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  double sing_gt[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  src->SetGeoTransform(sing_gt);

  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Dataset geotransform cannot be inverted"));
}

TEST_F(ZonalTest, VectorZonesFieldNotFound) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFieldDefn field_id("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&field_id));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("INCLUDE_FIELDS", "nonexistent_field");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Field nonexistent_field not found."));
}

TEST_F(ZonalTest, RasterZonesCannotIncludeFields) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("INCLUDE_FIELDS", "some_field");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Cannot include fields from raster zones"));
}

TEST_F(ZonalTest, SrsMismatchWarnings) {
  OGRSpatialReference srs4326;
  EXPECT_EQ(OGRERR_NONE, srs4326.importFromEPSG(4326));
  srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  EXPECT_EQ(OGRERR_NONE, srs3857.importFromEPSG(3857));
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  CapturingErrorHandler capturing;
  {
    // Inputs and zones mismatch.
    GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
    src->SetSpatialRef(&srs4326);
    GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
    zones->SetSpatialRef(&srs3857);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()), nullptr,
                                      nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("Inputs and zones do not have the same SRS"));
    capturing.clear();
  }
  {
    // Weights and zones mismatch.
    GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
    src->SetSpatialRef(&srs4326);
    GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
    zones->SetSpatialRef(&srs4326);
    GDALDatasetUniquePtr weights = CreateMemRaster(4, 4);
    weights->SetSpatialRef(&srs3857);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    options.AddNameValue("STATS", "weighted_mean");

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                      GDALDataset::ToHandle(weights.get()),
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("Weights and zones do not have the same SRS"));
    capturing.clear();
  }
  {
    // Inputs and weights mismatch.
    GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
    src->SetSpatialRef(&srs4326);
    GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
    zones->SetSpatialRef(&srs3857);
    GDALDatasetUniquePtr weights = CreateMemRaster(4, 4);
    weights->SetSpatialRef(&srs3857);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    options.AddNameValue("STATS", "weighted_mean");

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                      GDALDataset::ToHandle(weights.get()),
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("Inputs and weights do not have the same SRS"));
    capturing.clear();
  }
}

TEST_F(ZonalTest, ZonesDatasetAmbiguityAndLookupErrors) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  {
    // Zones dataset has 2 bands, no band or layer specified.
    GDALDatasetUniquePtr zones = CreateMemRaster(4, 4, 2);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                             nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Zones dataset has more than one band or layer"));
  }
  {
    // Zones dataset has 0 bands and 0 layers.
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr empty_zones(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(empty_zones.get()),
                             GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                             nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Zones dataset has no band or layer."));
  }
  {
    // ZONES_BAND out of range.
    GDALDatasetUniquePtr zones = CreateMemRaster(4, 4, 1);
    CPLStringList options;
    options.AddNameValue("ZONES_BAND", "5");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid zones band: 5"));
  }
  {
    // ZONES_LAYER not found.
    GDALDatasetUniquePtr zones = CreateMemVector();
    zones->CreateLayer("layer1", nullptr, wkbPolygon, nullptr);
    CPLStringList options;
    options.AddNameValue("ZONES_LAYER", "missing_layer");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Specified zones layer 'missing_layer' not found"));
  }
}

TEST_F(ZonalTest, WarnIfZonesNotCovered) {
  CapturingErrorHandler capturing;
  {
    // Source raster does not intersect zones.
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {100.0, 1.0, 0.0, 110.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(10, 10, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()), nullptr,
                                      nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("Source raster does not intersect zones raster"));
    capturing.clear();
  }
  {
    // Source raster does not fully cover zones raster (with nodata on src).
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {-2.0, 1.0, 0.0, 12.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    src->GetRasterBand(1)->SetNoDataValue(-9999);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(14, 14, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()), nullptr,
                                      nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("treated as having a NoData value"));
    capturing.clear();
  }
  {
    // Source raster does not fully cover zones raster (without nodata on src).
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {-2.0, 1.0, 0.0, 12.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(14, 14, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()), nullptr,
                                      nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("treated as having value of zero"));
    capturing.clear();
  }
  {
    // Weights raster does not intersect zones raster.
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double weights_gt[6] = {200.0, 1.0, 0.0, 210.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(10, 10, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr weights =
        CreateMemRaster(10, 10, 1, GDT_Float64, weights_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    options.AddNameValue("STATS", "weighted_mean");

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                      GDALDataset::ToHandle(weights.get()),
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("Weighting raster does not intersect zones raster"));
    capturing.clear();
  }
  {
    // Weights raster does not fully cover zones (with nodata on src).
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double weights_gt[6] = {2.0, 1.0, 0.0, 8.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    src->GetRasterBand(1)->SetNoDataValue(-9999);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(10, 10, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr weights =
        CreateMemRaster(6, 6, 1, GDT_Float64, weights_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    options.AddNameValue("STATS", "weighted_mean");

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                      GDALDataset::ToHandle(weights.get()),
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("treated as having a NoData weight"));
    capturing.clear();
  }
  {
    // Weights raster does not fully cover zones (without nodata on src).
    const double src_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double zones_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    const double weights_gt[6] = {2.0, 1.0, 0.0, 8.0, 0.0, -1.0};
    GDALDatasetUniquePtr src = CreateMemRaster(10, 10, 1, GDT_Float64, src_gt);
    GDALDatasetUniquePtr zones =
        CreateMemRaster(10, 10, 1, GDT_Float64, zones_gt);
    GDALDatasetUniquePtr weights =
        CreateMemRaster(6, 6, 1, GDT_Float64, weights_gt);
    GDALDatasetUniquePtr out = CreateMemVector();

    CPLStringList options;
    options.AddNameValue("WEIGHTS_BAND", "1");
    options.AddNameValue("STATS", "weighted_mean");

    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                      GDALDataset::ToHandle(weights.get()),
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));
    EXPECT_THAT(capturing.messages(),
                HasSubstr("treated as having a weight of zero"));
    capturing.clear();
  }
}

// -----------------------------------------------------------------------------
// Suite 3: Raster Zones Processing
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, RasterZonesBasicStats) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16);
  for (int i = 0; i < 16; ++i) {
    src_data[i] = i + 1;
  }
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  // Zone 1 in rows 0-1, Zone 2 in rows 2-3.
  std::vector<double> zones_data = {1, 1, 1, 1, 1, 1, 1, 1,
                                    2, 2, 2, 2, 2, 2, 2, 2};
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STATS", "count,min,max,mean,sum");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  EXPECT_EQ(2, out_layer->GetFeatureCount());

  OGRFeatureUniquePtr feat1(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat1);
  EXPECT_DOUBLE_EQ(1.0, feat1->GetFieldAsDouble("value"));
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(1.0, feat1->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(36.0, feat1->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(4.5, feat1->GetFieldAsDouble("mean"));

  OGRFeatureUniquePtr feat2(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat2);
  EXPECT_DOUBLE_EQ(2.0, feat2->GetFieldAsDouble("value"));
  EXPECT_DOUBLE_EQ(8.0, feat2->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(9.0, feat2->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(16.0, feat2->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(100.0, feat2->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(12.5, feat2->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, RasterZonesCategoricalAndHistogramStats) {
  GDALDatasetUniquePtr src = CreateMemRaster(2, 2);
  std::vector<double> src_data = {10, 10, 20, 30};
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemRaster(2, 2);
  std::vector<double> zones_data = {1, 1, 1, 1};
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STATS", "mode,minority,variety,unique,frac");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  EXPECT_DOUBLE_EQ(10.0, feat->GetFieldAsDouble("mode"));
  EXPECT_EQ(3, feat->GetFieldAsInteger("variety"));

  int count = 0;
  const double* unique_vals = feat->GetFieldAsDoubleList("unique", &count);
  EXPECT_EQ(3, count);
  ASSERT_NE(nullptr, unique_vals);

  const double* frac_vals = feat->GetFieldAsDoubleList("frac", &count);
  EXPECT_EQ(3, count);
  ASSERT_NE(nullptr, frac_vals);
}

TEST_F(ZonalTest, RasterZonesAll26StatsWithWeights) {
  GDALDatasetUniquePtr src = CreateMemRaster(2, 2);
  std::vector<double> src_data = {1, 2, 3, 4};
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr weights = CreateMemRaster(2, 2);
  std::vector<double> weights_data = {1.0, 1.0, 2.0, 2.0};
  WriteBandData(weights->GetRasterBand(1), weights_data);

  GDALDatasetUniquePtr zones = CreateMemRaster(2, 2);
  std::vector<double> zones_data = {1, 1, 1, 1};
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue(
      "STATS",
      "center_x,center_y,count,coverage,frac,max,max_center_x,max_center_y,"
      "mean,min,min_center_x,min_center_y,minority,mode,stdev,sum,unique,"
      "values,variance,variety,weighted_frac,weighted_mean,weighted_sum,"
      "weighted_stdev,weighted_variance,weights");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  EXPECT_DOUBLE_EQ(4.0, feat->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(1.0, feat->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(4.0, feat->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(10.0, feat->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(2.5, feat->GetFieldAsDouble("mean"));
  EXPECT_GT(feat->GetFieldAsDouble("variance"), 0.0);
  EXPECT_GT(feat->GetFieldAsDouble("stdev"), 0.0);
  EXPECT_GT(feat->GetFieldAsDouble("weighted_sum"), 0.0);
  EXPECT_GT(feat->GetFieldAsDouble("weighted_mean"), 0.0);

  int list_count = 0;
  EXPECT_NE(nullptr, feat->GetFieldAsDoubleList("center_x", &list_count));
  EXPECT_EQ(4, list_count);
  EXPECT_NE(nullptr, feat->GetFieldAsDoubleList("center_y", &list_count));
  EXPECT_EQ(4, list_count);
  EXPECT_NE(nullptr, feat->GetFieldAsDoubleList("values", &list_count));
  EXPECT_EQ(4, list_count);
  EXPECT_NE(nullptr, feat->GetFieldAsDoubleList("weights", &list_count));
  EXPECT_EQ(4, list_count);
}

TEST_F(ZonalTest, RasterZonesMultipleBands) {
  GDALDatasetUniquePtr src = CreateMemRaster(2, 2, 2);
  std::vector<double> band1 = {10, 20, 30, 40};
  std::vector<double> band2 = {100, 200, 300, 400};
  WriteBandData(src->GetRasterBand(1), band1);
  WriteBandData(src->GetRasterBand(2), band2);

  GDALDatasetUniquePtr zones = CreateMemRaster(2, 2);
  std::vector<double> zones_data = {1, 1, 1, 1};
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("BANDS", "1,2");
  options.AddNameValue("STATS", "mean,sum");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  EXPECT_DOUBLE_EQ(25.0, feat->GetFieldAsDouble("mean_band_1"));
  EXPECT_DOUBLE_EQ(100.0, feat->GetFieldAsDouble("sum_band_1"));
  EXPECT_DOUBLE_EQ(250.0, feat->GetFieldAsDouble("mean_band_2"));
  EXPECT_DOUBLE_EQ(1000.0, feat->GetFieldAsDouble("sum_band_2"));
}

TEST_F(ZonalTest, RasterZonesNodataAndWeightsNodata) {
  GDALDatasetUniquePtr src = CreateMemRaster(2, 2);
  std::vector<double> src_data = {10, -9999, 20, 30};
  WriteBandData(src->GetRasterBand(1), src_data);
  src->GetRasterBand(1)->SetNoDataValue(-9999);

  GDALDatasetUniquePtr weights = CreateMemRaster(2, 2);
  std::vector<double> weights_data = {1, 1, -9999, 2};
  WriteBandData(weights->GetRasterBand(1), weights_data);
  weights->GetRasterBand(1)->SetNoDataValue(-9999);

  GDALDatasetUniquePtr zones = CreateMemRaster(2, 2);
  std::vector<double> zones_data = {1, 1, 1, 1};
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "count,sum,mean,weighted_sum");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  // Pixel 1 is nodata, so only 3 pixels processed.
  EXPECT_DOUBLE_EQ(3.0, feat->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(60.0, feat->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(20.0, feat->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, RasterZonesSmallChunkAndProgress) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16, 5.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  std::vector<double> zones_data(16, 1.0);
  WriteBandData(zones->GetRasterBand(1), zones_data);

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("RASTER_CHUNK_SIZE_BYTES", "16");
  options.AddNameValue("STATS", "mean,sum");

  int progress_calls = 0;
  EXPECT_EQ(CE_None,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           TrackingProgress, &progress_calls));

  EXPECT_GT(progress_calls, 0);
  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_DOUBLE_EQ(5.0, feat->GetFieldAsDouble("mean"));
  EXPECT_DOUBLE_EQ(80.0, feat->GetFieldAsDouble("sum"));
}

TEST_F(ZonalTest, RasterZonesResamplingWarnings) {
  const double src_gt[6] = {0.0, 2.0, 0.0, 10.0, 0.0, -2.0};
  const double zones_gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  const double weights_gt[6] = {0.0, 2.0, 0.0, 10.0, 0.0, -2.0};

  GDALDatasetUniquePtr src = CreateMemRaster(5, 5, 1, GDT_Float64, src_gt);
  GDALDatasetUniquePtr zones =
      CreateMemRaster(10, 10, 1, GDT_Float64, zones_gt);
  GDALDatasetUniquePtr weights =
      CreateMemRaster(5, 5, 1, GDT_Float64, weights_gt);
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("average resampling"));
}

TEST_F(ZonalTest, RasterZonesWeightsNoGeoTransform) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr weights(
      mem_drv->Create("", 4, 4, 1, GDT_Float64, nullptr));
  GDALDatasetUniquePtr out = CreateMemVector();

  CPLStringList options;
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                       GDALDataset::ToHandle(weights.get()),
                                       GDALDataset::ToHandle(zones.get()),
                                       GDALDataset::ToHandle(out.get()),
                                       options.List(), nullptr, nullptr));
}

// -----------------------------------------------------------------------------
// Suite 4: Vector Zones - Feature Sequential (STRATEGY=FEATURE_SEQUENTIAL)
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, VectorZonesFeatureSequentialBasic) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16);
  for (int i = 0; i < 16; ++i) {
    src_data[i] = i + 1;
  }
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  // Polygon covering top half: X in [0, 4], Y in [2, 4].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.0, 2.0, 4.0, 4.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // Polygon covering bottom half: X in [0, 4], Y in [0, 2].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 2.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("STATS", "count,min,max,mean,sum");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  EXPECT_EQ(2, out_layer->GetFeatureCount());

  OGRFeatureUniquePtr feat1(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat1);
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(1.0, feat1->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(36.0, feat1->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(4.5, feat1->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialIncludeFields) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16, 7.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFieldDefn f_id("zone_id", OFTInteger);
  OGRFieldDefn f_name("zone_name", OFTString);
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_id));
  EXPECT_EQ(OGRERR_NONE, layer->CreateField(&f_name));

  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  feat->SetField("zone_id", 42);
  feat->SetField("zone_name", "test_zone");
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("INCLUDE_FIELDS", "zone_id,zone_name");
  options.AddNameValue("STATS", "mean");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr out_feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, out_feat);
  EXPECT_EQ(42, out_feat->GetFieldAsInteger("zone_id"));
  EXPECT_STREQ("test_zone", out_feat->GetFieldAsString("zone_name"));
  EXPECT_DOUBLE_EQ(7.0, out_feat->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialPixelIntersections) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16, 2.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.75, 0.75, 3.25, 3.25);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  constexpr const char* kModes[] = {"DEFAULT", "ALL_TOUCHED", "ALL-TOUCHED",
                                    "FRACTIONAL"};

  for (const char* mode : kModes) {
    GDALDatasetUniquePtr out = CreateMemVector();
    CPLStringList options;
    options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
    options.AddNameValue("PIXEL_INTERSECTION", mode);
    options.AddNameValue("STATS", "count,mean");

    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));

    OGRLayer* out_layer = out->GetLayerByName("stats");
    ASSERT_NE(nullptr, out_layer);
    OGRFeatureUniquePtr out_feat(out_layer->GetNextFeature());
    ASSERT_NE(nullptr, out_feat);
    EXPECT_DOUBLE_EQ(2.0, out_feat->GetFieldAsDouble("mean"));
    if (std::string_view(mode) == "DEFAULT") {
      EXPECT_DOUBLE_EQ(4.0, out_feat->GetFieldAsDouble("count"));
    } else if (std::string_view(mode) == "FRACTIONAL") {
      EXPECT_NEAR(6.25, out_feat->GetFieldAsDouble("count"), 1e-6);
    } else {
      EXPECT_DOUBLE_EQ(16.0, out_feat->GetFieldAsDouble("count"));
    }
  }
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialComplexGeometries) {
  GDALDatasetUniquePtr src = CreateMemRaster(10, 10);
  std::vector<double> src_data(100, 1.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);

  // Polygon with hole: [1, 9] x [1, 9] with hole [3, 7] x [3, 7].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly_hole =
        MakePolygonWithHole(1.0, 1.0, 9.0, 9.0, 3.0, 3.0, 7.0, 7.0);
    feat->SetGeometryDirectly(poly_hole.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // MultiPolygon: [0, 2] x [0, 2] and [8, 10] x [8, 10].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    std::vector<OGRGeometryUniquePtr> parts;
    parts.push_back(MakePolygon(0.0, 0.0, 2.0, 2.0));
    parts.push_back(MakePolygon(8.0, 8.0, 10.0, 10.0));
    OGRGeometryUniquePtr mp = MakeMultiPolygon(std::move(parts));
    feat->SetGeometryDirectly(mp.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("STATS", "count,mean");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  ASSERT_EQ(2, out_layer->GetFeatureCount());
  out_layer->ResetReading();
  OGRFeatureUniquePtr f1(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_DOUBLE_EQ(48.0, f1->GetFieldAsDouble("count"));
  OGRFeatureUniquePtr f2(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_DOUBLE_EQ(8.0, f2->GetFieldAsDouble("count"));
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialNullAndNonPolygonal) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);

  // Feature with null geometry.
  {
    GDALDatasetUniquePtr zones = CreateMemVector();
    OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    // No geometry set.
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

    GDALDatasetUniquePtr out = CreateMemVector();
    CPLStringList options;
    options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
    options.AddNameValue("STATS", "count,mean,min,max");

    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));

    OGRLayer* out_layer = out->GetLayerByName("stats");
    ASSERT_NE(nullptr, out_layer);
    OGRFeatureUniquePtr out_feat(out_layer->GetNextFeature());
    ASSERT_NE(nullptr, out_feat);
    EXPECT_DOUBLE_EQ(0.0, out_feat->GetFieldAsDouble("count"));
  }

  // Feature with non-polygonal (Point) geometry.
  {
    GDALDatasetUniquePtr zones = CreateMemVector();
    OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPoint, nullptr);
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    auto point = std::make_unique<OGRPoint>(2.0, 2.0);
    feat->SetGeometryDirectly(point.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

    GDALDatasetUniquePtr out = CreateMemVector();
    CPLStringList options;
    options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Non-polygonal geometry encountered."));
  }
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialTrimWindowToRaster) {
  GDALDatasetUniquePtr src = CreateMemRaster(10, 10);
  std::vector<double> src_data(100, 3.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);

  // 1. Polygon extending left: X [-5, 5], Y [2, 8].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(-5.0, 2.0, 5.0, 8.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // 2. Polygon completely to the right: X [15, 20], Y [2, 8].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(15.0, 2.0, 20.0, 8.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // 3. Polygon overlapping top edge: X [2, 8], Y [5, 15].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(2.0, 5.0, 8.0, 15.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // 4. Polygon completely below: X [2, 8], Y [-10, -5].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(2.0, -10.0, 8.0, -5.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // 5. Polygon overlapping right edge: X [5, 15], Y [2, 8].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(5.0, 2.0, 15.0, 8.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // 6. Polygon overlapping bottom edge: X [2, 8], Y [-5, 5].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(2.0, -5.0, 8.0, 5.0);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("STATS", "count,mean");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  ASSERT_EQ(6, out_layer->GetFeatureCount());
  out_layer->ResetReading();
  const double expected_counts[] = {30.0, 0.0, 30.0, 0.0, 30.0, 30.0};
  for (double exp : expected_counts) {
    OGRFeatureUniquePtr f(out_layer->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_DOUBLE_EQ(exp, f->GetFieldAsDouble("count"));
  }
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialSmallChunkAndProgress) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16, 5.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("RASTER_CHUNK_SIZE_BYTES", "8");
  options.AddNameValue("STATS", "mean,sum");
  options.AddNameValue("LCO_ADVERTIZE_UTF8", "YES");

  int progress_calls = 0;
  EXPECT_EQ(CE_None,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           TrackingProgress, &progress_calls));

  EXPECT_GT(progress_calls, 0);
  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  EXPECT_EQ(TRUE, out_layer->TestCapability(OLCStringsAsUTF8));
  OGRFeatureUniquePtr out_feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, out_feat);
  EXPECT_DOUBLE_EQ(5.0, out_feat->GetFieldAsDouble("mean"));
  EXPECT_DOUBLE_EQ(80.0, out_feat->GetFieldAsDouble("sum"));
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialWeightsAndResampling) {
  const double src_gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  const double weights_gt[6] = {0.0, 2.0, 0.0, 4.0, 0.0, -2.0};

  GDALDatasetUniquePtr src = CreateMemRaster(4, 4, 1, GDT_Float64, src_gt);
  GDALDatasetUniquePtr weights =
      CreateMemRaster(2, 2, 1, GDT_Float64, weights_gt);
  std::vector<double> w_data = {1.0, 2.0, 3.0, 4.0};
  WriteBandData(weights->GetRasterBand(1), w_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean,weighted_sum");

  CapturingErrorHandler capturing;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));
  EXPECT_THAT(capturing.messages(), HasSubstr("Resampled weights"));
}

TEST_F(ZonalTest, VectorZonesFeatureSequentialWeightsNoGeoTransform) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr weights(
      mem_drv->Create("", 4, 4, 1, GDT_Float64, nullptr));

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                       GDALDataset::ToHandle(weights.get()),
                                       GDALDataset::ToHandle(zones.get()),
                                       GDALDataset::ToHandle(out.get()),
                                       options.List(), nullptr, nullptr));
}

// -----------------------------------------------------------------------------
// Suite 5: Vector Zones - Raster Sequential (STRATEGY=RASTER_SEQUENTIAL)
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, VectorZonesRasterSequentialBasic) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  std::vector<double> src_data(16);
  for (int i = 0; i < 16; ++i) {
    src_data[i] = i + 1;
  }
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);

  // Top half: X in [0.1, 3.9], Y in [2.1, 3.9].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.1, 2.1, 3.9, 3.9);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }
  // Bottom half: X in [0.1, 3.9], Y in [0.1, 1.9].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.1, 0.1, 3.9, 1.9);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  options.AddNameValue("STATS", "count,min,max,mean,sum");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  EXPECT_EQ(2, out_layer->GetFeatureCount());

  OGRFeatureUniquePtr feat1(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, feat1);
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(1.0, feat1->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(8.0, feat1->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(36.0, feat1->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(4.5, feat1->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, VectorZonesRasterSequentialTrimWindowBranches) {
  GDALDatasetUniquePtr src = CreateMemRaster(8, 8);
  std::vector<double> src_data(64, 2.0);
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);

  // Feature 1: spans across multiple chunks (X [2.1, 5.9], Y [2.1, 5.9]).
  // Exercises nXOff < trimWindow.nXOff, nXOff + nXSize > trimWindowXMax,
  // nYOff < trimWindow.nYOff, nYOff + nYSize > trimWindowYMax in TrimWindow.
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(2.1, 2.1, 5.9, 5.9);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  // Feature 2: tiny polygon located only in one corner [0.1, 0.9] x [7.1, 7.9].
  {
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.1, 7.1, 0.9, 7.9);
    feat->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  options.AddNameValue("RASTER_CHUNK_SIZE_BYTES", "32");  // 4 cells/chunk
  options.AddNameValue("STATS", "count,mean");

  int progress_calls = 0;
  EXPECT_EQ(CE_None,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), options.List(),
                           TrackingProgress, &progress_calls));

  EXPECT_GT(progress_calls, 0);
  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  ASSERT_EQ(2, out_layer->GetFeatureCount());
  out_layer->ResetReading();
  OGRFeatureUniquePtr f1(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, f1);
  EXPECT_DOUBLE_EQ(16.0, f1->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(2.0, f1->GetFieldAsDouble("mean"));
  OGRFeatureUniquePtr f2(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_DOUBLE_EQ(1.0, f2->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(2.0, f2->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, VectorZonesRasterSequentialAllStatsWithWeights) {
  GDALDatasetUniquePtr src = CreateMemRaster(2, 2);
  std::vector<double> src_data = {1, 2, 3, 4};
  WriteBandData(src->GetRasterBand(1), src_data);

  GDALDatasetUniquePtr weights = CreateMemRaster(2, 2);
  std::vector<double> weights_data = {1.0, 1.0, 2.0, 2.0};
  WriteBandData(weights->GetRasterBand(1), weights_data);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 2.0, 2.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue(
      "STATS",
      "center_x,center_y,count,coverage,frac,max,max_center_x,max_center_y,"
      "mean,min,min_center_x,min_center_y,minority,mode,stdev,sum,unique,"
      "values,variance,variety,weighted_frac,weighted_mean,weighted_sum,"
      "weighted_stdev,weighted_variance,weights");

  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));

  OGRLayer* out_layer = out->GetLayerByName("stats");
  ASSERT_NE(nullptr, out_layer);
  OGRFeatureUniquePtr out_feat(out_layer->GetNextFeature());
  ASSERT_NE(nullptr, out_feat);

  EXPECT_DOUBLE_EQ(4.0, out_feat->GetFieldAsDouble("count"));
  EXPECT_DOUBLE_EQ(1.0, out_feat->GetFieldAsDouble("min"));
  EXPECT_DOUBLE_EQ(4.0, out_feat->GetFieldAsDouble("max"));
  EXPECT_DOUBLE_EQ(10.0, out_feat->GetFieldAsDouble("sum"));
  EXPECT_DOUBLE_EQ(2.5, out_feat->GetFieldAsDouble("mean"));
}

TEST_F(ZonalTest, VectorZonesRasterSequentialNullAndNonPolygonal) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);

  // Feature with null geometry in RASTER_SEQUENTIAL (skipped in tree build).
  {
    GDALDatasetUniquePtr zones = CreateMemVector();
    OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
    // Null geometry feature.
    OGRFeatureUniquePtr feat1(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat1.get()));
    // Valid feature.
    OGRFeatureUniquePtr feat2(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
    feat2->SetGeometryDirectly(poly.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat2.get()));

    GDALDatasetUniquePtr out = CreateMemVector();
    CPLStringList options;
    options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
    options.AddNameValue("STATS", "count,mean");

    EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                                      GDALDataset::ToHandle(zones.get()),
                                      GDALDataset::ToHandle(out.get()),
                                      options.List(), nullptr, nullptr));

    OGRLayer* out_layer = out->GetLayerByName("stats");
    ASSERT_NE(nullptr, out_layer);
    EXPECT_EQ(2, out_layer->GetFeatureCount());
  }

  // Feature with non-polygonal (Point) geometry in RASTER_SEQUENTIAL.
  {
    GDALDatasetUniquePtr zones = CreateMemVector();
    OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPoint, nullptr);
    OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
    auto pt = std::make_unique<OGRPoint>(1.0, 1.0);
    feat->SetGeometryDirectly(pt.release());
    EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

    GDALDatasetUniquePtr out = CreateMemVector();
    CPLStringList options;
    options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Non-polygonal geometry encountered."));
  }
}

TEST_F(ZonalTest, VectorZonesRasterSequentialWeightsAndResampling) {
  const double src_gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  const double weights_gt[6] = {0.0, 2.0, 0.0, 4.0, 0.0, -2.0};

  GDALDatasetUniquePtr src = CreateMemRaster(4, 4, 1, GDT_Float64, src_gt);
  GDALDatasetUniquePtr weights =
      CreateMemRaster(2, 2, 1, GDT_Float64, weights_gt);

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean");

  CapturingErrorHandler capturing;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                    GDALDataset::ToHandle(weights.get()),
                                    GDALDataset::ToHandle(zones.get()),
                                    GDALDataset::ToHandle(out.get()),
                                    options.List(), nullptr, nullptr));
  EXPECT_THAT(capturing.messages(), HasSubstr("Resampled weights"));
}

TEST_F(ZonalTest, VectorZonesRasterSequentialWeightsNoGeoTransform) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr weights(
      mem_drv->Create("", 4, 4, 1, GDT_Float64, nullptr));

  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  CPLStringList options;
  options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  options.AddNameValue("WEIGHTS_BAND", "1");
  options.AddNameValue("STATS", "weighted_mean");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALZonalStats(GDALDataset::ToHandle(src.get()),
                                       GDALDataset::ToHandle(weights.get()),
                                       GDALDataset::ToHandle(zones.get()),
                                       GDALDataset::ToHandle(out.get()),
                                       options.List(), nullptr, nullptr));
}

// -----------------------------------------------------------------------------
// Suite 6: Custom Datasets for Hard-to-Reach Error Paths
// -----------------------------------------------------------------------------

TEST_F(ZonalTest, FailCreateLayerInOutputDataset) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  FailCreateLayerDataset fail_out;

  WithQuietHandler quiet;
  // In raster zones:
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(&fail_out), nullptr, nullptr,
                           nullptr));

  // In vector zones (feature sequential):
  GDALDatasetUniquePtr vec_zones = CreateMemVector();
  OGRLayer* layer =
      vec_zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  CPLStringList feat_options;
  feat_options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(vec_zones.get()),
                           GDALDataset::ToHandle(&fail_out),
                           feat_options.List(), nullptr, nullptr));

  // In vector zones (raster sequential):
  CPLStringList raster_options;
  raster_options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(vec_zones.get()),
                           GDALDataset::ToHandle(&fail_out),
                           raster_options.List(), nullptr, nullptr));
}

TEST_F(ZonalTest, FailCreateFieldInOutputDataset) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  FailCreateFieldDataset fail_out;

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(&fail_out), nullptr, nullptr,
                           nullptr));
}

TEST_F(ZonalTest, FailCreateFeatureInOutputDataset) {
  GDALDatasetUniquePtr src = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  FailCreateFeatureDataset fail_out;

  WithQuietHandler quiet;
  // In raster zones:
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(&fail_out), nullptr, nullptr,
                           nullptr));

  // In vector zones (feature sequential):
  GDALDatasetUniquePtr vec_zones = CreateMemVector();
  OGRLayer* layer =
      vec_zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  CPLStringList feat_options;
  feat_options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(vec_zones.get()),
                           GDALDataset::ToHandle(&fail_out),
                           feat_options.List(), nullptr, nullptr));

  // In vector zones (raster sequential):
  CPLStringList raster_options;
  raster_options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(src.get()), nullptr,
                           GDALDataset::ToHandle(vec_zones.get()),
                           GDALDataset::ToHandle(&fail_out),
                           raster_options.List(), nullptr, nullptr));
}

TEST_F(ZonalTest, FailReadWindowInRasterZones) {
  FailReadDataset fail_src(4, 4);
  GDALDatasetUniquePtr zones = CreateMemRaster(4, 4);
  GDALDatasetUniquePtr out = CreateMemVector();

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALZonalStats(GDALDataset::ToHandle(&fail_src), nullptr,
                           GDALDataset::ToHandle(zones.get()),
                           GDALDataset::ToHandle(out.get()), nullptr, nullptr,
                           nullptr));
}

TEST_F(ZonalTest, FailReadWindowInVectorZones) {
  FailReadDataset fail_src(4, 4);
  GDALDatasetUniquePtr zones = CreateMemVector();
  OGRLayer* layer = zones->CreateLayer("zones", nullptr, wkbPolygon, nullptr);
  OGRFeatureUniquePtr feat(OGRFeature::CreateFeature(layer->GetLayerDefn()));
  OGRGeometryUniquePtr poly = MakePolygon(0.0, 0.0, 4.0, 4.0);
  feat->SetGeometryDirectly(poly.release());
  EXPECT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));

  GDALDatasetUniquePtr out = CreateMemVector();
  WithQuietHandler quiet;

  // Feature sequential:
  {
    CPLStringList options;
    options.AddNameValue("STRATEGY", "FEATURE_SEQUENTIAL");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(&fail_src), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
  }

  // Raster sequential:
  {
    CPLStringList options;
    options.AddNameValue("STRATEGY", "RASTER_SEQUENTIAL");
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALZonalStats(GDALDataset::ToHandle(&fail_src), nullptr,
                             GDALDataset::ToHandle(zones.get()),
                             GDALDataset::ToHandle(out.get()), options.List(),
                             nullptr, nullptr));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and defensive
// error paths.
