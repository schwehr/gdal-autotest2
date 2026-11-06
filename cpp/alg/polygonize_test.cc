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
// Tests raster to polygon conversion algorithm (alg/polygonize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/polygonize.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom layer that does not support OLCSequentialWrite capability.
class ReadOnlyLayer final : public OGRLayer {
 public:
  ReadOnlyLayer() {
    feature_defn_ = new OGRFeatureDefn("readonly");
    feature_defn_->Reference();
  }

  ~ReadOnlyLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

// Custom layer that claims OLCSequentialWrite capability but fails on
// CreateFeature.
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

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

// Raster band whose IReadBlock returns CE_Failure.
class FailRasterBand final : public GDALRasterBand {
 public:
  FailRasterBand(int nXSize, int nYSize, GDALDataType eType) {
    poDS = nullptr;
    nBand = 1;
    eDataType = eType;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nXSize;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void*) override { return CE_Failure; }
};

// Raster band whose width triggers the integer overflow check.
class WideRasterBand final : public GDALRasterBand {
 public:
  WideRasterBand() {
    poDS = nullptr;
    nBand = 1;
    eDataType = GDT_Byte;
    nRasterXSize = std::numeric_limits<int>::max() - 1;
    nRasterYSize = 1;
    nBlockXSize = 1;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void*) override { return CE_None; }
};

// Standalone raster band without an associated GDALDataset.
class StandaloneRasterBand final : public GDALRasterBand {
 public:
  StandaloneRasterBand(int nXSize, int nYSize, GInt32 val) : value_(val) {
    poDS = nullptr;
    nBand = 1;
    eDataType = GDT_Int32;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    nBlockXSize = nXSize;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void* pImage) override {
    GInt32* panLine = static_cast<GInt32*>(pImage);
    std::fill_n(panLine, nBlockXSize, value_);
    return CE_None;
  }

 private:
  GInt32 value_ = 0;
};

// Context holding an in-memory vector dataset and layer.
struct MemLayerContext {
  GDALDatasetUniquePtr ds;
  OGRLayer* layer = nullptr;
  int field_idx = -1;
};

class PolygonizeTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  GDALDriver* GetMemDriver() {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("MEM");
    EXPECT_NE(nullptr, driver);
    return driver;
  }

  MemLayerContext CreateMemLayer(const char* name,
                                 OGRwkbGeometryType geom_type = wkbPolygon,
                                 const char* field_name = nullptr,
                                 OGRFieldType field_type = OFTInteger64) {
    GDALDriver* mem_drv = GetMemDriver();
    EXPECT_NE(nullptr, mem_drv);
    MemLayerContext ctx;
    ctx.ds = GDALDatasetUniquePtr(
        mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    EXPECT_NE(nullptr, ctx.ds);
    ctx.layer = ctx.ds->CreateLayer(name, nullptr, geom_type, nullptr);
    EXPECT_NE(nullptr, ctx.layer);
    if (field_name != nullptr) {
      OGRFieldDefn field_defn(field_name, field_type);
      EXPECT_EQ(OGRERR_NONE, ctx.layer->CreateField(&field_defn));
      ctx.field_idx = ctx.layer->GetLayerDefn()->GetFieldIndex(field_name);
    }
    return ctx;
  }
};

// 1. Verify error return on NULL input band or NULL output layer.
TEST_F(PolygonizeTest, NullPointersValidation) {
  MemLayerContext ctx = CreateMemLayer("null_test");
  ASSERT_NE(nullptr, ctx.layer);

  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 5, 5, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  WithQuietHandler quiet;
  CPLErrorReset();

  EXPECT_EQ(CE_Failure,
            GDALPolygonize(nullptr, nullptr, OGRLayer::ToHandle(ctx.layer), -1,
                           nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                       nullptr, -1, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(CE_Failure,
            GDALFPolygonize(nullptr, nullptr, OGRLayer::ToHandle(ctx.layer), -1,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(CE_Failure,
            GDALFPolygonize(GDALRasterBand::ToHandle(band), nullptr, nullptr,
                            -1, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 2. Verify error when output layer lacks OLCSequentialWrite capability.
TEST_F(PolygonizeTest, UnsupportedLayerCapability) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 5, 5, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  ReadOnlyLayer ro_layer;
  WithQuietHandler quiet;
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                       OGRLayer::ToHandle(&ro_layer), -1,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Output feature layer does not appear to support "
                        "creation of features"));
  CPLErrorReset();
}

// 3. Verify error when raster band width exceeds max allowed dimensions.
TEST_F(PolygonizeTest, TooWideRaster) {
  MemLayerContext ctx = CreateMemLayer("too_wide");
  ASSERT_NE(nullptr, ctx.layer);

  WideRasterBand wide_band;
  WithQuietHandler quiet;
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(&wide_band),
                                       nullptr, OGRLayer::ToHandle(ctx.layer),
                                       -1, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too wide raster"));
  CPLErrorReset();
}

// 4. Verify DATASET_FOR_GEOREF options and dataset geotransform handling.
TEST_F(PolygonizeTest, DatasetForGeoRefOptions) {
  MemLayerContext ctx = CreateMemLayer("georef_layer", wkbPolygon, "val");
  ASSERT_NE(nullptr, ctx.layer);

  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 2, 2, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels = {5, 5, 5, 5};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 2, 2,
                                    const_cast<GInt32*>(pixels.data()), 2, 2,
                                    GDT_Int32, 0, 0));

  // Case A: DATASET_FOR_GEOREF pointing to non-existent dataset.
  {
    CPLStringList options;
    options.SetNameValue("DATASET_FOR_GEOREF",
                         "/vsimem/polygonize_test/georef_missing/nofile.tif");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx.layer), ctx.field_idx,
                             options.List(), nullptr, nullptr));
  }

  // Case B: DATASET_FOR_GEOREF with valid MEM dataset syntax specifying
  // GEOTRANSFORM.
  {
    CPLSetConfigOption("GDAL_MEM_ENABLE_OPEN", "YES");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GDAL_MEM_ENABLE_OPEN", nullptr);
    };

    GByte dummy_byte = 0;
    const std::string mem_ds_uri = std::format(
        "MEM:::DATAPOINTER={},PIXELS=1,LINES=1,GEOTRANSFORM=10/2/0/20/0/-2",
        reinterpret_cast<uintptr_t>(&dummy_byte));

    MemLayerContext ctx_georef =
        CreateMemLayer("georef_out", wkbPolygon, "val");
    ASSERT_NE(nullptr, ctx_georef.layer);

    CPLStringList options;
    options.SetNameValue("DATASET_FOR_GEOREF", mem_ds_uri.c_str());

    EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                      OGRLayer::ToHandle(ctx_georef.layer),
                                      ctx_georef.field_idx, options.List(),
                                      nullptr, nullptr));

    ctx_georef.layer->ResetReading();
    OGRFeatureUniquePtr feat(ctx_georef.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    OGREnvelope env;
    geom->getEnvelope(&env);
    // Origin is at 10, width is 2 * 2 = 4 -> MinX=10, MaxX=14
    EXPECT_DOUBLE_EQ(10.0, env.MinX);
    EXPECT_DOUBLE_EQ(14.0, env.MaxX);
  }

  // Case C: Source dataset has an explicit geotransform without
  // DATASET_FOR_GEOREF.
  {
    const GDALGeoTransform gt{100.0, 1.0, 0.0, 50.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, src_ds->SetGeoTransform(gt));

    MemLayerContext ctx_native = CreateMemLayer("native_gt", wkbPolygon, "val");
    ASSERT_NE(nullptr, ctx_native.layer);

    EXPECT_EQ(CE_None,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx_native.layer),
                             ctx_native.field_idx, nullptr, nullptr, nullptr));

    ctx_native.layer->ResetReading();
    OGRFeatureUniquePtr feat(ctx_native.layer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    OGREnvelope env;
    geom->getEnvelope(&env);
    EXPECT_DOUBLE_EQ(100.0, env.MinX);
    EXPECT_DOUBLE_EQ(102.0, env.MaxX);
  }

  // Case D: Standalone band without an associated GDALDataset.
  {
    StandaloneRasterBand standalone_band(2, 2, 7);
    MemLayerContext ctx_standalone =
        CreateMemLayer("standalone", wkbPolygon, "val");
    ASSERT_NE(nullptr, ctx_standalone.layer);

    EXPECT_EQ(
        CE_None,
        GDALPolygonize(GDALRasterBand::ToHandle(&standalone_band), nullptr,
                       OGRLayer::ToHandle(ctx_standalone.layer),
                       ctx_standalone.field_idx, nullptr, nullptr, nullptr));
    EXPECT_EQ(1, ctx_standalone.layer->GetFeatureCount());
  }
}

// 5. Verify source raster I/O read failure is properly handled.
TEST_F(PolygonizeTest, SourceBandReadFailure) {
  MemLayerContext ctx = CreateMemLayer("src_fail");
  ASSERT_NE(nullptr, ctx.layer);

  FailRasterBand fail_band(10, 10, GDT_Int64);
  WithQuietHandler quiet;
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(&fail_band),
                                       nullptr, OGRLayer::ToHandle(ctx.layer),
                                       -1, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 6. Verify mask band processing, nodata masking, and mask read failure.
TEST_F(PolygonizeTest, MaskBandHandling) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);

  // 3x3 raster with value 42 everywhere.
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 3, 3, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  const std::vector<GInt32> src_pixels(9, 42);
  ASSERT_EQ(CE_None, src_band->RasterIO(GF_Write, 0, 0, 3, 3,
                                        const_cast<GInt32*>(src_pixels.data()),
                                        3, 3, GDT_Int32, 0, 0));

  // 3x3 mask raster with center pixel masked out (0) and outer pixels valid
  // (1).
  GDALDatasetUniquePtr mask_ds(mem_drv->Create("", 3, 3, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mask_ds);
  GDALRasterBand* mask_band = mask_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, mask_band);

  const std::vector<GByte> mask_pixels = {
      1, 1, 1, 1, 0, 1, 1, 1, 1,
  };
  ASSERT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 3, 3,
                                         const_cast<GByte*>(mask_pixels.data()),
                                         3, 3, GDT_Byte, 0, 0));

  MemLayerContext ctx = CreateMemLayer("masked_poly", wkbPolygon, "val");
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(src_band),
                                    GDALRasterBand::ToHandle(mask_band),
                                    OGRLayer::ToHandle(ctx.layer),
                                    ctx.field_idx, nullptr, nullptr, nullptr));

  // The output feature should have an inner ring (hole) representing the masked
  // pixel.
  ASSERT_EQ(1, ctx.layer->GetFeatureCount());
  ctx.layer->ResetReading();
  OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(42, feat->GetFieldAsInteger64(ctx.field_idx));

  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_NE(nullptr, geom);
  OGRPolygon* poly = geom->toPolygon();
  ASSERT_NE(nullptr, poly);
  EXPECT_EQ(1, poly->getNumInteriorRings());

  // Verify mask band read failure causes error.
  FailRasterBand fail_mask(3, 3, GDT_Byte);
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(src_band),
                                       GDALRasterBand::ToHandle(&fail_mask),
                                       OGRLayer::ToHandle(ctx.layer), -1,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 7. Verify progress reporting and early user termination in both passes.
TEST_F(PolygonizeTest, ProgressAndInterruption) {
  MemLayerContext ctx = CreateMemLayer("progress_layer");
  ASSERT_NE(nullptr, ctx.layer);

  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 4, 4, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels(16, 1);
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 4,
                                    const_cast<GInt32*>(pixels.data()), 4, 4,
                                    GDT_Int32, 0, 0));

  // Pass 1 cancellation callback.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto CancelFirstPass = [](double, const char*, void*) -> int {
      return FALSE;
    };
    EXPECT_EQ(CE_Failure,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx.layer), -1, nullptr,
                             CancelFirstPass, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }

  // Pass 2 cancellation callback (allows pass 1 where dfComplete <= 0.10).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto CancelSecondPass = [](double dfComplete, const char*, void*) -> int {
      return dfComplete <= 0.10 ? TRUE : FALSE;
    };
    EXPECT_EQ(CE_Failure,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx.layer), -1, nullptr,
                             CancelSecondPass, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }
}

// 8. Verify error return when layer feature creation fails.
TEST_F(PolygonizeTest, LayerCreateFeatureFailure) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 2, 2, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels = {1, 2, 3, 4};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 2, 2,
                                    const_cast<GInt32*>(pixels.data()), 2, 2,
                                    GDT_Int32, 0, 0));

  FailCreateFeatureLayer fail_layer;
  WithQuietHandler quiet;
  CPLErrorReset();

  EXPECT_EQ(CE_Failure, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                       OGRLayer::ToHandle(&fail_layer), -1,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// 9. Verify 4-connectedness versus 8-connectedness options.
TEST_F(PolygonizeTest, Connectedness8vs4) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);

  // Diagonal pattern:
  // 1 0
  // 0 1
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 2, 2, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels = {
      1,
      0,
      0,
      1,
  };
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 2, 2,
                                    const_cast<GInt32*>(pixels.data()), 2, 2,
                                    GDT_Int32, 0, 0));

  // 4-connected: diagonal 1s remain separate polygons.
  {
    MemLayerContext ctx4 = CreateMemLayer("conn4", wkbPolygon, "val");
    ASSERT_NE(nullptr, ctx4.layer);
    EXPECT_EQ(CE_None,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx4.layer), ctx4.field_idx,
                             nullptr, nullptr, nullptr));
    // 2 features for 1, and 2 features for 0 -> 4 total features.
    EXPECT_EQ(4, ctx4.layer->GetFeatureCount());
  }

  // 8-connected: diagonal 1s merge into a single polygon.
  {
    MemLayerContext ctx8 = CreateMemLayer("conn8", wkbPolygon, "val");
    ASSERT_NE(nullptr, ctx8.layer);
    CPLStringList options;
    options.SetNameValue("8CONNECTED", "8");

    EXPECT_EQ(CE_None,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(ctx8.layer), ctx8.field_idx,
                             options.List(), nullptr, nullptr));
    // Merged diagonal 1s and diagonal 0s each produce 1 polygon -> 2 features.
    EXPECT_EQ(2, ctx8.layer->GetFeatureCount());
  }
}

// Minimal transactional dataset and layer to verify COMMIT_INTERVAL transaction
// control.
class FakeTransactionalDataset final : public GDALDataset {
 public:
  int TestCapability(const char* cap) const override {
    return EQUAL(cap, ODsCTransactions) ? TRUE : FALSE;
  }
  OGRErr StartTransaction(int /*bForce*/ = FALSE) override {
    ++start_count;
    return OGRERR_NONE;
  }
  OGRErr CommitTransaction() override {
    ++commit_count;
    if (fail_commit) return OGRERR_FAILURE;
    return OGRERR_NONE;
  }

  int start_count = 0;
  int commit_count = 0;
  bool fail_commit = false;
};

class FakeTransactionalLayer final : public OGRLayer {
 public:
  explicit FakeTransactionalLayer(GDALDataset* ds) : ds_(ds) {
    defn_ = new OGRFeatureDefn("fake");
    defn_->Reference();
  }
  ~FakeTransactionalLayer() override { defn_->Release(); }

  GDALDataset* GetDataset() override { return ds_; }
  int TestCapability(const char*) const override { return TRUE; }
  const OGRFeatureDefn* GetLayerDefn() const override { return defn_; }
  OGRErr ICreateFeature(OGRFeature*) override { return OGRERR_NONE; }
  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }

 private:
  GDALDataset* ds_;
  OGRFeatureDefn* defn_;
};

// 10. Verify COMMIT_INTERVAL option handling.
TEST_F(PolygonizeTest, CommitIntervalOptions) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 3, 3, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels = {
      1, 2, 1, 2, 1, 2, 1, 2, 1,
  };
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 3, 3,
                                    const_cast<GInt32*>(pixels.data()), 3, 3,
                                    GDT_Int32, 0, 0));

  // Non-transactional layer ignores COMMIT_INTERVAL safely.
  const char* intervals[] = {"1", "0", "-1", "50000"};
  for (const char* interval : intervals) {
    MemLayerContext ctx = CreateMemLayer("commit_test");
    ASSERT_NE(nullptr, ctx.layer);

    CPLStringList options;
    options.SetNameValue("COMMIT_INTERVAL", interval);

    EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                      OGRLayer::ToHandle(ctx.layer), -1,
                                      options.List(), nullptr, nullptr));
    EXPECT_GT(ctx.layer->GetFeatureCount(), 0);
  }

  // Transactional dataset with COMMIT_INTERVAL=1: commits each batch.
  {
    FakeTransactionalDataset fake_ds;
    FakeTransactionalLayer fake_layer(&fake_ds);
    CPLStringList options;
    options.SetNameValue("COMMIT_INTERVAL", "1");

    EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                      OGRLayer::ToHandle(&fake_layer), -1,
                                      options.List(), nullptr, nullptr));
    EXPECT_GE(fake_ds.commit_count, 2);
  }

  // Transactional dataset with COMMIT_INTERVAL=100: Finalize() commits
  // remainder.
  {
    FakeTransactionalDataset fake_ds;
    FakeTransactionalLayer fake_layer(&fake_ds);
    CPLStringList options;
    options.SetNameValue("COMMIT_INTERVAL", "100");

    EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                      OGRLayer::ToHandle(&fake_layer), -1,
                                      options.List(), nullptr, nullptr));
    EXPECT_EQ(1, fake_ds.commit_count);
  }

  // Commit failure causes CE_Failure.
  {
    FakeTransactionalDataset fake_ds;
    fake_ds.fail_commit = true;
    FakeTransactionalLayer fake_layer(&fake_ds);
    CPLStringList options;
    options.SetNameValue("COMMIT_INTERVAL", "1");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                             OGRLayer::ToHandle(&fake_layer), -1,
                             options.List(), nullptr, nullptr));
    // CommitTransaction failure sets CE_Failure on return without CPLError.
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
}

// 11. Verify polygonize with iPixValField = -1 (no field written).
TEST_F(PolygonizeTest, NoPixelValField) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 2, 2, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<GInt32> pixels = {1, 1, 1, 1};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 2, 2,
                                    const_cast<GInt32*>(pixels.data()), 2, 2,
                                    GDT_Int32, 0, 0));

  MemLayerContext ctx = CreateMemLayer("no_field");
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                    OGRLayer::ToHandle(ctx.layer), -1, nullptr,
                                    nullptr, nullptr));
  EXPECT_EQ(1, ctx.layer->GetFeatureCount());
}

// 12. Verify GDALFPolygonize with floating point raster data.
TEST_F(PolygonizeTest, GDALFPolygonizeBasic) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 3, 2, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const std::vector<float> pixels = {
      1.5f, 1.5f, 2.5f, 1.5f, 2.5f, 2.5f,
  };
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 3, 2,
                                    const_cast<float*>(pixels.data()), 3, 2,
                                    GDT_Float32, 0, 0));

  MemLayerContext ctx =
      CreateMemLayer("fpoly_layer", wkbPolygon, "fval", OFTReal);
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None, GDALFPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                     OGRLayer::ToHandle(ctx.layer),
                                     ctx.field_idx, nullptr, nullptr, nullptr));
  ASSERT_EQ(2, ctx.layer->GetFeatureCount());

  ctx.layer->ResetReading();
  OGRFeatureUniquePtr feat1(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat1);
  const double fval1 = feat1->GetFieldAsDouble(ctx.field_idx);

  OGRFeatureUniquePtr feat2(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat2);
  const double fval2 = feat2->GetFieldAsDouble(ctx.field_idx);

  EXPECT_TRUE((fval1 == 1.5 && fval2 == 2.5) || (fval1 == 2.5 && fval2 == 1.5));
}

// 13. Verify single pixel raster boundary condition.
TEST_F(PolygonizeTest, SinglePixelRaster) {
  GDALDriver* mem_drv = GetMemDriver();
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr src_ds(mem_drv->Create("", 1, 1, 1, GDT_Int32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  const GInt32 val = 99;
  ASSERT_EQ(CE_None,
            band->RasterIO(GF_Write, 0, 0, 1, 1, const_cast<GInt32*>(&val), 1,
                           1, GDT_Int32, 0, 0));

  MemLayerContext ctx = CreateMemLayer("single_pixel", wkbPolygon, "val");
  ASSERT_NE(nullptr, ctx.layer);

  EXPECT_EQ(CE_None, GDALPolygonize(GDALRasterBand::ToHandle(band), nullptr,
                                    OGRLayer::ToHandle(ctx.layer),
                                    ctx.field_idx, nullptr, nullptr, nullptr));
  ASSERT_EQ(1, ctx.layer->GetFeatureCount());

  ctx.layer->ResetReading();
  OGRFeatureUniquePtr feat(ctx.layer->GetNextFeature());
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(99, feat->GetFieldAsInteger64(ctx.field_idx));

  OGRGeometry* geom = feat->GetGeometryRef();
  ASSERT_NE(nullptr, geom);
  OGREnvelope env;
  geom->getEnvelope(&env);
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(1.0, env.MaxX);
  EXPECT_DOUBLE_EQ(0.0, env.MinY);
  EXPECT_DOUBLE_EQ(1.0, env.MaxY);
}

// 14. Verify GDALFloatEquals unit-in-the-last-place float comparisons.
TEST_F(PolygonizeTest, GDALFloatEqualsDirect) {
  // Identical values
  EXPECT_EQ(TRUE, GDALFloatEquals(1.0f, 1.0f));
  EXPECT_EQ(TRUE, GDALFloatEquals(-1.0f, -1.0f));
  EXPECT_EQ(TRUE, GDALFloatEquals(0.0f, 0.0f));
  EXPECT_EQ(TRUE, GDALFloatEquals(0.0f, -0.0f));

  // Opposite signs
  EXPECT_EQ(FALSE, GDALFloatEquals(1.0f, -1.0f));
  EXPECT_EQ(FALSE, GDALFloatEquals(-1.0f, 1.0f));

  // Close values within MAX_ULPS (10)
  float pos_close = 1.0f;
  for (int i = 0; i < 5; ++i) {
    pos_close = std::nextafter(pos_close, 2.0f);
  }
  EXPECT_EQ(TRUE, GDALFloatEquals(1.0f, pos_close));

  // Values exceeding MAX_ULPS
  float pos_far = 1.0f;
  for (int i = 0; i < 15; ++i) {
    pos_far = std::nextafter(pos_far, 2.0f);
  }
  EXPECT_EQ(FALSE, GDALFloatEquals(1.0f, pos_far));

  // Negative close within MAX_ULPS
  float neg_close = -1.0f;
  for (int i = 0; i < 5; ++i) {
    neg_close = std::nextafter(neg_close, -2.0f);
  }
  EXPECT_EQ(TRUE, GDALFloatEquals(-1.0f, neg_close));

  // Negative values exceeding MAX_ULPS
  float neg_far = -1.0f;
  for (int i = 0; i < 15; ++i) {
    neg_far = std::nextafter(neg_far, -2.0f);
  }
  EXPECT_EQ(FALSE, GDALFloatEquals(-1.0f, neg_far));

  // Far apart numbers
  EXPECT_EQ(FALSE, GDALFloatEquals(10.0f, 20.0f));
  EXPECT_EQ(FALSE, GDALFloatEquals(-10.0f, -20.0f));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered lines are OOM guards and defensive
// error paths.
