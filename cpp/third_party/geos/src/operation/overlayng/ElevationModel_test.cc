// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/overlayng/ElevationModel.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryCollection;
using geos::geom::GeometryFactory;
using geos::geom::LineString;
using geos::geom::Point;
using geos::geom::Polygon;
using geos::io::WKTReader;
using geos::operation::overlayng::ElevationModel;

class ElevationModelTest : public ::testing::Test {
 protected:
  GeometryFactory::Ptr factory_ = GeometryFactory::create();
  WKTReader reader_{*factory_};
};

TEST_F(ElevationModelTest, EmptyInputsYieldNanZAndNoOpPopulateZ) {
  auto empty_pt = reader_.read("POINT EMPTY");
  auto empty_line = reader_.read("LINESTRING EMPTY");

  auto single_model = ElevationModel::create(*empty_pt);
  EXPECT_TRUE(std::isnan(single_model->getZ(0.0, 0.0)));
  EXPECT_TRUE(std::isnan(single_model->getZ(10.0, -5.0)));

  auto pair_model = ElevationModel::create(*empty_pt, *empty_line);
  EXPECT_TRUE(std::isnan(pair_model->getZ(1.0, 2.0)));

  auto target_xy = reader_.read<Point>("POINT (1 2)");
  auto target_z = reader_.read<Point>("POINT Z (1 2 9)");
  pair_model->populateZ(*target_xy);
  pair_model->populateZ(*target_z);

  EXPECT_FALSE(target_xy->hasZ());
  EXPECT_TRUE(std::isnan(target_xy->getZ()));
  EXPECT_TRUE(target_z->hasZ());
  EXPECT_DOUBLE_EQ(target_z->getZ(), 9.0);
}

TEST_F(ElevationModelTest, CreatePairWithOneEmptyGeometry) {
  auto empty_geom = reader_.read("POLYGON EMPTY");
  auto pt_z = reader_.read("POINT Z (3 4 12.5)");

  auto first_empty = ElevationModel::create(*empty_geom, *pt_z);
  EXPECT_DOUBLE_EQ(first_empty->getZ(3.0, 4.0), 12.5);
  EXPECT_DOUBLE_EQ(first_empty->getZ(50.0, -20.0), 12.5);

  auto second_empty = ElevationModel::create(*pt_z, *empty_geom);
  EXPECT_DOUBLE_EQ(second_empty->getZ(3.0, 4.0), 12.5);
  EXPECT_DOUBLE_EQ(second_empty->getZ(-10.0, 99.0), 12.5);
}

TEST_F(ElevationModelTest, CellAveragesAndEmptyCellFallbackAndClamping) {
  // Extent [0, 9] x [0, 9] divided into 3x3 cells of size 3x3:
  // - Cell (0, 0): vertices (0, 0, 10) and (1, 1, 20) -> cell avg 15.0
  // - Cell (2, 0): vertex (7, 1, 45) -> cell avg 45.0
  // - Cell (2, 2): vertex (9, 9, 60) -> cell avg 60.0
  // Average over the 3 populated cells: (15 + 45 + 60) / 3 = 40.0.
  auto geom1 = reader_.read("LINESTRING Z (0 0 10, 1 1 20, 9 9 60)");
  auto geom2 = reader_.read("POINT Z (7 1 45)");
  auto model = ElevationModel::create(*geom1, *geom2);

  // First call initializes the model; subsequent calls reuse initialized state.
  EXPECT_DOUBLE_EQ(model->getZ(0.5, 0.5), 15.0);
  EXPECT_DOUBLE_EQ(model->getZ(8.0, 2.0), 45.0);
  EXPECT_DOUBLE_EQ(model->getZ(8.0, 8.0), 60.0);

  // Empty interior cell (1, 1) receives average across populated cells.
  EXPECT_DOUBLE_EQ(model->getZ(4.5, 4.5), 40.0);

  // Coordinates outside the extent clamp to nearest border cells.
  EXPECT_DOUBLE_EQ(model->getZ(-5.0, -5.0), 15.0);
  EXPECT_DOUBLE_EQ(model->getZ(15.0, -5.0), 45.0);
  EXPECT_DOUBLE_EQ(model->getZ(15.0, 15.0), 60.0);
  // Clamps to empty cell (0, 2), which falls back to model average.
  EXPECT_DOUBLE_EQ(model->getZ(-5.0, 15.0), 40.0);
}

TEST_F(ElevationModelTest, ZeroWidthAndZeroHeightDegenerateExtents) {
  // Zero width -> numCellX collapses to 1 while numCellY remains 3.
  auto vertical = reader_.read("LINESTRING Z (5 0 10, 5 6 20, 5 9 40)");
  auto v_model = ElevationModel::create(*vertical);
  // Cell (0, 0) = 10; Cell (0, 1) = empty; Cell (0, 2) = avg(20, 40) = 30.
  // Model average = (10 + 30) / 2 = 20.
  EXPECT_DOUBLE_EQ(v_model->getZ(0.0, 1.0), 10.0);
  EXPECT_DOUBLE_EQ(v_model->getZ(100.0, 4.5), 20.0);
  EXPECT_DOUBLE_EQ(v_model->getZ(-50.0, 8.0), 30.0);

  // Zero height -> numCellY collapses to 1 while numCellX remains 3.
  auto horizontal = reader_.read("LINESTRING Z (0 5 10, 9 5 50)");
  auto h_model = ElevationModel::create(*horizontal);
  // Cell (0, 0) = 10; Cell (1, 0) = empty; Cell (2, 0) = 50 -> model avg = 30.
  EXPECT_DOUBLE_EQ(h_model->getZ(1.0, -99.0), 10.0);
  EXPECT_DOUBLE_EQ(h_model->getZ(4.5, 99.0), 30.0);
  EXPECT_DOUBLE_EQ(h_model->getZ(8.5, 5.0), 50.0);

  // Single point -> zero width and zero height -> 1x1 grid.
  auto single_pt = reader_.read("POINT Z (2 3 77)");
  auto p_model = ElevationModel::create(*single_pt);
  EXPECT_DOUBLE_EQ(p_model->getZ(2.0, 3.0), 77.0);
  EXPECT_DOUBLE_EQ(p_model->getZ(-10.0, 20.0), 77.0);
}

TEST_F(ElevationModelTest, CustomGridDimensionsAndMultipleAddCalls) {
  Envelope extent(0.0, 10.0, 0.0, 4.0);
  ElevationModel model(extent, 5, 2);

  auto p1 = reader_.read("POINT Z (1 1 10)");
  auto p2 = reader_.read("POINT Z (3 1 30)");
  auto p3 = reader_.read("POINT Z (9 3 50)");
  model.add(*p1);
  model.add(*p2);
  model.add(*p3);

  EXPECT_DOUBLE_EQ(model.getZ(0.5, 0.5), 10.0);
  EXPECT_DOUBLE_EQ(model.getZ(2.5, 1.5), 30.0);
  EXPECT_DOUBLE_EQ(model.getZ(9.5, 3.5), 50.0);
  // Cell (2, 1) at (5, 3) is empty -> average of (10, 30, 50) = 30.
  EXPECT_DOUBLE_EQ(model.getZ(5.0, 3.0), 30.0);
}

TEST_F(ElevationModelTest, AddIgnoresNanZAndStopsOnXYGeometry) {
  auto seq = std::make_unique<CoordinateSequence>(3u, true, false);
  seq->setAt(Coordinate(1.0, 1.0, 10.0), 0);
  seq->setAt(Coordinate(5.0, 5.0, DoubleNotANumber), 1);
  seq->setAt(Coordinate(8.0, 8.0, 30.0), 2);
  auto line_with_nan_z = factory_->createLineString(std::move(seq));

  Envelope extent(0.0, 9.0, 0.0, 9.0);
  ElevationModel model(extent, 3, 3);
  model.add(*line_with_nan_z);

  EXPECT_DOUBLE_EQ(model.getZ(1.0, 1.0), 10.0);
  EXPECT_DOUBLE_EQ(model.getZ(8.0, 8.0), 30.0);
  // Vertex (5, 5, NaN) was skipped, so cell (1, 1) falls back to averageZ = 20.
  EXPECT_DOUBLE_EQ(model.getZ(5.0, 5.0), 20.0);

  // Adding a 2D geometry short-circuits Filter::isDone().
  auto line_xy = reader_.read("LINESTRING (0 0, 5 5, 9 9)");
  ElevationModel xy_model(extent, 3, 3);
  xy_model.add(*line_xy);
  EXPECT_TRUE(std::isnan(xy_model.getZ(5.0, 5.0)));

  // GeometryCollection exercises GeometryCollection::apply_ro with
  // Filter::isDone() and Filter::isGeometryChanged().
  std::vector<std::unique_ptr<Geometry>> parts;
  parts.push_back(reader_.read("POINT Z (1 1 12)"));
  parts.push_back(reader_.read("POINT (5 5)"));
  parts.push_back(reader_.read("POINT Z (8 8 40)"));
  auto collection = factory_->createGeometryCollection(std::move(parts));

  ElevationModel gc_model(extent, 3, 3);
  gc_model.add(*collection);
  // The first point contributes 12; the second point lacks Z and stops
  // scanning.
  EXPECT_DOUBLE_EQ(gc_model.getZ(1.0, 1.0), 12.0);
  EXPECT_DOUBLE_EQ(gc_model.getZ(8.0, 8.0), 12.0);
}

TEST_F(ElevationModelTest, PopulateZPopulatesMissingZAndPreservesExistingZ) {
  auto src = reader_.read("LINESTRING Z (0 0 10, 1 1 10, 9 9 30)");
  auto model = ElevationModel::create(*src);
  // Note: do not call getZ() beforehand so populateZ() triggers init().

  auto xy_line = reader_.read<LineString>("LINESTRING (1 1, 4.5 4.5, 8 8)");
  model->populateZ(*xy_line);
  ASSERT_TRUE(xy_line->hasZ());
  const CoordinateSequence* xy_coords = xy_line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(xy_coords->getAt(0).z, 10.0);
  EXPECT_DOUBLE_EQ(xy_coords->getAt(1).z, 20.0);
  EXPECT_DOUBLE_EQ(xy_coords->getAt(2).z, 30.0);

  // Mixed Z sequence: finite Z values are preserved, NaN Z is populated.
  auto mixed_seq = std::make_unique<CoordinateSequence>(2u, true, false);
  mixed_seq->setAt(Coordinate(1.0, 1.0, 99.0), 0);
  mixed_seq->setAt(Coordinate(8.0, 8.0, DoubleNotANumber), 1);
  auto mixed_line = factory_->createLineString(std::move(mixed_seq));
  model->populateZ(*mixed_line);
  const CoordinateSequence* mixed_coords = mixed_line->getCoordinatesRO();
  EXPECT_DOUBLE_EQ(mixed_coords->getAt(0).z, 99.0);
  EXPECT_DOUBLE_EQ(mixed_coords->getAt(1).z, 30.0);

  // 2D polygon receives per-cell / average Z values.
  auto poly = reader_.read<Polygon>("POLYGON ((1 1, 8 1, 8 8, 1 8, 1 1))");
  model->populateZ(*poly);
  ASSERT_TRUE(poly->hasZ());
  auto poly_coords = poly->getCoordinates();
  ASSERT_EQ(poly_coords->size(), 5u);
  EXPECT_DOUBLE_EQ(poly_coords->getAt(0).z, 10.0);
  EXPECT_DOUBLE_EQ(poly_coords->getAt(1).z, 20.0);
  EXPECT_DOUBLE_EQ(poly_coords->getAt(2).z, 30.0);
  EXPECT_DOUBLE_EQ(poly_coords->getAt(3).z, 20.0);
  EXPECT_DOUBLE_EQ(poly_coords->getAt(4).z, 10.0);

  // XYM point gets Z populated while preserving M.
  auto xym_pt = reader_.read<Point>("POINT M (1 1 42)");
  model->populateZ(*xym_pt);
  EXPECT_TRUE(xym_pt->hasZ());
  EXPECT_TRUE(xym_pt->hasM());
  EXPECT_DOUBLE_EQ(xym_pt->getZ(), 10.0);
  EXPECT_DOUBLE_EQ(xym_pt->getM(), 42.0);

  // Empty geometry passed to populateZ is a safe no-op.
  auto empty_pt = reader_.read<Point>("POINT EMPTY");
  model->populateZ(*empty_pt);
  EXPECT_TRUE(empty_pt->isEmpty());
}

TEST_F(ElevationModelTest, PopulateZShortCircuitsWhenModelHasNoZ) {
  auto xy_src = reader_.read("LINESTRING (0 0, 9 9)");
  auto model = ElevationModel::create(*xy_src);

  auto pt_xy = reader_.read<Point>("POINT (2 2)");
  auto pt_z = reader_.read<Point>("POINT Z (2 2 7.5)");
  model->populateZ(*pt_xy);
  model->populateZ(*pt_z);

  EXPECT_FALSE(pt_xy->hasZ());
  EXPECT_TRUE(std::isnan(pt_xy->getZ()));
  EXPECT_TRUE(pt_z->hasZ());
  EXPECT_DOUBLE_EQ(pt_z->getZ(), 7.5);
}

}  // namespace
}  // namespace geos_google
