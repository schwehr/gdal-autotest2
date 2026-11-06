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
// Tests polygonize_polygonizer (alg/polygonize_polygonizer.h and
// alg/polygonize_polygonizer.cpp).
//
// Build & Coverage Note:
// In third_party/gdal/BUILD, alg/polygonize_polygonizer.cpp is listed in
// textual_hdrs and #include'd by alg/polygonize_polygonizer_impl.cpp. Blaze's
// LCOV collector emits 0 DA lines for impl and omits textual_hdrs from
// coverage.dat.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/polygonize_polygonizer.h
//   https://github.com/OSGeo/gdal/blob/master/alg/polygonize_polygonizer.cpp

#include "third_party/gdal/alg/polygonize_polygonizer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
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

using ::gdal::polygonizer::Arc;
using ::gdal::polygonizer::IndexedArc;
using ::gdal::polygonizer::IndexType;
using ::gdal::polygonizer::OGRPolygonWriter;
using ::gdal::polygonizer::Point;
using ::gdal::polygonizer::Polygonizer;
using ::gdal::polygonizer::PolygonReceiver;
using ::gdal::polygonizer::RPolygon;
using ::gdal::polygonizer::TwoArm;

// Collects received polygons for testing Polygonizer in isolation.
template <typename DataType>
class TestPolygonReceiver : public PolygonReceiver<DataType> {
 public:
  struct ReceivedPolygon {
    DataType cell_value;
    IndexType bottom_right_row;
    IndexType bottom_right_col;
    std::size_t num_arcs;
  };

  void receive(RPolygon* poPolygon, DataType nPolygonCellValue) override {
    received_.push_back({
        .cell_value = nPolygonCellValue,
        .bottom_right_row = poPolygon->iBottomRightRow,
        .bottom_right_col = poPolygon->iBottomRightCol,
        .num_arcs = poPolygon->oArcs.size(),
    });
  }

  const std::vector<ReceivedPolygon>& received() const { return received_; }
  void clear() { received_.clear(); }

 private:
  std::vector<ReceivedPolygon> received_;
};

// Feeds a 2D raster through Polygonizer<GInt32, DataType>.
template <typename DataType>
void RunScanlines(PolygonReceiver<DataType>* receiver,
                  const std::vector<std::vector<GInt32>>& id_grid,
                  const std::vector<std::vector<DataType>>& val_grid,
                  GInt32 invalid_id = -1) {
  const IndexType nRows = static_cast<IndexType>(id_grid.size());
  ASSERT_GT(nRows, 0u);
  const IndexType nCols = static_cast<IndexType>(id_grid[0].size());
  ASSERT_GT(nCols, 0u);

  Polygonizer<GInt32, DataType> polygonizer(invalid_id, receiver);
  EXPECT_NE(nullptr, polygonizer.getTheOuterPolygon());

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

  // Termination line with THE_OUTER_POLYGON_ID.
  last_line_vals = val_grid[nRows - 1];
  const std::vector<GInt32> outer_line(
      nCols, Polygonizer<GInt32, DataType>::THE_OUTER_POLYGON_ID);
  const bool ok = polygonizer.processLine(
      outer_line.data(), last_line_vals.data(), this_line_arms.data(),
      last_line_arms.data(), nRows, nCols);
  EXPECT_TRUE(ok);
}

class PolygonizePolygonizerTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(PolygonizePolygonizerTest, RPolygonBasicOperations) {
  RPolygon poly;

  // 1. Initial default state.
  EXPECT_EQ(0u, poly.iBottomRightRow);
  EXPECT_EQ(0u, poly.iBottomRightCol);
  EXPECT_TRUE(poly.oArcs.empty());

  // 2. updateBottomRightPos.
  poly.updateBottomRightPos(4, 8);
  EXPECT_EQ(4u, poly.iBottomRightRow);
  EXPECT_EQ(8u, poly.iBottomRightCol);

  // 3. newArc with bFollowRighthand = true.
  IndexedArc arc0 = poly.newArc(/*bFollowRighthand=*/true);
  EXPECT_EQ(0u, arc0.iIndex);
  ASSERT_NE(nullptr, arc0.poArc);
  EXPECT_EQ(1u, poly.oArcs.size());
  EXPECT_EQ(0u, poly.oArcs[0].nConnection);
  EXPECT_TRUE(poly.oArcs[0].bFollowRighthand);

  // 4. newArc with bFollowRighthand = false.
  IndexedArc arc1 = poly.newArc(/*bFollowRighthand=*/false);
  EXPECT_EQ(1u, arc1.iIndex);
  ASSERT_NE(nullptr, arc1.poArc);
  EXPECT_EQ(2u, poly.oArcs.size());
  EXPECT_EQ(1u, poly.oArcs[1].nConnection);
  EXPECT_FALSE(poly.oArcs[1].bFollowRighthand);

  // 5. Append points to arcs.
  arc0.poArc->push_back(Point{0, 0});
  arc0.poArc->push_back(Point{0, 1});
  EXPECT_EQ(2u, arc0.poArc->size());
  EXPECT_EQ(0u, (*arc0.poArc)[0][0]);
  EXPECT_EQ(1u, (*arc0.poArc)[1][1]);

  // 6. setArcConnection.
  poly.setArcConnection(arc0, arc1);
  EXPECT_EQ(1u, poly.oArcs[0].nConnection);
  poly.setArcConnection(arc1, arc0);
  EXPECT_EQ(0u, poly.oArcs[1].nConnection);
}

TEST_F(PolygonizePolygonizerTest, TwoArmDefaultInitializationAndFields) {
  TwoArm arm;

  EXPECT_EQ(0u, arm.iRow);
  EXPECT_EQ(0u, arm.iCol);
  EXPECT_EQ(nullptr, arm.poPolyInside);
  EXPECT_EQ(nullptr, arm.poPolyAbove);
  EXPECT_EQ(nullptr, arm.poPolyLeft);
  EXPECT_FALSE(arm.bSolidHorizontal);
  EXPECT_FALSE(arm.bSolidVertical);
  EXPECT_EQ(nullptr, arm.oArcHorOuter.poArc);
  EXPECT_EQ(0u, arm.oArcHorOuter.iIndex);
  EXPECT_EQ(nullptr, arm.oArcHorInner.poArc);
  EXPECT_EQ(0u, arm.oArcHorInner.iIndex);
  EXPECT_EQ(nullptr, arm.oArcVerInner.poArc);
  EXPECT_EQ(0u, arm.oArcVerInner.iIndex);
  EXPECT_EQ(nullptr, arm.oArcVerOuter.poArc);
  EXPECT_EQ(0u, arm.oArcVerOuter.iIndex);

  RPolygon poly;
  IndexedArc arc = poly.newArc(true);
  arm.iRow = 5;
  arm.iCol = 7;
  arm.poPolyInside = &poly;
  arm.bSolidHorizontal = true;
  arm.bSolidVertical = true;
  arm.oArcHorOuter = arc;

  EXPECT_EQ(5u, arm.iRow);
  EXPECT_EQ(7u, arm.iCol);
  EXPECT_EQ(&poly, arm.poPolyInside);
  EXPECT_TRUE(arm.bSolidHorizontal);
  EXPECT_TRUE(arm.bSolidVertical);
  EXPECT_EQ(arc.poArc, arm.oArcHorOuter.poArc);
}

// ---------------------------------------------------------------------------
// Arm Connection Topology Tests (Types 0, 3, 5, 6, 7, 9, 10, 11, 12, 13, 14,
// 15)
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType0FlatRegion) {
  // All 4 pixels have the same value:
  // NW=1, NE=1
  // SW=1, SE=1
  // Interior junction at (1, 1) has all 4 arms virtual (Type 0).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 1}, {1, 1}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 10}, {10, 10}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(1u, receiver.received().size());
  EXPECT_EQ(10, receiver.received()[0].cell_value);
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType3SouthEastCorner) {
  // NW=1, NE=1
  // SW=1, SE=2
  // Interior junction at (1, 1): CUR_VERT=solid, CUR_HORIZ=solid (Type 3).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 1}, {1, 2}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 10}, {10, 20}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest,
       ProcessArmConnectionsType5HorizontalBoundary) {
  // NW=1, NE=1
  // SW=2, SE=2
  // Interior junction at (1, 1): LEFT=solid, CUR_HORIZ=solid (Type 5).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 1}, {2, 2}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 10}, {20, 20}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType6SouthWestCorner) {
  // NW=1, NE=1
  // SW=2, SE=1
  // Interior junction at (1, 1): LEFT=solid, CUR_VERT=solid (Type 6).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 1}, {2, 1}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 10}, {20, 10}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType7ThreeWayJunction) {
  // NW=1, NE=1
  // SW=2, SE=3
  // Interior junction at (1, 1): LEFT=solid, CUR_VERT=solid, CUR_HORIZ=solid
  // (Type 7).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 1}, {2, 3}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 10}, {20, 30}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType9NorthEastCorner) {
  // NW=1, NE=2
  // SW=1, SE=1
  // Interior junction at (1, 1): ABOVE=solid, CUR_HORIZ=solid (Type 9).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {1, 1}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {10, 10}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType10VerticalBoundary) {
  // NW=1, NE=2
  // SW=1, SE=2
  // Interior junction at (1, 1): ABOVE=solid, CUR_VERT=solid (Type 10).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {1, 2}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {10, 20}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType11ThreeWayJunction) {
  // NW=1, NE=2
  // SW=1, SE=3
  // Interior junction at (1, 1): ABOVE=solid, CUR_VERT=solid, CUR_HORIZ=solid
  // (Type 11).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {1, 3}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {10, 30}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType12NorthWestCorner) {
  // NW=1, NE=2
  // SW=2, SE=2
  // Interior junction at (1, 1): ABOVE=solid, LEFT=solid (Type 12).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {2, 2}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {20, 20}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType13ThreeWayJunction) {
  // NW=1, NE=2
  // SW=3, SE=3
  // Interior junction at (1, 1): ABOVE=solid, LEFT=solid, CUR_HORIZ=solid (Type
  // 13).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {3, 3}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {30, 30}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType14ThreeWayJunction) {
  // NW=1, NE=3
  // SW=2, SE=3
  // Interior junction at (1, 1): ABOVE=solid, LEFT=solid, CUR_VERT=solid (Type
  // 14).
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 3}, {2, 3}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 30}, {20, 30}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest,
       ProcessArmConnectionsType15CheckerboardBothDiagonalsSame) {
  // NW=1, NE=2
  // SW=2, SE=1
  // All 4 arms solid (Type 15).
  // Main diagonal: NW == SE (1 == 1) -> TRUE.
  // Secondary diagonal: NE == SW (2 == 2) -> TRUE.
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {2, 1}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {20, 10}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(2u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest, ProcessArmConnectionsType15NoTheSameValue) {
  // NW=1, NE=2
  // SW=3, SE=4
  // All 4 arms solid (Type 15).
  // Main diagonal: NW != SE (1 != 4) -> FALSE.
  // Secondary diagonal: NE != SW (2 != 3) -> FALSE.
  // This exercises the NO_THE_SAME_VALUE branch where all 4 pixels differ.
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {3, 4}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {30, 40}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(4u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest,
       ProcessArmConnectionsType15MainDiagSameSecDiagDiff) {
  // NW=1, NE=2
  // SW=3, SE=1
  // All 4 arms solid (Type 15).
  // Main diagonal: NW == SE (1 == 1) -> TRUE.
  // Secondary diagonal: NE != SW (2 != 3) -> FALSE.
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {3, 1}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {30, 10}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

TEST_F(PolygonizePolygonizerTest,
       ProcessArmConnectionsType15MainDiagDiffSecDiagSame) {
  // NW=1, NE=2
  // SW=2, SE=3
  // All 4 arms solid (Type 15).
  // Main diagonal: NW != SE (1 != 3) -> FALSE.
  // Secondary diagonal: NE == SW (2 == 2) -> TRUE.
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{1, 2}, {2, 3}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{10, 20}, {20, 30}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(3u, receiver.received().size());
}

// ---------------------------------------------------------------------------
// Complex Topologies: Inner Holes, Outer Rings, Multipolygon Merging, Masking
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerTest, InnerHoleTopology) {
  // 3x3 raster where polygon 1 forms a border around an inner hole polygon 2:
  // 1 1 1
  // 1 2 1
  // 1 1 1
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("holes", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("pixel_val", OFTInteger64);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));
  const int field_idx = layer->GetLayerDefn()->GetFieldIndex("pixel_val");

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), field_idx,
                                        gt, 0);

  const std::vector<std::vector<GInt32>> id_grid = {
      {1, 1, 1},
      {1, 2, 1},
      {1, 1, 1},
  };
  const std::vector<std::vector<std::int64_t>> val_grid = {
      {100, 100, 100},
      {100, 200, 100},
      {100, 100, 100},
  };

  RunScanlines(&writer, id_grid, val_grid);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(2, layer->GetFeatureCount());

  // Check geometries: polygon 1 should have an interior ring (hole).
  layer->ResetReading();
  bool found_polygon_with_hole = false;
  bool found_inner_polygon = false;

  while (std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()}) {
    const OGRGeometry* geom = feat->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    ASSERT_EQ(wkbPolygon, wkbFlatten(geom->getGeometryType()));
    const OGRPolygon* poly = geom->toPolygon();
    ASSERT_NE(nullptr, poly);

    const std::int64_t val = feat->GetFieldAsInteger64(field_idx);
    if (val == 100) {
      EXPECT_EQ(1, poly->getNumInteriorRings());
      found_polygon_with_hole = true;
    } else if (val == 200) {
      EXPECT_EQ(0, poly->getNumInteriorRings());
      found_inner_polygon = true;
    }
  }

  EXPECT_TRUE(found_polygon_with_hole);
  EXPECT_TRUE(found_inner_polygon);
}

TEST_F(PolygonizePolygonizerTest, OuterRingTracing) {
  // Uniform 2x2 raster bounded entirely by the outer polygon.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("outer", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), -1, gt, 0);

  const std::vector<std::vector<GInt32>> id_grid = {{5, 5}, {5, 5}};
  const std::vector<std::vector<std::int64_t>> val_grid = {{50, 50}, {50, 50}};

  RunScanlines(&writer, id_grid, val_grid);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  const OGRPolygon* poly = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly);
  EXPECT_EQ(0, poly->getNumInteriorRings());
  const OGRLinearRing* ext_ring = poly->getExteriorRing();
  ASSERT_NE(nullptr, ext_ring);
  EXPECT_EQ(5, ext_ring->getNumPoints());
}

TEST_F(PolygonizePolygonizerTest, MultipolygonMerging) {
  // Two disconnected components sharing the same polygon ID (1):
  // 1 2 1
  // 1 2 1
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("merged", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("poly_id", OFTInteger64);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));
  const int field_idx = layer->GetLayerDefn()->GetFieldIndex("poly_id");

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), field_idx,
                                        gt, 0);

  const std::vector<std::vector<GInt32>> id_grid = {
      {1, 2, 1},
      {1, 2, 1},
  };
  const std::vector<std::vector<std::int64_t>> val_grid = {
      {10, 20, 10},
      {10, 20, 10},
  };

  RunScanlines(&writer, id_grid, val_grid);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(2, layer->GetFeatureCount());
}

TEST_F(PolygonizePolygonizerTest, InvalidPolygonIdMasking) {
  // Masked pixels have invalid_id = -1 and must not be emitted to receiver.
  TestPolygonReceiver<std::int64_t> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {
      {-1, 1},
      {1, 1},
  };
  const std::vector<std::vector<std::int64_t>> val_grid = {
      {0, 10},
      {10, 10},
  };

  RunScanlines(&receiver, id_grid, val_grid, /*invalid_id=*/-1);
  EXPECT_EQ(1u, receiver.received().size());
  EXPECT_EQ(10, receiver.received()[0].cell_value);
}

TEST_F(PolygonizePolygonizerTest, SinglePixelRaster) {
  TestPolygonReceiver<float> receiver;
  const std::vector<std::vector<GInt32>> id_grid = {{42}};
  const std::vector<std::vector<float>> val_grid = {{3.14f}};

  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(1u, receiver.received().size());
  EXPECT_FLOAT_EQ(3.14f, receiver.received()[0].cell_value);
}

TEST_F(PolygonizePolygonizerTest, WideRasterScanline) {
  constexpr IndexType kCols = 16;
  std::vector<GInt32> row0_ids(kCols);
  std::vector<float> row0_vals(kCols);
  for (IndexType i = 0; i < kCols; ++i) {
    row0_ids[i] = static_cast<GInt32>(i);
    row0_vals[i] = static_cast<float>(i * 0.5f);
  }
  const std::vector<std::vector<GInt32>> id_grid = {row0_ids, row0_ids};
  const std::vector<std::vector<float>> val_grid = {row0_vals, row0_vals};

  TestPolygonReceiver<float> receiver;
  RunScanlines(&receiver, id_grid, val_grid);
  EXPECT_EQ(kCols, receiver.received().size());
}

// ---------------------------------------------------------------------------
// OGRPolygonWriter Unit and Edge Case Tests
// ---------------------------------------------------------------------------

TEST_F(PolygonizePolygonizerTest, OGRPolygonWriterBasicsAndFinalize) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer =
      ds->CreateLayer("writer_basics", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  const GDALGeoTransform gt;
  // Test constructor with commit interval > 0 (resets to 0 since MEM lacks
  // transactions).
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), -1, gt, 10);
  EXPECT_EQ(CE_None, writer.getErr());
  EXPECT_TRUE(writer.Finalize());
}

TEST_F(PolygonizePolygonizerTest, OGRPolygonWriterSyntheticArcFollowRightHand) {
  // Test direct receive with synthetic RPolygon where bFollowRighthand = true.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("righthand", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  OGRFieldDefn field_defn("val", OFTReal);
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&field_defn));
  const int field_idx = layer->GetLayerDefn()->GetFieldIndex("val");

  const GDALGeoTransform gt;
  OGRPolygonWriter<float> writer(OGRLayer::ToHandle(layer), field_idx, gt, 0);

  RPolygon poly;
  IndexedArc arc = poly.newArc(/*bFollowRighthand=*/true);
  arc.poArc->push_back(Point{0, 0});
  arc.poArc->push_back(Point{0, 1});
  arc.poArc->push_back(Point{1, 1});
  arc.poArc->push_back(Point{1, 0});
  poly.setArcConnection(arc, arc);
  poly.updateBottomRightPos(0, 0);

  writer.receive(&poly, 42.5f);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  EXPECT_FLOAT_EQ(42.5f, static_cast<float>(feat->GetFieldAsDouble(field_idx)));
  const OGRPolygon* poly_geom = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly_geom);
  const OGRLinearRing* ext_ring = poly_geom->getExteriorRing();
  ASSERT_NE(nullptr, ext_ring);
  EXPECT_EQ(5, ext_ring->getNumPoints());
  // Forward point order: Point{0, 0} (col 0, row 0) was first.
  EXPECT_DOUBLE_EQ(0.0, ext_ring->getX(0));
  EXPECT_DOUBLE_EQ(0.0, ext_ring->getY(0));
}

TEST_F(PolygonizePolygonizerTest, OGRPolygonWriterSyntheticArcFollowLeftHand) {
  // Test direct receive with synthetic RPolygon where bFollowRighthand = false.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("lefthand", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), -1, gt, 0);

  RPolygon poly;
  IndexedArc arc = poly.newArc(/*bFollowRighthand=*/false);
  arc.poArc->push_back(Point{0, 0});
  arc.poArc->push_back(Point{0, 1});
  arc.poArc->push_back(Point{1, 1});
  arc.poArc->push_back(Point{1, 0});
  poly.setArcConnection(arc, arc);
  poly.updateBottomRightPos(0, 0);

  writer.receive(&poly, 100);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  const OGRPolygon* poly_geom = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly_geom);
  const OGRLinearRing* ext_ring = poly_geom->getExteriorRing();
  ASSERT_NE(nullptr, ext_ring);
  EXPECT_EQ(5, ext_ring->getNumPoints());
  // Reverse point order: Point{1, 0} (col 0, row 1) was reversed to first.
  EXPECT_DOUBLE_EQ(0.0, ext_ring->getX(0));
  EXPECT_DOUBLE_EQ(1.0, ext_ring->getY(0));
}

TEST_F(PolygonizePolygonizerTest, OGRPolygonWriterMultiArcChainedRing) {
  // Test ring formed by 2 arcs linked together: arc0 -> arc1 -> arc0.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("chained", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), -1, gt, 0);

  RPolygon poly;
  IndexedArc arc0 = poly.newArc(/*bFollowRighthand=*/true);
  arc0.poArc->push_back(Point{0, 0});
  arc0.poArc->push_back(Point{0, 2});

  IndexedArc arc1 = poly.newArc(/*bFollowRighthand=*/true);
  arc1.poArc->push_back(Point{2, 2});
  arc1.poArc->push_back(Point{2, 0});

  poly.setArcConnection(arc0, arc1);
  poly.setArcConnection(arc1, arc0);
  poly.updateBottomRightPos(1, 1);

  writer.receive(&poly, 77);
  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());

  EXPECT_EQ(1, layer->GetFeatureCount());
  layer->ResetReading();
  std::unique_ptr<OGRFeature> feat{layer->GetNextFeature()};
  ASSERT_NE(nullptr, feat);
  const OGRPolygon* poly_geom = feat->GetGeometryRef()->toPolygon();
  ASSERT_NE(nullptr, poly_geom);
  EXPECT_EQ(5, poly_geom->getExteriorRing()->getNumPoints());
}

TEST_F(PolygonizePolygonizerTest, OGRPolygonWriterRingReuseAcrossReceives) {
  // Exercises the ring reuse branch in OGRPolygonWriter::receive:
  // 1st receive: poPolygon_ is initially empty (poFirstRing == nullptr).
  // 2nd receive: poPolygon_ has 1 ring and 0 interior rings
  // (poFirstRing->empty()). 3rd receive: poPolygon_ has interior rings
  // (poPolygon_->empty()).
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(mem_drv->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, ds);

  OGRLayer* layer = ds->CreateLayer("reuse", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, layer);

  const GDALGeoTransform gt;
  OGRPolygonWriter<std::int64_t> writer(OGRLayer::ToHandle(layer), -1, gt, 0);

  // Call 1: simple polygon with 1 ring.
  RPolygon poly1;
  IndexedArc a1 = poly1.newArc(true);
  a1.poArc->push_back(Point{0, 0});
  a1.poArc->push_back(Point{0, 1});
  a1.poArc->push_back(Point{1, 1});
  poly1.setArcConnection(a1, a1);
  writer.receive(&poly1, 1);

  // Call 2: simple polygon with 1 ring (reuses poFirstRing).
  RPolygon poly2;
  IndexedArc a2 = poly2.newArc(true);
  a2.poArc->push_back(Point{0, 0});
  a2.poArc->push_back(Point{0, 2});
  a2.poArc->push_back(Point{2, 2});
  poly2.setArcConnection(a2, a2);
  writer.receive(&poly2, 2);

  // Call 3: polygon with 2 rings (outer ring + interior ring).
  RPolygon poly3;
  IndexedArc a3_out = poly3.newArc(true);
  a3_out.poArc->push_back(Point{0, 0});
  a3_out.poArc->push_back(Point{0, 4});
  a3_out.poArc->push_back(Point{4, 4});
  poly3.setArcConnection(a3_out, a3_out);

  IndexedArc a3_in = poly3.newArc(true);
  a3_in.poArc->push_back(Point{1, 1});
  a3_in.poArc->push_back(Point{1, 2});
  a3_in.poArc->push_back(Point{2, 2});
  poly3.setArcConnection(a3_in, a3_in);
  writer.receive(&poly3, 3);

  // Call 4: simple polygon after a polygon with interior rings (takes else
  // branch).
  writer.receive(&poly1, 4);

  EXPECT_TRUE(writer.Finalize());
  EXPECT_EQ(CE_None, writer.getErr());
  EXPECT_EQ(4, layer->GetFeatureCount());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - alg/polygonize_polygonizer.cpp is compiled via
// textual_hdrs in google3 (polygonize_polygonizer.h is 100% covered).
