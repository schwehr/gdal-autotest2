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
// Tests for shpopen_wrapper.cpp (shapelib/shpopen.c).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/shape/shpopen_wrapper.cpp
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/shape/shapelib/shpopen.c

#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

static decltype(SAHooks::FOpen) g_orig_fopen = nullptr;
static decltype(SAHooks::FSeek) g_orig_fseek = nullptr;
static decltype(SAHooks::FRead) g_orig_fread = nullptr;
static decltype(SAHooks::FWrite) g_orig_fwrite = nullptr;
static bool g_fail_fopen = false;
static bool g_fail_fseek = false;
static bool g_fail_fread = false;
static bool g_fail_fwrite = false;
static int g_fopen_fail_countdown = -1;
static int g_fread_fail_countdown = -1;
static int g_fwrite_fail_countdown = -1;

void ResetHookGlobals() {
  g_fail_fopen = false;
  g_fail_fseek = false;
  g_fail_fread = false;
  g_fail_fwrite = false;
  g_fopen_fail_countdown = -1;
  g_fread_fail_countdown = -1;
  g_fwrite_fail_countdown = -1;
  g_orig_fopen = nullptr;
  g_orig_fseek = nullptr;
  g_orig_fread = nullptr;
  g_orig_fwrite = nullptr;
}

SAFile HookFOpen(const char* path, const char* mode, void* user_data) {
  if (g_fail_fopen ||
      (g_fopen_fail_countdown >= 0 && g_fopen_fail_countdown-- == 0)) {
    return nullptr;
  }
  return g_orig_fopen ? g_orig_fopen(path, mode, user_data) : nullptr;
}

SAOffset HookFSeek(SAFile file, SAOffset offset, int whence) {
  if (g_fail_fseek) return -1;
  return g_orig_fseek ? g_orig_fseek(file, offset, whence) : -1;
}

SAOffset HookFRead(void* buf, SAOffset size, SAOffset nmemb, SAFile file) {
  if (g_fail_fread ||
      (g_fread_fail_countdown >= 0 && g_fread_fail_countdown-- == 0)) {
    return 0;
  }
  return g_orig_fread ? g_orig_fread(buf, size, nmemb, file) : 0;
}

SAOffset HookFWrite(const void* buf, SAOffset size, SAOffset nmemb,
                    SAFile file) {
  if (g_fail_fwrite ||
      (g_fwrite_fail_countdown >= 0 && g_fwrite_fail_countdown-- == 0)) {
    return 0;
  }
  return g_orig_fwrite ? g_orig_fwrite(buf, size, nmemb, file) : 0;
}

[[nodiscard]] auto InstallIoHooks(SAHooks* hooks) {
  SASetupDefaultHooks(hooks);
  ResetHookGlobals();

  g_orig_fopen = hooks->FOpen;
  g_orig_fseek = hooks->FSeek;
  g_orig_fread = hooks->FRead;
  g_orig_fwrite = hooks->FWrite;

  hooks->FOpen = HookFOpen;
  hooks->FSeek = HookFSeek;
  hooks->FRead = HookFRead;
  hooks->FWrite = HookFWrite;

  return absl::MakeCleanup(ResetHookGlobals);
}

TEST(ShpOpenTest, TypeNamesAndPartNames) {
  EXPECT_STREQ(SHPTypeName(SHPT_NULL), "NullShape");
  EXPECT_STREQ(SHPTypeName(SHPT_POINT), "Point");
  EXPECT_STREQ(SHPTypeName(SHPT_ARC), "Arc");
  EXPECT_STREQ(SHPTypeName(SHPT_POLYGON), "Polygon");
  EXPECT_STREQ(SHPTypeName(SHPT_MULTIPOINT), "MultiPoint");
  EXPECT_STREQ(SHPTypeName(SHPT_POINTZ), "PointZ");
  EXPECT_STREQ(SHPTypeName(SHPT_ARCZ), "ArcZ");
  EXPECT_STREQ(SHPTypeName(SHPT_POLYGONZ), "PolygonZ");
  EXPECT_STREQ(SHPTypeName(SHPT_MULTIPOINTZ), "MultiPointZ");
  EXPECT_STREQ(SHPTypeName(SHPT_POINTM), "PointM");
  EXPECT_STREQ(SHPTypeName(SHPT_ARCM), "ArcM");
  EXPECT_STREQ(SHPTypeName(SHPT_POLYGONM), "PolygonM");
  EXPECT_STREQ(SHPTypeName(SHPT_MULTIPOINTM), "MultiPointM");
  EXPECT_STREQ(SHPTypeName(SHPT_MULTIPATCH), "MultiPatch");
  EXPECT_STREQ(SHPTypeName(-1), "UnknownShapeType");
  EXPECT_STREQ(SHPTypeName(999), "UnknownShapeType");

  EXPECT_STREQ(SHPPartTypeName(SHPP_TRISTRIP), "TriangleStrip");
  EXPECT_STREQ(SHPPartTypeName(SHPP_TRIFAN), "TriangleFan");
  EXPECT_STREQ(SHPPartTypeName(SHPP_OUTERRING), "OuterRing");
  EXPECT_STREQ(SHPPartTypeName(SHPP_INNERRING), "InnerRing");
  EXPECT_STREQ(SHPPartTypeName(SHPP_FIRSTRING), "FirstRing");
  EXPECT_STREQ(SHPPartTypeName(SHPP_RING), "Ring");
  EXPECT_STREQ(SHPPartTypeName(-1), "UnknownPartType");
  EXPECT_STREQ(SHPPartTypeName(999), "UnknownPartType");
}

void RoundTripAndVerify(const char* basePath, int nShapeType,
                        SHPObject* psObject) {
  std::string shpPath = std::string(basePath) + ".shp";
  std::string shxPath = std::string(basePath) + ".shx";
  auto cleanup = absl::MakeCleanup([shpPath, shxPath]() {
    VSIUnlink(shpPath.c_str());
    VSIUnlink(shxPath.c_str());
  });

  SHPHandle hSHP = SHPCreate(basePath, nShapeType);
  ASSERT_THAT(hSHP, NotNull());

  if (psObject != nullptr) {
    EXPECT_EQ(SHPWriteObject(hSHP, -1, psObject), 0);
  }

  // Also write a NULL shape
  SHPObject* psNull = SHPCreateObject(SHPT_NULL, -1, 0, nullptr, nullptr, 0,
                                      nullptr, nullptr, nullptr, nullptr);
  ASSERT_THAT(psNull, NotNull());
  EXPECT_EQ(SHPWriteObject(hSHP, -1, psNull), psObject ? 1 : 0);
  SHPDestroyObject(psNull);

  SHPWriteHeader(hSHP);
  SHPClose(hSHP);

  // Reopen read-only and verify info
  hSHP = SHPOpen(basePath, "rb");
  ASSERT_THAT(hSHP, NotNull());
  auto closeRead = absl::MakeCleanup([hSHP]() { SHPClose(hSHP); });
  int nEntities = 0;
  int nType = 0;
  double adfMin[4] = {};
  double adfMax[4] = {};
  SHPGetInfo(hSHP, &nEntities, &nType, adfMin, adfMax);
  EXPECT_EQ(nEntities, psObject ? 2 : 1);
  EXPECT_EQ(nType, nShapeType);

  // Normal read
  if (psObject != nullptr) {
    SHPObject* psRead = SHPReadObject(hSHP, 0);
    ASSERT_THAT(psRead, NotNull());
    EXPECT_EQ(psRead->nSHPType, psObject->nSHPType);
    EXPECT_EQ(psRead->nVertices, psObject->nVertices);
    EXPECT_EQ(psRead->nParts, psObject->nParts);
    SHPDestroyObject(psRead);
  }

  // Fast mode read
  SHPSetFastModeReadObject(hSHP, TRUE);
  if (psObject != nullptr) {
    SHPObject* psFast = SHPReadObject(hSHP, 0);
    ASSERT_THAT(psFast, NotNull());
    EXPECT_EQ(psFast->nSHPType, psObject->nSHPType);
    SHPDestroyObject(psFast);
  }
  SHPObject* psFastNull = SHPReadObject(hSHP, psObject ? 1 : 0);
  ASSERT_THAT(psFastNull, NotNull());
  EXPECT_EQ(psFastNull->nSHPType, SHPT_NULL);
  SHPDestroyObject(psFastNull);

  // Test error branch: calling SHPReadObject in fast mode without
  // SHPDestroyObject
  {
    WithQuietHandler quiet_handler;
    SHPObject* psFastFirst = SHPReadObject(hSHP, 0);
    ASSERT_THAT(psFastFirst, NotNull());
    EXPECT_EQ(SHPReadObject(hSHP, 0), nullptr);
    SHPDestroyObject(psFastFirst);
  }

  std::move(closeRead).Cancel();
  SHPClose(hSHP);

  // Verify SHPRestoreSHX
  EXPECT_EQ(VSIUnlink(shxPath.c_str()), 0);
  SAHooks hooks;
  SASetupDefaultHooks(&hooks);
  hSHP = SHPOpenLLEx(basePath, "r+b", &hooks, TRUE);
  ASSERT_THAT(hSHP, NotNull());
  auto closeRestore = absl::MakeCleanup([hSHP]() { SHPClose(hSHP); });
  EXPECT_EQ(hSHP->nRecords, psObject ? 2 : 1);
}

TEST(ShpOpenTest, AllShapeTypesRoundTrip) {
  double x[6] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  double y[6] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  double z[6] = {100.0, 200.0, 300.0, 400.0, 500.0, 600.0};
  double m[6] = {1000.0, 2000.0, 3000.0, 4000.0, 5000.0, 6000.0};
  int partStart[2] = {0, 3};
  int partType[2] = {SHPP_RING, SHPP_RING};

  // 1. SHPT_NULL
  RoundTripAndVerify("/vsimem/rt_null", SHPT_NULL, nullptr);

  // 2. SHPT_POINT
  SHPObject* psPt = SHPCreateSimpleObject(SHPT_POINT, 1, x, y, nullptr);
  RoundTripAndVerify("/vsimem/rt_pt", SHPT_POINT, psPt);
  SHPDestroyObject(psPt);

  // 3. SHPT_POINTZ
  SHPObject* psPtZ =
      SHPCreateObject(SHPT_POINTZ, -1, 0, nullptr, nullptr, 1, x, y, z, m);
  RoundTripAndVerify("/vsimem/rt_ptz", SHPT_POINTZ, psPtZ);
  SHPDestroyObject(psPtZ);

  // 4. SHPT_POINTM
  SHPObject* psPtM = SHPCreateObject(SHPT_POINTM, -1, 0, nullptr, nullptr, 1, x,
                                     y, nullptr, m);
  RoundTripAndVerify("/vsimem/rt_ptm", SHPT_POINTM, psPtM);
  SHPDestroyObject(psPtM);

  // 5. SHPT_ARC (multi-part)
  SHPObject* psArc = SHPCreateObject(SHPT_ARC, -1, 2, partStart, nullptr, 6, x,
                                     y, nullptr, nullptr);
  RoundTripAndVerify("/vsimem/rt_arc", SHPT_ARC, psArc);
  SHPDestroyObject(psArc);

  // 6. SHPT_ARCZ
  SHPObject* psArcZ =
      SHPCreateObject(SHPT_ARCZ, -1, 2, partStart, partType, 6, x, y, z, m);
  RoundTripAndVerify("/vsimem/rt_arcz", SHPT_ARCZ, psArcZ);
  SHPDestroyObject(psArcZ);

  // 7. SHPT_ARCM
  SHPObject* psArcM = SHPCreateObject(SHPT_ARCM, -1, 2, partStart, nullptr, 6,
                                      x, y, nullptr, m);
  RoundTripAndVerify("/vsimem/rt_arcm", SHPT_ARCM, psArcM);
  SHPDestroyObject(psArcM);

  // 8. SHPT_POLYGON
  double px[5] = {0.0, 10.0, 10.0, 0.0, 0.0};
  double py[5] = {0.0, 0.0, 10.0, 10.0, 0.0};
  int polyStart = 0;
  SHPObject* psPoly = SHPCreateObject(SHPT_POLYGON, -1, 1, &polyStart, nullptr,
                                      5, px, py, nullptr, nullptr);
  RoundTripAndVerify("/vsimem/rt_poly", SHPT_POLYGON, psPoly);
  SHPDestroyObject(psPoly);

  // 9. SHPT_POLYGONZ
  double pz[5] = {1.0, 1.0, 1.0, 1.0, 1.0};
  double pm[5] = {2.0, 2.0, 2.0, 2.0, 2.0};
  SHPObject* psPolyZ = SHPCreateObject(SHPT_POLYGONZ, -1, 1, &polyStart,
                                       nullptr, 5, px, py, pz, pm);
  RoundTripAndVerify("/vsimem/rt_polyz", SHPT_POLYGONZ, psPolyZ);
  SHPDestroyObject(psPolyZ);

  // 10. SHPT_POLYGONM
  SHPObject* psPolyM = SHPCreateObject(SHPT_POLYGONM, -1, 1, &polyStart,
                                       nullptr, 5, px, py, nullptr, pm);
  RoundTripAndVerify("/vsimem/rt_polym", SHPT_POLYGONM, psPolyM);
  SHPDestroyObject(psPolyM);

  // 11. SHPT_MULTIPOINT
  SHPObject* psMpt = SHPCreateSimpleObject(SHPT_MULTIPOINT, 4, x, y, nullptr);
  RoundTripAndVerify("/vsimem/rt_mpt", SHPT_MULTIPOINT, psMpt);
  SHPDestroyObject(psMpt);

  // 12. SHPT_MULTIPOINTZ
  SHPObject* psMptZ =
      SHPCreateObject(SHPT_MULTIPOINTZ, -1, 0, nullptr, nullptr, 4, x, y, z, m);
  RoundTripAndVerify("/vsimem/rt_mptz", SHPT_MULTIPOINTZ, psMptZ);
  SHPDestroyObject(psMptZ);

  // 13. SHPT_MULTIPOINTM
  SHPObject* psMptM = SHPCreateObject(SHPT_MULTIPOINTM, -1, 0, nullptr, nullptr,
                                      4, x, y, nullptr, m);
  RoundTripAndVerify("/vsimem/rt_mptm", SHPT_MULTIPOINTM, psMptM);
  SHPDestroyObject(psMptM);

  // 14. SHPT_MULTIPATCH
  int patchParts[3] = {SHPP_TRISTRIP, SHPP_TRIFAN, SHPP_OUTERRING};
  int patchStarts[3] = {0, 2, 4};
  SHPObject* psPatch = SHPCreateObject(SHPT_MULTIPATCH, -1, 3, patchStarts,
                                       patchParts, 6, x, y, z, m);
  RoundTripAndVerify("/vsimem/rt_patch", SHPT_MULTIPATCH, psPatch);
  SHPDestroyObject(psPatch);
}

TEST(ShpOpenTest, PolygonGeometriesAndRewind) {
  constexpr const char* kBase = "/vsimem/test_poly_rewind";
  constexpr const char* kShp = "/vsimem/test_poly_rewind.shp";
  constexpr const char* kShx = "/vsimem/test_poly_rewind.shx";
  auto cleanup = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POLYGONZ);
  ASSERT_THAT(hSHP, NotNull());

  // Outer ring CCW, inner ring CW
  double x[10] = {0.0, 10.0, 10.0, 0.0, 0.0, 2.0, 2.0, 8.0, 8.0, 2.0};
  double y[10] = {0.0, 0.0, 10.0, 10.0, 0.0, 2.0, 8.0, 8.0, 2.0, 2.0};
  double z[10] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  int partStart[2] = {0, 5};

  SHPObject* psPoly = SHPCreateObject(SHPT_POLYGONZ, -1, 2, partStart, nullptr,
                                      10, x, y, z, nullptr);
  ASSERT_THAT(psPoly, NotNull());
  EXPECT_NE(SHPRewindObject(hSHP, psPoly), 0);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, psPoly), 0);
  SHPDestroyObject(psPoly);

  // Unclosed ring test: 4 vertices without closing point
  double xUnclosed[4] = {0.0, 5.0, 5.0, 0.0};
  double yUnclosed[4] = {0.0, 0.0, 5.0, 5.0};
  double zUnclosed[4] = {1.0, 1.0, 1.0, 1.0};
  SHPObject* psUnclosed =
      SHPCreateObject(SHPT_POLYGONZ, -1, 1, partStart, nullptr, 4, xUnclosed,
                      yUnclosed, zUnclosed, nullptr);
  ASSERT_THAT(psUnclosed, NotNull());
  EXPECT_EQ(psUnclosed->nVertices, 4);
  EXPECT_NE(SHPRewindObject(hSHP, psUnclosed), 0);
  EXPECT_EQ(psUnclosed->nVertices, 4);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, psUnclosed), 1);
  SHPDestroyObject(psUnclosed);

  // Rewind on non-polygon does nothing
  SHPObject* psPt = SHPCreateSimpleObject(SHPT_POINT, 1, x, y, nullptr);
  EXPECT_EQ(SHPRewindObject(hSHP, psPt), 0);
  SHPDestroyObject(psPt);

  // Rewind on empty polygon does nothing
  SHPObject* psEmpty = SHPCreateObject(SHPT_POLYGON, -1, 0, nullptr, nullptr, 0,
                                       nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(SHPRewindObject(hSHP, psEmpty), 0);
  SHPDestroyObject(psEmpty);

  // 3D polygon with variable Z
  double zVar[5] = {1.0, 2.0, 3.0, 4.0, 1.0};
  SHPObject* psVarZ = SHPCreateObject(SHPT_POLYGONZ, -1, 1, partStart, nullptr,
                                      5, x, y, zVar, nullptr);
  ASSERT_THAT(psVarZ, NotNull());
  SHPRewindObject(hSHP, psVarZ);
  SHPDestroyObject(psVarZ);

  SHPClose(hSHP);
}

TEST(ShpOpenTest, InPlaceOverwriteGrowingAndShrinking) {
  constexpr const char* kBase = "/vsimem/test_overwrite";
  constexpr const char* kShp = "/vsimem/test_overwrite.shp";
  constexpr const char* kShx = "/vsimem/test_overwrite.shx";
  auto cleanup = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_ARC);
  ASSERT_THAT(hSHP, NotNull());

  double x2[2] = {0.0, 1.0};
  double y2[2] = {0.0, 1.0};

  // Record 0: 2 vertices
  SHPObject* ps0 = SHPCreateSimpleObject(SHPT_ARC, 2, x2, y2, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, ps0), 0);
  SHPDestroyObject(ps0);

  // Record 1: 2 vertices
  SHPObject* ps1 = SHPCreateSimpleObject(SHPT_ARC, 2, x2, y2, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, ps1), 1);
  SHPDestroyObject(ps1);

  // Record 2: 2 vertices
  SHPObject* ps2 = SHPCreateSimpleObject(SHPT_ARC, 2, x2, y2, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, ps2), 2);
  SHPDestroyObject(ps2);

  // 1. In-place overwrite record 1 with same size (2 vertices)
  double x2New[2] = {10.0, 20.0};
  double y2New[2] = {10.0, 20.0};
  SHPObject* ps1Same =
      SHPCreateSimpleObject(SHPT_ARC, 2, x2New, y2New, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, 1, ps1Same), 1);
  SHPDestroyObject(ps1Same);

  // 2. Overwrite record 0 with smaller object (NULL shape)
  SHPObject* psNull = SHPCreateObject(SHPT_NULL, -1, 0, nullptr, nullptr, 0,
                                      nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, 0, psNull), 0);
  SHPDestroyObject(psNull);

  // 3. Overwrite record 1 with LARGER object (10 vertices)
  // This triggers record growth: appends to EOF and updates SHX table
  std::vector<double> xGrow(10);
  std::vector<double> yGrow(10);
  for (int i = 0; i < 10; ++i) {
    xGrow[i] = i * 2.0;
    yGrow[i] = i * 3.0;
  }
  SHPObject* ps1Grow =
      SHPCreateSimpleObject(SHPT_ARC, 10, xGrow.data(), yGrow.data(), nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, 1, ps1Grow), 1);
  SHPDestroyObject(ps1Grow);

  // Read all records back
  SHPObject* psRead0 = SHPReadObject(hSHP, 0);
  ASSERT_THAT(psRead0, NotNull());
  EXPECT_EQ(psRead0->nSHPType, SHPT_NULL);
  SHPDestroyObject(psRead0);

  SHPObject* psRead1 = SHPReadObject(hSHP, 1);
  ASSERT_THAT(psRead1, NotNull());
  EXPECT_EQ(psRead1->nSHPType, SHPT_ARC);
  EXPECT_EQ(psRead1->nVertices, 10);
  EXPECT_DOUBLE_EQ(psRead1->padfX[9], 18.0);
  SHPDestroyObject(psRead1);

  SHPObject* psRead2 = SHPReadObject(hSHP, 2);
  ASSERT_THAT(psRead2, NotNull());
  EXPECT_EQ(psRead2->nVertices, 2);
  SHPDestroyObject(psRead2);

  SHPClose(hSHP);
}

TEST(ShpOpenTest, RestoreShxAndAccessModes) {
  constexpr const char* kBase = "/vsimem/test_restore_shx";
  constexpr const char* kShp = "/vsimem/test_restore_shx.shp";
  constexpr const char* kShx = "/vsimem/test_restore_shx.shx";
  auto cleanup = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle hSHP = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(hSHP, NotNull());
  double x = 100.0, y = 200.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(hSHP, -1, ps), 0);
  SHPDestroyObject(ps);
  SHPClose(hSHP);

  // Delete .shx
  EXPECT_EQ(VSIUnlink(kShx), 0);

  // Open with bRestoreSHX = TRUE via SHPOpenLLEx
  SAHooks hooks;
  SASetupDefaultHooks(&hooks);
  hSHP = SHPOpenLLEx(kBase, "r+b", &hooks, TRUE);
  ASSERT_THAT(hSHP, NotNull());
  int nEntities = 0;
  SHPGetInfo(hSHP, &nEntities, nullptr, nullptr, nullptr);
  EXPECT_EQ(nEntities, 1);
  SHPObject* psRead = SHPReadObject(hSHP, 0);
  ASSERT_THAT(psRead, NotNull());
  EXPECT_DOUBLE_EQ(psRead->padfX[0], 100.0);
  SHPDestroyObject(psRead);
  SHPClose(hSHP);

  // Open in "rl" lazy mode
  hSHP = SHPOpen(kBase, "rl");
  ASSERT_THAT(hSHP, NotNull());
  psRead = SHPReadObject(hSHP, 0);
  ASSERT_THAT(psRead, NotNull());
  EXPECT_DOUBLE_EQ(psRead->padfX[0], 100.0);
  SHPDestroyObject(psRead);
  SHPClose(hSHP);

  // Uppercase .SHP and .SHX fallback
  constexpr const char* kUpperBase = "/vsimem/UPPER_TEST";
  constexpr const char* kUpperShp = "/vsimem/UPPER_TEST.SHP";
  constexpr const char* kUpperShx = "/vsimem/UPPER_TEST.SHX";
  vsi_l_offset shpLen = 0, shxLen = 0;
  GByte* pabyShp = VSIGetMemFileBuffer(kShp, &shpLen, FALSE);
  GByte* pabyShx = VSIGetMemFileBuffer(kShx, &shxLen, FALSE);
  VSIFCloseL(VSIFileFromMemBuffer(kUpperShp, pabyShp, shpLen, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(kUpperShx, pabyShx, shxLen, FALSE));
  auto cleanupUpper = absl::MakeCleanup([kUpperShp, kUpperShx]() {
    VSIUnlink(kUpperShp);
    VSIUnlink(kUpperShx);
  });

  SHPHandle hUpper = SHPOpen(kUpperBase, "rb");
  ASSERT_THAT(hUpper, NotNull());
  EXPECT_EQ(hUpper->nRecords, 1);
  SHPClose(hUpper);
}

TEST(ShpOpenTest, ErrorPathsAndCorruptHeaders) {
  WithQuietHandler quiet_handler;

  // Non-existent file
  SAHooks hooks;
  SASetupDefaultHooks(&hooks);
  EXPECT_EQ(SHPOpen("/vsimem/does_not_exist.shp", "rb"), nullptr);
  EXPECT_EQ(SHPRestoreSHX("/vsimem/does_not_exist.shp", "rb", &hooks), 0);

  // Truncated header < 100 bytes
  constexpr const char* kShortShp = "/vsimem/short_file.shp";
  constexpr const char* kShortShx = "/vsimem/short_file.shx";
  unsigned char shortBuf[50] = {};
  VSIFCloseL(
      VSIFileFromMemBuffer(kShortShp, shortBuf, sizeof(shortBuf), FALSE));
  VSIFCloseL(
      VSIFileFromMemBuffer(kShortShx, shortBuf, sizeof(shortBuf), FALSE));
  auto clShort = absl::MakeCleanup([kShortShp, kShortShx]() {
    VSIUnlink(kShortShp);
    VSIUnlink(kShortShx);
  });
  EXPECT_EQ(SHPOpen(kShortShp, "rb"), nullptr);

  // Bad magic number (!= 9994) in 100-byte header
  constexpr const char* kBadMagicShp = "/vsimem/bad_magic.shp";
  constexpr const char* kBadMagicShx = "/vsimem/bad_magic.shx";
  unsigned char badMagic[100] = {};
  badMagic[0] = 0;
  badMagic[1] = 0;
  badMagic[2] = 4;
  badMagic[3] = 210;  // 1234 instead of 9994
  VSIFCloseL(
      VSIFileFromMemBuffer(kBadMagicShp, badMagic, sizeof(badMagic), FALSE));
  VSIFCloseL(
      VSIFileFromMemBuffer(kBadMagicShx, badMagic, sizeof(badMagic), FALSE));
  auto clBadMagic = absl::MakeCleanup([kBadMagicShp, kBadMagicShx]() {
    VSIUnlink(kBadMagicShp);
    VSIUnlink(kBadMagicShx);
  });
  EXPECT_EQ(SHPOpen(kBadMagicShp, "rb"), nullptr);

  // Close null handle is safe no-op
  SHPClose(nullptr);
  SHPDestroyObject(nullptr);
}

TEST(ShpOpenTest, CustomHooksAndIoFailures) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  // 1. FOpen failure on create
  g_fail_fopen = true;
  EXPECT_EQ(SHPCreateLL("/vsimem/fail_shp.shp", SHPT_POINT, &hooks), nullptr);
  g_fail_fopen = false;

  // Create valid shapefile
  constexpr const char* kHookBase = "/vsimem/hook_shp";
  constexpr const char* kHookShp = "/vsimem/hook_shp.shp";
  constexpr const char* kHookShx = "/vsimem/hook_shp.shx";
  auto clHook = absl::MakeCleanup([kHookShp, kHookShx]() {
    VSIUnlink(kHookShp);
    VSIUnlink(kHookShx);
  });
  SHPHandle h = SHPCreateLL(kHookBase, SHPT_POINT, &hooks);
  ASSERT_THAT(h, NotNull());
  double x = 1.0, y = 1.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, ps), 0);
  SHPDestroyObject(ps);
  SHPClose(h);

  // 2. FSeek failure during SHPReadObject
  h = SHPOpenLL(kHookBase, "rb", &hooks);
  ASSERT_THAT(h, NotNull());
  g_fail_fseek = true;
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  g_fail_fseek = false;

  // 3. FRead failure during SHPReadObject
  g_fail_fread = true;
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  g_fail_fread = false;

  SHPClose(h);
}

TEST(ShpOpenTest, ShpWriteHeaderIoFailures) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  constexpr const char* kBase = "/vsimem/shpwriteheader_io";
  constexpr const char* kShp = "/vsimem/shpwriteheader_io.shp";
  constexpr const char* kShx = "/vsimem/shpwriteheader_io.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle h = SHPCreateLL(kBase, SHPT_POINT, &hooks);
  ASSERT_THAT(h, NotNull());
  double x = 1.0, y = 1.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, ps), 0);
  SHPDestroyObject(ps);

  // 1. Fail writing .shp header in SHPWriteHeader
  g_fwrite_fail_countdown = 0;
  SHPWriteHeader(h);
  g_fwrite_fail_countdown = -1;

  // 2. Fail writing .shx header in SHPWriteHeader
  g_fwrite_fail_countdown = 1;
  SHPWriteHeader(h);
  g_fwrite_fail_countdown = -1;

  // 3. Fail writing .shx contents in SHPWriteHeader
  g_fwrite_fail_countdown = 2;
  SHPWriteHeader(h);
  g_fwrite_fail_countdown = -1;

  // 4. SHPWriteHeader failed when SHX file is closed
  auto origFpx = h->fpSHX;
  h->fpSHX = nullptr;
  SHPWriteHeader(h);
  h->fpSHX = origFpx;

  SHPClose(h);
}

TEST(ShpOpenTest, MissingShxAndCorruptHeaders) {
  WithQuietHandler quiet_handler;

  // 1. Missing .shx file
  constexpr const char* kBase = "/vsimem/missing_shx";
  constexpr const char* kShp = "/vsimem/missing_shx.shp";
  constexpr const char* kShx = "/vsimem/missing_shx.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle h = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(h, NotNull());
  double x = 1.0, y = 1.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, ps), 0);
  SHPDestroyObject(ps);
  SHPClose(h);

  EXPECT_EQ(VSIUnlink(kShx), 0);
  EXPECT_EQ(SHPOpen(kBase, "rb"), nullptr);

  // 2. Negative nRecords in .shx header (records = (10 - 50)/4 = -10 < 0)
  unsigned char negRecordsShx[100] = {};
  negRecordsShx[2] = 0x27;
  negRecordsShx[3] = 0x0a;
  negRecordsShx[27] = 10;
  VSIFCloseL(
      VSIFileFromMemBuffer(kShx, negRecordsShx, sizeof(negRecordsShx), FALSE));
  EXPECT_EQ(SHPOpen(kBase, "rb"), nullptr);

  // 3. Clamped nRecords >= 1024 * 1024
  std::vector<unsigned char> clampShx(108, 0);
  clampShx[2] = 0x27;
  clampShx[3] = 0x0a;
  clampShx[24] = 0x00;
  clampShx[25] = 0x40;
  clampShx[26] = 0x16;
  clampShx[27] = 0x72;
  clampShx[103] = 50;
  clampShx[107] = 14;
  VSIFCloseL(
      VSIFileFromMemBuffer(kShx, clampShx.data(), clampShx.size(), FALSE));
  h = SHPOpen(kBase, "rb");
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(h->nRecords, 1);
  SHPClose(h);

  // 4. Truncated .shx record table
  unsigned char truncShx[100] = {};
  truncShx[2] = 0x27;
  truncShx[3] = 0x0a;
  truncShx[27] = 70;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, truncShx, sizeof(truncShx), FALSE));
  EXPECT_EQ(SHPOpen(kBase, "rb"), nullptr);

  // 5. Invalid offset (> INT_MAX) and invalid length (> INT_MAX/2 - 4) in .shx
  // record
  std::vector<unsigned char> badOffsetShx(108, 0);
  badOffsetShx[2] = 0x27;
  badOffsetShx[3] = 0x0a;
  badOffsetShx[27] = 54;
  badOffsetShx[100] = 0x80;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, badOffsetShx.data(),
                                  badOffsetShx.size(), FALSE));
  EXPECT_EQ(SHPOpen(kBase, "rb"), nullptr);

  badOffsetShx[100] = 0;
  badOffsetShx[103] = 50;
  badOffsetShx[104] = 0x40;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, badOffsetShx.data(),
                                  badOffsetShx.size(), FALSE));
  EXPECT_EQ(SHPOpen(kBase, "rb"), nullptr);
}

TEST(ShpOpenTest, RestoreShxErrorPaths) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  constexpr const char* kBase = "/vsimem/restore_shx_err";
  constexpr const char* kShp = "/vsimem/restore_shx_err.shp";
  constexpr const char* kShx = "/vsimem/restore_shx_err.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle h = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(h, NotNull());
  double x = 1.0, y = 1.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, ps), 0);
  SHPDestroyObject(ps);
  SHPClose(h);

  // 1. FOpen fails on .shx write
  g_fopen_fail_countdown = 1;
  EXPECT_FALSE(SHPRestoreSHX(kShp, "rb", &hooks));
  g_fopen_fail_countdown = -1;

  // 2. Invalid record length < 1 in .shp
  std::vector<unsigned char> badShp(120, 0);
  badShp[2] = 0x27;
  badShp[3] = 0x0a;
  badShp[27] = 60;
  badShp[32] = SHPT_POINT;
  badShp[103] = 1;
  badShp[107] = 0;
  badShp[108] = SHPT_POINT;
  VSIFCloseL(VSIFileFromMemBuffer(kShp, badShp.data(), badShp.size(), FALSE));
  EXPECT_FALSE(SHPRestoreSHX(kShp, "rb", &hooks));

  // 3. Invalid shape type in record
  badShp[107] = 6;
  badShp[108] = 0xE7;
  badShp[109] = 0x03;
  VSIFCloseL(VSIFileFromMemBuffer(kShp, badShp.data(), badShp.size(), FALSE));
  EXPECT_FALSE(SHPRestoreSHX(kShp, "rb", &hooks));

  // 4. Unexpected EOF when reading record header
  badShp.resize(104);
  badShp[27] = 60;
  VSIFCloseL(VSIFileFromMemBuffer(kShp, badShp.data(), badShp.size(), FALSE));
  EXPECT_FALSE(SHPRestoreSHX(kShp, "rb", &hooks));
}

TEST(ShpOpenTest, ShpCreateErrorPaths) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  constexpr const char* kBase = "/vsimem/shpcreate_err";
  constexpr const char* kShp = "/vsimem/shpcreate_err.shp";
  constexpr const char* kShx = "/vsimem/shpcreate_err.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  // 1. Fail opening .shx
  g_fopen_fail_countdown = 1;
  EXPECT_EQ(SHPCreateLL(kBase, SHPT_POINT, &hooks), nullptr);
  g_fopen_fail_countdown = -1;

  // 2. Fail writing .shp header
  g_fwrite_fail_countdown = 0;
  EXPECT_EQ(SHPCreateLL(kBase, SHPT_POINT, &hooks), nullptr);
  g_fwrite_fail_countdown = -1;

  // 3. Fail writing .shx header
  g_fwrite_fail_countdown = 1;
  EXPECT_EQ(SHPCreateLL(kBase, SHPT_POINT, &hooks), nullptr);
  g_fwrite_fail_countdown = -1;
}

TEST(ShpOpenTest, ShpWriteObjectEdgeCases) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  constexpr const char* kBase = "/vsimem/shpwrite_edge";
  constexpr const char* kShp = "/vsimem/shpwrite_edge.shp";
  constexpr const char* kShx = "/vsimem/shpwrite_edge.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle h = SHPCreateLL(kBase, SHPT_ARC, &hooks);
  ASSERT_THAT(h, NotNull());

  double x[3] = {0.0, 1.0, 2.0};
  double y[3] = {0.0, 1.0, 2.0};
  SHPObject* psArc = SHPCreateSimpleObject(SHPT_ARC, 2, x, y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, psArc), 0);
  SHPClose(h);

  // Reopen in "r+b" mode (nMaxRecords == 1)
  h = SHPOpenLL(kBase, "r+b", &hooks);
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(h->nRecords, 1);

  // 1. Overwrite last record (bAppendToLastRecord = true)
  SHPObject* psArc3 = SHPCreateSimpleObject(SHPT_ARC, 3, x, y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, 0, psArc3), 0);

  // 2. Append new record triggering nRecords + 1 > nMaxRecords realloc
  EXPECT_EQ(SHPWriteObject(h, -1, psArc3), 1);
  EXPECT_EQ(h->nRecords, 2);

  // 3. FSeek failure in SHPWriteObject
  hooks.FSeek(h->fpSHP, 0, 0);  // position != nRecordOffset
  g_fail_fseek = true;
  EXPECT_EQ(SHPWriteObject(h, -1, psArc3), -1);
  g_fail_fseek = false;

  // 4. FWrite failure in SHPWriteObject
  g_fwrite_fail_countdown = 0;
  EXPECT_EQ(SHPWriteObject(h, -1, psArc3), -1);
  g_fwrite_fail_countdown = -1;

  // 5. Exceed 4GB file size limit
  const unsigned int savedSize = h->nFileSize;
  h->nFileSize = UINT_MAX - 4;
  EXPECT_EQ(SHPWriteObject(h, -1, psArc3), -1);
  h->nFileSize = savedSize;

  SHPDestroyObject(psArc);
  SHPDestroyObject(psArc3);
  SHPClose(h);
}

TEST(ShpOpenTest, ShpReadObjectCorruptRecords) {
  WithQuietHandler quiet_handler;

  SAHooks hooks;
  auto cleanup_hooks = InstallIoHooks(&hooks);

  constexpr const char* kBase = "/vsimem/shpread_corrupt";
  constexpr const char* kShp = "/vsimem/shpread_corrupt.shp";
  constexpr const char* kShx = "/vsimem/shpread_corrupt.shx";
  auto cl = absl::MakeCleanup([kShp, kShx]() {
    VSIUnlink(kShp);
    VSIUnlink(kShx);
  });

  SHPHandle h = SHPCreate(kBase, SHPT_POINT);
  ASSERT_THAT(h, NotNull());
  double x = 1.0, y = 1.0;
  SHPObject* ps = SHPCreateSimpleObject(SHPT_POINT, 1, &x, &y, nullptr);
  EXPECT_EQ(SHPWriteObject(h, -1, ps), 0);
  SHPDestroyObject(ps);
  SHPClose(h);

  // 1. Lazy SHX ("rl" mode) FRead failure
  h = SHPOpenLL(kBase, "rl", &hooks);
  ASSERT_THAT(h, NotNull());
  g_fail_fread = true;
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  g_fail_fread = false;
  SHPClose(h);

  // 2. Corrupt .shx record offset > INT_MAX and length > INT_MAX / 2 - 4 in
  // "rl" mode
  std::vector<unsigned char> shxBuf(108, 0);
  shxBuf[2] = 0x27;
  shxBuf[3] = 0x0a;
  shxBuf[27] = 54;
  shxBuf[100] = 0x80;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, shxBuf.data(), shxBuf.size(), FALSE));
  h = SHPOpenLL(kBase, "rl", &hooks);
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  SHPClose(h);

  shxBuf[100] = 0;
  shxBuf[103] = 50;
  shxBuf[104] = 0x40;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, shxBuf.data(), shxBuf.size(), FALSE));
  h = SHPOpenLL(kBase, "rl", &hooks);
  ASSERT_THAT(h, NotNull());
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  SHPClose(h);

  // Restore valid .shx
  shxBuf[104] = 0;
  shxBuf[107] = 10;
  VSIFCloseL(VSIFileFromMemBuffer(kShx, shxBuf.data(), shxBuf.size(), FALSE));

  // 3. >10MB buffer check (panRecSize[0] = 10MB while file is small)
  h = SHPOpen(kBase, "rb");
  ASSERT_THAT(h, NotNull());
  const unsigned int origSize = h->panRecSize[0];
  h->panRecSize[0] = 10 * 1024 * 1024;
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  h->panRecSize[0] = origSize;
  SHPClose(h);

  // 4. Inconsistent .shx/.shp content length check (lines 2123-2144)
  h = SHPOpen(kBase, "rb");
  ASSERT_THAT(h, NotNull());
  h->panRecSize[0] = 28;
  vsi_l_offset nShpLen = 0;
  GByte* pabyShp = VSIGetMemFileBuffer(kShp, &nShpLen, FALSE);
  if (pabyShp && nShpLen >= 108) {
    pabyShp[104] = 0;
    pabyShp[105] = 0;
    pabyShp[106] = 0;
    pabyShp[107] = 0;
  }
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  h->panRecSize[0] = origSize;
  SHPClose(h);

  // 5. Corrupt entity sizes & counts
  h = SHPOpen(kBase, "rb");
  ASSERT_THAT(h, NotNull());
  h->panRecSize[0] = 2;
  EXPECT_EQ(SHPReadObject(h, 0), nullptr);
  h->panRecSize[0] = origSize;
  SHPClose(h);

  // ARC corrupt entity sizes
  constexpr const char* kArcBase = "/vsimem/arc_corrupt";
  constexpr const char* kArcShp = "/vsimem/arc_corrupt.shp";
  constexpr const char* kArcShx = "/vsimem/arc_corrupt.shx";
  auto clArc = absl::MakeCleanup([kArcShp, kArcShx]() {
    VSIUnlink(kArcShp);
    VSIUnlink(kArcShx);
  });
  SHPHandle hArc = SHPCreate(kArcBase, SHPT_ARC);
  ASSERT_THAT(hArc, NotNull());
  double ax[2] = {0.0, 1.0};
  double ay[2] = {0.0, 1.0};
  SHPObject* psArc = SHPCreateSimpleObject(SHPT_ARC, 2, ax, ay, nullptr);
  EXPECT_EQ(SHPWriteObject(hArc, -1, psArc), 0);
  SHPDestroyObject(psArc);
  SHPClose(hArc);

  hArc = SHPOpen(kArcBase, "rb");
  ASSERT_THAT(hArc, NotNull());
  const unsigned int origArcSize = hArc->panRecSize[0];
  hArc->panRecSize[0] = 12;
  EXPECT_EQ(SHPReadObject(hArc, 0), nullptr);
  hArc->panRecSize[0] = origArcSize;

  GByte* pabyArc = VSIGetMemFileBuffer(kArcShp, &nShpLen, FALSE);
  ASSERT_THAT(pabyArc, NotNull());
  uint32_t badPoints = 60000000;
  std::memcpy(pabyArc + 148, &badPoints, 4);
  EXPECT_EQ(SHPReadObject(hArc, 0), nullptr);

  uint32_t tenPoints = 10;
  std::memcpy(pabyArc + 148, &tenPoints, 4);
  EXPECT_EQ(SHPReadObject(hArc, 0), nullptr);

  // Test panPartStart[0] >= nVertices and panPartStart[i] <= panPartStart[i-1]
  // Expand file buffer and set nParts = 2, nPoints = 2
  std::vector<unsigned char> arcBuf(200, 0);
  std::memcpy(arcBuf.data(), pabyArc, std::min<size_t>(nShpLen, 200));
  uint32_t twoParts = 2;
  uint32_t twoPoints = 2;
  std::memcpy(arcBuf.data() + 144, &twoParts, 4);
  std::memcpy(arcBuf.data() + 148, &twoPoints, 4);
  // panPartStart[0] = 5 >= nVertices (2)
  uint32_t part0 = 5;
  uint32_t part1 = 1;
  std::memcpy(arcBuf.data() + 152, &part0, 4);
  std::memcpy(arcBuf.data() + 156, &part1, 4);
  VSIFCloseL(
      VSIFileFromMemBuffer(kArcShp, arcBuf.data(), arcBuf.size(), FALSE));
  hArc->panRecSize[0] = 84;  // nEntitySize = 92
  EXPECT_EQ(SHPReadObject(hArc, 0), nullptr);

  // panPartStart[0] = 0, panPartStart[1] = 0 (part1 <= part0)
  part0 = 0;
  part1 = 0;
  std::memcpy(arcBuf.data() + 152, &part0, 4);
  std::memcpy(arcBuf.data() + 156, &part1, 4);
  VSIFCloseL(
      VSIFileFromMemBuffer(kArcShp, arcBuf.data(), arcBuf.size(), FALSE));
  EXPECT_EQ(SHPReadObject(hArc, 0), nullptr);

  SHPClose(hArc);

  // MULTIPOINT corrupt entity sizes
  constexpr const char* kMpBase = "/vsimem/mp_corrupt";
  constexpr const char* kMpShp = "/vsimem/mp_corrupt.shp";
  constexpr const char* kMpShx = "/vsimem/mp_corrupt.shx";
  auto clMp = absl::MakeCleanup([kMpShp, kMpShx]() {
    VSIUnlink(kMpShp);
    VSIUnlink(kMpShx);
  });
  SHPHandle hMp = SHPCreate(kMpBase, SHPT_MULTIPOINT);
  ASSERT_THAT(hMp, NotNull());
  double mpx[2] = {0.0, 1.0};
  double mpy[2] = {0.0, 1.0};
  SHPObject* psMp =
      SHPCreateSimpleObject(SHPT_MULTIPOINT, 2, mpx, mpy, nullptr);
  EXPECT_EQ(SHPWriteObject(hMp, -1, psMp), 0);
  SHPDestroyObject(psMp);
  SHPClose(hMp);

  hMp = SHPOpen(kMpBase, "rb");
  ASSERT_THAT(hMp, NotNull());
  const unsigned int origMpSize = hMp->panRecSize[0];
  hMp->panRecSize[0] = 12;
  EXPECT_EQ(SHPReadObject(hMp, 0), nullptr);

  GByte* pabyMp = VSIGetMemFileBuffer(kMpShp, &nShpLen, FALSE);
  ASSERT_THAT(pabyMp, NotNull());
  std::memcpy(pabyMp + 144, &badPoints, 4);
  hMp->panRecSize[0] = origMpSize;
  EXPECT_EQ(SHPReadObject(hMp, 0), nullptr);

  uint32_t fivePoints = 5;
  std::memcpy(pabyMp + 144, &fivePoints, 4);
  EXPECT_EQ(SHPReadObject(hMp, 0), nullptr);

  SHPClose(hMp);

  // PointZ corrupt entity sizes (20 + 8 + 8 > nEntitySize)
  constexpr const char* kPtzBase = "/vsimem/ptz_corrupt";
  constexpr const char* kPtzShp = "/vsimem/ptz_corrupt.shp";
  constexpr const char* kPtzShx = "/vsimem/ptz_corrupt.shx";
  auto clPtz = absl::MakeCleanup([kPtzShp, kPtzShx]() {
    VSIUnlink(kPtzShp);
    VSIUnlink(kPtzShx);
  });
  SHPHandle hPtz = SHPCreate(kPtzBase, SHPT_POINTZ);
  ASSERT_THAT(hPtz, NotNull());
  double ptzX = 1.0, ptzY = 1.0, ptzZ = 1.0;
  SHPObject* psPtz = SHPCreateSimpleObject(SHPT_POINTZ, 1, &ptzX, &ptzY, &ptzZ);
  EXPECT_EQ(SHPWriteObject(hPtz, -1, psPtz), 0);
  SHPDestroyObject(psPtz);
  SHPClose(hPtz);

  hPtz = SHPOpen(kPtzBase, "rb");
  ASSERT_THAT(hPtz, NotNull());
  hPtz->panRecSize[0] = 20;  // nEntitySize = 28 < 36
  EXPECT_EQ(SHPReadObject(hPtz, 0), nullptr);
  SHPClose(hPtz);
}

TEST(ShpOpenTest, PolygonZWithVaryingZAndRewind) {
  constexpr int nParts = 3;
  constexpr int nVertices = 12;
  const int panPartStart[nParts] = {0, 4, 8};
  const int panPartType[nParts] = {SHPP_RING, SHPP_RING, SHPP_RING};

  const double padfX[nVertices] = {0.0, 10.0, 10.0, 0.0, 2.0, 2.0,
                                   4.0, 4.0,  6.0,  6.0, 8.0, 8.0};
  const double padfY[nVertices] = {0.0, 0.0, 10.0, 10.0, 2.0, 4.0,
                                   4.0, 2.0, 6.0,  8.0,  8.0, 6.0};
  const double padfZ[nVertices] = {10.0, 10.0, 10.0, 10.0, 10.0, 20.0,
                                   10.0, 10.0, 5.0,  5.0,  5.0,  5.0};

  SHPObject* psPolyZ =
      SHPCreateObject(SHPT_POLYGONZ, -1, nParts, panPartStart, panPartType,
                      nVertices, padfX, padfY, padfZ, nullptr);
  ASSERT_THAT(psPolyZ, NotNull());

  EXPECT_GT(SHPRewindObject(nullptr, psPolyZ), 0);
  SHPDestroyObject(psPolyZ);
}

}  // namespace
}  // namespace autotest2
