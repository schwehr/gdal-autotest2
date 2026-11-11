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

#include "third_party/geos/include/geos/shape/fractal/HilbertEncoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/shape/fractal/HilbertCode.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::shape::fractal::HilbertCode;
using geos::shape::fractal::HilbertEncoder;
using geos::util::IllegalArgumentException;

TEST(HilbertEncoderTest, EncodeLevel1CornersAndBoxes) {
  Envelope extent(0.0, 10.0, 0.0, 10.0);
  HilbertEncoder encoder(1, extent);

  const Envelope p00(0.0, 0.0, 0.0, 0.0);
  const Envelope p01(0.0, 0.0, 10.0, 10.0);
  const Envelope p11(10.0, 10.0, 10.0, 10.0);
  const Envelope p10(10.0, 10.0, 0.0, 0.0);

  EXPECT_EQ(encoder.encode(&p00), 0u);
  EXPECT_EQ(encoder.encode(&p01), 1u);
  EXPECT_EQ(encoder.encode(&p11), 2u);
  EXPECT_EQ(encoder.encode(&p10), 3u);

  const Envelope box_origin(-2.0, 2.0, -2.0, 2.0);
  const Envelope box_top_right(8.0, 12.0, 8.0, 12.0);
  EXPECT_EQ(encoder.encode(&box_origin), 0u);
  EXPECT_EQ(encoder.encode(&box_top_right), 2u);
}

TEST(HilbertEncoderTest, EncodeLevel2ExactGridAndShiftedExtent) {
  Envelope unit_grid(0.0, 3.0, 0.0, 3.0);
  HilbertEncoder encoder(2, unit_grid);
  for (uint32_t x = 0; x <= 3; ++x) {
    for (uint32_t y = 0; y <= 3; ++y) {
      const Envelope pt(static_cast<double>(x), static_cast<double>(x),
                        static_cast<double>(y), static_cast<double>(y));
      EXPECT_EQ(encoder.encode(&pt), HilbertCode::encode(2, x, y))
          << "x=" << x << " y=" << y;
    }
  }

  Envelope shifted(10.0, 13.0, 20.0, 23.0);
  HilbertEncoder shifted_encoder(2, shifted);
  const Envelope target(12.0, 14.0, 20.0, 22.0);  // mid = (13, 21) -> (3, 1)
  EXPECT_EQ(shifted_encoder.encode(&target), HilbertCode::encode(2, 3, 1));
}

TEST(HilbertEncoderTest, EncodeBelowMinimumClampsOrdinatesToZero) {
  Envelope extent(10.0, 20.0, 30.0, 40.0);
  HilbertEncoder encoder(4, extent);

  const Envelope below_both(0.0, 4.0, 5.0, 9.0);
  EXPECT_EQ(encoder.encode(&below_both), HilbertCode::encode(4, 0, 0));

  const Envelope on_minx_below_miny(5.0, 15.0, 10.0, 20.0);
  EXPECT_EQ(encoder.encode(&on_minx_below_miny), HilbertCode::encode(4, 0, 0));

  const Envelope above_minx_below_miny(16.0, 16.0, 10.0, 20.0);
  const uint32_t expected_x =
      static_cast<uint32_t>((16.0 - 10.0) / (10.0 / 15.0));
  EXPECT_EQ(encoder.encode(&above_minx_below_miny),
            HilbertCode::encode(4, expected_x, 0));

  const Envelope below_minx_above_miny(0.0, 4.0, 36.0, 36.0);
  const uint32_t expected_y =
      static_cast<uint32_t>((36.0 - 30.0) / (10.0 / 15.0));
  EXPECT_EQ(encoder.encode(&below_minx_above_miny),
            HilbertCode::encode(4, 0, expected_y));
}

TEST(HilbertEncoderTest, EncodeZeroStrideExtents) {
  // Zero width -> strideX == 0, x always stays 0 even when midx > minx.
  Envelope vertical(5.0, 5.0, 0.0, 3.0);
  HilbertEncoder vertical_encoder(2, vertical);
  for (uint32_t y = 0; y <= 3; ++y) {
    const Envelope env(8.0, 8.0, static_cast<double>(y),
                       static_cast<double>(y));
    EXPECT_EQ(vertical_encoder.encode(&env), HilbertCode::encode(2, 0, y));
  }

  // Zero height -> strideY == 0, y always stays 0 even when midy > miny.
  Envelope horizontal(0.0, 3.0, 5.0, 5.0);
  HilbertEncoder horizontal_encoder(2, horizontal);
  for (uint32_t x = 0; x <= 3; ++x) {
    const Envelope env(static_cast<double>(x), static_cast<double>(x), 8.0,
                       8.0);
    EXPECT_EQ(horizontal_encoder.encode(&env), HilbertCode::encode(2, x, 0));
  }

  // Point extent -> strideX == 0 && strideY == 0.
  Envelope point_extent(5.0, 5.0, 5.0, 5.0);
  HilbertEncoder point_encoder(2, point_extent);
  const Envelope probe(10.0, 12.0, 10.0, 12.0);
  EXPECT_EQ(point_encoder.encode(&probe), 0u);
}

TEST(HilbertEncoderTest, EncodeWithOutOfRangeLevelThrows) {
  Envelope extent(0.0, 10.0, 0.0, 10.0);
  HilbertEncoder encoder(17, extent);
  EXPECT_THROW(encoder.encode(&extent), IllegalArgumentException);
}

TEST(HilbertEncoderTest, GetEnvelopeEmptyAndMixedGeometries) {
  const auto factory = GeometryFactory::create();

  std::vector<Geometry*> empty_vec;
  const Envelope empty_result =
      HilbertEncoder::getEnvelope(empty_vec.begin(), empty_vec.end());
  EXPECT_TRUE(empty_result.isNull());

  auto empty_pt = factory->createPoint();
  std::vector<Geometry*> only_empty = {empty_pt.get()};
  EXPECT_TRUE(HilbertEncoder::getEnvelope(only_empty.begin(), only_empty.end())
                  .isNull());

  auto p1 = factory->createPoint(Coordinate(-5.0, 10.0));
  const Envelope box_env(2.0, 15.0, -3.0, 4.0);
  auto box = factory->toGeometry(&box_env);
  auto trailing_empty = factory->createPoint();
  std::vector<Geometry*> geoms = {empty_pt.get(), p1.get(), box.get(),
                                  trailing_empty.get()};

  const Envelope combined =
      HilbertEncoder::getEnvelope(geoms.begin(), geoms.end());
  EXPECT_FALSE(combined.isNull());
  EXPECT_EQ(combined.getMinX(), -5.0);
  EXPECT_EQ(combined.getMaxX(), 15.0);
  EXPECT_EQ(combined.getMinY(), -3.0);
  EXPECT_EQ(combined.getMaxY(), 10.0);
}

TEST(HilbertEncoderTest, SortVectorCornersInDescendingHilbertOrder) {
  const auto factory = GeometryFactory::create();

  auto p00 = factory->createPoint(Coordinate(0.0, 0.0));
  auto p01 = factory->createPoint(Coordinate(0.0, 100.0));
  auto p11 = factory->createPoint(Coordinate(100.0, 100.0));
  auto p10 = factory->createPoint(Coordinate(100.0, 0.0));

  std::vector<Geometry*> geoms = {p00.get(), p11.get(), p01.get(), p10.get()};
  HilbertEncoder::sort(geoms);

  ASSERT_EQ(geoms.size(), 4u);
  EXPECT_EQ(geoms[0], p10.get());
  EXPECT_EQ(geoms[1], p11.get());
  EXPECT_EQ(geoms[2], p01.get());
  EXPECT_EQ(geoms[3], p00.get());
}

TEST(HilbertEncoderTest, SortMixedPointsAndPolygonsPreservesAllAndOrdersCodes) {
  const auto factory = GeometryFactory::create();

  std::vector<std::unique_ptr<Geometry>> owned;
  const std::vector<Envelope> boxes = {
      Envelope(-50.0, -40.0, -25.0, -15.0), Envelope(40.0, 50.0, -25.0, -15.0),
      Envelope(0.0, 10.0, 20.0, 30.0),      Envelope(-30.0, -10.0, 50.0, 75.0),
      Envelope(25.0, 45.0, 60.0, 70.0),     Envelope(-5.0, 5.0, -5.0, 5.0),
  };
  for (const Envelope& env : boxes) {
    owned.push_back(factory->toGeometry(&env));
  }
  owned.push_back(factory->createPoint(Coordinate(-45.0, 70.0)));
  owned.push_back(factory->createPoint(Coordinate(48.0, -20.0)));
  owned.push_back(factory->createPoint(Coordinate(12.0, 12.0)));

  std::vector<Geometry*> geoms;
  geoms.reserve(owned.size());
  for (const auto& g : owned) {
    geoms.push_back(g.get());
  }
  const std::set<Geometry*> original_set(geoms.begin(), geoms.end());

  Envelope extent = HilbertEncoder::getEnvelope(geoms.begin(), geoms.end());
  HilbertEncoder encoder(12, extent);

  HilbertEncoder::sort(geoms);

  EXPECT_EQ(geoms.size(), original_set.size());
  EXPECT_EQ(std::set<Geometry*>(geoms.begin(), geoms.end()), original_set);
  for (std::size_t i = 0; i + 1 < geoms.size(); ++i) {
    const uint32_t code_curr = encoder.encode(geoms[i]->getEnvelopeInternal());
    const uint32_t code_next =
        encoder.encode(geoms[i + 1]->getEnvelopeInternal());
    EXPECT_GE(code_curr, code_next) << "at index " << i;
  }
}

TEST(HilbertEncoderTest, SortIteratorRangeAndDegenerateCases) {
  const auto factory = GeometryFactory::create();

  // Template iterator range sort over an array of const Geometry*.
  auto a = factory->createPoint(Coordinate(0.0, 0.0));
  auto b = factory->createPoint(Coordinate(0.0, 10.0));
  auto c = factory->createPoint(Coordinate(10.0, 10.0));
  auto d = factory->createPoint(Coordinate(10.0, 0.0));
  std::array<const Geometry*, 4> arr = {a.get(), b.get(), c.get(), d.get()};
  HilbertEncoder::sort(arr.begin(), arr.end());
  EXPECT_EQ(arr[0], d.get());
  EXPECT_EQ(arr[1], c.get());
  EXPECT_EQ(arr[2], b.get());
  EXPECT_EQ(arr[3], a.get());

  // Single geometry.
  std::vector<Geometry*> single = {a.get()};
  HilbertEncoder::sort(single);
  ASSERT_EQ(single.size(), 1u);
  EXPECT_EQ(single[0], a.get());

  // Coincident points (zero width and zero height).
  auto dup1 = factory->createPoint(Coordinate(3.0, 7.0));
  auto dup2 = factory->createPoint(Coordinate(3.0, 7.0));
  std::vector<Geometry*> coincident = {dup1.get(), dup2.get()};
  HilbertEncoder::sort(coincident);
  EXPECT_EQ(coincident.size(), 2u);

  // Vertical collinear points (strideX == 0).
  auto v1 = factory->createPoint(Coordinate(5.0, 0.0));
  auto v2 = factory->createPoint(Coordinate(5.0, 10.0));
  auto v3 = factory->createPoint(Coordinate(5.0, 20.0));
  std::vector<Geometry*> vertical = {v1.get(), v2.get(), v3.get()};
  Envelope v_ext =
      HilbertEncoder::getEnvelope(vertical.begin(), vertical.end());
  HilbertEncoder v_enc(12, v_ext);
  HilbertEncoder::sort(vertical);
  EXPECT_GE(v_enc.encode(vertical[0]->getEnvelopeInternal()),
            v_enc.encode(vertical[1]->getEnvelopeInternal()));
  EXPECT_GE(v_enc.encode(vertical[1]->getEnvelopeInternal()),
            v_enc.encode(vertical[2]->getEnvelopeInternal()));

  // Horizontal collinear points (strideY == 0).
  auto h1 = factory->createPoint(Coordinate(0.0, 5.0));
  auto h2 = factory->createPoint(Coordinate(10.0, 5.0));
  auto h3 = factory->createPoint(Coordinate(20.0, 5.0));
  std::vector<Geometry*> horizontal = {h1.get(), h2.get(), h3.get()};
  Envelope h_ext =
      HilbertEncoder::getEnvelope(horizontal.begin(), horizontal.end());
  HilbertEncoder h_enc(12, h_ext);
  HilbertEncoder::sort(horizontal);
  EXPECT_GE(h_enc.encode(horizontal[0]->getEnvelopeInternal()),
            h_enc.encode(horizontal[1]->getEnvelopeInternal()));
  EXPECT_GE(h_enc.encode(horizontal[1]->getEnvelopeInternal()),
            h_enc.encode(horizontal[2]->getEnvelopeInternal()));
}

}  // namespace
}  // namespace geos_google
