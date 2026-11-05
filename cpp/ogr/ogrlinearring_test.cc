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
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

constexpr auto OGR_G_3D = OGRGeometry::OGR_G_3D;
constexpr auto OGR_G_MEASURED = OGRGeometry::OGR_G_MEASURED;

class TestLinearRing : public OGRLinearRing {
 public:
  TestLinearRing() = default;
  explicit TestLinearRing(const OGRLinearRing* poSrc) : OGRLinearRing(poSrc) {}

  using OGRLinearRing::_exportToWkb;
  using OGRLinearRing::_importFromWkb;
  using OGRLinearRing::_WkbSize;
  using OGRLinearRing::CastToLineString;
  using OGRLinearRing::GetCasterToLinearRing;
  using OGRLinearRing::GetCasterToLineString;
};

class UnclosingTransform : public OGRCoordinateTransformation {
 public:
  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  OGRCoordinateTransformation* Clone() const override { return nullptr; }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }

  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    if (nCount > 1) {
      x[nCount - 1] += 5.0;  // Perturb the last coordinate to unclose
    }
    if (pabSuccess) {
      for (size_t i = 0; i < nCount; ++i) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }
};

TEST(OgrLinearRingTest, ConstructorsAndAssignment) {
  WithQuietHandler quiet_handler;

  // Pointer constructor with nullptr
  TestLinearRing null_ring(nullptr);
  EXPECT_EQ(0, null_ring.getNumPoints());

  // 2D ring
  OGRLinearRing ring2d;
  ring2d.addPoint(0, 0);
  ring2d.addPoint(1, 1);
  ring2d.addPoint(0, 0);
  TestLinearRing copy_ptr2d(&ring2d);
  EXPECT_EQ(3, copy_ptr2d.getNumPoints());
  EXPECT_FALSE(copy_ptr2d.Is3D());

  // 3D ring
  OGRLinearRing ring3d;
  ring3d.addPoint(0, 0, 10);
  ring3d.addPoint(1, 1, 20);
  ring3d.addPoint(0, 0, 10);
  TestLinearRing copy_ptr3d(&ring3d);
  EXPECT_EQ(3, copy_ptr3d.getNumPoints());
  EXPECT_TRUE(copy_ptr3d.Is3D());
  EXPECT_DOUBLE_EQ(10, copy_ptr3d.getZ(0));

  // Copy constructor & assignment operator
  OGRLinearRing copy_ctor(ring3d);
  EXPECT_EQ(3, copy_ctor.getNumPoints());

  OGRLinearRing assigned;
  assigned = ring3d;
  EXPECT_EQ(3, assigned.getNumPoints());
  // Self assignment
  auto& self = assigned;
  assigned = self;
  EXPECT_EQ(3, assigned.getNumPoints());

  // Clone
  std::unique_ptr<OGRLinearRing> cloned(ring3d.clone());
  ASSERT_NE(nullptr, cloned);
  EXPECT_EQ(3, cloned->getNumPoints());
  EXPECT_TRUE(cloned->Is3D());
}

TEST(OgrLinearRingTest, GeometryNameAndDirectWkbMethods) {
  OGRLinearRing ring;
  EXPECT_STREQ("LINEARRING", ring.getGeometryName());

  // Direct WkbSize is disabled
  const OGRGeometry& geom = ring;
  EXPECT_EQ(0, geom.WkbSize());

  // Direct import/export is disabled
  unsigned char buf[64];
  size_t bytes_consumed = 0;
  OGRGeometry& mut_geom = ring;
  EXPECT_EQ(
      OGRERR_UNSUPPORTED_OPERATION,
      mut_geom.importFromWkb(buf, sizeof(buf), wkbVariantIso, bytes_consumed));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, geom.exportToWkb(buf, nullptr));
}

TEST(OgrLinearRingTest, InternalWkbSizeAndExportImport) {
  TestLinearRing ring;
  ring.addPoint(0, 1);
  ring.addPoint(2, 3);
  ring.addPoint(0, 1);

  // 2D size
  EXPECT_EQ(4 + 16 * 3, ring._WkbSize(0));
  // 3D size
  EXPECT_EQ(4 + 24 * 3, ring._WkbSize(OGR_G_3D));
  // Measured size
  EXPECT_EQ(4 + 24 * 3, ring._WkbSize(OGR_G_MEASURED));
  // 3D + Measured size
  EXPECT_EQ(4 + 32 * 3, ring._WkbSize(OGR_G_3D | OGR_G_MEASURED));

  // Export and import 2D NDR
  OGRwkbExportOptions opt_ndr;
  opt_ndr.eByteOrder = wkbNDR;
  std::vector<unsigned char> data(ring._WkbSize(0));
  EXPECT_EQ(OGRERR_NONE, ring._exportToWkb(0, data.data(), &opt_ndr));

  TestLinearRing imported;
  size_t consumed = 0;
  EXPECT_EQ(OGRERR_NONE, imported._importFromWkb(wkbNDR, 0, data.data(),
                                                 data.size(), consumed));
  EXPECT_EQ(data.size(), consumed);
  EXPECT_EQ(3, imported.getNumPoints());
  EXPECT_DOUBLE_EQ(2, imported.getX(1));
  EXPECT_DOUBLE_EQ(3, imported.getY(1));

  // Export and import 2D XDR (byte swapped)
  OGRwkbExportOptions opt_xdr;
  opt_xdr.eByteOrder = wkbXDR;
  EXPECT_EQ(OGRERR_NONE, ring._exportToWkb(0, data.data(), &opt_xdr));
  EXPECT_EQ(OGRERR_NONE, imported._importFromWkb(wkbXDR, 0, data.data(),
                                                 data.size(), consumed));
  EXPECT_DOUBLE_EQ(2, imported.getX(1));

  // Export and import 3D with XDR
  ring.setZ(0, 10.0);
  ring.setZ(1, 20.0);
  ring.setZ(2, 10.0);
  data.resize(ring._WkbSize(OGR_G_3D));
  EXPECT_EQ(OGRERR_NONE, ring._exportToWkb(OGR_G_3D, data.data(), &opt_xdr));
  EXPECT_EQ(OGRERR_NONE, imported._importFromWkb(wkbXDR, OGR_G_3D, data.data(),
                                                 data.size(), consumed));
  EXPECT_TRUE(imported.Is3D());
  EXPECT_DOUBLE_EQ(20.0, imported.getZ(1));

  // Export and import Measured with XDR
  TestLinearRing ring_m;
  ring_m.addPointM(0, 1, 100);
  ring_m.addPointM(2, 3, 200);
  ring_m.addPointM(0, 1, 100);
  data.resize(ring_m._WkbSize(OGR_G_MEASURED));
  EXPECT_EQ(OGRERR_NONE,
            ring_m._exportToWkb(OGR_G_MEASURED, data.data(), &opt_xdr));
  EXPECT_EQ(OGRERR_NONE,
            imported._importFromWkb(wkbXDR, OGR_G_MEASURED, data.data(),
                                    data.size(), consumed));
  EXPECT_TRUE(imported.IsMeasured());
  EXPECT_DOUBLE_EQ(200.0, imported.getM(1));

  // Export and import 3D + Measured with XDR
  TestLinearRing ring_zm;
  ring_zm.addPoint(0, 1, 10, 100);
  ring_zm.addPoint(2, 3, 20, 200);
  ring_zm.addPoint(0, 1, 10, 100);
  data.resize(ring_zm._WkbSize(OGR_G_3D | OGR_G_MEASURED));
  EXPECT_EQ(OGRERR_NONE, ring_zm._exportToWkb(OGR_G_3D | OGR_G_MEASURED,
                                              data.data(), &opt_xdr));
  EXPECT_EQ(OGRERR_NONE,
            imported._importFromWkb(wkbXDR, OGR_G_3D | OGR_G_MEASURED,
                                    data.data(), data.size(), consumed));
  EXPECT_TRUE(imported.Is3D());
  EXPECT_TRUE(imported.IsMeasured());
  EXPECT_DOUBLE_EQ(20.0, imported.getZ(1));
  EXPECT_DOUBLE_EQ(200.0, imported.getM(1));

  // Export when padfZ or padfM is null but flags request 3D/M
  TestLinearRing ring_2d_only;
  ring_2d_only.addPoint(0, 0);
  data.resize(ring_2d_only._WkbSize(OGR_G_3D | OGR_G_MEASURED));
  EXPECT_EQ(OGRERR_NONE, ring_2d_only._exportToWkb(OGR_G_3D | OGR_G_MEASURED,
                                                   data.data(), &opt_ndr));
  EXPECT_EQ(OGRERR_NONE,
            ring_2d_only._exportToWkb(OGR_G_3D, data.data(), &opt_ndr));
  EXPECT_EQ(OGRERR_NONE,
            ring_2d_only._exportToWkb(OGR_G_MEASURED, data.data(), &opt_ndr));

  // Empty ring export / import
  TestLinearRing empty_ring;
  data.resize(empty_ring._WkbSize(0));
  EXPECT_EQ(OGRERR_NONE, empty_ring._exportToWkb(0, data.data(), &opt_ndr));
  EXPECT_EQ(OGRERR_NONE, imported._importFromWkb(wkbNDR, 0, data.data(),
                                                 data.size(), consumed));
  EXPECT_EQ(0, imported.getNumPoints());
}

TEST(OgrLinearRingTest, ImportFromWkbErrorPaths) {
  WithQuietHandler quiet_handler;
  TestLinearRing ring;
  size_t consumed = 0;

  // Less than 4 bytes available
  unsigned char tiny[3] = {0, 0, 0};
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            ring._importFromWkb(wkbNDR, 0, tiny, sizeof(tiny), consumed));

  // Negative point count
  int negative_count = -1;
  unsigned char neg_buf[4];
  memcpy(neg_buf, &negative_count, 4);
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            ring._importFromWkb(wkbNDR, 0, neg_buf, sizeof(neg_buf), consumed));

  // Point count exceeds buffer
  int large_count = 1000;
  unsigned char short_buf[16];
  memcpy(short_buf, &large_count, 4);
  EXPECT_EQ(
      OGRERR_NOT_ENOUGH_DATA,
      ring._importFromWkb(wkbNDR, 0, short_buf, sizeof(short_buf), consumed));
}

TEST(OgrLinearRingTest, ReverseWindingOrderAndCloseRings) {
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(1, 2);
  ring.addPoint(3, 4);

  ring.reverseWindingOrder();
  EXPECT_DOUBLE_EQ(3, ring.getX(0));
  EXPECT_DOUBLE_EQ(4, ring.getY(0));
  EXPECT_DOUBLE_EQ(0, ring.getX(2));
  EXPECT_DOUBLE_EQ(0, ring.getY(2));

  // closeRings with < 2 points
  OGRLinearRing single_pt;
  single_pt.addPoint(1, 1);
  single_pt.closeRings();
  EXPECT_EQ(1, single_pt.getNumPoints());

  // closeRings when already closed
  ring.addPoint(3, 4);
  EXPECT_EQ(4, ring.getNumPoints());
  ring.closeRings();
  EXPECT_EQ(4, ring.getNumPoints());

  // closeRings when open in X, Y, or Z
  OGRLinearRing open_ring;
  open_ring.addPoint(0, 0, 5);
  open_ring.addPoint(1, 1, 5);
  open_ring.closeRings();
  EXPECT_EQ(3, open_ring.getNumPoints());
  EXPECT_DOUBLE_EQ(0, open_ring.getX(2));
  EXPECT_DOUBLE_EQ(0, open_ring.getY(2));
  EXPECT_DOUBLE_EQ(5, open_ring.getZ(2));
}

TEST(OgrLinearRingTest, IsPointInRing) {
  WithQuietHandler quiet_handler;
  OGRLinearRing ring;
  // Square from (0, 0) to (10, 10)
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);

  // Null point
  EXPECT_FALSE(ring.isPointInRing(nullptr, TRUE));

  // Empty point
  OGRPoint empty_pt;
  EXPECT_FALSE(ring.isPointInRing(&empty_pt, TRUE));

  // Ring with < 4 points
  OGRLinearRing small_ring;
  small_ring.addPoint(0, 0);
  small_ring.addPoint(1, 1);
  OGRPoint pt(5, 5);
  EXPECT_FALSE(small_ring.isPointInRing(&pt, TRUE));

  // Point strictly inside
  EXPECT_TRUE(ring.isPointInRing(&pt, TRUE));
  EXPECT_TRUE(ring.isPointInRing(&pt, FALSE));

  // Point outside envelope
  OGRPoint pt_far(20, 20);
  EXPECT_FALSE(ring.isPointInRing(&pt_far, TRUE));
  EXPECT_FALSE(ring.isPointInRing(&pt_far, FALSE));

  // Point outside ring but inside envelope
  // Ring: triangle (0, 0), (10, 0), (0, 10), (0, 0)
  OGRLinearRing tri;
  tri.addPoint(0, 0);
  tri.addPoint(10, 0);
  tri.addPoint(0, 10);
  tri.addPoint(0, 0);
  OGRPoint pt_in_env_out_tri(8, 8);
  EXPECT_FALSE(tri.isPointInRing(&pt_in_env_out_tri, TRUE));
  EXPECT_FALSE(tri.isPointInRing(&pt_in_env_out_tri, FALSE));
}

TEST(OgrLinearRingTest, IsPointOnRingBoundary) {
  WithQuietHandler quiet_handler;
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 10);  // duplicate consecutive vertex
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);

  // Null point
  EXPECT_FALSE(ring.isPointOnRingBoundary(nullptr, TRUE));

  // Ring with < 4 points
  OGRLinearRing small_ring;
  small_ring.addPoint(0, 0);
  OGRPoint pt(0, 5);
  EXPECT_FALSE(small_ring.isPointOnRingBoundary(&pt, TRUE));

  // Point on boundary segment (0, 5)
  EXPECT_TRUE(ring.isPointOnRingBoundary(&pt, TRUE));
  EXPECT_TRUE(ring.isPointOnRingBoundary(&pt, FALSE));

  // Point on vertex (10, 10)
  OGRPoint pt_vertex(10, 10);
  EXPECT_TRUE(ring.isPointOnRingBoundary(&pt_vertex, TRUE));

  // Point strictly inside (5, 5) -> not on boundary
  OGRPoint pt_inside(5, 5);
  EXPECT_FALSE(ring.isPointOnRingBoundary(&pt_inside, TRUE));

  // Point outside envelope
  OGRPoint pt_outside(50, 50);
  EXPECT_FALSE(ring.isPointOnRingBoundary(&pt_outside, TRUE));
  EXPECT_FALSE(ring.isPointOnRingBoundary(&pt_outside, FALSE));
}

TEST(OgrLinearRingTest, TransformAndSafetyBelt) {
  OGRLinearRing ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  ASSERT_TRUE(ring.get_IsClosed());

  // Use UnclosingTransform: it will perturb the last point, but transform()
  // detects !get_IsClosed() and forces the last point back to the first point!
  UnclosingTransform unclosing_ct;
  EXPECT_EQ(OGRERR_NONE, ring.transform(&unclosing_ct));
  EXPECT_TRUE(ring.get_IsClosed());
  EXPECT_DOUBLE_EQ(ring.getX(0), ring.getX(ring.getNumPoints() - 1));
  EXPECT_DOUBLE_EQ(ring.getY(0), ring.getY(ring.getNumPoints() - 1));
}

TEST(OgrLinearRingTest, CastMethods) {
  OGRLinearRing* ring = new OGRLinearRing();
  ring->addPoint(0, 0);
  ring->addPoint(1, 1);
  ring->addPoint(0, 0);

  // CastToLineString consumes ring
  OGRLineString* ls = TestLinearRing::CastToLineString(ring);
  ASSERT_NE(nullptr, ls);
  EXPECT_STREQ("LINESTRING", ls->getGeometryName());
  EXPECT_EQ(3, ls->getNumPoints());

  // GetCasterToLineString
  TestLinearRing test_ring;
  OGRCurveCasterToLineString caster_to_ls = test_ring.GetCasterToLineString();
  ASSERT_NE(nullptr, caster_to_ls);
  OGRLinearRing* ring2 = new OGRLinearRing();
  ring2->addPoint(2, 2);
  ring2->addPoint(3, 3);
  OGRLineString* ls2 = caster_to_ls(ring2);
  ASSERT_NE(nullptr, ls2);
  EXPECT_STREQ("LINESTRING", ls2->getGeometryName());
  delete ls2;

  // GetCasterToLinearRing
  OGRCurveCasterToLinearRing caster_to_lr = test_ring.GetCasterToLinearRing();
  ASSERT_NE(nullptr, caster_to_lr);
  OGRLinearRing* ring3 = new OGRLinearRing();
  OGRLinearRing* lr3 = caster_to_lr(ring3);
  EXPECT_EQ(ring3, lr3);
  delete lr3;

  delete ls;
}

}  // namespace
}  // namespace autotest2
