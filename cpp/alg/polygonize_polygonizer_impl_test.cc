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
// Tests polygonize_polygonizer_impl (alg/polygonize_polygonizer_impl.cpp).
//
// Build & Coverage Note:
// In third_party/gdal/BUILD, alg/polygonize_polygonizer.cpp is listed in
// textual_hdrs and #include'd by alg/polygonize_polygonizer_impl.cpp. Blaze's
// LCOV collector emits 0 DA lines for impl and omits textual_hdrs from
// coverage.dat.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/polygonize_polygonizer_impl.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/polygonize_polygonizer.h

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/polygonize_polygonizer.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::gdal::polygonizer::IndexedArc;
using ::gdal::polygonizer::IndexType;
using ::gdal::polygonizer::OGRPolygonWriter;
using ::gdal::polygonizer::Point;
using ::gdal::polygonizer::Polygonizer;
using ::gdal::polygonizer::PolygonReceiver;
using ::gdal::polygonizer::RPolygon;
using ::gdal::polygonizer::TwoArm;

// Dedicated receiver for verifying Polygonizer template instantiations.
template <typename DataType>
class ReceiverImpl : public PolygonReceiver<DataType> {
 public:
  struct PolygonEntry {
    DataType value;
    IndexType bottom_right_row;
    IndexType bottom_right_col;
    std::size_t arc_count;
  };

  void receive(RPolygon* poPolygon, DataType nPolygonCellValue) override {
    entries_.push_back({
        .value = nPolygonCellValue,
        .bottom_right_row = poPolygon->iBottomRightRow,
        .bottom_right_col = poPolygon->iBottomRightCol,
        .arc_count = poPolygon->oArcs.size(),
    });
  }

  const std::vector<PolygonEntry>& entries() const { return entries_; }

 private:
  std::vector<PolygonEntry> entries_;
};

// Feeds a 2D grid through Polygonizer<GInt32, DataType>.
template <typename DataType>
void RunPipeline(Polygonizer<GInt32, DataType>& polygonizer,
                 const std::vector<std::vector<GInt32>>& id_grid,
                 const std::vector<std::vector<DataType>>& val_grid) {
  const IndexType nRows = static_cast<IndexType>(id_grid.size());
  ASSERT_GT(nRows, 0u);
  const IndexType nCols = static_cast<IndexType>(id_grid[0].size());
  ASSERT_GT(nCols, 0u);

  std::vector<TwoArm> last_line_arms(nCols + 2);
  std::vector<TwoArm> this_line_arms(nCols + 2);
  for (IndexType i = 0; i < nCols + 2; ++i) {
    last_line_arms[i].poPolyInside = polygonizer.getTheOuterPolygon();
  }

  std::vector<DataType> last_line_vals(nCols, DataType{});

  for (IndexType row = 0; row < nRows; ++row) {
    if (row > 0) {
      last_line_vals = val_grid[row - 1];
    }
    const bool ok = polygonizer.processLine(
        id_grid[row].data(), last_line_vals.data(), this_line_arms.data(),
        last_line_arms.data(), row, nCols);
    EXPECT_TRUE(ok);
    std::swap(this_line_arms, last_line_arms);
  }

  last_line_vals = val_grid[nRows - 1];
  const std::vector<GInt32> outer_line(
      nCols, Polygonizer<GInt32, DataType>::THE_OUTER_POLYGON_ID);
  const bool ok = polygonizer.processLine(
      outer_line.data(), last_line_vals.data(), this_line_arms.data(),
      last_line_arms.data(), nRows, nCols);
  EXPECT_TRUE(ok);
}

class PolygonizePolygonizerImplTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// ---------------------------------------------------------------------------
// Instantiation 1: Polygonizer<GInt32, std::int64_t>
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerImplTest, PolygonizerInt64Instantiation) {
  ReceiverImpl<std::int64_t> receiver;
  Polygonizer<GInt32, std::int64_t> polygonizer(/*nInvalidPolyId=*/-1,
                                                &receiver);

  EXPECT_NE(nullptr, polygonizer.getTheOuterPolygon());
  EXPECT_EQ(std::numeric_limits<GInt32>::max(),
            (Polygonizer<GInt32, std::int64_t>::THE_OUTER_POLYGON_ID));

  // 2x2 raster with distinct 64-bit integer values:
  constexpr std::int64_t kVal1 = 123456789012345LL;
  constexpr std::int64_t kVal2 = -98765432109876LL;
  const std::vector<std::vector<GInt32>> id_grid = {
      {1, 2},
      {1, 2},
  };
  const std::vector<std::vector<std::int64_t>> val_grid = {
      {kVal1, kVal2},
      {kVal1, kVal2},
  };

  RunPipeline(polygonizer, id_grid, val_grid);

  ASSERT_EQ(2u, receiver.entries().size());
  // Verify that the full 64-bit precision was preserved without truncation.
  EXPECT_EQ(kVal1, receiver.entries()[0].value);
  EXPECT_EQ(kVal2, receiver.entries()[1].value);
}

// ---------------------------------------------------------------------------
// Instantiation 2: Polygonizer<GInt32, float>
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerImplTest, PolygonizerFloatInstantiation) {
  ReceiverImpl<float> receiver;
  Polygonizer<GInt32, float> polygonizer(/*nInvalidPolyId=*/-1, &receiver);

  EXPECT_NE(nullptr, polygonizer.getTheOuterPolygon());
  EXPECT_EQ(std::numeric_limits<GInt32>::max(),
            (Polygonizer<GInt32, float>::THE_OUTER_POLYGON_ID));

  // 2x2 raster with float values:
  constexpr float kVal1 = 3.14159f;
  constexpr float kVal2 = -0.00125f;
  const std::vector<std::vector<GInt32>> id_grid = {
      {1, 2},
      {1, 2},
  };
  const std::vector<std::vector<float>> val_grid = {
      {kVal1, kVal2},
      {kVal1, kVal2},
  };

  RunPipeline(polygonizer, id_grid, val_grid);

  ASSERT_EQ(2u, receiver.entries().size());
  EXPECT_FLOAT_EQ(kVal1, receiver.entries()[0].value);
  EXPECT_FLOAT_EQ(kVal2, receiver.entries()[1].value);
}

// ---------------------------------------------------------------------------
// Instantiation 3: OGRPolygonWriter<std::int64_t>
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerImplTest, OGRPolygonWriterInt64Instantiation) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer =
      ds->CreateLayer("int64_writer", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("int64_val", OFTInteger64);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));
  const int field_idx = layer->GetLayerDefn()->GetFieldIndex("int64_val");

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), field_idx,
                                        gt, 0);
  EXPECT_EQ(CE_None, writer.getErr());

  // Construct synthetic RPolygon with a closed square loop.
  constexpr std::int64_t kExpectedValue = 9876543210123LL;
  RPolygon poly;
  IndexedArc arc = poly.newArc(/*bFollowRighthand=*/true);
  arc.poArc->push_back(Point{0, 0});
  arc.poArc->push_back(Point{0, 2});
  arc.poArc->push_back(Point{2, 2});
  arc.poArc->push_back(Point{2, 0});
  poly.setArcConnection(arc, arc);
  poly.updateBottomRightPos(1, 1);

  writer.receive(&poly, kExpectedValue);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  EXPECT_EQ(kExpectedValue, feat->GetFieldAsInteger64(field_idx));

  const OGRPolygon* poly_geom = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly_geom);
  EXPECT_EQ(0, poly_geom->getNumInteriorRings());
  EXPECT_EQ(5, poly_geom->getExteriorRing()->getNumPoints());
}

// ---------------------------------------------------------------------------
// Instantiation 4: OGRPolygonWriter<float>
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerImplTest, OGRPolygonWriterFloatInstantiation) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer =
      ds->CreateLayer("float_writer", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("float_val", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));
  const int field_idx = layer->GetLayerDefn()->GetFieldIndex("float_val");

  const GDALGeoTransform gt;
  OGRPolygonWriter<float> writer(OGRLayer::ToHandle(layer), field_idx, gt, 0);
  EXPECT_EQ(CE_None, writer.getErr());

  constexpr float kExpectedValue = 99.875f;
  RPolygon poly;
  IndexedArc arc = poly.newArc(/*bFollowRighthand=*/true);
  arc.poArc->push_back(Point{0, 0});
  arc.poArc->push_back(Point{0, 3});
  arc.poArc->push_back(Point{3, 3});
  arc.poArc->push_back(Point{3, 0});
  poly.setArcConnection(arc, arc);
  poly.updateBottomRightPos(2, 2);

  writer.receive(&poly, kExpectedValue);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  EXPECT_FLOAT_EQ(kExpectedValue,
                  static_cast<float>(feat->GetFieldAsDouble(field_idx)));

  const OGRPolygon* poly_geom = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly_geom);
  EXPECT_EQ(0, poly_geom->getNumInteriorRings());
  EXPECT_EQ(5, poly_geom->getExteriorRing()->getNumPoints());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - alg/polygonize_polygonizer.cpp is in textual_hdrs in
// google3 and lacks standalone LCOV DA lines.
