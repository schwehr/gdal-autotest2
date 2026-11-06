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
//
// Tests for OGRSQLiteLayer base class (ogrsqlitelayer.cpp).
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitelayer.cpp
//
// google3 build caveats:
//   - Compiled without HAVE_SPATIALITE; raw SpatiaLite binary blobs assume a
//     little-endian host (enforced via static_assert below).

#include <bit>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitebase.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqliteutility.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

static_assert(std::endian::native == std::endian::little);

struct ExposedSQLiteLayer : public OGRSQLiteLayer {
  using OGRSQLiteLayer::FormatSpatialFilterFromMBR;
  using OGRSQLiteLayer::FormatSpatialFilterFromRTree;
  ExposedSQLiteLayer() : OGRSQLiteLayer(nullptr) {}
  OGRErr ResetStatement() override { return OGRERR_NONE; }
};

constexpr const char* kTestDir = "/vsimem/sqlite_layer_test";

class OGRSQLiteLayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterOGRSQLite();
    VSIRmdirRecursive(kTestDir);
    VSIMkdir(kTestDir, 0777);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  static void TearDownTestSuite() { OSRCleanup(); }
};

TEST_F(OGRSQLiteLayerTest, SpatiaLiteGeometryRoundTripAllTypesAndModes) {
  const std::vector<std::string> wkt_list = {
      // 1. Point
      "POINT (1 2)",
      "POINT Z (1 2 3)",
      "POINT M (1 2 4)",
      "POINT ZM (1 2 3 4)",
      // 2. LineString
      "LINESTRING (0 0, 1 1, 2 0, 3 3)",
      "LINESTRING Z (0 0 1, 1 1 2, 2 0 3, 3 3 4)",
      "LINESTRING M (0 0 5, 1 1 6, 2 0 7, 3 3 8)",
      "LINESTRING ZM (0 0 1 5, 1 1 2 6, 2 0 3 7, 3 3 4 8)",
      // 3. Polygon (exterior ring + interior ring)
      "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))",
      "POLYGON Z ((0 0 1, 4 0 1, 4 4 1, 0 4 1, 0 0 1), (1 1 2, 2 1 2, 2 2 2, 1 "
      "2 2, 1 1 2))",
      "POLYGON M ((0 0 5, 4 0 5, 4 4 5, 0 4 5, 0 0 5), (1 1 6, 2 1 6, 2 2 6, 1 "
      "2 6, 1 1 6))",
      "POLYGON ZM ((0 0 1 5, 4 0 1 5, 4 4 1 5, 0 4 1 5, 0 0 1 5), (1 1 2 6, 2 "
      "1 2 6, 2 2 2 6, 1 2 2 6, 1 1 2 6))",
      // 4. MultiPoint
      "MULTIPOINT ((1 2), (3 4))",
      "MULTIPOINT Z ((1 2 3), (3 4 5))",
      "MULTIPOINT M ((1 2 6), (3 4 7))",
      "MULTIPOINT ZM ((1 2 3 6), (3 4 5 7))",
      // 5. MultiLineString
      "MULTILINESTRING ((0 0, 1 1, 2 2), (3 3, 4 4, 5 5))",
      "MULTILINESTRING Z ((0 0 1, 1 1 2, 2 2 3), (3 3 4, 4 4 5, 5 5 6))",
      "MULTILINESTRING M ((0 0 7, 1 1 8, 2 2 9), (3 3 10, 4 4 11, 5 5 12))",
      "MULTILINESTRING ZM ((0 0 1 7, 1 1 2 8, 2 2 3 9), (3 3 4 10, 4 4 5 11, 5 "
      "5 6 12))",
      // 6. MultiPolygon
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1)), "
      "((10 10, 14 10, 14 14, 10 14, 10 10)))",
      "MULTIPOLYGON Z (((0 0 1, 4 0 1, 4 4 1, 0 4 1, 0 0 1), (1 1 2, 2 1 2, 2 "
      "2 2, 1 2 2, 1 1 2)), ((10 10 3, 14 10 3, 14 14 3, 10 14 3, 10 10 3)))",
      "MULTIPOLYGON M (((0 0 4, 4 0 4, 4 4 4, 0 4 4, 0 0 4), (1 1 5, 2 1 5, 2 "
      "2 5, 1 2 5, 1 1 5)), ((10 10 6, 14 10 6, 14 14 6, 10 14 6, 10 10 6)))",
      "MULTIPOLYGON ZM (((0 0 1 4, 4 0 1 4, 4 4 1 4, 0 4 1 4, 0 0 1 4), (1 1 2 "
      "5, 2 1 2 5, 2 2 2 5, 1 2 2 5, 1 1 2 5)), ((10 10 3 6, 14 10 3 6, 14 14 "
      "3 6, 10 14 3 6, 10 10 3 6)))",
      // 7. GeometryCollection
      "GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (0 0, 1 1, 2 2), POLYGON "
      "((0 0, 3 0, 3 3, 0 3, 0 0)))",
      "GEOMETRYCOLLECTION Z (POINT Z (1 2 3), LINESTRING Z (0 0 1, 1 1 2, 2 2 "
      "3), POLYGON Z ((0 0 1, 3 0 1, 3 3 1, 0 3 1, 0 0 1)))",
      "GEOMETRYCOLLECTION M (POINT M (1 2 4), LINESTRING M (0 0 5, 1 1 6, 2 2 "
      "7), POLYGON M ((0 0 8, 3 0 8, 3 3 8, 0 3 8, 0 0 8)))",
      "GEOMETRYCOLLECTION ZM (POINT ZM (1 2 3 4), LINESTRING ZM (0 0 1 5, 1 1 "
      "2 6, 2 2 3 7), POLYGON ZM ((0 0 1 8, 3 0 1 8, 3 3 1 8, 0 3 1 8, 0 0 1 "
      "8)))",
  };

  for (const auto& wkt : wkt_list) {
    auto [poGeom, err] = OGRGeometryFactory::createFromWkt(wkt.c_str());
    ASSERT_EQ(OGRERR_NONE, err) << "Failed to parse: " << wkt;
    ASSERT_NE(nullptr, poGeom);

    for (bool bSpatialite2D : {false, true}) {
      if (bSpatialite2D && poGeom->IsMeasured() &&
          wkbFlatten(poGeom->getGeometryType()) != wkbPoint) {
        continue;
      }
      for (bool bUseComprGeom : {false, true}) {
        for (OGRwkbByteOrder eByteOrder : {wkbNDR, wkbXDR}) {
          GByte* pabyData = nullptr;
          int nDataLen = 0;
          OGRErr expErr = OGRSQLiteExportSpatiaLiteGeometry(
              poGeom.get(), 4326, eByteOrder, bSpatialite2D, bUseComprGeom,
              &pabyData, &nDataLen);
          EXPECT_EQ(OGRERR_NONE, expErr) << "Export failed for " << wkt;
          if (expErr != OGRERR_NONE || !pabyData) continue;
          absl::Cleanup freeData = [&pabyData] { CPLFree(pabyData); };

          EXPECT_GT(nDataLen, 44);

          // Header inspection
          int nSRID = 0;
          OGRwkbGeometryType eType = wkbUnknown;
          bool bIsEmpty = true;
          double dfMinX = 0, dfMinY = 0, dfMaxX = 0, dfMaxY = 0;
          EXPECT_EQ(OGRERR_NONE,
                    OGRSQLiteGetSpatialiteGeometryHeader(
                        pabyData, nDataLen, &nSRID, &eType, &bIsEmpty, &dfMinX,
                        &dfMinY, &dfMaxX, &dfMaxY));
          EXPECT_EQ(4326, nSRID);
          EXPECT_FALSE(bIsEmpty);

          // 4-argument Import
          OGRGeometry* poImported = nullptr;
          int nImportSRID = 0;
          EXPECT_EQ(OGRERR_NONE,
                    OGRSQLiteImportSpatiaLiteGeometry(
                        pabyData, nDataLen, &poImported, &nImportSRID))
              << "Import failed for " << wkt << " 2D=" << bSpatialite2D
              << " compr=" << bUseComprGeom
              << " XDR=" << (eByteOrder == wkbXDR);
          EXPECT_NE(nullptr, poImported);
          delete poImported;

          // 3-argument Import (from OGRSQLiteLayer directly)
          OGRGeometry* poImported2 = nullptr;
          EXPECT_EQ(OGRERR_NONE, OGRSQLiteLayer::ImportSpatiaLiteGeometry(
                                     pabyData, nDataLen, &poImported2));
          EXPECT_NE(nullptr, poImported2);
          delete poImported2;

          // Truncation & count checks (on uncompressed and compressed NDR)
          if (!bSpatialite2D && eByteOrder == wkbNDR) {
            for (int truncLen = 44; truncLen < nDataLen; ++truncLen) {
              std::vector<GByte> buf(pabyData, pabyData + truncLen);
              buf[truncLen - 1] = 0xFE;  // fake end marker
              OGRGeometry* poTrunc = nullptr;
              CPLPushErrorHandler(CPLQuietErrorHandler);
              OGRSQLiteLayer::ImportSpatiaLiteGeometry(buf.data(), truncLen,
                                                       &poTrunc);
              CPLPopErrorHandler();
              delete poTrunc;
            }

            // Test bad counts at offset 43 and 47
            for (int offset : {43, 47}) {
              if (offset + 4 <= nDataLen) {
                for (int badCount : {-1, (INT_MAX / 16) + 100}) {
                  std::vector<GByte> bufCorrupt(pabyData, pabyData + nDataLen);
                  memcpy(bufCorrupt.data() + offset, &badCount, 4);
                  OGRGeometry* poCorrupt = nullptr;
                  CPLPushErrorHandler(CPLQuietErrorHandler);
                  OGRSQLiteLayer::ImportSpatiaLiteGeometry(
                      bufCorrupt.data(), nDataLen, &poCorrupt);
                  CPLPopErrorHandler();
                  delete poCorrupt;
                }
              }
            }
          }
        }
      }
    }
  }

  // Empty geometries
  for (const char* pszEmptyWkt : {
           "LINESTRING EMPTY",
           "POLYGON EMPTY",
           "MULTIPOINT EMPTY",
           "MULTILINESTRING EMPTY",
           "MULTIPOLYGON EMPTY",
           "GEOMETRYCOLLECTION EMPTY",
       }) {
    auto [poEmpty, eErr] = OGRGeometryFactory::createFromWkt(pszEmptyWkt);
    ASSERT_EQ(OGRERR_NONE, eErr);
    GByte* pabyData = nullptr;
    int nDataLen = 0;
    EXPECT_EQ(OGRERR_NONE, OGRSQLiteExportSpatiaLiteGeometry(
                               poEmpty.get(), 4326, wkbNDR, false, false,
                               &pabyData, &nDataLen));
    ASSERT_NE(nullptr, pabyData);
    bool bIsEmpty = false;
    EXPECT_EQ(OGRERR_NONE, OGRSQLiteGetSpatialiteGeometryHeader(
                               pabyData, nDataLen, nullptr, nullptr, &bIsEmpty,
                               nullptr, nullptr, nullptr, nullptr));
    EXPECT_TRUE(bIsEmpty);
    CPLFree(pabyData);
  }

  // 1-point LineString (cannot be compressed)
  OGRLineString oLS1;
  oLS1.addPoint(1.0, 2.0);
  GByte* pabyLS1 = nullptr;
  int nLS1Len = 0;
  EXPECT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&oLS1, 0, wkbNDR, false, true,
                                              &pabyLS1, &nLS1Len));
  CPLFree(pabyLS1);

  // Polygon with 1-point ring (cannot be compressed)
  OGRPolygon oPolyBad;
  OGRLinearRing oRingBad;
  oRingBad.addPoint(1.0, 1.0);
  oPolyBad.addRing(&oRingBad);
  GByte* pabyPolyBad = nullptr;
  int nPolyBadLen = 0;
  EXPECT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&oPolyBad, 0, wkbNDR, false, true,
                                              &pabyPolyBad, &nPolyBadLen));
  CPLFree(pabyPolyBad);

  // Nested GeometryCollection (exercises recursive collectSimpleGeometries)
  auto poNestedGC = std::make_unique<OGRGeometryCollection>();
  auto poInnerGC = std::make_unique<OGRGeometryCollection>();
  poInnerGC->addGeometryDirectly(new OGRPoint(1.0, 2.0));
  poNestedGC->addGeometryDirectly(poInnerGC.release());
  GByte* pabyNested = nullptr;
  int nNestedLen = 0;
  EXPECT_EQ(OGRERR_NONE, OGRSQLiteExportSpatiaLiteGeometry(
                             poNestedGC.get(), 0, wkbNDR, false, false,
                             &pabyNested, &nNestedLen));
  ASSERT_NE(nullptr, pabyNested);
  OGRGeometry* poImportedNested = nullptr;
  EXPECT_EQ(OGRERR_NONE,
            OGRSQLiteImportSpatiaLiteGeometry(pabyNested, nNestedLen,
                                              &poImportedNested, nullptr));
  delete poImportedNested;
  CPLFree(pabyNested);

  // Curve geometry (CIRCULARSTRING) exercises hasCurveGeometry in
  // ExportSpatiaLiteGeometry
  OGRCircularString oCS;
  oCS.addPoint(0.0, 0.0);
  oCS.addPoint(1.0, 1.0);
  oCS.addPoint(2.0, 0.0);
  GByte* pabyCS = nullptr;
  int nCSLen = 0;
  EXPECT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&oCS, 4326, wkbNDR, false, false,
                                              &pabyCS, &nCSLen));
  ASSERT_NE(nullptr, pabyCS);
  OGRGeometry* poImportedCS = nullptr;
  EXPECT_EQ(OGRERR_NONE, OGRSQLiteImportSpatiaLiteGeometry(
                             pabyCS, nCSLen, &poImportedCS, nullptr));
  delete poImportedCS;
  CPLFree(pabyCS);

  // Unsupported geometry type (Triangle / PolyhedralSurface)
  OGRTriangle oTriangle;
  GByte* pabyTri = nullptr;
  int nTriLen = 0;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&oTriangle, 0, wkbNDR, false,
                                              false, &pabyTri, &nTriLen));
  CPLPopErrorHandler();
}

TEST_F(OGRSQLiteLayerTest, SpatiaLiteGeometrySpecialAndCorruptCases) {
  // 1. Compressed top-level type codes (1000005..1003007) for MultiLineString,
  // MultiPolygon, GeometryCollection
  const char* apszMultiWkt[] = {
      "MULTILINESTRING ((0 0, 1 1, 2 2), (3 3, 4 4, 5 5))",
      "MULTILINESTRING Z ((0 0 1, 1 1 2, 2 2 3), (3 3 4, 4 4 5, 5 5 6))",
      "MULTILINESTRING M ((0 0 7, 1 1 8, 2 2 9), (3 3 10, 4 4 11, 5 5 12))",
      "MULTILINESTRING ZM ((0 0 1 7, 1 1 2 8, 2 2 3 9), (3 3 4 10, 4 4 5 11, 5 "
      "5 6 12))",
      "MULTIPOLYGON (((0 0, 4 0, 4 4, 0 4, 0 0)), ((10 10, 14 10, 14 14, 10 "
      "14, 10 10)))",
      "MULTIPOLYGON Z (((0 0 1, 4 0 1, 4 4 1, 0 4 1, 0 0 1)), ((10 10 3, 14 10 "
      "3, 14 14 3, 10 14 3, 10 10 3)))",
      "MULTIPOLYGON M (((0 0 4, 4 0 4, 4 4 4, 0 4 4, 0 0 4)), ((10 10 6, 14 10 "
      "6, 14 14 6, 10 14 6, 10 10 6)))",
      "MULTIPOLYGON ZM (((0 0 1 4, 4 0 1 4, 4 4 1 4, 0 4 1 4, 0 0 1 4)), ((10 "
      "10 3 6, 14 10 3 6, 14 14 3 6, 10 14 3 6, 10 10 3 6)))",
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 1, 2 2))",
      "GEOMETRYCOLLECTION Z (LINESTRING Z (0 0 1, 1 1 2, 2 2 3))",
      "GEOMETRYCOLLECTION M (LINESTRING M (0 0 7, 1 1 8, 2 2 9))",
      "GEOMETRYCOLLECTION ZM (LINESTRING ZM (0 0 1 7, 1 1 2 8, 2 2 3 9))",
  };

  for (const char* pszWkt : apszMultiWkt) {
    auto [poGeom, err] = OGRGeometryFactory::createFromWkt(pszWkt);
    ASSERT_EQ(OGRERR_NONE, err);

    for (OGRwkbByteOrder eOrder : {wkbNDR, wkbXDR}) {
      GByte* pabyData = nullptr;
      int nDataLen = 0;
      ASSERT_EQ(OGRERR_NONE, OGRSQLiteExportSpatiaLiteGeometry(
                                 poGeom.get(), 4326, eOrder, false, true,
                                 &pabyData, &nDataLen));
      ASSERT_NE(nullptr, pabyData);
      absl::Cleanup free_data = [&pabyData] { CPLFree(pabyData); };

      // Overwrite type code at offset 39 with compressed code (code + 1000000)
      int nCode = 0;
      memcpy(&nCode, pabyData + 39, 4);
      if (eOrder == wkbXDR) CPL_SWAP32PTR(&nCode);
      nCode += 1000000;
      if (eOrder == wkbXDR) CPL_SWAP32PTR(&nCode);
      memcpy(pabyData + 39, &nCode, 4);

      // Verify header and import
      OGRwkbGeometryType eType = wkbUnknown;
      EXPECT_EQ(OGRERR_NONE, OGRSQLiteGetSpatialiteGeometryHeader(
                                 pabyData, nDataLen, nullptr, &eType, nullptr,
                                 nullptr, nullptr, nullptr, nullptr));
      EXPECT_NE(wkbUnknown, eType);

      OGRGeometry* poImp = nullptr;
      EXPECT_EQ(OGRERR_NONE, OGRSQLiteImportSpatiaLiteGeometry(
                                 pabyData, nDataLen, &poImp, nullptr));
      delete poImp;
    }
  }

  // 2. Appended original curve WKB after 0xFE
  OGRPoint oPt(10.0, 20.0);
  GByte* pabyPt = nullptr;
  int nPtLen = 0;
  ASSERT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&oPt, 4326, wkbNDR, false, false,
                                              &pabyPt, &nPtLen));
  ASSERT_NE(nullptr, pabyPt);
  absl::Cleanup free_pt = [&pabyPt] { CPLFree(pabyPt); };

  OGRCircularString oCurve;
  oCurve.addPoint(0.0, 0.0);
  oCurve.addPoint(1.0, 1.0);
  oCurve.addPoint(2.0, 0.0);
  const int nWkbSize = oCurve.WkbSize();
  std::vector<GByte> curveWkb(nWkbSize);
  OGRwkbExportOptions sOptions;
  sOptions.eByteOrder = wkbNDR;
  ASSERT_EQ(OGRERR_NONE, oCurve.exportToWkb(curveWkb.data(), &sOptions));

  // Construct [SpatiaLite blob] + [Curve WKB] + [0xFE]
  std::vector<GByte> combined(nPtLen + nWkbSize + 1);
  memcpy(combined.data(), pabyPt, nPtLen);
  memcpy(combined.data() + nPtLen, curveWkb.data(), nWkbSize);
  combined[combined.size() - 1] = 0xFE;

  OGRGeometry* poRecoveredCurve = nullptr;
  EXPECT_EQ(OGRERR_NONE, OGRSQLiteImportSpatiaLiteGeometry(
                             combined.data(), static_cast<int>(combined.size()),
                             &poRecoveredCurve, nullptr));
  ASSERT_NE(nullptr, poRecoveredCurve);
  EXPECT_EQ(wkbCircularString, wkbFlatten(poRecoveredCurve->getGeometryType()));
  delete poRecoveredCurve;

  // With invalid WKB appended after 0xFE
  std::vector<GByte> corruptAppended(nPtLen + 5 + 1);
  memcpy(corruptAppended.data(), pabyPt, nPtLen);
  memset(corruptAppended.data() + nPtLen, 0xFF, 5);
  corruptAppended[corruptAppended.size() - 1] = 0xFE;
  OGRGeometry* poBadAppended = nullptr;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(OGRERR_NONE, OGRSQLiteImportSpatiaLiteGeometry(
                             corruptAppended.data(),
                             static_cast<int>(corruptAppended.size()),
                             &poBadAppended, nullptr));
  CPLPopErrorHandler();
  delete poBadAppended;

  // 3. Corrupt entity marker (0x00 instead of 0x69) in MultiPoint
  auto [poMP, mpErr] =
      OGRGeometryFactory::createFromWkt("MULTIPOINT ((1 2), (3 4))");
  ASSERT_EQ(OGRERR_NONE, mpErr);
  GByte* pabyMP = nullptr;
  int nMPLen = 0;
  ASSERT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(poMP.get(), 4326, wkbNDR, false,
                                              false, &pabyMP, &nMPLen));
  ASSERT_NE(nullptr, pabyMP);
  absl::Cleanup free_mp = [&pabyMP] { CPLFree(pabyMP); };
  // Offset 47 is the first subgeometry marker 0x69
  EXPECT_EQ(0x69, pabyMP[47]);
  pabyMP[47] = 0x00;
  OGRGeometry* poCorruptMP = nullptr;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRSQLiteImportSpatiaLiteGeometry(
                                     pabyMP, nMPLen, &poCorruptMP, nullptr));
  CPLPopErrorHandler();
  delete poCorruptMP;

  // 4. Incompatible sub-geometry in MultiPoint (MultiLineString blob with type
  // changed to MultiPoint)
  auto [poMLS, mlsErr] =
      OGRGeometryFactory::createFromWkt("MULTILINESTRING ((0 0, 1 1, 2 2))");
  ASSERT_EQ(OGRERR_NONE, mlsErr);
  GByte* pabyMLS = nullptr;
  int nMLSLen = 0;
  ASSERT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(poMLS.get(), 4326, wkbNDR, false,
                                              false, &pabyMLS, &nMLSLen));
  ASSERT_NE(nullptr, pabyMLS);
  absl::Cleanup free_mls = [&pabyMLS] { CPLFree(pabyMLS); };
  int nMultiPointCode = OGRSpliteMultiPointXY;
  memcpy(pabyMLS + 39, &nMultiPointCode, 4);
  OGRGeometry* poIncompatible = nullptr;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_NE(OGRERR_NONE, OGRSQLiteImportSpatiaLiteGeometry(
                             pabyMLS, nMLSLen, &poIncompatible, nullptr));
  CPLPopErrorHandler();
  delete poIncompatible;

  // 5. 33 levels of nested GeometryCollections hitting nRecLevel == 32
  // Structure: header (44 bytes), then 33 nested layers of [code (4B), nParts=1
  // (4B), 0x69 (1B)], then a Point [code (4B), x (8B), y (8B)], and end marker
  // 0xFE (1B).
  std::vector<GByte> recBuf(44 + 33 * 9 + 20 + 1, 0);
  recBuf[0] = 0x00;
  recBuf[1] = wkbNDR;
  recBuf[38] = 0x7C;
  int nOuterCode = OGRSpliteGeometryCollectionXY;
  memcpy(recBuf.data() + 39, &nOuterCode, 4);
  int offset = 43;
  for (int level = 0; level < 33; ++level) {
    int nParts = 1;
    memcpy(recBuf.data() + offset, &nParts, 4);
    recBuf[offset + 4] = 0x69;
    memcpy(recBuf.data() + offset + 5, &nOuterCode, 4);
    offset += 9;
  }
  int nPtCode = OGRSplitePointXY;
  memcpy(recBuf.data() + offset, &nPtCode, 4);
  offset += 20;
  recBuf[offset] = 0xFE;
  recBuf.resize(offset + 1);

  OGRGeometry* poRecGeom = nullptr;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(
      OGRERR_CORRUPT_DATA,
      OGRSQLiteImportSpatiaLiteGeometry(
          recBuf.data(), static_cast<int>(recBuf.size()), &poRecGeom, nullptr));
  CPLPopErrorHandler();
  delete poRecGeom;

  // 6. Invalid header checks
  GByte shortBuf[43] = {0};
  OGRGeometry* poShort = nullptr;
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            OGRSQLiteImportSpatiaLiteGeometry(shortBuf, 43, &poShort, nullptr));

  // Invalid start byte
  std::vector<GByte> badStart(44, 0);
  badStart[0] = 0x01;
  badStart[1] = wkbNDR;
  badStart[38] = 0x7C;
  badStart[43] = 0xFE;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRSQLiteImportSpatiaLiteGeometry(
                                     badStart.data(), 44, &poShort, nullptr));

  // Invalid byte order
  std::vector<GByte> badOrder(44, 0);
  badOrder[1] = 0x05;
  badOrder[38] = 0x7C;
  badOrder[43] = 0xFE;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRSQLiteImportSpatiaLiteGeometry(
                                     badOrder.data(), 44, &poShort, nullptr));

  // Missing 0x7C
  std::vector<GByte> bad7C(44, 0);
  bad7C[1] = wkbNDR;
  bad7C[38] = 0x00;
  bad7C[43] = 0xFE;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRSQLiteImportSpatiaLiteGeometry(
                                     bad7C.data(), 44, &poShort, nullptr));

  // Missing 0xFE
  std::vector<GByte> badFE(44, 0);
  badFE[1] = wkbNDR;
  badFE[38] = 0x7C;
  badFE[43] = 0x00;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, OGRSQLiteImportSpatiaLiteGeometry(
                                     badFE.data(), 44, &poShort, nullptr));

  // Unknown geometry type code
  std::vector<GByte> unkType(44, 0);
  unkType[1] = wkbNDR;
  unkType[38] = 0x7C;
  unkType[43] = 0xFE;
  int unkCode = 999999;
  memcpy(unkType.data() + 39, &unkCode, 4);
  EXPECT_EQ(
      OGRERR_UNSUPPORTED_GEOMETRY_TYPE,
      OGRSQLiteImportSpatiaLiteGeometry(unkType.data(), 44, &poShort, nullptr));
}

TEST_F(OGRSQLiteLayerTest, BuildFeatureDefnAndGetNextRawFeatureTypes) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, poDriver);

  std::string db_path = std::string(kTestDir) + "/types.db";
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(db_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poDS);
  sqlite3* hDB =
      static_cast<sqlite3*>(poDS->GetInternalHandle("SQLITE_HANDLE"));
  ASSERT_NE(nullptr, hDB);

  // Hex encoded "my_dom" is 6D795F646F6D
  const char* pszCreateSQL =
      "CREATE TABLE raw_types ("
      "PKUID INTEGER PRIMARY KEY, "
      "OGC_FID INTEGER, "
      "OGR_NATIVE_DATA VARCHAR, "
      "OGR_NATIVE_MEDIA_TYPE VARCHAR, "
      "col_domain VARCHAR_BEGIN_DOMAIN_NAME_6D795F646F6D_END_DOMAIN_NAME, "
      "col_bool INTEGER_BOOLEAN, "
      "col_bool2 BOOLEAN, "
      "col_int16 INTEGER_INT16, "
      "col_int_or_text INTEGER_OR_TEXT, "
      "col_intlist JSONINTEGERLIST, "
      "col_int64list JSONINTEGER64LIST, "
      "col_reallist JSONREALLIST, "
      "col_strlist JSONSTRINGLIST, "
      "col_bigint BIGINT, "
      "col_int8 INT8, "
      "col_ubigint \"UNSIGNED BIG INT\", "
      "col_int INTEGER, "
      "col_tiny TINYINT, "
      "col_small SMALLINT, "
      "col_medium MEDIUMINT, "
      "col_int2 INT2, "
      "col_flt32 FLOAT_FLOAT32, "
      "col_flt FLOAT, "
      "col_dec DECIMAL, "
      "col_real REAL, "
      "col_dbl DOUBLE, "
      "col_dblprec \"DOUBLE PRECISION\", "
      "col_num NUMERIC, "
      "col_blob BLOB, "
      "col_blob_pt BLOB_POINT_XY_4326, "
      "col_blob_ptz BLOB_POINT_XYZ_4326, "
      "col_blob_ptm BLOB_POINT_XYM_4326, "
      "col_blob_ptzm BLOB_POINT_XYZM_4326, "
      "col_text TEXT, "
      "col_varchar VARCHAR, "
      "col_char CHARACTER, "
      "col_varchar2 \"VARYING CHARACTER\", "
      "col_nchar NCHAR, "
      "col_nat_char \"NATIVE CHARACTER\", "
      "col_nvarchar NVARCHAR, "
      "col_clob CLOB, "
      "col_deflate VARCHAR_deflate, "
      "col_ts TIMESTAMP, "
      "col_dt DATETIME, "
      "col_date DATE, "
      "col_time TIME"
      ");";

  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hDB, pszCreateSQL, nullptr, nullptr, nullptr));

  // Prepare a SpatiaLite Point blob
  OGRPoint pt(1.0, 2.0);
  GByte* pabyPt = nullptr;
  int nPtLen = 0;
  ASSERT_EQ(OGRERR_NONE,
            OGRSQLiteExportSpatiaLiteGeometry(&pt, 4326, wkbNDR, false, false,
                                              &pabyPt, &nPtLen));
  absl::Cleanup free_pt = [&pabyPt] { CPLFree(pabyPt); };

  // Deflate compressed text
  const char* pszDeflatedText = "Deflated string content for zlib test";
  size_t nOutBytes = 0;
  void* pDeflated = CPLZLibDeflate(pszDeflatedText, strlen(pszDeflatedText) + 1,
                                   -1, nullptr, 0, &nOutBytes);
  ASSERT_NE(nullptr, pDeflated);
  absl::Cleanup free_deflated = [pDeflated] { CPLFree(pDeflated); };

  // Row 1: normal typed values
  {
    sqlite3_stmt* hStmt = nullptr;
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_prepare_v2(
            hDB,
            "INSERT INTO raw_types (PKUID, OGC_FID, OGR_NATIVE_DATA, "
            "OGR_NATIVE_MEDIA_TYPE, col_domain, col_bool, col_bool2, "
            "col_int16, "
            "col_int_or_text, col_intlist, col_int64list, col_reallist, "
            "col_strlist, "
            "col_bigint, col_int8, col_ubigint, col_int, col_tiny, col_small, "
            "col_medium, col_int2, col_flt32, col_flt, col_dec, col_real, "
            "col_dbl, "
            "col_dblprec, col_num, col_blob, col_blob_pt, col_blob_ptz, "
            "col_blob_ptm, "
            "col_blob_ptzm, col_text, col_varchar, col_char, col_varchar2, "
            "col_nchar, "
            "col_nat_char, col_nvarchar, col_clob, col_deflate, col_ts, "
            "col_dt, "
            "col_date, col_time) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, ?, "
            "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
            "?, "
            "?, ?, ?, ?, ?, ?, ?, ?, ?);",
            -1, &hStmt, nullptr));
    absl::Cleanup finalize_stmt1 = [hStmt] { sqlite3_finalize(hStmt); };

    int idx = 1;
    sqlite3_bind_int64(hStmt, idx++, 1);
    sqlite3_bind_int64(hStmt, idx++, 101);
    sqlite3_bind_text(hStmt, idx++, "{\"feature\":1}", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "application/json", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "dom_val", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(hStmt, idx++, 1);
    sqlite3_bind_int(hStmt, idx++, 0);
    sqlite3_bind_int(hStmt, idx++, 32000);
    sqlite3_bind_text(hStmt, idx++, "some_text", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "[1, 2, 3]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "[10000000000, 20000000000]", -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "[1.1, 2.2]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "[\"a\", \"b\"]", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(hStmt, idx++, 10000000000LL);
    sqlite3_bind_int(hStmt, idx++, 8);
    sqlite3_bind_int64(hStmt, idx++, 20000000000LL);
    sqlite3_bind_int(hStmt, idx++, 42);
    sqlite3_bind_int(hStmt, idx++, 1);
    sqlite3_bind_int(hStmt, idx++, 2);
    sqlite3_bind_int(hStmt, idx++, 3);
    sqlite3_bind_int(hStmt, idx++, 4);
    sqlite3_bind_double(hStmt, idx++, 1.5);
    sqlite3_bind_double(hStmt, idx++, 2.5);
    sqlite3_bind_double(hStmt, idx++, 3.5);
    sqlite3_bind_double(hStmt, idx++, 4.5);
    sqlite3_bind_double(hStmt, idx++, 5.5);
    sqlite3_bind_double(hStmt, idx++, 6.5);
    sqlite3_bind_double(hStmt, idx++, 7.5);
    sqlite3_bind_blob(hStmt, idx++, "\x01\x02\x03", 3, SQLITE_TRANSIENT);
    sqlite3_bind_blob(hStmt, idx++, pabyPt, nPtLen, SQLITE_TRANSIENT);
    sqlite3_bind_blob(hStmt, idx++, pabyPt, nPtLen, SQLITE_TRANSIENT);
    sqlite3_bind_blob(hStmt, idx++, pabyPt, nPtLen, SQLITE_TRANSIENT);
    sqlite3_bind_blob(hStmt, idx++, pabyPt, nPtLen, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "text", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "varchar", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "char", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "varchar2", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "nchar", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "nat_char", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "nvarchar", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "clob", -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(hStmt, idx++, pDeflated, static_cast<int>(nOutBytes),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "2026-11-03 12:34:56", -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "2026-11-03 12:34:56", -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "2026-11-03", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(hStmt, idx++, "12:34:56", -1, SQLITE_TRANSIENT);

    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
  }

  // Row 2: text in numeric columns, integer overflow in int32, uncompressed
  // text in deflate, Julian days
  {
    sqlite3_stmt* hStmt = nullptr;
    ASSERT_EQ(
        SQLITE_OK,
        sqlite3_prepare_v2(
            hDB,
            "INSERT INTO raw_types (PKUID, col_int, col_bigint, col_real, "
            "col_deflate, col_date, col_dt, col_time) VALUES "
            "(2, ?, ?, ?, 'uncompressed_text', 'bad_date', 2460000.5, "
            "2460000.5);",
            -1, &hStmt, nullptr));
    absl::Cleanup finalize_stmt2 = [hStmt] { sqlite3_finalize(hStmt); };
    // Test text in integer column
    sqlite3_bind_text(hStmt, 1, "999", -1, SQLITE_TRANSIENT);
    // Test text in bigint column
    sqlite3_bind_text(hStmt, 2, "888", -1, SQLITE_TRANSIENT);
    // Test text in real column
    sqlite3_bind_text(hStmt, 3, "777.7", -1, SQLITE_TRANSIENT);
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(hStmt));
  }

  // Row 3: NULLs
  ASSERT_EQ(SQLITE_OK,
            sqlite3_exec(hDB, "INSERT INTO raw_types (PKUID) VALUES (3);",
                         nullptr, nullptr, nullptr));

  // Row 4: int column holding 64-bit int (> INT_MAX)
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_exec(
          hDB,
          "INSERT INTO raw_types (PKUID, col_int) VALUES (4, 10000000000);",
          nullptr, nullptr, nullptr));

  // Close and reopen dataset with LIST_ALL_TABLES=YES to exercise
  // BuildFeatureDefn (bIsSelect = false)
  poDS.reset();
  char** papszOpenOpts = CSLAddString(nullptr, "LIST_ALL_TABLES=YES");
  poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(db_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                        nullptr, papszOpenOpts, nullptr));
  CSLDestroy(papszOpenOpts);
  ASSERT_NE(nullptr, poDS);

  OGRLayer* poLyr = poDS->GetLayerByName("raw_types");
  ASSERT_NE(nullptr, poLyr);
  EXPECT_STREQ("PKUID", poLyr->GetFIDColumn());

  auto* poLayerCast = dynamic_cast<OGRSQLiteLayer*>(poLyr);
  ASSERT_NE(nullptr, poLayerCast);
  EXPECT_EQ(poDS.get(), poLayerCast->GetDataset());

  // Test capabilities (both table layer and base class)
  EXPECT_FALSE(poLayerCast->OGRSQLiteLayer::TestCapability(OLCRandomRead));
  EXPECT_FALSE(
      poLayerCast->OGRSQLiteLayer::TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(
      poLayerCast->OGRSQLiteLayer::TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(poLayerCast->OGRSQLiteLayer::TestCapability(OLCIgnoreFields));
  EXPECT_TRUE(poLayerCast->OGRSQLiteLayer::TestCapability(OLCTransactions));
  EXPECT_TRUE(poLayerCast->OGRSQLiteLayer::TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(poLayerCast->OGRSQLiteLayer::TestCapability("UNKNOWN_CAP"));

  // Transactions
  EXPECT_EQ(OGRERR_NONE, poLyr->StartTransaction());
  EXPECT_EQ(OGRERR_NONE, poLyr->CommitTransaction());
  EXPECT_EQ(OGRERR_NONE, poLyr->StartTransaction());
  EXPECT_EQ(OGRERR_NONE, poLyr->RollbackTransaction());

  // Read Row 1
  poLyr->ResetReading();
  OGRFeature* poF1 = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF1);
  EXPECT_EQ(1, poF1->GetFID());
  EXPECT_STREQ("dom_val", poF1->GetFieldAsString("col_domain"));
  EXPECT_EQ("my_dom", poF1->GetFieldDefnRef(poF1->GetFieldIndex("col_domain"))
                          ->GetDomainName());
  EXPECT_EQ(1, poF1->GetFieldAsInteger("col_bool"));
  EXPECT_EQ(0, poF1->GetFieldAsInteger("col_bool2"));
  EXPECT_EQ(32000, poF1->GetFieldAsInteger("col_int16"));
  EXPECT_STREQ("some_text", poF1->GetFieldAsString("col_int_or_text"));
  EXPECT_EQ(10000000000LL, poF1->GetFieldAsInteger64("col_bigint"));
  EXPECT_EQ(42, poF1->GetFieldAsInteger("col_int"));
  EXPECT_DOUBLE_EQ(4.5, poF1->GetFieldAsDouble("col_real"));
  EXPECT_STREQ("Deflated string content for zlib test",
               poF1->GetFieldAsString("col_deflate"));
  EXPECT_STREQ("{\"feature\":1}", poF1->GetNativeData());
  EXPECT_STREQ("application/json", poF1->GetNativeMediaType());
  EXPECT_GE(poF1->GetFieldCount(), 10);
  delete poF1;

  // Read Row 2 (text-in-numeric, uncompressed text in deflate fallback, invalid
  // date)
  OGRFeature* poF2 = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF2);
  EXPECT_EQ(2, poF2->GetFID());
  EXPECT_STREQ("999", poF2->GetFieldAsString("col_int"));
  EXPECT_STREQ("888", poF2->GetFieldAsString("col_bigint"));
  EXPECT_STREQ("777.7", poF2->GetFieldAsString("col_real"));
  EXPECT_STREQ("uncompressed_text", poF2->GetFieldAsString("col_deflate"));
  EXPECT_FALSE(poF2->IsFieldSetAndNotNull(
      poF2->GetFieldIndex("col_date")));  // 'bad_date' unsets
  delete poF2;

  // Read Row 3 (all NULLs)
  OGRFeature* poF3 = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF3);
  EXPECT_EQ(3, poF3->GetFID());
  EXPECT_FALSE(poF3->IsFieldSetAndNotNull(poF3->GetFieldIndex("col_int")));
  delete poF3;

  // Read Row 4 (int column holding > INT_MAX)
  OGRFeature* poF4 = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poF4);
  EXPECT_EQ(4, poF4->GetFID());
  EXPECT_EQ(INT_MAX, poF4->GetFieldAsInteger("col_int"));
  delete poF4;

  // EOF
  EXPECT_EQ(nullptr, poLyr->GetNextFeature());

  // GetFeature by ID
  OGRFeature* poFById = poLyr->GetFeature(1);
  ASSERT_NE(nullptr, poFById);
  EXPECT_EQ(1, poFById->GetFID());
  delete poFById;

  // SetIgnoredFields
  const char* apszIgnored[] = {"col_int", "col_real", "col_blob_pt", nullptr};
  EXPECT_EQ(OGRERR_NONE, poLyr->SetIgnoredFields(apszIgnored));
  poLyr->ResetReading();
  OGRFeature* poFIgnored = poLyr->GetNextFeature();
  ASSERT_NE(nullptr, poFIgnored);
  EXPECT_FALSE(
      poFIgnored->IsFieldSetAndNotNull(poFIgnored->GetFieldIndex("col_int")));
  delete poFIgnored;
  poLyr->SetIgnoredFields(nullptr);

  // ExecuteSQL query with untyped columns, OGR_PROMOTE_TO_INTEGER64, MIN/MAX
  // date inference
  CPLSetConfigOption("OGR_PROMOTE_TO_INTEGER64", "YES");
  OGRLayer* poUntypedLyr = poDS->ExecuteSQL(
      "SELECT 1 AS untyped_int, 10000000000 AS untyped_int64, 1.5 AS "
      "untyped_real, "
      "X'0102' AS untyped_blob, '2026-11-03' AS \"MIN(d)\", "
      "'2026/11/03 12:30' AS \"MAX(dt)\", '12:30' AS \"MIN(t)\" "
      "FROM raw_types LIMIT 1",
      nullptr, nullptr);
  CPLSetConfigOption("OGR_PROMOTE_TO_INTEGER64", nullptr);
  ASSERT_NE(nullptr, poUntypedLyr);
  OGRFeature* poUntypedF = poUntypedLyr->GetNextFeature();
  ASSERT_NE(nullptr, poUntypedF);
  EXPECT_EQ(OFTInteger64, poUntypedF->GetFieldDefnRef(0)->GetType());
  EXPECT_EQ(OFTReal, poUntypedF->GetFieldDefnRef(2)->GetType());
  EXPECT_EQ(OFTBinary, poUntypedF->GetFieldDefnRef(3)->GetType());
  EXPECT_EQ(OFTDate, poUntypedF->GetFieldDefnRef(4)->GetType());
  EXPECT_EQ(OFTDateTime, poUntypedF->GetFieldDefnRef(5)->GetType());
  EXPECT_EQ(OFTTime, poUntypedF->GetFieldDefnRef(6)->GetType());
  delete poUntypedF;
  poDS->ReleaseResultSet(poUntypedLyr);

  // ExecuteSQL query testing BLOB_POINT decltype parsing in
  // OGRSQLiteSelectLayer
  OGRLayer* poBlobPtLyr = poDS->ExecuteSQL(
      "SELECT col_blob_pt, col_blob_ptz, col_blob_ptm, col_blob_ptzm "
      "FROM raw_types LIMIT 1",
      nullptr, nullptr);
  ASSERT_NE(nullptr, poBlobPtLyr);
  EXPECT_EQ(4, poBlobPtLyr->GetLayerDefn()->GetGeomFieldCount());
  OGRFeature* poBlobPtF = poBlobPtLyr->GetNextFeature();
  ASSERT_NE(nullptr, poBlobPtF);
  EXPECT_NE(nullptr, poBlobPtF->GetGeomFieldRef(0));
  delete poBlobPtF;
  poDS->ReleaseResultSet(poBlobPtLyr);

  // ExecuteSQL query recognizing geometry columns: wkt_geometry, geometry,
  // GaiaGeometry, spatial functions
  OGRLayer* poGeomColsLyr = poDS->ExecuteSQL(
      "SELECT 'POINT (1 2)' AS wkt_geometry, "
      "X'0101000000000000000000F03F0000000000000040' AS geometry, "
      "X'"
      "0001E6100000000000000000F03F0000000000000040000000000000F03F000000000000"
      "00407C01000000000000000000F03F0000000000000040FE' AS GaiaGeometry, "
      "NULL AS \"ST_Buffer(geom, 1)\" "
      "FROM raw_types LIMIT 1",
      nullptr, nullptr);
  ASSERT_NE(nullptr, poGeomColsLyr);
  EXPECT_GE(poGeomColsLyr->GetLayerDefn()->GetGeomFieldCount(), 1);
  OGRFeature* poGeomF = poGeomColsLyr->GetNextFeature();
  ASSERT_NE(nullptr, poGeomF);
  EXPECT_NE(nullptr, poGeomF->GetGeometryRef());
  delete poGeomF;
  poDS->ReleaseResultSet(poGeomColsLyr);

  // FormatSpatialFilterFromRTree & FormatSpatialFilterFromMBR
  auto poFilterGeom = std::make_unique<OGRPolygon>();
  auto poRing = std::make_unique<OGRLinearRing>();
  poRing->addPoint(0, 0);
  poRing->addPoint(10, 0);
  poRing->addPoint(10, 10);
  poRing->addPoint(0, 10);
  poRing->addPoint(0, 0);
  poFilterGeom->addRingDirectly(poRing.release());

  CPLString osRTree = ExposedSQLiteLayer::FormatSpatialFilterFromRTree(
      poFilterGeom.get(), "rowid", "tbl", "geom");
  EXPECT_TRUE(osRTree.find("idx_tbl_geom") != std::string::npos);

  CPLString osMBR = ExposedSQLiteLayer::FormatSpatialFilterFromMBR(
      poFilterGeom.get(), "geom");
  EXPECT_TRUE(osMBR.find("MBRIntersects") != std::string::npos);

  // Infinite filter envelope returns empty string
  class InfiniteGeom : public OGRPoint {
   public:
    void getEnvelope(OGREnvelope* psEnvelope) const override {
      psEnvelope->MinX = -std::numeric_limits<double>::infinity();
      psEnvelope->MinY = -std::numeric_limits<double>::infinity();
      psEnvelope->MaxX = std::numeric_limits<double>::infinity();
      psEnvelope->MaxY = std::numeric_limits<double>::infinity();
    }
  };
  InfiniteGeom infGeom;
  EXPECT_TRUE(ExposedSQLiteLayer::FormatSpatialFilterFromRTree(
                  &infGeom, "rowid", "tbl", "geom")
                  .empty());
  EXPECT_TRUE(
      ExposedSQLiteLayer::FormatSpatialFilterFromMBR(&infGeom, "geom").empty());
}

// TODO: b/335317901 - Cover OGRSQLiteLayer FastGetExtent / spatial index filter
// paths if spatialite is loaded.

}  // namespace
}  // namespace autotest2
