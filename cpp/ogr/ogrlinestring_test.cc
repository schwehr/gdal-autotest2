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
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_proj_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestLineString : public OGRLineString {
 public:
  TestLineString() = default;
  using OGRLineString::CastToLinearRing;
  using OGRLineString::get_AreaOfCurveSegments;
  using OGRLineString::GetCasterToLinearRing;
  using OGRLineString::GetCasterToLineString;
  using OGRLineString::TransferMembersAndDestroy;
  using OGRSimpleCurve::AddM;
  using OGRSimpleCurve::get_LinearArea;
  using OGRSimpleCurve::importFromWKTListOnly;
  using OGRSimpleCurve::Make2D;
  using OGRSimpleCurve::Make3D;
  using OGRSimpleCurve::RemoveM;
};

class FakeCoordTransform : public OGRCoordinateTransformation {
 public:
  explicit FakeCoordTransform(bool bAllSuccess, bool bOffset = true,
                              bool bAllFail = false)
      : bAllSuccess_(bAllSuccess), bOffset_(bOffset), bAllFail_(bAllFail) {}

  OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  OGRCoordinateTransformation* Clone() const override { return nullptr; }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }

  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    for (size_t i = 0; i < nCount; ++i) {
      if (bOffset_) {
        x[i] += 10.0;
        y[i] += 20.0;
        if (z) z[i] += 5.0;
        if (m) m[i] += 1.0;
      }
      if (pabSuccess) {
        if (bAllFail_) {
          pabSuccess[i] = FALSE;
        } else {
          pabSuccess[i] = bAllSuccess_ ? TRUE : (i % 2 == 0 ? TRUE : FALSE);
        }
      }
    }
    return TRUE;
  }

 private:
  bool bAllSuccess_;
  bool bOffset_;
  bool bAllFail_;
};

// ============================================================================
// Basic Constructors and Dimensionality
// ============================================================================

TEST(OGRLineStringTest, BasicConstructorsAndEmpty) {
  OGRLineString line;
  EXPECT_TRUE(line.IsEmpty());
  EXPECT_EQ(line.getNumPoints(), 0);
  EXPECT_EQ(line.getGeometryType(), wkbLineString);
  EXPECT_STREQ(line.getGeometryName(), "LINESTRING");
  EXPECT_EQ(line.WkbSize(), 9);
  EXPECT_EQ(line.get_Area(), 0.0);

  line.addPoint(1.0, 2.0);
  line.addPoint(3.0, 4.0);
  EXPECT_FALSE(line.IsEmpty());
  EXPECT_EQ(line.getNumPoints(), 2);

  // Copy constructor
  OGRLineString lineCopy(line);
  EXPECT_EQ(lineCopy.getNumPoints(), 2);
  EXPECT_TRUE(line.Equals(&lineCopy));

  // Move constructor
  OGRLineString lineMoved(std::move(lineCopy));
  EXPECT_EQ(lineMoved.getNumPoints(), 2);
  EXPECT_TRUE(line.Equals(&lineMoved));

  // Copy assignment
  OGRLineString lineAssign;
  lineAssign = line;
  EXPECT_TRUE(line.Equals(&lineAssign));

  // Move assignment
  OGRLineString lineMoveAssign;
  lineMoveAssign = std::move(lineAssign);
  EXPECT_TRUE(line.Equals(&lineMoveAssign));

  // Clone
  std::unique_ptr<OGRLineString> poCloned(line.clone());
  ASSERT_NE(poCloned, nullptr);
  EXPECT_TRUE(line.Equals(poCloned.get()));

  // Empty
  line.empty();
  EXPECT_TRUE(line.IsEmpty());
  EXPECT_EQ(line.getNumPoints(), 0);
}

TEST(OGRLineStringTest, DimensionalityAndFlags) {
  TestLineString line;
  line.addPoint(1.0, 2.0);
  line.addPoint(3.0, 4.0);

  EXPECT_FALSE(line.Is3D());
  EXPECT_FALSE(line.IsMeasured());
  EXPECT_EQ(line.getCoordinateDimension(), 2);

  // 3D
  EXPECT_TRUE(line.set3D(TRUE));
  EXPECT_TRUE(line.Is3D());
  EXPECT_EQ(line.getCoordinateDimension(), 3);
  EXPECT_EQ(line.getGeometryType(), wkbLineString25D);
  EXPECT_EQ(line.WkbSize(), 9 + 2 * 24);

  EXPECT_TRUE(line.set3D(FALSE));
  EXPECT_FALSE(line.Is3D());
  EXPECT_EQ(line.getCoordinateDimension(), 2);

  EXPECT_TRUE(line.Make3D());
  EXPECT_TRUE(line.Is3D());
  line.Make2D();
  EXPECT_FALSE(line.Is3D());

  // Measured
  EXPECT_TRUE(line.setMeasured(TRUE));
  EXPECT_TRUE(line.IsMeasured());
  EXPECT_EQ(line.getGeometryType(), wkbLineStringM);
  EXPECT_EQ(line.WkbSize(), 9 + 2 * 24);

  EXPECT_TRUE(line.setMeasured(FALSE));
  EXPECT_FALSE(line.IsMeasured());

  EXPECT_TRUE(line.AddM());
  EXPECT_TRUE(line.IsMeasured());
  line.RemoveM();
  EXPECT_FALSE(line.IsMeasured());

  // ZM
  EXPECT_TRUE(line.Make3D());
  EXPECT_TRUE(line.AddM());
  EXPECT_TRUE(line.Is3D());
  EXPECT_TRUE(line.IsMeasured());
  EXPECT_EQ(line.getGeometryType(), wkbLineStringZM);
  EXPECT_EQ(line.WkbSize(), 9 + 2 * 32);

  // FlattenTo2D
  line.flattenTo2D();
  EXPECT_FALSE(line.Is3D());
  EXPECT_FALSE(line.IsMeasured());
  EXPECT_EQ(line.getGeometryType(), wkbLineString);

  // setCoordinateDimension
  EXPECT_TRUE(line.setCoordinateDimension(3));
  EXPECT_TRUE(line.Is3D());
  EXPECT_TRUE(line.setCoordinateDimension(2));
  EXPECT_FALSE(line.Is3D());
}

// ============================================================================
// Point Accessors and Mutators
// ============================================================================

TEST(OGRLineStringTest, PointAccessorsAndMutators) {
  OGRLineString line;

  // addPoint variants
  EXPECT_TRUE(line.addPoint(1.0, 2.0));
  EXPECT_TRUE(line.addPoint(3.0, 4.0, 5.0));
  EXPECT_TRUE(line.Is3D());

  EXPECT_TRUE(line.addPoint(5.0, 6.0, 7.0, 8.0));
  EXPECT_TRUE(line.IsMeasured());

  EXPECT_TRUE(line.addPointM(9.0, 10.0, 11.0));

  OGRPoint pt2D(12.0, 13.0);
  EXPECT_TRUE(line.addPoint(&pt2D));

  OGRPoint pt3D(14.0, 15.0, 16.0);
  EXPECT_TRUE(line.addPoint(&pt3D));

  OGRPoint ptZM(17.0, 18.0, 19.0, 20.0);
  EXPECT_TRUE(line.addPoint(&ptZM));

  EXPECT_EQ(line.getNumPoints(), 7);

  // getX, getY, getZ, getM
  EXPECT_DOUBLE_EQ(line.getX(0), 1.0);
  EXPECT_DOUBLE_EQ(line.getY(0), 2.0);
  EXPECT_DOUBLE_EQ(line.getZ(0), 0.0);
  EXPECT_DOUBLE_EQ(line.getM(0), 0.0);

  EXPECT_DOUBLE_EQ(line.getX(1), 3.0);
  EXPECT_DOUBLE_EQ(line.getY(1), 4.0);
  EXPECT_DOUBLE_EQ(line.getZ(1), 5.0);

  EXPECT_DOUBLE_EQ(line.getX(2), 5.0);
  EXPECT_DOUBLE_EQ(line.getY(2), 6.0);
  EXPECT_DOUBLE_EQ(line.getZ(2), 7.0);
  EXPECT_DOUBLE_EQ(line.getM(2), 8.0);

  // getPoint out of range or not 3D
  EXPECT_DOUBLE_EQ(line.getZ(-1), 0.0);
  EXPECT_DOUBLE_EQ(line.getZ(100), 0.0);
  EXPECT_DOUBLE_EQ(line.getM(-1), 0.0);
  EXPECT_DOUBLE_EQ(line.getM(100), 0.0);

  OGRPoint ptOut;
  line.getPoint(2, &ptOut);
  EXPECT_DOUBLE_EQ(ptOut.getX(), 5.0);
  EXPECT_DOUBLE_EQ(ptOut.getY(), 6.0);
  EXPECT_DOUBLE_EQ(ptOut.getZ(), 7.0);
  EXPECT_DOUBLE_EQ(ptOut.getM(), 8.0);

  // setPoint variants
  EXPECT_TRUE(line.setPoint(0, 100.0, 200.0));
  EXPECT_DOUBLE_EQ(line.getX(0), 100.0);
  EXPECT_DOUBLE_EQ(line.getY(0), 200.0);

  EXPECT_TRUE(line.setPoint(0, 101.0, 201.0, 301.0));
  EXPECT_DOUBLE_EQ(line.getZ(0), 301.0);

  EXPECT_TRUE(line.setPointM(0, 102.0, 202.0, 402.0));
  EXPECT_DOUBLE_EQ(line.getM(0), 402.0);

  EXPECT_TRUE(line.setPoint(0, 103.0, 203.0, 303.0, 403.0));
  EXPECT_DOUBLE_EQ(line.getZ(0), 303.0);
  EXPECT_DOUBLE_EQ(line.getM(0), 403.0);

  OGRPoint ptSet(50.0, 60.0, 70.0, 80.0);
  EXPECT_TRUE(line.setPoint(1, &ptSet));
  EXPECT_DOUBLE_EQ(line.getX(1), 50.0);
  EXPECT_DOUBLE_EQ(line.getZ(1), 70.0);

  // setZ and setM
  EXPECT_TRUE(line.setZ(0, 999.0));
  EXPECT_DOUBLE_EQ(line.getZ(0), 999.0);

  EXPECT_TRUE(line.setM(0, 888.0));
  EXPECT_DOUBLE_EQ(line.getM(0), 888.0);

  // setPoint extending to nPointCount
  const int curCount = line.getNumPoints();
  EXPECT_TRUE(line.setPoint(curCount, 99.0, 99.0));
  EXPECT_EQ(line.getNumPoints(), curCount + 1);

  // removePoint
  const int beforeRemove = line.getNumPoints();
  EXPECT_FALSE(line.removePoint(-1));
  EXPECT_FALSE(line.removePoint(beforeRemove));
  EXPECT_TRUE(line.removePoint(0));
  EXPECT_EQ(line.getNumPoints(), beforeRemove - 1);
  EXPECT_TRUE(line.removePoint(line.getNumPoints() - 1));

  // reversePoints
  OGRLineString lineRev;
  lineRev.reversePoints();  // empty
  lineRev.addPoint(1.0, 10.0, 100.0, 1000.0);
  lineRev.reversePoints();  // 1 point
  EXPECT_DOUBLE_EQ(lineRev.getX(0), 1.0);

  lineRev.addPoint(2.0, 20.0, 200.0, 2000.0);
  lineRev.addPoint(3.0, 30.0, 300.0, 3000.0);
  lineRev.reversePoints();
  EXPECT_DOUBLE_EQ(lineRev.getX(0), 3.0);
  EXPECT_DOUBLE_EQ(lineRev.getZ(0), 300.0);
  EXPECT_DOUBLE_EQ(lineRev.getM(0), 3000.0);
  EXPECT_DOUBLE_EQ(lineRev.getX(2), 1.0);
  EXPECT_DOUBLE_EQ(lineRev.getZ(2), 100.0);
  EXPECT_DOUBLE_EQ(lineRev.getM(2), 1000.0);

  // swapXY
  lineRev.swapXY();
  EXPECT_DOUBLE_EQ(lineRev.getX(0), 30.0);
  EXPECT_DOUBLE_EQ(lineRev.getY(0), 3.0);
}

// ============================================================================
// Batch Points & Arrays
// ============================================================================

TEST(OGRLineStringTest, BatchPointsAndArrays) {
  OGRLineString line;

  // setNumPoints shrinking and expanding
  EXPECT_TRUE(line.setNumPoints(5, TRUE));
  EXPECT_EQ(line.getNumPoints(), 5);
  EXPECT_DOUBLE_EQ(line.getX(0), 0.0);
  EXPECT_DOUBLE_EQ(line.getY(4), 0.0);

  EXPECT_TRUE(line.setNumPoints(2, FALSE));
  EXPECT_EQ(line.getNumPoints(), 2);

  EXPECT_TRUE(line.setNumPoints(0));
  EXPECT_EQ(line.getNumPoints(), 0);
  EXPECT_TRUE(line.setNumPoints(3, TRUE));
  EXPECT_EQ(line.getNumPoints(), 3);

  // setPoints with raw points
  std::vector<OGRRawPoint> rawPoints = {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}};
  std::vector<double> zVals = {10.0, 20.0, 30.0};
  std::vector<double> mVals = {100.0, 200.0, 300.0};

  EXPECT_TRUE(line.setPoints(3, rawPoints.data(), zVals.data()));
  EXPECT_TRUE(line.Is3D());
  EXPECT_DOUBLE_EQ(line.getZ(1), 20.0);

  EXPECT_TRUE(line.setPointsM(3, rawPoints.data(), mVals.data()));
  EXPECT_TRUE(line.IsMeasured());
  EXPECT_DOUBLE_EQ(line.getM(2), 300.0);

  std::vector<double> xVals = {1.0, 3.0, 5.0};
  std::vector<double> yVals = {2.0, 4.0, 6.0};

  EXPECT_TRUE(line.setPoints(3, xVals.data(), yVals.data(), zVals.data()));
  EXPECT_DOUBLE_EQ(line.getZ(0), 10.0);

  EXPECT_TRUE(line.setPointsM(3, xVals.data(), yVals.data(), mVals.data()));
  EXPECT_DOUBLE_EQ(line.getM(1), 200.0);

  EXPECT_TRUE(line.setPoints(3, xVals.data(), yVals.data(), zVals.data(),
                             mVals.data()));
  EXPECT_TRUE(line.Is3D());
  EXPECT_TRUE(line.IsMeasured());

  // getPoints(OGRRawPoint*, double*)
  std::vector<OGRRawPoint> outRaw(3);
  std::vector<double> outZ(3);
  line.getPoints(outRaw.data(), outZ.data());
  EXPECT_DOUBLE_EQ(outRaw[1].x, 3.0);
  EXPECT_DOUBLE_EQ(outZ[1], 20.0);

  // getPoints strided
  struct StridedPoint {
    double x, y, z, m;
  };
  std::vector<StridedPoint> outStrided(3);
  line.getPoints(&outStrided[0].x, sizeof(StridedPoint), &outStrided[0].y,
                 sizeof(StridedPoint), &outStrided[0].z, sizeof(StridedPoint),
                 &outStrided[0].m, sizeof(StridedPoint));
  EXPECT_DOUBLE_EQ(outStrided[2].x, 5.0);
  EXPECT_DOUBLE_EQ(outStrided[2].y, 6.0);
  EXPECT_DOUBLE_EQ(outStrided[2].z, 30.0);
  EXPECT_DOUBLE_EQ(outStrided[2].m, 300.0);

  // getPoints with zero strides (early exit)
  line.getPoints(&outStrided[0].x, 0, nullptr, 0, nullptr, 0, nullptr, 0);

  // contiguous fast path in getPoints
  std::vector<OGRRawPoint> outContig(3);
  line.getPoints(&outContig[0].x, sizeof(OGRRawPoint), &outContig[0].y,
                 sizeof(OGRRawPoint), outZ.data(), sizeof(double), nullptr, 0);
  EXPECT_DOUBLE_EQ(outContig[0].x, 1.0);

  // 2D line getPoints strided (null Z and M padded with 0)
  OGRLineString line2D;
  line2D.setPoints(3, xVals.data(), yVals.data());
  std::vector<double> zNull(3);
  std::vector<double> mNull(3);
  line2D.getPoints(nullptr, 0, nullptr, 0, zNull.data(), sizeof(double),
                   mNull.data(), sizeof(double));
  EXPECT_DOUBLE_EQ(zNull[0], 0.0);
  EXPECT_DOUBLE_EQ(mNull[0], 0.0);

  // addSubLineString
  OGRLineString subLine;
  subLine.addSubLineString(&line, 0, 1);
  EXPECT_EQ(subLine.getNumPoints(), 2);
  EXPECT_DOUBLE_EQ(subLine.getX(0), 1.0);
  EXPECT_DOUBLE_EQ(subLine.getX(1), 3.0);

  // reverse copy
  OGRLineString revSubLine;
  revSubLine.addSubLineString(&line, 2, 0);
  EXPECT_EQ(revSubLine.getNumPoints(), 3);
  EXPECT_DOUBLE_EQ(revSubLine.getX(0), 5.0);
  EXPECT_DOUBLE_EQ(revSubLine.getX(2), 1.0);

  // addSubLineString empty other
  OGRLineString emptyLine;
  subLine.addSubLineString(&emptyLine, 0, 0);
}

// ============================================================================
// Iterators
// ============================================================================

TEST(OGRLineStringTest, Iterators) {
  OGRLineString line;
  line.addPoint(1.0, 2.0, 3.0);
  line.addPoint(4.0, 5.0, 6.0);

  // Range-based for (non-const)
  double sumX = 0.0;
  for (auto&& pt : line) {
    sumX += pt.getX();
  }
  EXPECT_DOUBLE_EQ(sumX, 5.0);

  // Range-based for (const)
  const OGRLineString& cLine = line;
  double sumY = 0.0;
  for (const auto& pt : cLine) {
    sumY += pt.getY();
  }
  EXPECT_DOUBLE_EQ(sumY, 7.0);

  // OGRPointIterator
  auto it = std::unique_ptr<OGRPointIterator>(line.getPointIterator());
  ASSERT_NE(it, nullptr);
  OGRPoint pt;
  EXPECT_TRUE(it->getNextPoint(&pt));
  EXPECT_DOUBLE_EQ(pt.getX(), 1.0);
  EXPECT_TRUE(it->getNextPoint(&pt));
  EXPECT_DOUBLE_EQ(pt.getX(), 4.0);
  EXPECT_FALSE(it->getNextPoint(&pt));
}

// ============================================================================
// Spatial Relations, Analysis, Envelopes
// ============================================================================

TEST(OGRLineStringTest, SpatialRelationsAndMeasurements) {
  OGRLineString line;

  // Empty envelopes
  OGREnvelope env;
  line.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(env.MinX, 0.0);
  EXPECT_DOUBLE_EQ(env.MaxX, 0.0);

  OGREnvelope3D env3D;
  line.getEnvelope(&env3D);
  EXPECT_DOUBLE_EQ(env3D.MinZ, 0.0);
  EXPECT_DOUBLE_EQ(env3D.MaxZ, 0.0);

  line.addPoint(0.0, 0.0, 10.0);
  line.addPoint(3.0, 4.0, 20.0);

  line.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(env.MinX, 0.0);
  EXPECT_DOUBLE_EQ(env.MaxX, 3.0);
  EXPECT_DOUBLE_EQ(env.MinY, 0.0);
  EXPECT_DOUBLE_EQ(env.MaxY, 4.0);

  line.getEnvelope(&env3D);
  EXPECT_DOUBLE_EQ(env3D.MinZ, 10.0);
  EXPECT_DOUBLE_EQ(env3D.MaxZ, 20.0);

  // get_Length
  EXPECT_NEAR(line.get_Length(), 5.0, 1e-5);

  // StartPoint & EndPoint
  OGRPoint ptStart, ptEnd;
  line.StartPoint(&ptStart);
  EXPECT_DOUBLE_EQ(ptStart.getX(), 0.0);
  line.EndPoint(&ptEnd);
  EXPECT_DOUBLE_EQ(ptEnd.getX(), 3.0);

  // Value
  OGRPoint ptVal;
  line.Value(-1.0, &ptVal);  // < 0 -> start
  EXPECT_DOUBLE_EQ(ptVal.getX(), 0.0);

  line.Value(10.0, &ptVal);  // >= length -> end
  EXPECT_DOUBLE_EQ(ptVal.getX(), 3.0);

  line.Value(2.5, &ptVal);  // midpoint
  EXPECT_NEAR(ptVal.getX(), 1.5, 1e-5);
  EXPECT_NEAR(ptVal.getY(), 2.0, 1e-5);
  EXPECT_NEAR(ptVal.getZ(), 15.0, 1e-5);

  // Project
  OGRPoint ptProj(1.5, 2.0);
  EXPECT_GE(line.Project(&ptProj), 0.0);

  // getSubLine
  std::unique_ptr<OGRLineString> sub1(line.getSubLine(0.0, 2.5, FALSE));
  ASSERT_NE(sub1, nullptr);
  EXPECT_EQ(sub1->getNumPoints(), 3);
  EXPECT_NEAR(sub1->get_Length(), 2.5, 1e-5);

  std::unique_ptr<OGRLineString> subRatio(line.getSubLine(0.0, 0.5, TRUE));
  ASSERT_NE(subRatio, nullptr);
  EXPECT_NEAR(subRatio->get_Length(), 2.5, 1e-5);

  // SubLine with multiple segments and reaching the end
  OGRLineString multiLine;
  multiLine.addPoint(0.0, 0.0);
  multiLine.addPoint(10.0, 0.0);
  multiLine.addPoint(10.0, 10.0);
  multiLine.addPoint(20.0, 10.0);
  // Total length = 30.
  std::unique_ptr<OGRLineString> subSeg1(multiLine.getSubLine(2.0, 8.0, FALSE));
  ASSERT_NE(subSeg1, nullptr);
  EXPECT_EQ(subSeg1->getNumPoints(), 2);
  EXPECT_NEAR(subSeg1->get_Length(), 6.0, 1e-5);

  std::unique_ptr<OGRLineString> subMulti(
      multiLine.getSubLine(5.0, 15.0, FALSE));
  ASSERT_NE(subMulti, nullptr);
  EXPECT_GT(subMulti->getNumPoints(), 2);
  EXPECT_NEAR(subMulti->get_Length(), 10.0, 1e-5);

  std::unique_ptr<OGRLineString> subEnd(multiLine.getSubLine(5.0, 30.0, FALSE));
  ASSERT_NE(subEnd, nullptr);
  EXPECT_NEAR(subEnd->get_Length(), 25.0, 1e-5);

  // 3D SubLine
  OGRLineString multiLine3D;
  multiLine3D.addPoint(0.0, 0.0, 1.0);
  multiLine3D.addPoint(10.0, 0.0, 2.0);
  multiLine3D.addPoint(10.0, 10.0, 3.0);
  std::unique_ptr<OGRLineString> sub3DSeg(
      multiLine3D.getSubLine(2.0, 8.0, FALSE));
  ASSERT_NE(sub3DSeg, nullptr);
  EXPECT_TRUE(sub3DSeg->Is3D());

  std::unique_ptr<OGRLineString> sub3DMulti(
      multiLine3D.getSubLine(5.0, 15.0, FALSE));
  ASSERT_NE(sub3DMulti, nullptr);
  EXPECT_TRUE(sub3DMulti->Is3D());

  std::unique_ptr<OGRLineString> sub3DEnd(
      multiLine3D.getSubLine(5.0, 20.0, FALSE));
  ASSERT_NE(sub3DEnd, nullptr);
  EXPECT_TRUE(sub3DEnd->Is3D());

  // getSubLine reaching end via duplicate points at end (dfSegLength == 0)
  OGRLineString dupEndLine;
  dupEndLine.addPoint(0.0, 0.0);
  dupEndLine.addPoint(10.0, 0.0);
  dupEndLine.addPoint(10.0, 0.0);  // zero-length segment at end
  std::unique_ptr<OGRLineString> subDup(
      dupEndLine.getSubLine(0.0, 10.0, FALSE));
  ASSERT_NE(subDup, nullptr);

  // 3D duplicate at end
  OGRLineString dupEndLine3D;
  dupEndLine3D.addPoint(0.0, 0.0, 1.0);
  dupEndLine3D.addPoint(10.0, 0.0, 2.0);
  dupEndLine3D.addPoint(10.0, 0.0, 2.0);
  std::unique_ptr<OGRLineString> subDup3D(
      dupEndLine3D.getSubLine(0.0, 10.0, FALSE));
  ASSERT_NE(subDup3D, nullptr);

  WithQuietHandler quiet;
  EXPECT_EQ(line.getSubLine(3.0, 2.0, FALSE), nullptr);  // invalid
  EXPECT_EQ(line.getSubLine(10.0, 20.0, FALSE), nullptr);

  // Equals
  EXPECT_TRUE(line.Equals(&line));
  EXPECT_FALSE(line.Equals(&ptStart));  // wrong type

  OGRLineString other;
  other.addPoint(0.0, 0.0, 10.0);
  EXPECT_FALSE(line.Equals(&other));  // diff point count

  other.addPoint(3.0, 4.0, 20.0);
  EXPECT_TRUE(line.Equals(&other));

  other.setZ(1, 99.0);
  EXPECT_FALSE(line.Equals(&other));

  OGRLineString empty1, empty2;
  EXPECT_TRUE(empty1.Equals(&empty2));

  // get_LinearArea & isClockwise
  TestLineString closedRing;
  closedRing.addPoint(0.0, 0.0);
  closedRing.addPoint(0.0, 4.0);
  closedRing.addPoint(4.0, 4.0);
  closedRing.addPoint(4.0, 0.0);
  closedRing.addPoint(0.0, 0.0);

  EXPECT_NEAR(closedRing.get_LinearArea(), 16.0, 1e-5);
  EXPECT_TRUE(closedRing.isClockwise());  // 0,0 -> 0,4 -> 4,4 -> 4,0 is CW

  closedRing.reversePoints();
  EXPECT_FALSE(closedRing.isClockwise());  // CCW

  TestLineString openLine;
  openLine.addPoint(0.0, 0.0);
  openLine.addPoint(4.0, 0.0);
  EXPECT_DOUBLE_EQ(openLine.get_LinearArea(), 0.0);
  EXPECT_FALSE(openLine.isClockwise());  // open line fallback

  // Duplicate lowest rightmost vertex for isClockwise fallback
  OGRLineString dupLine;
  dupLine.addPoint(0.0, 0.0);
  dupLine.addPoint(0.0, 0.0);
  dupLine.addPoint(2.0, 2.0);
  dupLine.addPoint(0.0, 0.0);
  dupLine.isClockwise();
}

// ============================================================================
// Segmentize & Transformations
// ============================================================================

TEST(OGRLineStringTest, Segmentize) {
  WithQuietHandler quiet;
  OGRLineString line;
  EXPECT_FALSE(line.segmentize(0.0));
  EXPECT_FALSE(line.segmentize(-1.0));
  EXPECT_TRUE(line.segmentize(10.0));  // empty

  line.addPoint(0.0, 0.0);
  line.addPoint(10.0, 0.0);
  EXPECT_TRUE(line.segmentize(2.0));
  EXPECT_EQ(line.getNumPoints(), 6);

  // Reverse direction segmentize
  OGRLineString line2;
  line2.addPoint(10.0, 0.0);
  line2.addPoint(0.0, 0.0);
  EXPECT_TRUE(line2.segmentize(2.0));
  EXPECT_EQ(line2.getNumPoints(), 6);

  // 3D & Measured segmentize (GDAL copies Z/M without interpolation)
  OGRLineString lineZM;
  lineZM.addPoint(0.0, 0.0, 0.0, 0.0);
  lineZM.addPoint(10.0, 0.0, 100.0, 1000.0);
  EXPECT_TRUE(lineZM.segmentize(5.0));
  EXPECT_EQ(lineZM.getNumPoints(), 3);
  EXPECT_DOUBLE_EQ(lineZM.getZ(1), 100.0);
  EXPECT_DOUBLE_EQ(lineZM.getM(1), 1000.0);
}

TEST(OGRLineStringTest, CoordinateTransformation) {
  OGRLineString line;
  line.addPoint(0.0, 0.0, 1.0);
  line.addPoint(10.0, 10.0, 2.0);
  line.addPoint(20.0, 20.0, 3.0);

  FakeCoordTransform successTransform(true);
  EXPECT_EQ(line.transform(&successTransform), OGRERR_NONE);
  EXPECT_DOUBLE_EQ(line.getX(0), 10.0);
  EXPECT_DOUBLE_EQ(line.getY(0), 20.0);
  EXPECT_DOUBLE_EQ(line.getZ(0), 6.0);

  // Partial reprojection failure without config option set (triggers warning)
  WithQuietHandler quiet;
  FakeCoordTransform partialFail(false);
  CPLSetConfigOption("OGR_ENABLE_PARTIAL_REPROJECTION", nullptr);
  EXPECT_EQ(line.transform(&partialFail), OGRERR_FAILURE);

  // Point 0 fails, point 1 succeeds (covers lines 2589-2593)
  class PartialFailP0Transform : public OGRCoordinateTransformation {
   public:
    OGRSpatialReference* GetSourceCS() const override { return nullptr; }
    OGRSpatialReference* GetTargetCS() const override { return nullptr; }
    OGRCoordinateTransformation* Clone() const override { return nullptr; }
    OGRCoordinateTransformation* GetInverse() const override { return nullptr; }
    int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                  int* pabSuccess) override {
      for (size_t i = 0; i < nCount; ++i) {
        if (pabSuccess) pabSuccess[i] = (i > 0 ? TRUE : FALSE);
      }
      return TRUE;
    }
  };
  PartialFailP0Transform p0Fail;
  EXPECT_EQ(line.transform(&p0Fail), OGRERR_FAILURE);

  // Partial reprojection failure with option set to FALSE
  CPLSetConfigOption("OGR_ENABLE_PARTIAL_REPROJECTION", "FALSE");
  EXPECT_EQ(line.transform(&partialFail), OGRERR_FAILURE);

  // Partial reprojection enabled (partial success)
  CPLSetConfigOption("OGR_ENABLE_PARTIAL_REPROJECTION", "TRUE");
  EXPECT_EQ(line.transform(&partialFail), OGRERR_NONE);

  // Partial reprojection enabled but all points fail
  FakeCoordTransform allFail(false, false, /*bAllFail=*/true);
  EXPECT_EQ(line.transform(&allFail), OGRERR_FAILURE);

  CPLSetConfigOption("OGR_ENABLE_PARTIAL_REPROJECTION", nullptr);
}

// ============================================================================
// Geodesic Calculations
// ============================================================================

TEST(OGRLineStringTest, GeodesicCalculations) {
  WithQuietHandler quiet;
  {
    OGRLineString line;
    line.addPoint(0.0, 0.0);
    line.addPoint(0.0, 1.0);
    line.addPoint(1.0, 1.0);
    line.addPoint(1.0, 0.0);
    line.addPoint(0.0, 0.0);

    // Without SRS
    EXPECT_DOUBLE_EQ(line.get_GeodesicArea(nullptr), -1.0);
    EXPECT_DOUBLE_EQ(line.get_GeodesicLength(nullptr), -1.0);

    // With WGS84 SRS
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    double dfLength = line.get_GeodesicLength(&srs);
    EXPECT_GT(dfLength, 0.0);

    double dfArea = line.get_GeodesicArea(&srs);
    EXPECT_GT(dfArea, 0.0);

    // With line attached SRS
    auto* poSRS = srs.Clone();
    line.assignSpatialReference(poSRS);
    poSRS->Release();
    EXPECT_GT(line.get_GeodesicLength(nullptr), 0.0);
    EXPECT_GT(line.get_GeodesicArea(nullptr), 0.0);
  }

  OSRCleanup();
  OSRCleanupTLSContext();
}

// ============================================================================
// WKT and WKB Serialization
// ============================================================================

TEST(OGRLineStringTest, WktSerialization) {
  OGRLineString line;
  const char* pszWkt = "LINESTRING (0 0, 1 1, 2 2)";
  EXPECT_EQ(line.importFromWkt(&pszWkt), OGRERR_NONE);
  EXPECT_EQ(line.getNumPoints(), 3);

  // 3D
  const char* pszWktZ = "LINESTRING Z (0 0 1, 1 1 2)";
  EXPECT_EQ(line.importFromWkt(&pszWktZ), OGRERR_NONE);
  EXPECT_TRUE(line.Is3D());

  // M
  const char* pszWktM = "LINESTRING M (0 0 5, 1 1 6)";
  EXPECT_EQ(line.importFromWkt(&pszWktM), OGRERR_NONE);
  EXPECT_TRUE(line.IsMeasured());

  // Fresh 2D line importing M
  OGRLineString lineFreshM;
  const char* pszWktFreshM = "LINESTRING M (0 0 5, 1 1 6)";
  EXPECT_EQ(lineFreshM.importFromWkt(&pszWktFreshM), OGRERR_NONE);
  EXPECT_TRUE(lineFreshM.IsMeasured());
  EXPECT_FALSE(lineFreshM.Is3D());

  // Fresh 2D line importing Z
  OGRLineString lineFreshZ;
  const char* pszWktFreshZ = "LINESTRING Z (0 0 1, 1 1 2)";
  EXPECT_EQ(lineFreshZ.importFromWkt(&pszWktFreshZ), OGRERR_NONE);
  EXPECT_TRUE(lineFreshZ.Is3D());
  EXPECT_FALSE(lineFreshZ.IsMeasured());

  // ZM
  const char* pszWktZM = "LINESTRING ZM (0 0 1 5, 1 1 2 6)";
  EXPECT_EQ(line.importFromWkt(&pszWktZM), OGRERR_NONE);
  EXPECT_TRUE(line.Is3D());
  EXPECT_TRUE(line.IsMeasured());

  // Implied 3D / 4D without keyword
  const char* pszImpliedZ = "LINESTRING (0 0 1, 1 1 2)";
  EXPECT_EQ(line.importFromWkt(&pszImpliedZ), OGRERR_NONE);
  EXPECT_TRUE(line.Is3D());

  OGRLineString lineFresh2D;
  const char* pszImpliedZM = "LINESTRING (0 0 1 2, 3 4 5 6)";
  EXPECT_EQ(lineFresh2D.importFromWkt(&pszImpliedZM), OGRERR_NONE);
  EXPECT_TRUE(lineFresh2D.Is3D());
  EXPECT_TRUE(lineFresh2D.IsMeasured());

  // Empty
  const char* pszEmpty = "LINESTRING EMPTY";
  EXPECT_EQ(line.importFromWkt(&pszEmpty), OGRERR_NONE);
  EXPECT_TRUE(line.IsEmpty());

  // Malformed
  WithQuietHandler quiet;
  const char* pszBad = "LINESTRING (0)";
  EXPECT_NE(line.importFromWkt(&pszBad), OGRERR_NONE);

  const char* pszWrong = "POINT (0 0)";
  EXPECT_NE(line.importFromWkt(&pszWrong), OGRERR_NONE);

  // exportToWkt
  line.addPoint(1.0, 2.0);
  char* pszOut = nullptr;
  EXPECT_EQ(line.exportToWkt(&pszOut), OGRERR_NONE);
  EXPECT_NE(strstr(pszOut, "LINESTRING (1 2)"), nullptr);
  CPLFree(pszOut);

  // importFromWKTListOnly via TestLineString
  TestLineString testLine;
  const char* pszList = "(0 0, 10 10)";
  OGRRawPoint* paoPts = nullptr;
  int nMaxPts = 0;
  double* padfZ = nullptr;
  EXPECT_EQ(testLine.importFromWKTListOnly(&pszList, FALSE, FALSE, paoPts,
                                           nMaxPts, padfZ),
            OGRERR_NONE);
  CPLFree(paoPts);
  CPLFree(padfZ);

  // 3D without flags
  const char* pszList3D = "(0 0 1, 10 10 2)";
  paoPts = nullptr;
  nMaxPts = 0;
  padfZ = nullptr;
  EXPECT_EQ(testLine.importFromWKTListOnly(&pszList3D, FALSE, FALSE, paoPts,
                                           nMaxPts, padfZ),
            OGRERR_NONE);
  CPLFree(paoPts);
  CPLFree(padfZ);

  // 4D without flags
  const char* pszList4D = "(0 0 1 2, 10 10 3 4)";
  paoPts = nullptr;
  nMaxPts = 0;
  padfZ = nullptr;
  EXPECT_EQ(testLine.importFromWKTListOnly(&pszList4D, FALSE, FALSE, paoPts,
                                           nMaxPts, padfZ),
            OGRERR_NONE);
  CPLFree(paoPts);
  CPLFree(padfZ);
}

TEST(OGRLineStringTest, WkbSerialization) {
  OGRLineString line;
  line.addPoint(1.0, 2.0, 3.0, 4.0);
  line.addPoint(5.0, 6.0, 7.0, 8.0);

  // exportToWkb NDR and XDR
  OGRwkbExportOptions opts;
  opts.eByteOrder = wkbNDR;
  opts.eWkbVariant = wkbVariantIso;
  std::vector<unsigned char> buf(line.WkbSize());
  EXPECT_EQ(line.exportToWkb(buf.data(), &opts), OGRERR_NONE);

  OGRLineString imported;
  size_t nConsumed = 0;
  EXPECT_EQ(
      imported.importFromWkb(buf.data(), buf.size(), wkbVariantIso, nConsumed),
      OGRERR_NONE);
  EXPECT_TRUE(line.Equals(&imported));
  EXPECT_DOUBLE_EQ(imported.getM(1), 8.0);

  // XDR
  opts.eByteOrder = wkbXDR;
  EXPECT_EQ(line.exportToWkb(buf.data(), &opts), OGRERR_NONE);
  OGRLineString importedXDR;
  EXPECT_EQ(importedXDR.importFromWkb(buf.data(), buf.size(), wkbVariantIso,
                                      nConsumed),
            OGRERR_NONE);
  EXPECT_TRUE(line.Equals(&importedXDR));

  // Truncated WKB error handling
  WithQuietHandler quiet;
  EXPECT_EQ(imported.importFromWkb(buf.data(), 8, wkbVariantIso, nConsumed),
            OGRERR_NOT_ENOUGH_DATA);
}

// ============================================================================
// Casting and Helpers
// ============================================================================

TEST(OGRLineStringTest, CastingAndHelpers) {
  WithQuietHandler quiet;
  TestLineString line;
  line.addPoint(0.0, 0.0);
  line.addPoint(1.0, 1.0);

  // Unclosed cast fails
  OGRLineString* poLS = new OGRLineString(line);
  EXPECT_EQ(TestLineString::CastToLinearRing(poLS), nullptr);

  // Closed cast succeeds
  line.addPoint(0.0, 0.0);
  OGRLineString* poClosed = new OGRLineString(line);
  OGRLinearRing* poRing = TestLineString::CastToLinearRing(poClosed);
  ASSERT_NE(poRing, nullptr);
  EXPECT_EQ(poRing->getNumPoints(), 3);
  delete poRing;

  // Caster functions
  auto pfnLS = line.GetCasterToLineString();
  ASSERT_NE(pfnLS, nullptr);
  EXPECT_EQ(pfnLS(&line), &line);

  auto pfnLR = line.GetCasterToLinearRing();
  ASSERT_NE(pfnLR, nullptr);

  // get_Area & get_AreaOfCurveSegments
  EXPECT_GE(line.get_Area(), 0.0);
  EXPECT_DOUBLE_EQ(line.get_AreaOfCurveSegments(), 0.0);

  // TransferMembersAndDestroy on 3D+Measured
  auto* poSrcZM = new OGRLineString();
  poSrcZM->setNumPoints(2);
  poSrcZM->setPoint(0, 1.0, 2.0, 3.0, 4.0);
  poSrcZM->setPoint(1, 5.0, 6.0, 7.0, 8.0);
  OGRLineString dstZM;
  TestLineString::TransferMembersAndDestroy(poSrcZM, &dstZM);
  EXPECT_TRUE(dstZM.Is3D());
  EXPECT_TRUE(dstZM.IsMeasured());
  EXPECT_EQ(dstZM.getNumPoints(), 2);

  // getCurveGeometry
  std::unique_ptr<OGRGeometry> poCurve(line.getCurveGeometry(nullptr));
  ASSERT_NE(poCurve, nullptr);
  EXPECT_EQ(poCurve->getGeometryType(), wkbLineString);
}

TEST(OGRLineStringTest, WkbEndianAndVariants) {
  OGRLineString lineSwap;
  lineSwap.addPoint(1.0, 2.0);
  lineSwap.addPoint(3.0, 4.0);

  OGRwkbExportOptions optsSwap;
  optsSwap.eByteOrder = (CPL_IS_LSB ? wkbXDR : wkbNDR);
  optsSwap.sPrecision.nXYBitPrecision = 16;
  std::vector<unsigned char> wkbSwap(lineSwap.WkbSize());
  EXPECT_EQ(lineSwap.exportToWkb(wkbSwap.data(), &optsSwap), OGRERR_NONE);

  OGRLineString lineSwapIn;
  size_t nConsumed = 0;
  EXPECT_EQ(lineSwapIn.importFromWkb(wkbSwap.data(), wkbSwap.size(),
                                     wkbVariantOldOgc, nConsumed),
            OGRERR_NONE);
  EXPECT_EQ(lineSwapIn.getNumPoints(), 2);

  // 3D swap
  OGRLineString line3DSwap;
  line3DSwap.addPoint(1.0, 2.0, 3.0);
  line3DSwap.addPoint(4.0, 5.0, 6.0);
  optsSwap.sPrecision.nZBitPrecision = 16;
  std::vector<unsigned char> wkb3DSwap(line3DSwap.WkbSize());
  EXPECT_EQ(line3DSwap.exportToWkb(wkb3DSwap.data(), &optsSwap), OGRERR_NONE);
  OGRLineString line3DSwapIn;
  EXPECT_EQ(line3DSwapIn.importFromWkb(wkb3DSwap.data(), wkb3DSwap.size(),
                                       wkbVariantIso, nConsumed),
            OGRERR_NONE);
  EXPECT_TRUE(line3DSwapIn.Is3D());

  // Measured swap (2D + M, not 3D!)
  OGRLineString lineMSwap;
  lineMSwap.setNumPoints(2);
  lineMSwap.setPointM(0, 1.0, 2.0, 7.0);
  lineMSwap.setPointM(1, 3.0, 4.0, 8.0);
  optsSwap.sPrecision.nMBitPrecision = 16;
  std::vector<unsigned char> wkbMSwap(lineMSwap.WkbSize());
  EXPECT_EQ(lineMSwap.exportToWkb(wkbMSwap.data(), &optsSwap), OGRERR_NONE);
  OGRLineString lineMSwapIn;
  EXPECT_EQ(lineMSwapIn.importFromWkb(wkbMSwap.data(), wkbMSwap.size(),
                                      wkbVariantIso, nConsumed),
            OGRERR_NONE);
  EXPECT_TRUE(lineMSwapIn.IsMeasured());
  EXPECT_FALSE(lineMSwapIn.Is3D());

  // ZM swap
  OGRLineString lineZMSwap;
  lineZMSwap.addPoint(1.0, 2.0, 3.0, 7.0);
  lineZMSwap.addPoint(4.0, 5.0, 6.0, 8.0);
  std::vector<unsigned char> wkbZMSwap(lineZMSwap.WkbSize());
  EXPECT_EQ(lineZMSwap.exportToWkb(wkbZMSwap.data(), &optsSwap), OGRERR_NONE);
  OGRLineString lineZMSwapIn;
  EXPECT_EQ(lineZMSwapIn.importFromWkb(wkbZMSwap.data(), wkbZMSwap.size(),
                                       wkbVariantIso, nConsumed),
            OGRERR_NONE);
  EXPECT_TRUE(lineZMSwapIn.Is3D());
  EXPECT_TRUE(lineZMSwapIn.IsMeasured());

  // PostGIS1 variant for 3D and M
  OGRwkbExportOptions optsPG;
  optsPG.eWkbVariant = wkbVariantPostGIS1;
  std::vector<unsigned char> wkbPG3D(line3DSwap.WkbSize());
  EXPECT_EQ(line3DSwap.exportToWkb(wkbPG3D.data(), &optsPG), OGRERR_NONE);
  std::vector<unsigned char> wkbPGM(lineMSwap.WkbSize());
  EXPECT_EQ(lineMSwap.exportToWkb(wkbPGM.data(), &optsPG), OGRERR_NONE);

  // WKB too small
  WithQuietHandler quietWkb;
  EXPECT_EQ(
      lineSwapIn.importFromWkb(wkbSwap.data(), 12, wkbVariantIso, nConsumed),
      OGRERR_NOT_ENOUGH_DATA);
}

TEST(OGRLineStringTest, SetPointsVariations) {
  OGRLineString line;
  OGRRawPoint pts[2] = {{1.0, 2.0}, {3.0, 4.0}};
  double z[2] = {10.0, 20.0};
  double m[2] = {100.0, 200.0};

  // 2D line setPoints with Z -> promotes to 3D
  EXPECT_TRUE(line.setPoints(2, pts, z, nullptr));
  EXPECT_TRUE(line.Is3D());
  EXPECT_FALSE(line.IsMeasured());

  // 3D line setPoints without Z -> demotes to 2D
  EXPECT_TRUE(line.setPoints(2, pts, nullptr, nullptr));
  EXPECT_FALSE(line.Is3D());

  // 2D line setPoints with M -> promotes to Measured
  EXPECT_TRUE(line.setPoints(2, pts, nullptr, m));
  EXPECT_TRUE(line.IsMeasured());
  EXPECT_FALSE(line.Is3D());

  // Measured line setPoints without M -> removes M
  EXPECT_TRUE(line.setPoints(2, pts, nullptr, nullptr));
  EXPECT_FALSE(line.IsMeasured());

  // 2D line setPoints with both Z and M
  EXPECT_TRUE(line.setPoints(2, pts, z, m));
  EXPECT_TRUE(line.Is3D());
  EXPECT_TRUE(line.IsMeasured());
}

TEST(OGRLineStringTest, SetPointWithOGRPoint) {
  OGRPoint pt2D(1.0, 2.0);
  OGRPoint pt3D(1.0, 2.0, 3.0);
  OGRPoint ptM(1.0, 2.0, 0.0, 4.0);
  OGRPoint ptZM(1.0, 2.0, 3.0, 4.0);

  OGRLineString line2D;
  line2D.setNumPoints(1);
  EXPECT_TRUE(line2D.setPoint(0, &pt2D));

  OGRLineString line3D;
  line3D.setNumPoints(1);
  line3D.set3D(TRUE);
  EXPECT_TRUE(line3D.setPoint(0, &pt3D));

  OGRLineString lineM;
  lineM.setNumPoints(1);
  lineM.setMeasured(TRUE);
  EXPECT_TRUE(lineM.setPoint(0, &ptM));

  OGRLineString lineZM;
  lineZM.setNumPoints(1);
  lineZM.set3D(TRUE);
  lineZM.setMeasured(TRUE);
  EXPECT_TRUE(lineZM.setPoint(0, &ptZM));
}

TEST(OGRLineStringTest, SetZAndSetMExpansions) {
  OGRLineString line;
  // setZ on 2D line promotes to 3D and expands count
  EXPECT_TRUE(line.setZ(2, 50.0));
  EXPECT_TRUE(line.Is3D());
  EXPECT_EQ(line.getNumPoints(), 3);
  EXPECT_DOUBLE_EQ(line.getZ(2), 50.0);

  // setM on non-measured line promotes to Measured and expands count
  EXPECT_TRUE(line.setM(4, 100.0));
  EXPECT_TRUE(line.IsMeasured());
  EXPECT_EQ(line.getNumPoints(), 5);
  EXPECT_DOUBLE_EQ(line.getM(4), 100.0);

  // setPointM on 2D line
  OGRLineString line2;
  EXPECT_TRUE(line2.setPointM(2, 1.0, 2.0, 30.0));
  EXPECT_TRUE(line2.IsMeasured());
  EXPECT_EQ(line2.getNumPoints(), 3);
}

TEST(OGRLineStringTest, CurveToLineAndBufferFree) {
  OGRLineString line;
  line.addPoint(1.0, 2.0);
  std::unique_ptr<OGRLineString> cloned(line.CurveToLine());
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getNumPoints(), 1);

  // Buffer free optimization when nPointCount == 0 && paoPoints != nullptr
  line.setNumPoints(10);
  line.setNumPoints(0, FALSE);
  line.setNumPoints(50);
  EXPECT_EQ(line.getNumPoints(), 50);

  // Exceed point limit
  WithQuietHandler quiet;
  EXPECT_FALSE(line.setNumPoints(
      std::numeric_limits<int>::max() / sizeof(OGRRawPoint) + 1));

  // segmentize too many points
  OGRLineString segLine;
  segLine.addPoint(0.0, 0.0);
  segLine.addPoint(1e9, 0.0);
  EXPECT_FALSE(segLine.segmentize(1e-5));
}

}  // namespace
}  // namespace autotest2
