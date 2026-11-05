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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestMultiPolygon : public OGRMultiPolygon {
 public:
  using OGRMultiPolygon::isCompatibleSubType;
};

using AddExpectedFn = OGRErr (OGRMultiPolygon::*)(const OGRGeometry*,
                                                  OGRwkbGeometryType);
AddExpectedFn g_add_expected_fn = nullptr;

template <AddExpectedFn Fn>
struct AccessAddExpected {
  AccessAddExpected() { g_add_expected_fn = Fn; }
  static AccessAddExpected instance;
};
template <AddExpectedFn Fn>
AccessAddExpected<Fn> AccessAddExpected<Fn>::instance;
template struct AccessAddExpected<
    &OGRMultiPolygon::_addGeometryWithExpectedSubGeometryType>;

std::unique_ptr<OGRPolygon> CreateSquarePolygon() {
  auto poly = std::make_unique<OGRPolygon>();
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(10, 0);
  ring.addPoint(10, 10);
  ring.addPoint(0, 10);
  ring.addPoint(0, 0);
  poly->addRing(&ring);
  return poly;
}

TEST(OgrMultiPolygonTest, BasicProperties) {
  TestMultiPolygon mp;
  EXPECT_STREQ("MULTIPOLYGON", mp.getGeometryName());
  EXPECT_EQ(wkbMultiPolygon, mp.getGeometryType());
  EXPECT_FALSE(mp.hasCurveGeometry(FALSE));
  EXPECT_FALSE(mp.hasCurveGeometry(TRUE));

  mp.set3D(TRUE);
  EXPECT_EQ(wkbMultiPolygon25D, mp.getGeometryType());

  mp.setMeasured(TRUE);
  EXPECT_EQ(wkbMultiPolygonZM, mp.getGeometryType());

  mp.set3D(FALSE);
  EXPECT_EQ(wkbMultiPolygonM, mp.getGeometryType());

  EXPECT_TRUE(mp.isCompatibleSubType(wkbPolygon));
  EXPECT_TRUE(mp.isCompatibleSubType(wkbPolygon25D));
  EXPECT_FALSE(mp.isCompatibleSubType(wkbPoint));
}

TEST(OgrMultiPolygonTest, CopyAndClone) {
  OGRMultiPolygon mp;
  auto poly = CreateSquarePolygon();
  mp.addGeometry(poly.get());

  // Copy constructor
  OGRMultiPolygon mp_copy(mp);
  EXPECT_EQ(1, mp_copy.getNumGeometries());

  // Assignment operator
  OGRMultiPolygon mp_assigned;
  mp_assigned = mp;
  EXPECT_EQ(1, mp_assigned.getNumGeometries());

  // Self assignment
  const auto* ptr = &mp_assigned;
  mp_assigned = *ptr;
  EXPECT_EQ(1, mp_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRMultiPolygon> mp_cloned(mp.clone());
  ASSERT_NE(nullptr, mp_cloned);
  EXPECT_EQ(1, mp_cloned->getNumGeometries());
}

TEST(OgrMultiPolygonTest, ExportToWkt) {
  OGRMultiPolygon mp;
  auto poly = CreateSquarePolygon();
  mp.addGeometry(poly.get());

  OGRWktOptions opts;
  OGRErr err = OGRERR_NONE;
  std::string wkt = mp.exportToWkt(opts, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_EQ("MULTIPOLYGON (((0 0,10 0,10 10,0 10,0 0)))", wkt);
}

TEST(OgrMultiPolygonTest, CastToMultiSurface) {
  auto* mp = new OGRMultiPolygon();
  auto poly = CreateSquarePolygon();
  mp->addGeometry(poly.get());

  OGRMultiSurface* ms = OGRMultiPolygon::CastToMultiSurface(mp);
  ASSERT_NE(nullptr, ms);
  EXPECT_STREQ("MULTISURFACE", ms->getGeometryName());
  EXPECT_EQ(1, ms->getNumGeometries());
  delete ms;
}

TEST(OgrMultiPolygonTest, ImportFromWkbFastPathAndFallback) {
  OGRMultiPolygon mp_template;
  auto poly = CreateSquarePolygon();
  mp_template.addGeometry(poly.get());

  std::vector<unsigned char> wkb(mp_template.WkbSize());
  EXPECT_EQ(OGRERR_NONE, mp_template.OGRGeometry::exportToWkb(
                             wkbNDR, wkb.data(), wkbVariantIso));

  // 1-part fast path import onto existing 1-part multipolygon
  OGRMultiPolygon mp_fast;
  auto poly2 = CreateSquarePolygon();
  mp_fast.addGeometry(poly2.get());

  size_t bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE, mp_fast.importFromWkb(wkb.data(), wkb.size(),
                                               wkbVariantIso, bytes_consumed));
  EXPECT_EQ(wkb.size(), bytes_consumed);
  EXPECT_EQ(1, mp_fast.getNumGeometries());

  // Fast path with nSize == static_cast<size_t>(-1)
  bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            mp_fast.importFromWkb(wkb.data(), static_cast<size_t>(-1),
                                  wkbVariantIso, bytes_consumed));
  EXPECT_EQ(wkb.size(), bytes_consumed);

  // Fast path failure with truncated polygon data
  std::vector<unsigned char> truncated_wkb(wkb.begin(), wkb.begin() + 10);
  bytes_consumed = 0;
  EXPECT_NE(OGRERR_NONE,
            mp_fast.importFromWkb(truncated_wkb.data(), truncated_wkb.size(),
                                  wkbVariantIso, bytes_consumed));
  EXPECT_TRUE(mp_fast.IsEmpty());

  // Fallback path (import onto empty geometry)
  OGRMultiPolygon mp_fallback;
  bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            mp_fallback.importFromWkb(wkb.data(), wkb.size(), wkbVariantIso,
                                      bytes_consumed));
  EXPECT_EQ(1, mp_fallback.getNumGeometries());
}

TEST(OgrMultiPolygonTest, AddGeometryWithExpectedSubGeometryType) {
  OGRMultiPolygon mp;
  auto poly = CreateSquarePolygon();

  ASSERT_NE(nullptr, g_add_expected_fn);

  // Type matches expected sub-geometry type (wkbPolygon)
  EXPECT_EQ(OGRERR_NONE, (mp.*g_add_expected_fn)(poly.get(), wkbPolygon));
  EXPECT_EQ(1, mp.getNumGeometries());

  // Type mismatch via _addGeometryWithExpectedSubGeometryType
  EXPECT_EQ(OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
            (mp.*g_add_expected_fn)(poly.get(), wkbLineString));
  EXPECT_EQ(1, mp.getNumGeometries());
}

}  // namespace
}  // namespace autotest2
