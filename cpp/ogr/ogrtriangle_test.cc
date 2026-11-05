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
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestTriangle : public OGRTriangle {
 public:
  using OGRTriangle::GetCasterToPolygon;
  using OGRTriangle::importFromWKTListOnly;
};

TEST(OgrTriangleTest, ConstructorsAndProperties) {
  TestTriangle tri;
  EXPECT_STREQ("TRIANGLE", tri.getGeometryName());
  EXPECT_EQ(wkbTriangle, tri.getGeometryType());

  tri.set3D(TRUE);
  EXPECT_EQ(wkbTriangleZ, tri.getGeometryType());

  tri.setMeasured(TRUE);
  EXPECT_EQ(wkbTriangleZM, tri.getGeometryType());

  tri.set3D(FALSE);
  EXPECT_EQ(wkbTriangleM, tri.getGeometryType());

  // 3-point constructor
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);
  OGRTriangle tri_points(p1, p2, p3);
  EXPECT_EQ(1, tri_points.getNumInteriorRings() + 1);

  // Constructor from valid polygon
  OGRPolygon valid_poly;
  OGRLinearRing valid_ring;
  valid_ring.addPoint(0, 0);
  valid_ring.addPoint(1, 0);
  valid_ring.addPoint(0, 1);
  valid_ring.addPoint(0, 0);
  valid_poly.addRing(&valid_ring);

  OGRErr err = OGRERR_FAILURE;
  OGRTriangle tri_from_poly(valid_poly, err);
  EXPECT_EQ(OGRERR_NONE, err);

  // Constructor from invalid polygon (rectangle with 5 points)
  OGRPolygon invalid_poly;
  OGRLinearRing invalid_ring;
  invalid_ring.addPoint(0, 0);
  invalid_ring.addPoint(1, 0);
  invalid_ring.addPoint(1, 1);
  invalid_ring.addPoint(0, 1);
  invalid_ring.addPoint(0, 0);
  invalid_poly.addRing(&invalid_ring);

  err = OGRERR_FAILURE;
  WithQuietHandler quiet_handler;
  OGRTriangle tri_from_invalid(invalid_poly, err);
  EXPECT_EQ(OGRERR_FAILURE, err);
  EXPECT_TRUE(tri_from_invalid.IsEmpty());
}

TEST(OgrTriangleTest, CopyAndClone) {
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);
  OGRTriangle tri(p1, p2, p3);

  // Copy constructor
  OGRTriangle tri_copy(tri);
  EXPECT_FALSE(tri_copy.IsEmpty());

  // Assignment operator
  OGRTriangle tri_assigned;
  tri_assigned = tri;
  EXPECT_FALSE(tri_assigned.IsEmpty());

  // Self assignment
  const auto* ptr = &tri_assigned;
  tri_assigned = *ptr;
  EXPECT_FALSE(tri_assigned.IsEmpty());

  // Clone
  std::unique_ptr<OGRTriangle> tri_cloned(tri.clone());
  ASSERT_NE(nullptr, tri_cloned);
  EXPECT_FALSE(tri_cloned->IsEmpty());
}

TEST(OgrTriangleTest, AddRingDirectly) {
  OGRTriangle tri;
  auto* ring1 = new OGRLinearRing();
  ring1->addPoint(0, 0);
  ring1->addPoint(1, 0);
  ring1->addPoint(0, 1);
  ring1->addPoint(0, 0);

  EXPECT_EQ(OGRERR_NONE, tri.addRingDirectly(ring1));

  // Second ring should fail because a triangle can only have 1 ring
  auto* ring2 = new OGRLinearRing();
  EXPECT_EQ(OGRERR_FAILURE, tri.addRingDirectly(ring2));
  delete ring2;
}

TEST(OgrTriangleTest, ImportFromWkb) {
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);
  OGRTriangle tri(p1, p2, p3);

  std::vector<unsigned char> wkb(tri.WkbSize());
  EXPECT_EQ(OGRERR_NONE,
            tri.OGRGeometry::exportToWkb(wkbNDR, wkb.data(), wkbVariantIso));

  OGRTriangle imported_tri;
  size_t consumed = 0;
  EXPECT_EQ(OGRERR_NONE, imported_tri.importFromWkb(wkb.data(), wkb.size(),
                                                    wkbVariantIso, consumed));
  EXPECT_FALSE(imported_tri.IsEmpty());

  // Import invalid polygon WKB (e.g. square with 5 points)
  OGRPolygon poly;
  OGRLinearRing square;
  square.addPoint(0, 0);
  square.addPoint(1, 0);
  square.addPoint(1, 1);
  square.addPoint(0, 1);
  square.addPoint(0, 0);
  poly.addRing(&square);
  std::vector<unsigned char> square_wkb(poly.WkbSize());
  EXPECT_EQ(OGRERR_NONE, poly.OGRGeometry::exportToWkb(
                             wkbNDR, square_wkb.data(), wkbVariantIso));

  OGRTriangle invalid_tri;
  consumed = 0;
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            invalid_tri.importFromWkb(square_wkb.data(), square_wkb.size(),
                                      wkbVariantIso, consumed));
  EXPECT_TRUE(invalid_tri.IsEmpty());
}

TEST(OgrTriangleTest, ImportFromWktListOnly) {
  TestTriangle tri;
  const char* pszValid = "((0 0, 1 0, 0 1, 0 0))";
  OGRRawPoint* paoPoints = nullptr;
  int nMaxPoints = 0;
  double* padfZ = nullptr;

  EXPECT_EQ(OGRERR_NONE,
            tri.importFromWKTListOnly(&pszValid, FALSE, FALSE, paoPoints,
                                      nMaxPoints, padfZ));
  CPLFree(paoPoints);
  CPLFree(padfZ);

  // Invalid list with 5 points (not a triangle)
  TestTriangle tri2;
  const char* pszInvalid = "((0 0, 1 0, 1 1, 0 1, 0 0))";
  paoPoints = nullptr;
  nMaxPoints = 0;
  padfZ = nullptr;
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            tri2.importFromWKTListOnly(&pszInvalid, FALSE, FALSE, paoPoints,
                                       nMaxPoints, padfZ));
  CPLFree(paoPoints);
  CPLFree(padfZ);
}

TEST(OgrTriangleTest, CastToPolygon) {
  OGRPoint p1(0, 0);
  OGRPoint p2(1, 0);
  OGRPoint p3(0, 1);

  // Cast via OGRSurface::CastToPolygon
  auto* tri1 = new OGRTriangle(p1, p2, p3);
  OGRPolygon* poly1 = OGRSurface::CastToPolygon(tri1);
  ASSERT_NE(nullptr, poly1);
  EXPECT_STREQ("POLYGON", poly1->getGeometryName());
  delete poly1;

  // Cast via OGRTriangle::CastToPolygon
  auto* tri2 = new OGRTriangle(p1, p2, p3);
  OGRGeometry* poly2 = OGRTriangle::CastToPolygon(tri2);
  ASSERT_NE(nullptr, poly2);
  EXPECT_STREQ("POLYGON", poly2->getGeometryName());
  delete poly2;
}

}  // namespace
}  // namespace autotest2
