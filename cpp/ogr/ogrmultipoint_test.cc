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

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestMultiPoint : public OGRMultiPoint {
 public:
  using OGRMultiPoint::isCompatibleSubType;
};

class FailingMultiPoint : public OGRMultiPoint {
 public:
  OGRErr addGeometryDirectly(OGRGeometry* /* poNewGeom */) override {
    return OGRERR_FAILURE;
  }
};

TEST(OgrMultiPointTest, BasicProperties) {
  TestMultiPoint mp;
  EXPECT_STREQ("MULTIPOINT", mp.getGeometryName());
  EXPECT_EQ(0, mp.getDimension());
  EXPECT_EQ(wkbMultiPoint, mp.getGeometryType());
  EXPECT_FALSE(mp.hasCurveGeometry(FALSE));
  EXPECT_FALSE(mp.hasCurveGeometry(TRUE));

  mp.set3D(TRUE);
  EXPECT_EQ(wkbMultiPoint25D, mp.getGeometryType());

  mp.setMeasured(TRUE);
  EXPECT_EQ(wkbMultiPointZM, mp.getGeometryType());

  mp.set3D(FALSE);
  EXPECT_EQ(wkbMultiPointM, mp.getGeometryType());

  EXPECT_TRUE(mp.isCompatibleSubType(wkbPoint));
  EXPECT_TRUE(mp.isCompatibleSubType(wkbPoint25D));
  EXPECT_FALSE(mp.isCompatibleSubType(wkbLineString));
}

TEST(OgrMultiPointTest, CopyAndClone) {
  OGRMultiPoint mp;
  auto pt = std::make_unique<OGRPoint>(1, 2);
  mp.addGeometry(pt.get());

  // Copy constructor
  OGRMultiPoint mp_copy(mp);
  EXPECT_EQ(1, mp_copy.getNumGeometries());

  // Assignment operator
  OGRMultiPoint mp_assigned;
  mp_assigned = mp;
  EXPECT_EQ(1, mp_assigned.getNumGeometries());

  // Self assignment
  const auto* ptr = &mp_assigned;
  mp_assigned = *ptr;
  EXPECT_EQ(1, mp_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRMultiPoint> mp_cloned(mp.clone());
  ASSERT_NE(nullptr, mp_cloned);
  EXPECT_EQ(1, mp_cloned->getNumGeometries());
}

TEST(OgrMultiPointTest, ExportToWkt) {
  // Empty
  {
    OGRMultiPoint mp;
    OGRWktOptions opts;
    OGRErr err = OGRERR_NONE;
    EXPECT_EQ("MULTIPOINT EMPTY", mp.exportToWkt(opts, &err));
    EXPECT_EQ(OGRERR_NONE, err);
  }

  // All empty subgeometries
  {
    OGRMultiPoint mp;
    auto pt = std::make_unique<OGRPoint>();
    mp.addGeometry(pt.get());
    OGRWktOptions opts;
    OGRErr err = OGRERR_NONE;
    EXPECT_EQ("MULTIPOINT EMPTY", mp.exportToWkt(opts, &err));
    EXPECT_EQ(OGRERR_NONE, err);
  }

  // Old OGC variant vs ISO variant
  {
    OGRMultiPoint mp;
    auto p1 = std::make_unique<OGRPoint>(1, 2);
    auto p2 = std::make_unique<OGRPoint>(3, 4);
    mp.addGeometry(p1.get());
    mp.addGeometry(p2.get());

    OGRWktOptions opts_ogc;
    opts_ogc.variant = wkbVariantOldOgc;
    OGRErr err = OGRERR_NONE;
    EXPECT_EQ("MULTIPOINT (1 2,3 4)", mp.exportToWkt(opts_ogc, &err));
    EXPECT_EQ(OGRERR_NONE, err);

    OGRWktOptions opts_iso;
    opts_iso.variant = wkbVariantIso;
    EXPECT_EQ("MULTIPOINT ((1 2),(3 4))", mp.exportToWkt(opts_iso, &err));
    EXPECT_EQ(OGRERR_NONE, err);
  }
}

TEST(OgrMultiPointTest, ImportFromWktNonBracketed) {
  // Empty
  {
    const char* pszWkt = "MULTIPOINT EMPTY";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_NONE, mp.importFromWkt(&pszWkt));
    EXPECT_TRUE(mp.IsEmpty());
  }

  // 2D
  {
    const char* pszWkt = "MULTIPOINT (0 0, 1 1)";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_NONE, mp.importFromWkt(&pszWkt));
    EXPECT_EQ(2, mp.getNumGeometries());
  }

  // Z, M, ZM
  {
    const char* pszWktZ = "MULTIPOINT Z (0 0 1, 1 1 2)";
    OGRMultiPoint mpZ;
    EXPECT_EQ(OGRERR_NONE, mpZ.importFromWkt(&pszWktZ));
    EXPECT_TRUE(mpZ.Is3D());

    const char* pszWktM = "MULTIPOINT M (0 0 1, 1 1 2)";
    OGRMultiPoint mpM;
    EXPECT_EQ(OGRERR_NONE, mpM.importFromWkt(&pszWktM));
    EXPECT_TRUE(mpM.IsMeasured());

    const char* pszWktZM = "MULTIPOINT ZM (0 0 1 2, 1 1 3 4)";
    OGRMultiPoint mpZM;
    EXPECT_EQ(OGRERR_NONE, mpZM.importFromWkt(&pszWktZM));
    EXPECT_TRUE(mpZM.Is3D());
    EXPECT_TRUE(mpZM.IsMeasured());
  }

  // Implicit 3D
  {
    const char* pszWkt = "MULTIPOINT (0 0 1, 1 1 2)";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_NONE, mp.importFromWkt(&pszWkt));
    EXPECT_TRUE(mp.Is3D());
  }

  // Implicit 4D
  {
    const char* pszWkt4D = "MULTIPOINT (0 0 1 2, 1 1 3 4)";
    OGRMultiPoint mp4D;
    EXPECT_EQ(OGRERR_NONE, mp4D.importFromWkt(&pszWkt4D));
    EXPECT_TRUE(mp4D.Is3D());
    EXPECT_TRUE(mp4D.IsMeasured());
  }

  // Invalid preamble
  {
    WithQuietHandler quiet_handler;
    const char* pszPoint = "POINT (1 2)";
    OGRMultiPoint mp;
    EXPECT_NE(OGRERR_NONE, mp.importFromWkt(&pszPoint));
  }

  // addGeometryDirectly failure
  {
    const char* pszWkt = "MULTIPOINT (1 2)";
    FailingMultiPoint fmp;
    EXPECT_EQ(OGRERR_FAILURE, fmp.importFromWkt(&pszWkt));
  }

  // Corrupt non-bracketed WKT
  {
    WithQuietHandler quiet_handler;
    const char* pszBad = "MULTIPOINT (INVALID)";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, mp.importFromWkt(&pszBad));
  }
}

TEST(OgrMultiPointTest, ImportFromWktBracketed) {
  // 2D bracketed
  {
    const char* pszWkt = "MULTIPOINT ((0 0), (1 1))";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_NONE, mp.importFromWkt(&pszWkt));
    EXPECT_EQ(2, mp.getNumGeometries());
  }

  // Bracketed with EMPTY, Z, M, ZM
  {
    const char* pszWktZ = "MULTIPOINT Z ((0 0 1), EMPTY, (1 1 2))";
    OGRMultiPoint mpZ;
    EXPECT_EQ(OGRERR_NONE, mpZ.importFromWkt(&pszWktZ));
    EXPECT_EQ(3, mpZ.getNumGeometries());
    EXPECT_TRUE(mpZ.Is3D());

    const char* pszWktM = "MULTIPOINT M ((0 0 1))";
    OGRMultiPoint mpM;
    EXPECT_EQ(OGRERR_NONE, mpM.importFromWkt(&pszWktM));
    EXPECT_TRUE(mpM.IsMeasured());

    const char* pszWktZM = "MULTIPOINT ZM ((0 0 1 2))";
    OGRMultiPoint mpZM;
    EXPECT_EQ(OGRERR_NONE, mpZM.importFromWkt(&pszWktZM));
    EXPECT_TRUE(mpZM.Is3D());
    EXPECT_TRUE(mpZM.IsMeasured());
  }

  // Implicit 3D bracketed
  {
    const char* pszWkt = "MULTIPOINT ((0 0 1), (1 1 2))";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_NONE, mp.importFromWkt(&pszWkt));
    EXPECT_TRUE(mp.Is3D());
  }

  // Implicit 4D bracketed
  {
    const char* pszWkt4D = "MULTIPOINT ((0 0 1 2), (1 1 3 4))";
    OGRMultiPoint mp4D;
    EXPECT_EQ(OGRERR_NONE, mp4D.importFromWkt(&pszWkt4D));
    EXPECT_TRUE(mp4D.Is3D());
    EXPECT_TRUE(mp4D.IsMeasured());
  }

  // FailingMultiPoint with EMPTY
  {
    const char* pszWktEmpty = "MULTIPOINT ((0 0), EMPTY)";
    FailingMultiPoint fmp;
    EXPECT_EQ(OGRERR_FAILURE, fmp.importFromWkt(&pszWktEmpty));
  }

  // FailingMultiPoint with point
  {
    const char* pszWkt = "MULTIPOINT ((1 2))";
    FailingMultiPoint fmp;
    EXPECT_EQ(OGRERR_FAILURE, fmp.importFromWkt(&pszWkt));
  }

  // Corrupt bracketed (more than 1 point inside a single bracket)
  {
    WithQuietHandler quiet_handler;
    const char* pszBad = "MULTIPOINT ((0 0, 1 1))";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, mp.importFromWkt(&pszBad));
  }

  // Missing closing bracket
  {
    WithQuietHandler quiet_handler;
    const char* pszBad = "MULTIPOINT ((0 0)";
    OGRMultiPoint mp;
    EXPECT_EQ(OGRERR_CORRUPT_DATA, mp.importFromWkt(&pszBad));
  }
}

}  // namespace
}  // namespace autotest2
