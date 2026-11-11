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

#include "third_party/geos/include/geos/geom/util/GeometryMapper.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::PrecisionModel;
using geos::geom::util::GeometryMapper;
using geos::io::WKTReader;
using geos::util::IllegalArgumentException;

class GeometryMapperTest : public ::testing::Test {
 protected:
  GeometryMapperTest()
      : factory_(GeometryFactory::create()), reader_(*factory_) {}

  std::unique_ptr<Geometry> ReadWkt(const std::string& wkt) const {
    return reader_.read(wkt);
  }

  GeometryFactory::Ptr factory_;
  WKTReader reader_;
};

TEST_F(GeometryMapperTest, MapAtomicAndEmptyAtomicGeometry) {
  auto pt = ReadWkt("POINT (1 2)");
  auto mapped = GeometryMapper::map(*pt, [this](const Geometry& g) {
    const CoordinateXY* c = g.getCoordinate();
    return factory_->createPoint(CoordinateXY(c->x * 2.0, c->y * 2.0));
  });
  auto expected = ReadWkt("POINT (2 4)");
  EXPECT_EQ(mapped->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(mapped->equalsExact(expected.get()));

  auto dropped = GeometryMapper::map(
      *pt,
      [](const Geometry&) -> std::unique_ptr<Geometry> { return nullptr; });
  EXPECT_EQ(dropped->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(dropped->isEmpty());

  auto empty_pt = ReadWkt("POINT EMPTY");
  int calls = 0;
  auto mapped_empty =
      GeometryMapper::map(*empty_pt, [&calls](const Geometry& g) {
        ++calls;
        return g.clone();
      });
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(mapped_empty->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(mapped_empty->isEmpty());
}

TEST_F(GeometryMapperTest, MapEmptyCollectionNeverInvokesOp) {
  for (const char* wkt : {"GEOMETRYCOLLECTION EMPTY", "MULTIPOINT EMPTY",
                          "MULTILINESTRING EMPTY", "MULTIPOLYGON EMPTY"}) {
    auto empty_col = ReadWkt(wkt);
    int calls = 0;
    auto mapped = GeometryMapper::map(*empty_col, [&calls](const Geometry& g) {
      ++calls;
      return g.clone();
    });
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(mapped->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_TRUE(mapped->isEmpty());
  }
}

TEST_F(GeometryMapperTest, MapHomogeneousMultiGeometryAndFiltering) {
  auto mp = ReadWkt("MULTIPOINT ((0 0), (10 0), (20 0))");
  auto buffered =
      GeometryMapper::map(*mp, [](const Geometry& g) { return g.buffer(1.0); });
  EXPECT_EQ(buffered->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(buffered->getNumGeometries(), 3u);

  auto filtered_single = GeometryMapper::map(
      *mp, [](const Geometry& g) -> std::unique_ptr<Geometry> {
        if (g.getCoordinate()->x > 15.0) {
          return g.clone();
        }
        return nullptr;
      });
  EXPECT_EQ(filtered_single->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(filtered_single->equalsExact(ReadWkt("POINT (20 0)").get()));

  auto filtered_two = GeometryMapper::map(
      *mp, [](const Geometry& g) -> std::unique_ptr<Geometry> {
        if (g.getCoordinate()->x >= 10.0) {
          return g.clone();
        }
        return nullptr;
      });
  EXPECT_EQ(filtered_two->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(
      filtered_two->equalsExact(ReadWkt("MULTIPOINT ((10 0), (20 0))").get()));

  auto filtered_all = GeometryMapper::map(
      *mp,
      [](const Geometry&) -> std::unique_ptr<Geometry> { return nullptr; });
  EXPECT_EQ(filtered_all->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(filtered_all->isEmpty());
}

TEST_F(GeometryMapperTest, MapNestedCollectionOnlyMapsTopLevel) {
  auto nested = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (0 0), "
      "MULTIPOINT ((1 1), (2 2)), "
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 1)))");

  std::vector<geos::geom::GeometryTypeId> seen_types;
  auto mapped = GeometryMapper::map(*nested, [&seen_types](const Geometry& g) {
    seen_types.push_back(g.getGeometryTypeId());
    return g.clone();
  });

  ASSERT_EQ(seen_types.size(), 3u);
  EXPECT_EQ(seen_types[0], geos::geom::GEOS_POINT);
  EXPECT_EQ(seen_types[1], geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(seen_types[2], geos::geom::GEOS_GEOMETRYCOLLECTION);

  ASSERT_EQ(mapped->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(mapped->getNumGeometries(), 3u);
  EXPECT_EQ(mapped->getGeometryN(1)->getGeometryTypeId(),
            geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(mapped->getGeometryN(2)->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
}

TEST_F(GeometryMapperTest, FlatMapEmptyInputsAndEmptyOutputs) {
  auto empty_gc = ReadWkt("GEOMETRYCOLLECTION EMPTY");

  auto res_gc = GeometryMapper::flatMap(
      *empty_gc, -1, [](const Geometry& g) { return g.clone(); });
  EXPECT_EQ(res_gc->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(res_gc->isEmpty());

  auto res_pt = GeometryMapper::flatMap(
      *empty_gc, 0, [](const Geometry& g) { return g.clone(); });
  EXPECT_EQ(res_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(res_pt->isEmpty());

  auto res_ls = GeometryMapper::flatMap(
      *empty_gc, 1, [](const Geometry& g) { return g.clone(); });
  EXPECT_EQ(res_ls->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(res_ls->isEmpty());

  auto res_poly = GeometryMapper::flatMap(
      *empty_gc, 2, [](const Geometry& g) { return g.clone(); });
  EXPECT_EQ(res_poly->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(res_poly->isEmpty());

  EXPECT_THROW(GeometryMapper::flatMap(
                   *empty_gc, 3, [](const Geometry& g) { return g.clone(); }),
               IllegalArgumentException);

  auto mp = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  auto all_dropped = GeometryMapper::flatMap(
      *mp, 2, [this](const Geometry& g) -> std::unique_ptr<Geometry> {
        if (g.getCoordinate()->x == 0.0) {
          return nullptr;
        }
        return factory_->createPoint();
      });
  EXPECT_EQ(all_dropped->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
  EXPECT_TRUE(all_dropped->isEmpty());
}

TEST_F(GeometryMapperTest,
       FlatMapSingleResultUnwrappedAndEmptyElementsSkipped) {
  auto mp = ReadWkt("MULTIPOINT ((0 0), (1 1))");
  auto one_pt = GeometryMapper::flatMap(
      *mp, 0, [this](const Geometry& g) -> std::unique_ptr<Geometry> {
        if (g.getCoordinate()->x == 0.0) {
          return nullptr;
        }
        return factory_->createPoint(CoordinateXY(10.0, 20.0));
      });
  EXPECT_EQ(one_pt->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(one_pt->equalsExact(ReadWkt("POINT (10 20)").get()));

  auto seed = ReadWkt("POINT (1 2)");
  auto expanded = GeometryMapper::flatMap(*seed, 0, [this](const Geometry&) {
    return ReadWkt("MULTIPOINT ((1 2), (3 4))");
  });
  EXPECT_EQ(expanded->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(
      expanded->equalsExact(ReadWkt("MULTIPOINT ((1 2), (3 4))").get()));

  auto with_empty_elem =
      GeometryMapper::flatMap(*seed, 0, [this](const Geometry&) {
        return ReadWkt("GEOMETRYCOLLECTION (POINT EMPTY, POINT (5 6))");
      });
  EXPECT_EQ(with_empty_elem->getGeometryTypeId(), geos::geom::GEOS_POINT);
  EXPECT_TRUE(with_empty_elem->equalsExact(ReadWkt("POINT (5 6)").get()));
}

TEST_F(GeometryMapperTest, FlatMapRecursiveInputFlattening) {
  auto nested = ReadWkt(
      "GEOMETRYCOLLECTION ("
      "POINT (0 0), "
      "MULTIPOINT ((1 1), (2 2)), "
      "MULTIPOLYGON EMPTY, "
      "GEOMETRYCOLLECTION (POINT (3 3), LINESTRING (0 0, 4 0)))");

  int atomic_calls = 0;
  auto flat_hetero =
      GeometryMapper::flatMap(*nested, -1, [&atomic_calls](const Geometry& g) {
        ++atomic_calls;
        return g.clone();
      });
  EXPECT_EQ(atomic_calls, 5);
  ASSERT_EQ(flat_hetero->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(flat_hetero->getNumGeometries(), 5u);
  for (std::size_t i = 0; i < flat_hetero->getNumGeometries(); ++i) {
    EXPECT_FALSE(flat_hetero->getGeometryN(i)->isCollection());
  }

  auto flat_pts =
      GeometryMapper::flatMap(*nested, 0, [this](const Geometry& g) {
        const CoordinateXY* c = g.getCoordinate();
        return factory_->createPoint(*c);
      });
  EXPECT_EQ(flat_pts->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_EQ(flat_pts->getNumGeometries(), 5u);
}

TEST_F(GeometryMapperTest, FlatMapFlattensNestedCollectionsReturnedByOp) {
  auto line = ReadWkt("LINESTRING (0 0, 1 1)");
  auto flattened = GeometryMapper::flatMap(*line, 0, [this](const Geometry&) {
    return ReadWkt(
        "GEOMETRYCOLLECTION ("
        "GEOMETRYCOLLECTION (POINT (0 0), POINT EMPTY), "
        "MULTIPOINT ((1 1), (2 2)))");
  });

  EXPECT_EQ(flattened->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
  EXPECT_TRUE(flattened->equalsExact(
      ReadWkt("MULTIPOINT ((0 0), (1 1), (2 2))").get()));
}

TEST_F(GeometryMapperTest, PreservesCustomFactoryAndSrid) {
  PrecisionModel pm(10.0);
  GeometryFactory::Ptr custom_factory = GeometryFactory::create(&pm, 3857);
  WKTReader custom_reader(*custom_factory);

  auto mp = custom_reader.read("MULTIPOINT ((1 2), (3 4))");

  auto mapped =
      GeometryMapper::map(*mp, [](const Geometry& g) { return g.clone(); });
  EXPECT_EQ(mapped->getFactory(), custom_factory.get());
  EXPECT_EQ(mapped->getSRID(), 3857);

  auto flat_empty = GeometryMapper::flatMap(
      *mp, 1,
      [](const Geometry&) -> std::unique_ptr<Geometry> { return nullptr; });
  EXPECT_EQ(flat_empty->getFactory(), custom_factory.get());
  EXPECT_EQ(flat_empty->getSRID(), 3857);
  EXPECT_EQ(flat_empty->getGeometryTypeId(), geos::geom::GEOS_LINESTRING);
  EXPECT_TRUE(flat_empty->isEmpty());
}

}  // namespace
}  // namespace geos_google
