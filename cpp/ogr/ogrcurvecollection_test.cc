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
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

class TestCT : public OGRCoordinateTransformation {
 public:
  explicit TestCT(int fail_at_index = -1) : fail_at_index_(fail_at_index) {}
  const OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  const OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  int Transform(size_t nCount, double* x, double* y, double* z, double* m,
                int* pabSuccess) override {
    call_count_++;
    if (fail_at_index_ >= 0 && call_count_ - 1 == fail_at_index_) {
      if (pabSuccess) {
        for (size_t i = 0; i < nCount; ++i) pabSuccess[i] = FALSE;
      }
      return FALSE;
    }
    for (size_t i = 0; i < nCount; ++i) {
      if (x) x[i] += 10.0;
      if (y) y[i] += 10.0;
      if (pabSuccess) pabSuccess[i] = TRUE;
    }
    return TRUE;
  }
  OGRCoordinateTransformation* Clone() const override {
    return new TestCT(fail_at_index_);
  }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }

 private:
  int fail_at_index_;
  int call_count_ = 0;
};

std::unique_ptr<OGRLineString> CreateSampleLine(double offset_x = 0.0) {
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(offset_x + 0.0, 0.0);
  ls->addPoint(offset_x + 10.0, 0.0);
  ls->addPoint(offset_x + 10.0, 10.0);
  return ls;
}

TEST(OgrCurveCollectionTest, DefaultConstructorAndEmpty) {
  OGRCurveCollection col;
  EXPECT_EQ(0, col.getNumCurves());
  EXPECT_TRUE(col.IsEmpty());
  EXPECT_FALSE(col.hasEmptyParts());

  OGRCompoundCurve cc;
  col.empty(&cc);
  EXPECT_EQ(0, col.getNumCurves());

  OGREnvelope env;
  col.getEnvelope(&env);
  EXPECT_DOUBLE_EQ(0.0, env.MinX);
  EXPECT_DOUBLE_EQ(0.0, env.MaxX);
  EXPECT_DOUBLE_EQ(0.0, env.MinY);
  EXPECT_DOUBLE_EQ(0.0, env.MaxY);

  OGREnvelope3D env3d;
  col.getEnvelope(&env3d);
  EXPECT_DOUBLE_EQ(0.0, env3d.MinZ);
  EXPECT_DOUBLE_EQ(0.0, env3d.MaxZ);
}

TEST(OgrCurveCollectionTest, AddGetStealRemoveCurve) {
  OGRCurveCollection col;
  OGRCompoundCurve cc;

  auto ls1 = CreateSampleLine(0.0);
  auto ls2 = CreateSampleLine(20.0);
  auto ls3 = CreateSampleLine(40.0);

  // Add with bNeedRealloc = TRUE
  EXPECT_EQ(OGRERR_NONE, col.addCurveDirectly(&cc, ls1.release(), TRUE));
  EXPECT_EQ(1, col.getNumCurves());
  EXPECT_FALSE(col.IsEmpty());

  // Add with bNeedRealloc = FALSE
  EXPECT_EQ(OGRERR_NONE, col.addCurveDirectly(&cc, ls2.release(), TRUE));
  EXPECT_EQ(OGRERR_NONE, col.addCurveDirectly(&cc, ls3.release(), TRUE));
  EXPECT_EQ(3, col.getNumCurves());

  // Out of bounds getCurve
  EXPECT_EQ(nullptr, col.getCurve(-1));
  EXPECT_EQ(nullptr, col.getCurve(3));
  EXPECT_THAT(col.getCurve(0), NotNull());
  const auto& const_col = col;
  EXPECT_EQ(nullptr, const_col.getCurve(-1));
  EXPECT_EQ(nullptr, const_col.getCurve(3));
  EXPECT_THAT(const_col.getCurve(1), NotNull());

  // Steal curve out of bounds
  EXPECT_EQ(nullptr, col.stealCurve(-1));
  EXPECT_EQ(nullptr, col.stealCurve(3));

  // Steal middle curve (index 1)
  OGRCurve* stolen = col.stealCurve(1);
  ASSERT_THAT(stolen, NotNull());
  EXPECT_EQ(2, col.getNumCurves());
  delete stolen;

  // Remove curve out of bounds
  EXPECT_EQ(OGRERR_FAILURE, col.removeCurve(-2, true));
  EXPECT_EQ(OGRERR_FAILURE, col.removeCurve(2, true));

  // Remove curve at index 0 (without deleting)
  OGRCurve* c0 = col.getCurve(0);
  EXPECT_EQ(OGRERR_NONE, col.removeCurve(0, false));
  EXPECT_EQ(1, col.getNumCurves());
  delete c0;

  // Remove all with index -1
  EXPECT_EQ(OGRERR_NONE, col.removeCurve(-1, true));
  EXPECT_EQ(0, col.getNumCurves());
}

TEST(OgrCurveCollectionTest, CopyAndMoveConstructorsAndAssignments) {
  OGRCurveCollection col1;
  OGRCompoundCurve cc;
  col1.addCurveDirectly(&cc, CreateSampleLine(0.0).release(), TRUE);
  col1.addCurveDirectly(&cc, CreateSampleLine(10.0).release(), TRUE);

  // Copy constructor
  OGRCurveCollection col2(col1);
  EXPECT_EQ(2, col2.getNumCurves());
  EXPECT_TRUE(col1.Equals(&col2));

  // Copy assignment
  OGRCurveCollection col3;
  col3 = col1;
  EXPECT_EQ(2, col3.getNumCurves());
  EXPECT_TRUE(col1.Equals(&col3));

  // Self-assignment
  auto& ref = col3;
  col3 = ref;
  EXPECT_EQ(2, col3.getNumCurves());

  // Move constructor
  OGRCurveCollection col4(std::move(col2));
  EXPECT_EQ(2, col4.getNumCurves());
  EXPECT_EQ(0, col2.getNumCurves());

  // Move assignment
  OGRCurveCollection col5;
  col5 = std::move(col4);
  EXPECT_EQ(2, col5.getNumCurves());
  EXPECT_EQ(0, col4.getNumCurves());
}

TEST(OgrCurveCollectionTest, EqualsComparisons) {
  OGRCurveCollection col1;
  OGRCurveCollection col2;
  OGRCompoundCurve cc;

  EXPECT_TRUE(col1.Equals(&col2));

  col1.addCurveDirectly(&cc, CreateSampleLine(0.0).release(), TRUE);
  EXPECT_FALSE(col1.Equals(&col2));

  col2.addCurveDirectly(&cc, CreateSampleLine(5.0).release(), TRUE);
  EXPECT_FALSE(col1.Equals(&col2));  // different coords

  col2.removeCurve(-1, true);
  col2.addCurveDirectly(&cc, CreateSampleLine(0.0).release(), TRUE);
  EXPECT_TRUE(col1.Equals(&col2));
}

TEST(OgrCurveCollectionTest, WktSizeAndExport) {
  OGRCurveCollection col;
  OGRCompoundCurve cc;

  EXPECT_EQ(9, col.WkbSize());

  auto ls1 = CreateSampleLine(0.0);
  auto ls2 = CreateSampleLine(10.0);
  col.addCurveDirectly(&cc, ls1.release(), TRUE);
  col.addCurveDirectly(&cc, ls2.release(), TRUE);

  EXPECT_GT(col.WkbSize(), 9);

  OGRWktOptions opts;
  OGRErr err = OGRERR_NONE;
  std::string wkt = col.exportToWkt(&cc, opts, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_THAT(wkt, HasSubstr("COMPOUNDCURVE"));

  // Empty curves in collection
  OGRCurveCollection empty_col;
  OGRCompoundCurve cc_empty;
  auto empty_ls = std::make_unique<OGRLineString>();
  empty_col.addCurveDirectly(&cc_empty, empty_ls.release(), TRUE);
  std::string empty_wkt = empty_col.exportToWkt(&cc_empty, opts, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_THAT(empty_wkt, HasSubstr("EMPTY"));
}

TEST(OgrCurveCollectionTest, WkbImportAndExportVariants) {
  OGRCurveCollection col;
  OGRCurvePolygon cp;
  cp.set3D(TRUE);

  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0, 5);
  ls->addPoint(10, 0, 5);
  ls->addPoint(10, 10, 5);
  ls->addPoint(0, 10, 5);
  ls->addPoint(0, 0, 5);
  col.addCurveDirectly(&cp, ls.release(), TRUE);

  std::vector<unsigned char> wkb(col.WkbSize() + 100);

  // PostGIS1 variant
  OGRwkbExportOptions opt_pg1;
  opt_pg1.eWkbVariant = wkbVariantPostGIS1;
  opt_pg1.eByteOrder = wkbNDR;
  EXPECT_EQ(OGRERR_NONE, col.exportToWkb(&cp, wkb.data(), &opt_pg1));

  // ISO variant with XDR byte order
  OGRwkbExportOptions opt_xdr;
  opt_xdr.eWkbVariant = wkbVariantIso;
  opt_xdr.eByteOrder = wkbXDR;
  EXPECT_EQ(OGRERR_NONE, col.exportToWkb(&cp, wkb.data(), &opt_xdr));

  // Import preamble and body
  OGRCurveCollection imported_col;
  OGRCurvePolygon imported_cp;
  size_t nSize = wkb.size();
  size_t nDataOffset = 0;
  OGRwkbByteOrder eByteOrder = wkbXDR;
  EXPECT_EQ(OGRERR_NONE, imported_col.importPreambleFromWkb(
                             &imported_cp, wkb.data(), nSize, nDataOffset,
                             eByteOrder, 9, wkbVariantIso));

  size_t nBytesConsumed = 0;
  auto add_curve_fn = [](OGRGeometry* poGeom, OGRCurve* poCurve) -> OGRErr {
    return poGeom->toCurvePolygon()->addRingDirectly(poCurve);
  };
  EXPECT_EQ(OGRERR_NONE,
            imported_col.importBodyFromWkb(
                &imported_cp, wkb.data() + nDataOffset, nSize - nDataOffset,
                true, add_curve_fn, wkbVariantIso, nBytesConsumed));
  EXPECT_THAT(imported_cp.getExteriorRingCurve(), NotNull());

  // Error cases in importBodyFromWkb: truncated data
  OGRCurveCollection trunc_col;
  size_t small_size = 5;
  size_t nDataOffset2 = 0;
  OGRwkbByteOrder eByteOrder2 = wkbXDR;
  EXPECT_EQ(OGRERR_NONE, trunc_col.importPreambleFromWkb(
                             &imported_cp, wkb.data(), nSize, nDataOffset2,
                             eByteOrder2, 9, wkbVariantIso));
  EXPECT_EQ(OGRERR_NOT_ENOUGH_DATA,
            trunc_col.importBodyFromWkb(&imported_cp, wkb.data() + nDataOffset2,
                                        small_size, true, add_curve_fn,
                                        wkbVariantIso, nBytesConsumed));

  // Unsupported geometry type (e.g. Point inside collection body)
  OGRPoint pt(1, 2);
  std::vector<unsigned char> pt_wkb(pt.WkbSize());
  pt.exportToWkb(pt_wkb.data());

  std::vector<unsigned char> bad_wkb;
  bad_wkb.push_back(wkbNDR);
  uint32_t bad_type = wkbCurvePolygon;
  bad_wkb.insert(bad_wkb.end(), reinterpret_cast<unsigned char*>(&bad_type),
                 reinterpret_cast<unsigned char*>(&bad_type) + 4);
  uint32_t bad_count = 1;
  bad_wkb.insert(bad_wkb.end(), reinterpret_cast<unsigned char*>(&bad_count),
                 reinterpret_cast<unsigned char*>(&bad_count) + 4);
  bad_wkb.insert(bad_wkb.end(), pt_wkb.begin(), pt_wkb.end());

  OGRCurvePolygon bad_cp;
  OGRCurveCollection bad_col;
  size_t bad_size = bad_wkb.size();
  size_t bad_offset = 0;
  OGRwkbByteOrder bad_order = wkbNDR;
  EXPECT_EQ(OGRERR_NONE, bad_col.importPreambleFromWkb(
                             &bad_cp, bad_wkb.data(), bad_size, bad_offset,
                             bad_order, 9, wkbVariantIso));
  EXPECT_EQ(
      OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
      bad_col.importBodyFromWkb(&bad_cp, bad_wkb.data() + bad_offset,
                                bad_wkb.size() - bad_offset, false,
                                add_curve_fn, wkbVariantIso, nBytesConsumed));
}

TEST(OgrCurveCollectionTest, DimensionsAndSpatialReference) {
  OGRCurveCollection col;
  OGRCompoundCurve cc;

  auto ls = CreateSampleLine(0.0);
  col.addCurveDirectly(&cc, ls.release(), TRUE);

  EXPECT_TRUE(col.setCoordinateDimension(&cc, 3));
  EXPECT_TRUE(col.set3D(&cc, TRUE));
  EXPECT_TRUE(col.setMeasured(&cc, TRUE));

  auto* srs = new OGRSpatialReference();
  srs->SetWellKnownGeogCS("WGS84");
  col.assignSpatialReference(&cc, srs);
  srs->Release();
  OSRCleanup();

  col.flattenTo2D(&cc);
  EXPECT_FALSE(col.getCurve(0)->Is3D());

  col.swapXY();
  EXPECT_DOUBLE_EQ(10.0, col.getCurve(0)->toLineString()->getY(1));

  EXPECT_TRUE(col.segmentize(2.0));
  EXPECT_GT(col.getCurve(0)->toLineString()->getNumPoints(), 3);

  EXPECT_FALSE(col.hasCurveGeometry(FALSE));
  EXPECT_FALSE(col.hasCurveGeometry(TRUE));

  // Add circular string to test hasCurveGeometry == TRUE
  OGRCircularString cs;
  cs.addPoint(0, 0);
  cs.addPoint(1, 1);
  cs.addPoint(2, 0);
  col.addCurveDirectly(&cc, cs.clone(), TRUE);
  EXPECT_TRUE(col.hasCurveGeometry(FALSE));
}

TEST(OgrCurveCollectionTest, EmptyPartsAndReversePoints) {
  OGRCurveCollection col;
  OGRCompoundCurve cc;

  col.addCurveDirectly(&cc, CreateSampleLine(0.0).release(), TRUE);
  col.addCurveDirectly(&cc, std::make_unique<OGRLineString>().release(), TRUE);
  col.addCurveDirectly(&cc, CreateSampleLine(20.0).release(), TRUE);

  EXPECT_TRUE(col.hasEmptyParts());
  col.removeEmptyParts();
  EXPECT_FALSE(col.hasEmptyParts());
  EXPECT_EQ(2, col.getNumCurves());

  // Add 3 curves to test reversePoints
  col.addCurveDirectly(&cc, CreateSampleLine(40.0).release(), TRUE);
  EXPECT_EQ(3, col.getNumCurves());
  col.reversePoints();
  EXPECT_EQ(3, col.getNumCurves());
}

TEST(OgrCurveCollectionTest, TransformSuccessAndFailures) {
  WithQuietHandler quiet_handler;
  OGRCurveCollection col;
  OGRCompoundCurve cc;

  col.addCurveDirectly(&cc, CreateSampleLine(0.0).release(), TRUE);
  col.addCurveDirectly(&cc, CreateSampleLine(20.0).release(), TRUE);

  // Success
  TestCT success_ct(-1);
  EXPECT_EQ(OGRERR_NONE, col.transform(&cc, &success_ct));

  // Failure on first geometry (iGeom == 0)
  TestCT fail_first_ct(0);
  EXPECT_EQ(OGRERR_FAILURE, col.transform(&cc, &fail_first_ct));

  // Failure on second geometry (iGeom == 1) - tests lines 661-668 rollback
  TestCT fail_second_ct(1);
  EXPECT_EQ(OGRERR_FAILURE, col.transform(&cc, &fail_second_ct));
}

}  // namespace
}  // namespace autotest2
