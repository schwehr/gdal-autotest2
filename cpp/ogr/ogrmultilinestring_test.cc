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

class TestMultiLineString : public OGRMultiLineString {
 public:
  using OGRMultiLineString::isCompatibleSubType;
};

TEST(OgrMultiLineStringTest, BasicProperties) {
  TestMultiLineString mls;
  EXPECT_STREQ("MULTILINESTRING", mls.getGeometryName());
  EXPECT_EQ(wkbMultiLineString, mls.getGeometryType());
  EXPECT_FALSE(mls.hasCurveGeometry(FALSE));
  EXPECT_FALSE(mls.hasCurveGeometry(TRUE));

  mls.set3D(TRUE);
  EXPECT_EQ(wkbMultiLineString25D, mls.getGeometryType());

  mls.setMeasured(TRUE);
  EXPECT_EQ(wkbMultiLineStringZM, mls.getGeometryType());

  mls.set3D(FALSE);
  EXPECT_EQ(wkbMultiLineStringM, mls.getGeometryType());

  EXPECT_TRUE(mls.isCompatibleSubType(wkbLineString));
  EXPECT_TRUE(mls.isCompatibleSubType(wkbLineString25D));
  EXPECT_FALSE(mls.isCompatibleSubType(wkbPolygon));
}

TEST(OgrMultiLineStringTest, CopyAndClone) {
  OGRMultiLineString mls;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mls.addGeometry(ls.get());

  // Copy constructor
  OGRMultiLineString mls_copy(mls);
  EXPECT_EQ(1, mls_copy.getNumGeometries());

  // Assignment operator
  OGRMultiLineString mls_assigned;
  mls_assigned = mls;
  EXPECT_EQ(1, mls_assigned.getNumGeometries());

  // Self-assignment
  const auto* ptr = &mls_assigned;
  mls_assigned = *ptr;
  EXPECT_EQ(1, mls_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRMultiLineString> mls_cloned(mls.clone());
  ASSERT_NE(nullptr, mls_cloned);
  EXPECT_EQ(1, mls_cloned->getNumGeometries());
}

TEST(OgrMultiLineStringTest, ExportToWkt) {
  OGRMultiLineString mls;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mls.addGeometry(ls.get());

  OGRWktOptions opts;
  OGRErr err = OGRERR_NONE;
  std::string wkt = mls.exportToWkt(opts, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_EQ("MULTILINESTRING ((0 0,1 1))", wkt);
}

TEST(OgrMultiLineStringTest, CastToMultiCurve) {
  auto* mls = new OGRMultiLineString();
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mls->addGeometry(ls.get());

  OGRMultiCurve* mc = OGRMultiLineString::CastToMultiCurve(mls);
  ASSERT_NE(nullptr, mc);
  EXPECT_STREQ("MULTICURVE", mc->getGeometryName());
  EXPECT_EQ(1, mc->getNumGeometries());
  delete mc;
}

TEST(OgrMultiLineStringTest, ImportFromWkbFastPathAndFallback) {
  OGRMultiLineString mls_template;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(10, 20);
  ls->addPoint(30, 40);
  mls_template.addGeometry(ls.get());

  std::vector<unsigned char> wkb(mls_template.WkbSize());
  EXPECT_EQ(OGRERR_NONE, mls_template.OGRGeometry::exportToWkb(
                             wkbNDR, wkb.data(), wkbVariantIso));

  // 1-part fast path import onto existing 1-part multilinestring
  OGRMultiLineString mls_fast;
  auto ls2 = std::make_unique<OGRLineString>();
  ls2->addPoint(0, 0);
  ls2->addPoint(1, 1);
  mls_fast.addGeometry(ls2.get());

  size_t bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE, mls_fast.importFromWkb(wkb.data(), wkb.size(),
                                                wkbVariantIso, bytes_consumed));
  EXPECT_EQ(wkb.size(), bytes_consumed);
  EXPECT_EQ(1, mls_fast.getNumGeometries());

  // Fast path with nSize == static_cast<size_t>(-1)
  bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            mls_fast.importFromWkb(wkb.data(), static_cast<size_t>(-1),
                                   wkbVariantIso, bytes_consumed));
  EXPECT_EQ(wkb.size(), bytes_consumed);

  // Fast path failure with truncated line string data
  std::vector<unsigned char> truncated_wkb(wkb.begin(), wkb.begin() + 10);
  bytes_consumed = 0;
  EXPECT_NE(OGRERR_NONE,
            mls_fast.importFromWkb(truncated_wkb.data(), truncated_wkb.size(),
                                   wkbVariantIso, bytes_consumed));
  EXPECT_TRUE(mls_fast.IsEmpty());

  // General fallback path (import onto empty geometry)
  OGRMultiLineString mls_fallback;
  bytes_consumed = 0;
  EXPECT_EQ(OGRERR_NONE,
            mls_fallback.importFromWkb(wkb.data(), wkb.size(), wkbVariantIso,
                                       bytes_consumed));
  EXPECT_EQ(1, mls_fallback.getNumGeometries());
}

}  // namespace
}  // namespace autotest2
