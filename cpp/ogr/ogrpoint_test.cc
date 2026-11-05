// Tests ogr/ogrpoint.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <string.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

// The getDimension() method is referring points (as in an x, y, z
// location) having zero dimensions.  A line is 1D, a polygon is
// 2D. It does not refer to how many coordinates are set in the point
// (x/y versus x/y/z).

TEST(OgrPointTest, DefaultConstructor) {
  // Tests starting from a default constructor.
  OGRPoint point;
  EXPECT_EQ(0, point.getDimension());
  EXPECT_TRUE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(0.0, point.getX());
  EXPECT_DOUBLE_EQ(0.0, point.getY());
  EXPECT_DOUBLE_EQ(0.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setX(123.0);
  EXPECT_FALSE(point.Equals(&point2));
  point2.empty();
  EXPECT_TRUE(point.Equals(&point2));
  EXPECT_EQ(0, point2.getDimension());

  point.setX(123.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, ConstructWithTwoCoords) {
  // Tests that starting from a constructor with X and Y is sane.
  OGRPoint point(-1.0, -2.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_FALSE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(-1.0, point.getX());
  EXPECT_DOUBLE_EQ(-2.0, point.getY());
  EXPECT_DOUBLE_EQ(0.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2(-1.0, -2.0);
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setX(123.0);
  EXPECT_FALSE(point.Equals(&point2));
  point2.empty();
  EXPECT_FALSE(point.Equals(&point2));
  EXPECT_EQ(0, point2.getDimension());

  point.setX(123.0);
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_DOUBLE_EQ(789.0, point.getZ());

  point.swapXY();
  EXPECT_DOUBLE_EQ(456.0, point.getX());
  EXPECT_DOUBLE_EQ(123.0, point.getY());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, ConstructWithThreeCoords) {
  // Tests that starting from a constructor with X, Y, and Z is sane.
  OGRPoint point(-3.0, -4.0, 5.0);
  EXPECT_EQ(0, point.getDimension());
  EXPECT_FALSE(point.IsEmpty());
  EXPECT_DOUBLE_EQ(-3.0, point.getX());
  EXPECT_DOUBLE_EQ(-4.0, point.getY());
  EXPECT_DOUBLE_EQ(5.0, point.getZ());

  EXPECT_STREQ("POINT", point.getGeometryName());
  EXPECT_EQ(wkbPoint25D, point.getGeometryType());

  // Do not compare with nullptr as that will crash.
  EXPECT_TRUE(point.Equals(&point));

  OGRPoint point2(-3.0, -4.0, 5.0);
  EXPECT_TRUE(point.Equals(&point2));  // Uses an int for boolean return.
  point2.setZ(6.0);
  EXPECT_FALSE(point.Equals(&point2));

  point.setX(123.0);
  EXPECT_DOUBLE_EQ(123.0, point.getX());
  point.setY(456.0);
  EXPECT_DOUBLE_EQ(456.0, point.getY());
  point.setZ(789.0);
  EXPECT_DOUBLE_EQ(789.0, point.getZ());

  point.swapXY();
  EXPECT_DOUBLE_EQ(456.0, point.getX());
  EXPECT_DOUBLE_EQ(123.0, point.getY());
  EXPECT_DOUBLE_EQ(789.0, point.getZ());
}

TEST(OgrPointTest, WktImportEmptyString) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> empty_str(strdup(""));
  char *empty_ptr = empty_str.get();
  EXPECT_EQ(OGRERR_CORRUPT_DATA, point.importFromWkt(&empty_ptr));
  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktImportEmpty) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> empty_str(strdup("POINT EMPTY"));
  char *empty_ptr = empty_str.get();
  EXPECT_EQ(OGRERR_NONE, point.importFromWkt(&empty_ptr));
  OGRPoint point2;
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktImportPoint) {
  OGRPoint point;
  std::unique_ptr<char, FreeDeleter> wkt(strdup("POINT (2 3)"));
  char *wkt_ptr = wkt.get();
  EXPECT_EQ(OGRERR_NONE, point.importFromWkt(&wkt_ptr));
  OGRPoint point2(2.0, 3.0);
  EXPECT_TRUE(point.Equals(&point2));
}

TEST(OgrPointTest, WktExport) {
  OGRPoint point;

  char *wkt = nullptr;
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT EMPTY", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setX(1);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 0)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  // Try using a float rather than an int.
  point.setY(2.0);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 2)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setZ(3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (1 2 3)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.empty();
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT EMPTY", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setX(-1);
  point.setY(-1.2);
  point.setZ(-1.3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (-1 -1.2 -1.3)", wkt);
  CPLFree(wkt);
  wkt = nullptr;

  point.setZ(1.3);
  EXPECT_EQ(OGRERR_NONE, point.exportToWkt(&wkt));
  EXPECT_STREQ("POINT (-1 -1.2 1.3)", wkt);
  CPLFree(wkt);
}

class TestPointTransform : public OGRCoordinateTransformation {
 public:
  explicit TestPointTransform(bool succeed) : succeed_(succeed) {}

  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  OGRCoordinateTransformation* Clone() const override { return nullptr; }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }

  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    if (!succeed_) return FALSE;
    for (size_t i = 0; i < nCount; ++i) {
      x[i] += 10.0;
      y[i] += 20.0;
      if (z) z[i] += 30.0;
      if (pabSuccess) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }

 private:
  bool succeed_;
};

TEST(OgrPointTest, ConstructWithFourCoordsAndXYM) {
  OGRPoint pt(1.0, 2.0, 3.0, 4.0);
  EXPECT_EQ(wkbPointZM, pt.getGeometryType());
  EXPECT_TRUE(pt.Is3D());
  EXPECT_TRUE(pt.IsMeasured());
  EXPECT_DOUBLE_EQ(1.0, pt.getX());
  EXPECT_DOUBLE_EQ(2.0, pt.getY());
  EXPECT_DOUBLE_EQ(3.0, pt.getZ());
  EXPECT_DOUBLE_EQ(4.0, pt.getM());

  std::unique_ptr<OGRPoint> xym(OGRPoint::createXYM(5.0, 6.0, 7.0));
  ASSERT_NE(nullptr, xym);
  EXPECT_EQ(wkbPointM, xym->getGeometryType());
  EXPECT_FALSE(xym->Is3D());
  EXPECT_TRUE(xym->IsMeasured());
  EXPECT_DOUBLE_EQ(5.0, xym->getX());
  EXPECT_DOUBLE_EQ(6.0, xym->getY());
  EXPECT_DOUBLE_EQ(7.0, xym->getM());

  // NaN coordinates produce empty point
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  OGRPoint nan_pt(nan_val, 0.0);
  EXPECT_TRUE(nan_pt.IsEmpty());
  OGRPoint nan_pt2(0.0, nan_val);
  EXPECT_TRUE(nan_pt2.IsEmpty());
  OGRPoint nan_pt3(nan_val, nan_val, 1.0);
  EXPECT_TRUE(nan_pt3.IsEmpty());
  OGRPoint nan_pt4(nan_val, nan_val, 1.0, 2.0);
  EXPECT_TRUE(nan_pt4.IsEmpty());
}

TEST(OgrPointTest, CopyCloneAndDimension) {
  OGRPoint pt(1.0, 2.0, 3.0, 4.0);
  OGRPoint copy_pt(pt);
  EXPECT_TRUE(pt.Equals(&copy_pt));

  OGRPoint assign_pt;
  assign_pt = pt;
  EXPECT_TRUE(pt.Equals(&assign_pt));
  auto& self = assign_pt;
  assign_pt = self;
  EXPECT_TRUE(pt.Equals(&assign_pt));

  std::unique_ptr<OGRPoint> cloned(pt.clone());
  ASSERT_NE(nullptr, cloned);
  EXPECT_TRUE(pt.Equals(cloned.get()));

  // flattenTo2D
  pt.flattenTo2D();
  EXPECT_EQ(wkbPoint, pt.getGeometryType());
  EXPECT_DOUBLE_EQ(0.0, pt.getZ());
  EXPECT_DOUBLE_EQ(0.0, pt.getM());

  // setCoordinateDimension
  pt.setCoordinateDimension(3);
  EXPECT_TRUE(pt.Is3D());
  pt.setCoordinateDimension(2);
  EXPECT_FALSE(pt.Is3D());
}

TEST(OgrPointTest, WkbImportExportAllVariants) {
  // 2D, 3D, M, ZM points
  std::vector<OGRPoint> points = {
      OGRPoint(1.0, 2.0),
      OGRPoint(1.0, 2.0, 3.0),
      *std::unique_ptr<OGRPoint>(OGRPoint::createXYM(1.0, 2.0, 4.0)),
      OGRPoint(1.0, 2.0, 3.0, 4.0),
  };

  std::vector<size_t> expected_sizes = {21, 29, 29, 37};

  for (size_t i = 0; i < points.size(); ++i) {
    const auto& pt = points[i];
    EXPECT_EQ(expected_sizes[i], pt.WkbSize());

    for (auto byte_order : {wkbNDR, wkbXDR}) {
      for (auto variant : {wkbVariantIso, wkbVariantPostGIS1}) {
        OGRwkbExportOptions opt;
        opt.eByteOrder = byte_order;
        opt.eWkbVariant = variant;
        std::vector<unsigned char> data(pt.WkbSize());
        EXPECT_EQ(OGRERR_NONE, pt.exportToWkb(data.data(), &opt));

        OGRPoint imported;
        size_t consumed = 0;
        EXPECT_EQ(OGRERR_NONE, imported.importFromWkb(data.data(), data.size(),
                                                      variant, consumed));
        EXPECT_EQ(data.size(), consumed);
        EXPECT_TRUE(pt.Equals(&imported));
      }
    }
  }

  // Empty point exported with Iso variant (writes NaN coords)
  for (auto byte_order : {wkbNDR, wkbXDR}) {
    OGRwkbExportOptions opt;
    opt.eByteOrder = byte_order;
    opt.eWkbVariant = wkbVariantIso;

    OGRPoint empty_2d;
    std::vector<unsigned char> data(empty_2d.WkbSize());
    EXPECT_EQ(OGRERR_NONE, empty_2d.exportToWkb(data.data(), &opt));
    OGRPoint imported;
    size_t consumed = 0;
    EXPECT_EQ(OGRERR_NONE, imported.importFromWkb(data.data(), data.size(),
                                                  wkbVariantIso, consumed));
    EXPECT_TRUE(imported.IsEmpty());

    OGRPoint empty_3d;
    empty_3d.setCoordinateDimension(3);
    data.resize(empty_3d.WkbSize());
    EXPECT_EQ(OGRERR_NONE, empty_3d.exportToWkb(data.data(), &opt));
    EXPECT_EQ(OGRERR_NONE, imported.importFromWkb(data.data(), data.size(),
                                                  wkbVariantIso, consumed));
    EXPECT_TRUE(imported.IsEmpty());

    OGRPoint empty_zm;
    empty_zm.setCoordinateDimension(3);
    empty_zm.setMeasured(TRUE);
    data.resize(empty_zm.WkbSize());
    EXPECT_EQ(OGRERR_NONE, empty_zm.exportToWkb(data.data(), &opt));
    EXPECT_EQ(OGRERR_NONE, imported.importFromWkb(data.data(), data.size(),
                                                  wkbVariantIso, consumed));
    EXPECT_TRUE(imported.IsEmpty());
  }

  // Truncated buffer errors
  OGRPoint pt_zm(1, 2, 3, 4);
  std::vector<unsigned char> full_data(pt_zm.WkbSize());
  EXPECT_EQ(OGRERR_NONE, pt_zm.exportToWkb(full_data.data()));
  OGRPoint imp;
  size_t consumed = 0;
  EXPECT_EQ(
      OGRERR_NOT_ENOUGH_DATA,
      imp.importFromWkb(full_data.data(), 10, wkbVariantOldOgc, consumed));
  EXPECT_EQ(
      OGRERR_NOT_ENOUGH_DATA,
      imp.importFromWkb(full_data.data(), 25, wkbVariantOldOgc, consumed));
  EXPECT_EQ(
      OGRERR_NOT_ENOUGH_DATA,
      imp.importFromWkb(full_data.data(), 35, wkbVariantOldOgc, consumed));
}

TEST(OgrPointTest, WktImportAndExportVariants) {
  OGRPoint pt;

  // 3D Point WKT
  const char* wkt_3d = "POINT Z (10 20 30)";
  EXPECT_EQ(OGRERR_NONE, pt.importFromWkt(&wkt_3d));
  EXPECT_TRUE(pt.Is3D());
  EXPECT_DOUBLE_EQ(30.0, pt.getZ());

  // M Point WKT
  const char* wkt_m = "POINT M (10 20 40)";
  EXPECT_EQ(OGRERR_NONE, pt.importFromWkt(&wkt_m));
  EXPECT_TRUE(pt.IsMeasured());
  EXPECT_DOUBLE_EQ(40.0, pt.getM());

  // ZM Point WKT
  const char* wkt_zm = "POINT ZM (10 20 30 40)";
  EXPECT_EQ(OGRERR_NONE, pt.importFromWkt(&wkt_zm));
  EXPECT_TRUE(pt.Is3D());
  EXPECT_TRUE(pt.IsMeasured());
  EXPECT_DOUBLE_EQ(30.0, pt.getZ());
  EXPECT_DOUBLE_EQ(40.0, pt.getM());

  // Implicit 4D coordinate
  const char* wkt_implicit_4d = "POINT (10 20 30 40)";
  EXPECT_EQ(OGRERR_NONE, pt.importFromWkt(&wkt_implicit_4d));
  EXPECT_TRUE(pt.Is3D());
  EXPECT_TRUE(pt.IsMeasured());
  EXPECT_DOUBLE_EQ(30.0, pt.getZ());
  EXPECT_DOUBLE_EQ(40.0, pt.getM());

  // Multiple points passed to single point import -> corrupt data
  const char* wkt_multi = "POINT (1 2, 3 4)";
  EXPECT_EQ(OGRERR_CORRUPT_DATA, pt.importFromWkt(&wkt_multi));

  // Reset to 3D and measured point before export
  pt.set3D(TRUE);
  pt.setMeasured(TRUE);
  pt.setX(10.0);
  pt.setY(20.0);
  pt.setZ(30.0);
  pt.setM(40.0);

  // Export with ISO variant
  OGRWktOptions opt_iso;
  opt_iso.variant = wkbVariantIso;
  OGRErr err = OGRERR_NONE;
  std::string export_wkt = pt.exportToWkt(opt_iso, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_THAT(export_wkt, testing::HasSubstr("POINT ZM"));
}

TEST(OgrPointTest, EnvelopeAndEqualsAndTransform) {
  OGRPoint pt(1.0, 2.0, 3.0);

  OGREnvelope env;
  pt.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(1.0, env.MinX);
  EXPECT_DOUBLE_EQ(1.0, env.MaxX);
  EXPECT_DOUBLE_EQ(2.0, env.MinY);
  EXPECT_DOUBLE_EQ(2.0, env.MaxY);

  OGREnvelope3D env3d;
  pt.getEnvelope(&env3d);
  EXPECT_DOUBLE_EQ(1.0, env3d.MinX);
  EXPECT_DOUBLE_EQ(1.0, env3d.MaxX);
  EXPECT_DOUBLE_EQ(2.0, env3d.MinY);
  EXPECT_DOUBLE_EQ(2.0, env3d.MaxY);
  EXPECT_DOUBLE_EQ(3.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(3.0, env3d.MaxZ);

  // Equals comparisons
  OGRPoint pt2(1.0, 2.0, 4.0);
  EXPECT_FALSE(pt.Equals(&pt2));  // z differs
  OGRPoint pt_2d(1.0, 2.0);
  EXPECT_FALSE(pt.Equals(&pt_2d));  // flags differ
  OGRLineString ls;
  EXPECT_FALSE(pt.Equals(&ls));  // type differs

  // Transform success and failure
  TestPointTransform success_ct(true);
  EXPECT_EQ(OGRERR_NONE, pt.transform(&success_ct));
  EXPECT_DOUBLE_EQ(11.0, pt.getX());
  EXPECT_DOUBLE_EQ(22.0, pt.getY());
  EXPECT_DOUBLE_EQ(33.0, pt.getZ());

  TestPointTransform fail_ct(false);
  EXPECT_EQ(OGRERR_FAILURE, pt.transform(&fail_ct));
}

TEST(OgrPointTest, WithinAndIntersects) {
  WithQuietHandler quiet_handler;
  OGRPoint pt(5.0, 5.0);

  // With CurvePolygon
  OGRCurvePolygon curve_poly;
  OGRLineString ring;
  ring.addPoint(0, 0);
  ring.addPoint(0, 10);
  ring.addPoint(10, 10);
  ring.addPoint(10, 0);
  ring.addPoint(0, 0);
  ASSERT_EQ(OGRERR_NONE, curve_poly.addRing(&ring));

  EXPECT_TRUE(pt.Within(&curve_poly));
  EXPECT_TRUE(pt.Intersects(&curve_poly));

  OGRPoint outside_pt(50.0, 50.0);
  EXPECT_FALSE(outside_pt.Within(&curve_poly));
  EXPECT_FALSE(outside_pt.Intersects(&curve_poly));

  // With standard OGRPolygon (covers OGRGeometry::Within / Intersects fallback)
  OGRLinearRing poly_ring;
  poly_ring.addPoint(0, 0);
  poly_ring.addPoint(0, 10);
  poly_ring.addPoint(10, 10);
  poly_ring.addPoint(10, 0);
  poly_ring.addPoint(0, 0);
  OGRPolygon poly;
  ASSERT_EQ(OGRERR_NONE, poly.addRing(&poly_ring));

  EXPECT_TRUE(pt.Within(&poly));
  EXPECT_TRUE(pt.Intersects(&poly));
  EXPECT_FALSE(outside_pt.Within(&poly));
  EXPECT_FALSE(outside_pt.Intersects(&poly));
}

}  // namespace
}  // namespace autotest2
