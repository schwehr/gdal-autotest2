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
// Tests contour generation algorithms (alg/contour.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/contour.cpp

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr std::array<double, 6> kGeoTransform = {1000.0, 10.0, 0.0,
                                                 2000.0, 0.0,  -10.0};

constexpr std::array<double, 25> kHill5x5 = {
    0.0, 0.0,  0.0,   0.0,  0.0,  // Row 0
    0.0, 50.0, 50.0,  50.0, 0.0,  // Row 1
    0.0, 50.0, 100.0, 50.0, 0.0,  // Row 2
    0.0, 50.0, 50.0,  50.0, 0.0,  // Row 3
    0.0, 0.0,  0.0,   0.0,  0.0,  // Row 4
};

constexpr std::array<double, 25> kValley5x5 = {
    100.0, 100.0, 100.0, 100.0, 100.0,  // Row 0
    100.0, 50.0,  50.0,  50.0,  100.0,  // Row 1
    100.0, 50.0,  0.0,   50.0,  100.0,  // Row 2
    100.0, 50.0,  50.0,  50.0,  100.0,  // Row 3
    100.0, 100.0, 100.0, 100.0, 100.0,  // Row 4
};

constexpr std::array<double, 16> kSaddle4x4 = {
    100.0, 0.0,  0.0,  100.0,  // Row 0
    0.0,   50.0, 50.0, 0.0,    // Row 1
    0.0,   50.0, 50.0, 0.0,    // Row 2
    100.0, 0.0,  0.0,  100.0,  // Row 3
};

constexpr std::array<double, 25> kDonut5x5 = {
    100.0, 100.0, 100.0, 100.0, 100.0,  // Row 0
    100.0, 0.0,   0.0,   0.0,   100.0,  // Row 1
    100.0, 0.0,   100.0, 0.0,   100.0,  // Row 2
    100.0, 0.0,   0.0,   0.0,   100.0,  // Row 3
    100.0, 100.0, 100.0, 100.0, 100.0,  // Row 4
};

constexpr std::array<double, 16> kFlat4x4 = {
    50.0, 50.0, 50.0, 50.0,  // Row 0
    50.0, 50.0, 50.0, 50.0,  // Row 1
    50.0, 50.0, 50.0, 50.0,  // Row 2
    50.0, 50.0, 50.0, 50.0,  // Row 3
};

constexpr std::array<double, 1> kSinglePixel1x1 = {42.0};

constexpr std::array<double, 16> kExponential4x4 = {
    1.0, 2.0,  4.0,  8.0,   // Row 0
    2.0, 4.0,  8.0,  16.0,  // Row 1
    4.0, 8.0,  16.0, 32.0,  // Row 2
    8.0, 16.0, 32.0, 64.0,  // Row 3
};

constexpr double kNoDataValue = -9999.0;
constexpr std::array<double, 25> kNodata5x5 = {
    kNoDataValue, kNoDataValue, 100.0, 100.0, 100.0,         // Row 0
    kNoDataValue, 50.0,         50.0,  50.0,  100.0,         // Row 1
    100.0,        50.0,         10.0,  50.0,  100.0,         // Row 2
    100.0,        50.0,         50.0,  50.0,  100.0,         // Row 3
    100.0,        100.0,        100.0, 100.0, kNoDataValue,  // Row 4
};

// Raster band whose IReadBlock fails with CE_Failure.
class FailRasterBand final : public GDALRasterBand {
 public:
  FailRasterBand(int x_size, int y_size) {
    poDS = nullptr;
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

// Standalone raster band without an associated GDALDataset.
class StandaloneRasterBand final : public GDALRasterBand {
 public:
  StandaloneRasterBand(int x_size, int y_size, double val) : value_(val) {
    poDS = nullptr;
    nBand = 1;
    eDataType = GDT_Float64;
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    nBlockXSize = x_size;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void* image) override {
    double* line = static_cast<double*>(image);
    std::fill_n(line, nBlockXSize, value_);
    return CE_None;
  }

 private:
  double value_ = 0.0;
};

// Custom layer that fails on CreateFeature.
class FailCreateFeatureLayer final : public OGRLayer {
 public:
  FailCreateFeatureLayer() {
    feature_defn_ = new OGRFeatureDefn("fail_layer");
    feature_defn_->Reference();
  }

  ~FailCreateFeatureLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }

  OGRErr ICreateFeature(OGRFeature*) override {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated CreateFeature failure");
    return OGRERR_FAILURE;
  }

  int TestCapability(const char*) const override { return FALSE; }

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

// Custom layer that can fail StartTransaction or CommitTransaction.
class FailTransactionLayer final : public OGRLayer {
 public:
  explicit FailTransactionLayer(bool fail_start, bool fail_commit)
      : fail_start_(fail_start), fail_commit_(fail_commit) {
    feature_defn_ = new OGRFeatureDefn("fail_tx_layer");
    feature_defn_->Reference();
  }

  ~FailTransactionLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }

  OGRErr ICreateFeature(OGRFeature*) override { return OGRERR_NONE; }
  int TestCapability(const char*) const override { return FALSE; }

  OGRErr StartTransaction() override {
    if (fail_start_) {
      return OGRERR_FAILURE;
    }
    return OGRERR_NONE;
  }

  OGRErr CommitTransaction() override {
    ++commit_count_;
    if (fail_commit_) {
      return OGRERR_FAILURE;
    }
    return OGRERR_NONE;
  }

  int commit_count() const { return commit_count_; }

 private:
  bool fail_start_ = false;
  bool fail_commit_ = false;
  int commit_count_ = 0;
  OGRFeatureDefn* feature_defn_ = nullptr;
};

struct MemLayerContext {
  GDALDatasetUniquePtr ds;
  OGRLayer* layer = nullptr;
  int id_field = -1;
  int elev_field = -1;
  int elev_min_field = -1;
  int elev_max_field = -1;
};

class ContourTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  GDALDriver* GetMemDriver() {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
    EXPECT_NE(nullptr, driver);
    return driver;
  }

  MemLayerContext CreateMemLayer(const char* name,
                                 OGRwkbGeometryType geom_type = wkbLineString,
                                 bool add_id = false, bool add_elev = false,
                                 bool add_elev_min_max = false) {
    GDALDriver* mem_drv = GetMemDriver();
    EXPECT_NE(nullptr, mem_drv);
    MemLayerContext ctx;
    ctx.ds = GDALDatasetUniquePtr(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ctx.ds);
    ctx.layer = ctx.ds->CreateLayer(name, nullptr, geom_type, nullptr);
    EXPECT_NE(nullptr, ctx.layer);
    if (add_id) {
      OGRFieldDefn field_defn("ID", OFTInteger);
      EXPECT_EQ(OGRERR_NONE, ctx.layer->CreateField(&field_defn));
      ctx.id_field = ctx.layer->GetLayerDefn()->GetFieldIndex("ID");
    }
    if (add_elev) {
      OGRFieldDefn field_defn("ELEV", OFTReal);
      EXPECT_EQ(OGRERR_NONE, ctx.layer->CreateField(&field_defn));
      ctx.elev_field = ctx.layer->GetLayerDefn()->GetFieldIndex("ELEV");
    }
    if (add_elev_min_max) {
      OGRFieldDefn min_defn("ELEV_MIN", OFTReal);
      EXPECT_EQ(OGRERR_NONE, ctx.layer->CreateField(&min_defn));
      ctx.elev_min_field = ctx.layer->GetLayerDefn()->GetFieldIndex("ELEV_MIN");
      OGRFieldDefn max_defn("ELEV_MAX", OFTReal);
      EXPECT_EQ(OGRERR_NONE, ctx.layer->CreateField(&max_defn));
      ctx.elev_max_field = ctx.layer->GetLayerDefn()->GetFieldIndex("ELEV_MAX");
    }
    return ctx;
  }

  GDALDatasetUniquePtr CreateMemRaster(int width, int height,
                                       std::span<const double> data,
                                       GDALDataType data_type = GDT_Float64,
                                       const double* geotransform = nullptr) {
    GDALDriver* mem_drv = GetMemDriver();
    EXPECT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr ds(
        mem_drv->Create("", width, height, 1, data_type, nullptr));
    EXPECT_NE(nullptr, ds);
    if (geotransform != nullptr) {
      EXPECT_EQ(CE_None,
                ds->SetGeoTransform(const_cast<double*>(geotransform)));
    }
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_NE(nullptr, band);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<double*>(data.data()), width,
                                      height, GDT_Float64, 0, 0));
    return ds;
  }
};

// 1. Verify error return on NULL input band or NULL generator handle.
TEST_F(ContourTest, NullPointersValidation) {
  MemLayerContext ctx = CreateMemLayer("null_pointers");
  ASSERT_NE(nullptr, ctx.layer);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALContourGenerateEx(nullptr, OGRLayer::ToHandle(ctx.layer),
                                  nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  std::array<double, 3> scanline = {1.0, 2.0, 3.0};
  EXPECT_EQ(CE_Failure, GDAL_CG_FeedLine(nullptr, scanline.data()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 2. Verify error handling for invalid options (non-positive interval, unsorted
// fixed levels).
TEST_F(ContourTest, InvalidOptionsValidation) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("invalid_options");
  ASSERT_NE(nullptr, ctx.layer);

  // LEVEL_INTERVAL must be strictly positive.
  const char* invalid_intervals[] = {"0", "-5.0", "nan", "abc"};
  for (const char* val : invalid_intervals) {
    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", val);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                    OGRLayer::ToHandle(ctx.layer),
                                    options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("strictly positive"));
    CPLErrorReset();
  }

  // FIXED_LEVELS must be strictly increasing.
  const char* unsorted_levels[] = {"50,25", "10,nan,20", "10,10,-5"};
  for (const char* val : unsorted_levels) {
    CPLStringList options;
    options.AddNameValue("FIXED_LEVELS", val);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                    OGRLayer::ToHandle(ctx.layer),
                                    options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("strictly increasing"));
    CPLErrorReset();
  }
}

// 3. Verify no-op behavior with empty options.
TEST_F(ContourTest, EmptyOptionsNoOp) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("empty_options");
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           nullptr, nullptr, nullptr));
  EXPECT_EQ(0, ctx.layer->GetFeatureCount(TRUE));
}

// 4. Verify progress reporting and cancellation.
TEST_F(ContourTest, ProgressCallbackAndCancellation) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "25");

  // 1. Cancellation via returning FALSE.
  {
    MemLayerContext ctx = CreateMemLayer("progress_cancel");
    ASSERT_NE(nullptr, ctx.layer);

    auto cancel_cb = [](double /*dfComplete*/, const char* /*pszMessage*/,
                        void* /*pData*/) -> int { return FALSE; };

    EXPECT_EQ(CE_Failure,
              GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                    OGRLayer::ToHandle(ctx.layer),
                                    options.List(), cancel_cb, nullptr));
  }

  // 2. Normal completion reporting dfComplete = 1.0.
  {
    MemLayerContext ctx = CreateMemLayer("progress_ok");
    ASSERT_NE(nullptr, ctx.layer);

    struct ProgressRecord {
      int count = 0;
      double last_val = -1.0;
    } record;

    auto ok_cb = [](double dfComplete, const char* /*pszMessage*/,
                    void* pData) -> int {
      auto* r = static_cast<ProgressRecord*>(pData);
      r->count++;
      r->last_val = dfComplete;
      return TRUE;
    };

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), ok_cb, &record));
    EXPECT_GT(record.count, 0);
    EXPECT_DOUBLE_EQ(1.0, record.last_val);
  }
}

// 5. Line contouring with fixed interval, geotransform, ID and elevation
// fields.
TEST_F(ContourTest, LineContourFixedInterval2D) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("line_interval_2d", wkbLineString,
                                       /*add_id=*/true, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "25.0");
  options.AddNameValue("LEVEL_BASE", "0.0");
  options.AddNameValue("ID_FIELD", std::to_string(ctx.id_field).c_str());
  options.AddNameValue("ELEV_FIELD", std::to_string(ctx.elev_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));

  const GIntBig feat_count = ctx.layer->GetFeatureCount(TRUE);
  EXPECT_GT(feat_count, 0);

  ctx.layer->ResetReading();
  int expected_id = 0;
  for (GIntBig i = 0; i < feat_count; ++i) {
    OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(expected_id++, feat->GetFieldAsInteger(ctx.id_field));
    const double elev = feat->GetFieldAsDouble(ctx.elev_field);
    EXPECT_TRUE(elev == 0.0 || elev == 25.0 || elev == 50.0 || elev == 75.0 ||
                elev == 100.0);

    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbLineString, geom->getGeometryType());
    OGRLineString* ls = geom->toLineString();
    ASSERT_NE(nullptr, ls);
    EXPECT_GE(ls->getNumPoints(), 2);

    for (int p = 0; p < ls->getNumPoints(); ++p) {
      EXPECT_GE(ls->getX(p), 1000.0);
      EXPECT_LE(ls->getX(p), 1050.0);
      EXPECT_GE(ls->getY(p), 1950.0);
      EXPECT_LE(ls->getY(p), 2000.0);
    }
  }
}

// 6. 3D line contouring with Z coordinate matching elevation.
TEST_F(ContourTest, LineContour3D) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("line_3d", wkbLineString25D,
                                       /*add_id=*/false, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "50.0");
  options.AddNameValue("ELEV_FIELD", std::to_string(ctx.elev_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));

  const GIntBig feat_count = ctx.layer->GetFeatureCount(TRUE);
  EXPECT_GT(feat_count, 0);

  ctx.layer->ResetReading();
  for (GIntBig i = 0; i < feat_count; ++i) {
    OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const double elev = feat->GetFieldAsDouble(ctx.elev_field);

    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbLineString25D, geom->getGeometryType());
    OGRLineString* ls = geom->toLineString();
    ASSERT_NE(nullptr, ls);
    for (int p = 0; p < ls->getNumPoints(); ++p) {
      EXPECT_DOUBLE_EQ(elev, ls->getZ(p));
    }
  }
}

// 7. Line contouring with FIXED_LEVELS list including MIN and MAX tokens.
TEST_F(ContourTest, LineContourFixedLevelsMinMax) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("fixed_min_max", wkbLineString,
                                       /*add_id=*/false, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("FIXED_LEVELS", "MIN,25.0,75.0,MAX");
  options.AddNameValue("ELEV_FIELD", std::to_string(ctx.elev_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

// 8. Line contouring with exponential levels (LEVEL_EXP_BASE).
TEST_F(ContourTest, LineContourExponentialLevels) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(4, 4, kExponential4x4);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("line_exp", wkbLineString,
                                       /*add_id=*/false, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_EXP_BASE", "2.0");
  options.AddNameValue("ELEV_FIELD", std::to_string(ctx.elev_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

// 9. Nodata handling on Float32 raster (exercises value clamping).
TEST_F(ContourTest, LineContourNodataFloat32) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kNodata5x5, GDT_Float32, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("nodata_f32", wkbLineString);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "25.0");
  options.AddNameValue("NODATA", std::to_string(kNoDataValue).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

// 10. Nodata handling on Float64 raster.
TEST_F(ContourTest, LineContourNodataFloat64) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kNodata5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("nodata_f64", wkbLineString);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "25.0");
  options.AddNameValue("NODATA", std::to_string(kNoDataValue).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

// 11. Polygon contouring with fixed interval, ID, ELEV_MIN, and ELEV_MAX.
TEST_F(ContourTest, PolygonContourFixedInterval2D) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("poly_interval_2d", wkbMultiPolygon,
                                       /*add_id=*/true, /*add_elev=*/false,
                                       /*add_elev_min_max=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("POLYGONIZE", "YES");
  options.AddNameValue("LEVEL_INTERVAL", "50.0");
  options.AddNameValue("LEVEL_BASE", "0.0");
  options.AddNameValue("ID_FIELD", std::to_string(ctx.id_field).c_str());
  options.AddNameValue("ELEV_FIELD_MIN",
                       std::to_string(ctx.elev_min_field).c_str());
  options.AddNameValue("ELEV_FIELD_MAX",
                       std::to_string(ctx.elev_max_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));

  const GIntBig feat_count = ctx.layer->GetFeatureCount(TRUE);
  EXPECT_GT(feat_count, 0);

  ctx.layer->ResetReading();
  int expected_id = 0;
  for (GIntBig i = 0; i < feat_count; ++i) {
    OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_EQ(expected_id++, feat->GetFieldAsInteger(ctx.id_field));
    const double min_elev = feat->GetFieldAsDouble(ctx.elev_min_field);
    const double max_elev = feat->GetFieldAsDouble(ctx.elev_max_field);
    EXPECT_LE(min_elev, max_elev);

    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbMultiPolygon, geom->getGeometryType());
  }
}

// 12. 3D polygon contouring with Z coordinate matching max elevation.
TEST_F(ContourTest, PolygonContour3D) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("poly_3d", wkbMultiPolygon25D,
                                       /*add_id=*/false, /*add_elev=*/false,
                                       /*add_elev_min_max=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("POLYGONIZE", "YES");
  options.AddNameValue("LEVEL_INTERVAL", "50.0");
  options.AddNameValue("ELEV_FIELD_MIN",
                       std::to_string(ctx.elev_min_field).c_str());
  options.AddNameValue("ELEV_FIELD_MAX",
                       std::to_string(ctx.elev_max_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));

  const GIntBig feat_count = ctx.layer->GetFeatureCount(TRUE);
  EXPECT_GT(feat_count, 0);

  ctx.layer->ResetReading();
  for (GIntBig i = 0; i < feat_count; ++i) {
    OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    const double max_elev = feat->GetFieldAsDouble(ctx.elev_max_field);

    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbMultiPolygon25D, geom->getGeometryType());
    OGRMultiPolygon* mp = geom->toMultiPolygon();
    ASSERT_NE(nullptr, mp);
    for (int p = 0; p < mp->getNumGeometries(); ++p) {
      const OGRPolygon* poly = mp->getGeometryRef(p);
      const OGRLinearRing* ext = poly->getExteriorRing();
      if (ext != nullptr && ext->getNumPoints() > 0) {
        EXPECT_DOUBLE_EQ(max_elev, ext->getZ(0));
      }
    }
  }
}

// 13. Polygon contouring with holes (interior rings).
TEST_F(ContourTest, PolygonContourWithHoles) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kDonut5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("poly_holes", wkbMultiPolygon,
                                       /*add_id=*/true, /*add_elev=*/false,
                                       /*add_elev_min_max=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("POLYGONIZE", "YES");
  options.AddNameValue("FIXED_LEVELS", "MIN,25.0,75.0,MAX");
  options.AddNameValue("ID_FIELD", std::to_string(ctx.id_field).c_str());
  options.AddNameValue("ELEV_FIELD_MIN",
                       std::to_string(ctx.elev_min_field).c_str());
  options.AddNameValue("ELEV_FIELD_MAX",
                       std::to_string(ctx.elev_max_field).c_str());

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));

  const GIntBig feat_count = ctx.layer->GetFeatureCount(TRUE);
  EXPECT_GT(feat_count, 0);

  bool found_interior_ring = false;
  ctx.layer->ResetReading();
  for (GIntBig i = 0; i < feat_count; ++i) {
    OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    OGRMultiPolygon* mp = geom->toMultiPolygon();
    ASSERT_NE(nullptr, mp);
    for (int p = 0; p < mp->getNumGeometries(); ++p) {
      const OGRPolygon* poly = mp->getGeometryRef(p);
      if (poly->getNumInteriorRings() > 0) {
        found_interior_ring = true;
      }
    }
  }
  EXPECT_TRUE(found_interior_ring);
}

// 14. Boundary condition adjustments for FIXED_LEVELS in polygon mode.
TEST_F(ContourTest, PolygonContourFixedLevelsBoundaryConditions) {
  std::vector<double> data = {
      25.0, 50.0, 75.0,  // Row 0
      50.0, 75.0, 50.0,  // Row 1
      75.0, 50.0, 25.0,  // Row 2
  };
  GDALDatasetUniquePtr src_ds = CreateMemRaster(3, 3, data);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // 1. fixedLevels[0] < dfMinimum and maxLevel > dfMaximum.
  {
    MemLayerContext ctx = CreateMemLayer("boundary_out", wkbMultiPolygon);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("POLYGONIZE", "YES");
    options.AddNameValue("FIXED_LEVELS", "10.0,20.0,30.0,70.0,80.0,90.0");

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }

  // 2. maxLevel == dfMaximum (exercises std::nextafter branch).
  {
    MemLayerContext ctx = CreateMemLayer("boundary_equal", wkbMultiPolygon);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("POLYGONIZE", "YES");
    options.AddNameValue("FIXED_LEVELS", "25.0,50.0,75.0");

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }
}

// 15. Polygon contouring with exponential levels.
TEST_F(ContourTest, PolygonContourExponentialLevels) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(4, 4, kExponential4x4);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("poly_exp", wkbMultiPolygon);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("POLYGONIZE", "YES");
  options.AddNameValue("LEVEL_EXP_BASE", "2.0");

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

// 16. Transaction handling with COMMIT_INTERVAL.
TEST_F(ContourTest, TransactionsCommitInterval) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // 1. Positive COMMIT_INTERVAL in polygon mode.
  {
    MemLayerContext ctx = CreateMemLayer("tx_poly_interval", wkbMultiPolygon);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("POLYGONIZE", "YES");
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    options.AddNameValue("COMMIT_INTERVAL", "1");

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }

  // 2. Negative COMMIT_INTERVAL in line mode (single transaction).
  {
    MemLayerContext ctx = CreateMemLayer("tx_line_single", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    options.AddNameValue("COMMIT_INTERVAL", "-1");

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }

  // 3. StartTransaction failure resets commit interval.
  {
    FailTransactionLayer fail_tx_layer(/*fail_start=*/true,
                                       /*fail_commit=*/false);
    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    options.AddNameValue("COMMIT_INTERVAL", "5");

    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(&fail_tx_layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_EQ(0, fail_tx_layer.commit_count());
  }

  // 4. CommitTransaction failure causes failure return.
  {
    FailTransactionLayer fail_tx_layer(/*fail_start=*/false,
                                       /*fail_commit=*/true);
    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    options.AddNameValue("COMMIT_INTERVAL", "-1");

    EXPECT_EQ(CE_Failure,
              GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                    OGRLayer::ToHandle(&fail_tx_layer),
                                    options.List(), nullptr, nullptr));
  }
}

// 17. Various DEM surfaces: valleys, saddles, flat terrain, single pixel, out
// of bounds, and NaNs.
TEST_F(ContourTest, SurfacesVariations) {
  // 1. Valley DEM.
  {
    GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kValley5x5);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    MemLayerContext ctx = CreateMemLayer("valley", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }

  // 2. Saddle DEM.
  {
    GDALDatasetUniquePtr src_ds = CreateMemRaster(4, 4, kSaddle4x4);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    MemLayerContext ctx = CreateMemLayer("saddle", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
  }

  // 3. Flat DEM (line contour produces 0 features, polygon produces 1
  // polygon).
  {
    GDALDatasetUniquePtr src_ds = CreateMemRaster(4, 4, kFlat4x4);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    // Line contour.
    {
      MemLayerContext ctx = CreateMemLayer("flat_line", wkbLineString);
      ASSERT_NE(nullptr, ctx.layer);

      CPLStringList options;
      options.AddNameValue("LEVEL_INTERVAL", "25.0");
      EXPECT_EQ(CE_None,
                GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                      OGRLayer::ToHandle(ctx.layer),
                                      options.List(), nullptr, nullptr));
      EXPECT_EQ(0, ctx.layer->GetFeatureCount(TRUE));
    }

    // Polygon contour.
    {
      MemLayerContext ctx = CreateMemLayer("flat_poly", wkbMultiPolygon);
      ASSERT_NE(nullptr, ctx.layer);

      CPLStringList options;
      options.AddNameValue("POLYGONIZE", "YES");
      options.AddNameValue("FIXED_LEVELS", "25.0,75.0");
      EXPECT_EQ(CE_None,
                GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                      OGRLayer::ToHandle(ctx.layer),
                                      options.List(), nullptr, nullptr));
      EXPECT_EQ(1, ctx.layer->GetFeatureCount(TRUE));
    }
  }

  // 4. Single pixel DEM (1x1).
  {
    GDALDatasetUniquePtr src_ds = CreateMemRaster(1, 1, kSinglePixel1x1);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    MemLayerContext ctx = CreateMemLayer("single_pixel", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "10.0");
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_EQ(0, ctx.layer->GetFeatureCount(TRUE));
  }

  // 5. Out of bounds levels (all levels above or below DEM values).
  {
    std::vector<double> data = {10.0, 15.0, 15.0, 20.0};
    GDALDatasetUniquePtr src_ds = CreateMemRaster(2, 2, data);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    MemLayerContext ctx = CreateMemLayer("out_of_bounds", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("FIXED_LEVELS", "50.0,60.0");
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(ctx.layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_EQ(0, ctx.layer->GetFeatureCount(TRUE));
  }

  // 6. NaN and Inf handling.
  {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double pinf = std::numeric_limits<double>::infinity();
    const double ninf = -std::numeric_limits<double>::infinity();
    std::vector<double> nan_inf = {
        qnan, 10.0, 20.0, pinf,  // Row 0
        15.0, 25.0, 35.0, 45.0,  // Row 1
        ninf, 30.0, 40.0, qnan,  // Row 2
        50.0, pinf, 60.0, 70.0,  // Row 3
    };
    GDALDatasetUniquePtr src_ds = CreateMemRaster(4, 4, nan_inf);
    ASSERT_NE(nullptr, src_ds);
    GDALRasterBand* band = src_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    MemLayerContext ctx = CreateMemLayer("nan_inf", wkbLineString);
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "20.0");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                    OGRLayer::ToHandle(ctx.layer),
                                    options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("too many levels"));
    CPLErrorReset();
  }
}

// 18. Legacy GDALContourGenerate C API covering fixed levels and intervals.
TEST_F(ContourTest, LegacyGDALContourGenerateFixedLevels) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("legacy_fixed", wkbLineString,
                                       /*add_id=*/true, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  // Test with nFixedLevelCount = 2 (exercises comma joining and trailing
  // element).
  std::array<double, 2> levels = {25.0, 75.0};
  EXPECT_EQ(CE_None,
            GDALContourGenerate(GDALRasterBand::ToHandle(band),
                                /*dfContourInterval=*/0.0,
                                /*dfContourBase=*/0.0,
                                /*nFixedLevelCount=*/2, levels.data(),
                                /*bUseNoData=*/FALSE, /*dfNoDataValue=*/0.0,
                                OGRLayer::ToHandle(ctx.layer), ctx.id_field,
                                ctx.elev_field, nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

TEST_F(ContourTest, LegacyGDALContourGenerateSingleFixedLevel) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("legacy_single_fixed", wkbLineString,
                                       /*add_id=*/false, /*add_elev=*/false);
  ASSERT_NE(nullptr, ctx.layer);

  // Test with nFixedLevelCount = 1 (exercises single-element loop path).
  std::array<double, 1> levels = {50.0};
  EXPECT_EQ(CE_None,
            GDALContourGenerate(GDALRasterBand::ToHandle(band),
                                /*dfContourInterval=*/0.0,
                                /*dfContourBase=*/0.0,
                                /*nFixedLevelCount=*/1, levels.data(),
                                /*bUseNoData=*/FALSE, /*dfNoDataValue=*/0.0,
                                OGRLayer::ToHandle(ctx.layer), -1, -1, nullptr,
                                nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

TEST_F(ContourTest, LegacyGDALContourGenerateIntervalAndBaseAndNoData) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("legacy_interval", wkbLineString,
                                       /*add_id=*/true, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None,
            GDALContourGenerate(GDALRasterBand::ToHandle(band),
                                /*dfContourInterval=*/25.0,
                                /*dfContourBase=*/5.0,
                                /*nFixedLevelCount=*/0, nullptr,
                                /*bUseNoData=*/TRUE, /*dfNoDataValue=*/-999.0,
                                OGRLayer::ToHandle(ctx.layer), ctx.id_field,
                                ctx.elev_field, nullptr, nullptr));
  EXPECT_GT(ctx.layer->GetFeatureCount(TRUE), 0);
}

TEST_F(ContourTest, LegacyGDALContourGenerateAllDisabled) {
  GDALDatasetUniquePtr src_ds =
      CreateMemRaster(5, 5, kHill5x5, GDT_Float64, kGeoTransform.data());
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  MemLayerContext ctx = CreateMemLayer("legacy_none", wkbLineString,
                                       /*add_id=*/false, /*add_elev=*/false);
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None,
            GDALContourGenerate(GDALRasterBand::ToHandle(band),
                                /*dfContourInterval=*/0.0,
                                /*dfContourBase=*/0.0,
                                /*nFixedLevelCount=*/0, nullptr,
                                /*bUseNoData=*/FALSE, /*dfNoDataValue=*/0.0,
                                OGRLayer::ToHandle(ctx.layer), -1, -1, nullptr,
                                nullptr));
  EXPECT_EQ(0, ctx.layer->GetFeatureCount(TRUE));
}

// 19. ContourGenerator C API (GDAL_CG_Create, GDAL_CG_FeedLine,
// GDAL_CG_Destroy).
TEST_F(ContourTest, ContourGeneratorCAPISuccess) {
  struct RecordedLine {
    double level;
    int point_count;
  };
  std::vector<RecordedLine> lines;

  auto writer_cb = [](double dfLevel, int nPoints, double* /*padfX*/,
                      double* /*padfY*/, void* pData) -> CPLErr {
    auto* recorded = static_cast<std::vector<RecordedLine>*>(pData);
    recorded->push_back(RecordedLine{.level = dfLevel, .point_count = nPoints});
    return CE_None;
  };

  GDALContourGeneratorH cg = GDAL_CG_Create(
      /*nWidth=*/3, /*nHeight=*/3, /*bNoDataSet=*/FALSE, /*dfNoDataValue=*/0.0,
      /*dfContourInterval=*/10.0, /*dfContourBase=*/0.0, writer_cb, &lines);
  ASSERT_NE(nullptr, cg);
  absl::Cleanup cg_cleaner = [cg] { GDAL_CG_Destroy(cg); };

  std::array<double, 3> row0 = {0.0, 0.0, 0.0};
  std::array<double, 3> row1 = {0.0, 25.0, 0.0};
  std::array<double, 3> row2 = {0.0, 0.0, 0.0};
  // Extra row to test lineIdx_ > height_ branch.
  std::array<double, 3> row_extra = {0.0, 0.0, 0.0};

  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row0.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row1.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row2.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row_extra.data()));

  EXPECT_FALSE(lines.empty());
}

TEST_F(ContourTest, ContourGeneratorCAPIWithNoData) {
  auto writer_cb = [](double /*dfLevel*/, int /*nPoints*/, double* /*padfX*/,
                      double* /*padfY*/,
                      void* /*pData*/) -> CPLErr { return CE_None; };

  GDALContourGeneratorH cg = GDAL_CG_Create(
      /*nWidth=*/3, /*nHeight=*/3, /*bNoDataSet=*/TRUE,
      /*dfNoDataValue=*/-999.0, /*dfContourInterval=*/10.0,
      /*dfContourBase=*/0.0, writer_cb, nullptr);
  ASSERT_NE(nullptr, cg);
  absl::Cleanup cg_cleaner = [cg] { GDAL_CG_Destroy(cg); };

  std::array<double, 3> row0 = {-999.0, -999.0, -999.0};
  std::array<double, 3> row1 = {0.0, 20.0, 0.0};
  std::array<double, 3> row2 = {0.0, 0.0, 0.0};

  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row0.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row1.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row2.data()));
}

TEST_F(ContourTest, ContourGeneratorCAPIWriterFails) {
  auto failing_cb = [](double /*dfLevel*/, int /*nPoints*/, double* /*padfX*/,
                       double* /*padfY*/,
                       void* /*pData*/) -> CPLErr { return CE_Failure; };

  GDALContourGeneratorH cg = GDAL_CG_Create(
      /*nWidth=*/3, /*nHeight=*/3, /*bNoDataSet=*/FALSE, /*dfNoDataValue=*/0.0,
      /*dfContourInterval=*/10.0, /*dfContourBase=*/0.0, failing_cb, nullptr);
  ASSERT_NE(nullptr, cg);
  absl::Cleanup cg_cleaner = [cg] { GDAL_CG_Destroy(cg); };

  std::array<double, 3> row0 = {0.0, 0.0, 0.0};
  std::array<double, 3> row1 = {0.0, 25.0, 0.0};
  std::array<double, 3> row2 = {0.0, 0.0, 0.0};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row0.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row1.data()));
  EXPECT_EQ(CE_None, GDAL_CG_FeedLine(cg, row2.data()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot write linestring"));
  CPLErrorReset();
}

// 20. Direct unit tests for OGRContourWriter function.
TEST_F(ContourTest, OGRContourWriterDirect2D) {
  MemLayerContext ctx = CreateMemLayer("writer_2d", wkbLineString,
                                       /*add_id=*/true, /*add_elev=*/true);
  ASSERT_NE(nullptr, ctx.layer);

  OGRContourWriterInfo info;
  info.hLayer = OGRLayer::ToHandle(ctx.layer);
  info.adfGeoTransform[0] = 100.0;
  info.adfGeoTransform[1] = 2.0;
  info.adfGeoTransform[2] = 0.0;
  info.adfGeoTransform[3] = 200.0;
  info.adfGeoTransform[4] = 0.0;
  info.adfGeoTransform[5] = -2.0;
  info.nIDField = ctx.id_field;
  info.nElevField = ctx.elev_field;
  info.nElevFieldMin = -1;
  info.nElevFieldMax = -1;
  info.nNextID = 42;
  info.nWrittenFeatureCountSinceLastCommit = 0;
  info.nTransactionCommitInterval = 0;

  std::array<double, 3> xs = {1.0, 2.0, 3.0};
  std::array<double, 3> ys = {10.0, 20.0, 30.0};

  EXPECT_EQ(CE_None, OGRContourWriter(50.0, 3, xs.data(), ys.data(), &info));
  EXPECT_EQ(43, info.nNextID);

  ctx.layer->ResetReading();
  OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(42, feat->GetFieldAsInteger(ctx.id_field));
  EXPECT_DOUBLE_EQ(50.0, feat->GetFieldAsDouble(ctx.elev_field));

  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbLineString, geom->getGeometryType());
  OGRLineString* ls = geom->toLineString();
  ASSERT_NE(nullptr, ls);
  EXPECT_EQ(3, ls->getNumPoints());

  EXPECT_DOUBLE_EQ(102.0, ls->getX(0));
  EXPECT_DOUBLE_EQ(180.0, ls->getY(0));
  EXPECT_DOUBLE_EQ(104.0, ls->getX(1));
  EXPECT_DOUBLE_EQ(160.0, ls->getY(1));
  EXPECT_DOUBLE_EQ(106.0, ls->getX(2));
  EXPECT_DOUBLE_EQ(140.0, ls->getY(2));
}

TEST_F(ContourTest, OGRContourWriterDirect3D) {
  MemLayerContext ctx = CreateMemLayer("writer_3d", wkbLineString25D,
                                       /*add_id=*/false, /*add_elev=*/false);
  ASSERT_NE(nullptr, ctx.layer);

  OGRContourWriterInfo info;
  info.hLayer = OGRLayer::ToHandle(ctx.layer);
  info.adfGeoTransform[0] = 0.0;
  info.adfGeoTransform[1] = 1.0;
  info.adfGeoTransform[2] = 0.0;
  info.adfGeoTransform[3] = 0.0;
  info.adfGeoTransform[4] = 0.0;
  info.adfGeoTransform[5] = 1.0;
  info.nIDField = -1;
  info.nElevField = -1;
  info.nElevFieldMin = -1;
  info.nElevFieldMax = -1;
  info.nNextID = 0;
  info.nWrittenFeatureCountSinceLastCommit = 0;
  info.nTransactionCommitInterval = 0;

  std::array<double, 2> xs = {5.0, 10.0};
  std::array<double, 2> ys = {15.0, 20.0};

  EXPECT_EQ(CE_None, OGRContourWriter(75.5, 2, xs.data(), ys.data(), &info));

  ctx.layer->ResetReading();
  OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);

  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_NE(nullptr, geom);
  EXPECT_EQ(wkbLineString25D, geom->getGeometryType());
  OGRLineString* ls = geom->toLineString();
  ASSERT_NE(nullptr, ls);
  EXPECT_DOUBLE_EQ(75.5, ls->getZ(0));
  EXPECT_DOUBLE_EQ(75.5, ls->getZ(1));
}

TEST_F(ContourTest, OGRContourWriterDirectFail) {
  FailCreateFeatureLayer fail_layer;

  OGRContourWriterInfo info;
  info.hLayer = OGRLayer::ToHandle(&fail_layer);
  info.adfGeoTransform[0] = 0.0;
  info.adfGeoTransform[1] = 1.0;
  info.adfGeoTransform[2] = 0.0;
  info.adfGeoTransform[3] = 0.0;
  info.adfGeoTransform[4] = 0.0;
  info.adfGeoTransform[5] = 1.0;
  info.nIDField = -1;
  info.nElevField = -1;
  info.nElevFieldMin = -1;
  info.nElevFieldMax = -1;
  info.nNextID = 0;
  info.nWrittenFeatureCountSinceLastCommit = 0;
  info.nTransactionCommitInterval = 0;

  std::array<double, 2> xs = {1.0, 2.0};
  std::array<double, 2> ys = {3.0, 4.0};

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, OGRContourWriter(10.0, 2, xs.data(), ys.data(), &info));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 21. Standalone raster band without dataset (tests fallback geotransform).
TEST_F(ContourTest, StandaloneBandNoDataset) {
  StandaloneRasterBand band(3, 3, 50.0);

  MemLayerContext ctx = CreateMemLayer("standalone", wkbLineString);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "10.0");

  EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(&band),
                                           OGRLayer::ToHandle(ctx.layer),
                                           options.List(), nullptr, nullptr));
}

// 22. Raster read error during contour generation.
TEST_F(ContourTest, FailRasterBandReadError) {
  FailRasterBand band(4, 4);

  MemLayerContext ctx = CreateMemLayer("fail_read", wkbLineString);
  ASSERT_NE(nullptr, ctx.layer);

  CPLStringList options;
  options.AddNameValue("LEVEL_INTERVAL", "10.0");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALContourGenerateEx(GDALRasterBand::ToHandle(&band),
                                  OGRLayer::ToHandle(ctx.layer), options.List(),
                                  nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 23. Layer CreateFeature failure during contour generation (line and polygon).
// Note: Upstream GDALRingAppender::addLine and PolygonContourWriter::endPolygon
// emit CPLError(CE_Failure, ...) but do not propagate failure to
// GDALContourGenerateEx's return value, which returns CE_None.
TEST_F(ContourTest, FailCreateFeatureDuringContour) {
  GDALDatasetUniquePtr src_ds = CreateMemRaster(5, 5, kHill5x5);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  FailCreateFeatureLayer fail_layer;

  // 1. Line mode failure.
  {
    CPLStringList options;
    options.AddNameValue("LEVEL_INTERVAL", "25.0");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(&fail_layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cannot write linestring"));
    CPLErrorReset();
  }

  // 2. Polygon mode failure.
  {
    CPLStringList options;
    options.AddNameValue("POLYGONIZE", "YES");
    options.AddNameValue("LEVEL_INTERVAL", "25.0");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, GDALContourGenerateEx(GDALRasterBand::ToHandle(band),
                                             OGRLayer::ToHandle(&fail_layer),
                                             options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Simulated CreateFeature failure"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
