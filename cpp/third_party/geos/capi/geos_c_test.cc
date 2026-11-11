// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/capi/geos_c.h"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

namespace geos_google {
namespace {

using ::testing::HasSubstr;

std::vector<std::string>& NoticeLog() {
  static auto* const log = new std::vector<std::string>();
  return *log;
}

std::vector<std::string>& ErrorLog() {
  static auto* const log = new std::vector<std::string>();
  return *log;
}

GEOS_PRINTF_FORMAT_ATTR(2, 0)
void AppendFormattedMessage(std::vector<std::string>& log, const char* fmt,
                            va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);
  const int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);
  if (needed < 0) {
    return;
  }
  std::string msg(static_cast<std::size_t>(needed) + 1, '\0');
  std::vsnprintf(msg.data(), msg.size(), fmt, args);
  msg.pop_back();
  log.push_back(std::move(msg));
}

GEOS_PRINTF_FORMAT_ATTR(1, 2)
void NoticeHandler(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AppendFormattedMessage(NoticeLog(), fmt, args);
  va_end(args);
}

GEOS_PRINTF_FORMAT_ATTR(1, 2)
void ErrorHandler(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AppendFormattedMessage(ErrorLog(), fmt, args);
  va_end(args);
}

GEOS_PRINTF_FORMAT_ATTR(1, 2)
void AltNoticeHandler(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AppendFormattedMessage(NoticeLog(), fmt, args);
  va_end(args);
  NoticeLog().back().insert(0, "ALT:");
}

GEOS_PRINTF_FORMAT_ATTR(1, 2)
void AltErrorHandler(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AppendFormattedMessage(ErrorLog(), fmt, args);
  va_end(args);
  ErrorLog().back().insert(0, "ALT:");
}

struct GeomDeleter {
  void operator()(GEOSGeometry* g) const { GEOSGeom_destroy(g); }
};
using UniqueGeom = std::unique_ptr<GEOSGeometry, GeomDeleter>;

struct CoordSeqDeleter {
  void operator()(GEOSCoordSequence* cs) const { GEOSCoordSeq_destroy(cs); }
};
using UniqueCoordSeq = std::unique_ptr<GEOSCoordSequence, CoordSeqDeleter>;

struct GeosFreeDeleter {
  void operator()(void* p) const { GEOSFree(p); }
};
using UniqueCStr = std::unique_ptr<char, GeosFreeDeleter>;
using UniqueBytes = std::unique_ptr<unsigned char, GeosFreeDeleter>;

struct PreparedGeomDeleter {
  void operator()(const GEOSPreparedGeometry* pg) const {
    GEOSPreparedGeom_destroy(pg);
  }
};
using UniquePreparedGeom =
    std::unique_ptr<const GEOSPreparedGeometry, PreparedGeomDeleter>;

struct BufferParamsDeleter {
  void operator()(GEOSBufferParams* p) const { GEOSBufferParams_destroy(p); }
};
using UniqueBufferParams =
    std::unique_ptr<GEOSBufferParams, BufferParamsDeleter>;

struct MakeValidParamsDeleter {
  void operator()(GEOSMakeValidParams* p) const {
    GEOSMakeValidParams_destroy(p);
  }
};
using UniqueMakeValidParams =
    std::unique_ptr<GEOSMakeValidParams, MakeValidParamsDeleter>;

struct CoverageCleanParamsDeleter {
  void operator()(GEOSCoverageCleanParams* p) const {
    GEOSCoverageCleanParams_destroy(p);
  }
};
using UniqueCoverageCleanParams =
    std::unique_ptr<GEOSCoverageCleanParams, CoverageCleanParamsDeleter>;

struct ClusterInfoDeleter {
  void operator()(GEOSClusterInfo* c) const { GEOSClusterInfo_destroy(c); }
};
using UniqueClusterInfo = std::unique_ptr<GEOSClusterInfo, ClusterInfoDeleter>;

struct STRtreeDeleter {
  void operator()(GEOSSTRtree* t) const { GEOSSTRtree_destroy(t); }
};
using UniqueSTRtree = std::unique_ptr<GEOSSTRtree, STRtreeDeleter>;

struct WKTReaderDeleter {
  void operator()(GEOSWKTReader* r) const { GEOSWKTReader_destroy(r); }
};
using UniqueWKTReader = std::unique_ptr<GEOSWKTReader, WKTReaderDeleter>;

struct WKTWriterDeleter {
  void operator()(GEOSWKTWriter* w) const { GEOSWKTWriter_destroy(w); }
};
using UniqueWKTWriter = std::unique_ptr<GEOSWKTWriter, WKTWriterDeleter>;

struct WKBReaderDeleter {
  void operator()(GEOSWKBReader* r) const { GEOSWKBReader_destroy(r); }
};
using UniqueWKBReader = std::unique_ptr<GEOSWKBReader, WKBReaderDeleter>;

struct WKBWriterDeleter {
  void operator()(GEOSWKBWriter* w) const { GEOSWKBWriter_destroy(w); }
};
using UniqueWKBWriter = std::unique_ptr<GEOSWKBWriter, WKBWriterDeleter>;

struct GeoJSONReaderDeleter {
  void operator()(GEOSGeoJSONReader* r) const { GEOSGeoJSONReader_destroy(r); }
};
using UniqueGeoJSONReader =
    std::unique_ptr<GEOSGeoJSONReader, GeoJSONReaderDeleter>;

struct GeoJSONWriterDeleter {
  void operator()(GEOSGeoJSONWriter* w) const { GEOSGeoJSONWriter_destroy(w); }
};
using UniqueGeoJSONWriter =
    std::unique_ptr<GEOSGeoJSONWriter, GeoJSONWriterDeleter>;

int& InterruptCallbackCount() {
  static int count = 0;
  return count;
}

void CountingCallback() { ++InterruptCallbackCount(); }

void RequestingCallback() {
  ++InterruptCallbackCount();
  GEOS_interruptRequest();
}

class GeosCTest : public ::testing::Test {
 protected:
  void SetUp() override {
    NoticeLog().clear();
    ErrorLog().clear();
    initGEOS(&NoticeHandler, &ErrorHandler);
  }

  void TearDown() override {
    GEOS_interruptRegisterCallback(nullptr);
    GEOS_interruptCancel();
    finishGEOS();
  }

  UniqueGeom ReadWKT(const char* wkt) {
    UniqueWKTReader reader(GEOSWKTReader_create());
    return UniqueGeom(GEOSWKTReader_read(reader.get(), wkt));
  }
};

TEST(GeosCInitAndInterruptTest, InitFinishHandlersFreeAndInterrupts) {
  NoticeLog().clear();
  ErrorLog().clear();
  InterruptCallbackCount() = 0;

  // finishGEOS before initGEOS exercises the handle == NULL branch safely.
  finishGEOS();

  initGEOS(&NoticeHandler, &ErrorHandler);
  const absl::Cleanup cleanup = [] {
    GEOS_interruptRegisterCallback(nullptr);
    GEOS_interruptCancel();
    finishGEOS();
    // Second finishGEOS exercises the handle == NULL branch after teardown.
    finishGEOS();
  };

  {
    // Re-initializing when handle is already non-null updates handlers.
    initGEOS(&AltNoticeHandler, &AltErrorHandler);

    UniqueGeom bowtie(
        GEOSGeomFromWKT("POLYGON ((0 0, 10 10, 0 10, 10 0, 0 0))"));
    ASSERT_NE(bowtie, nullptr);
    EXPECT_EQ(GEOSisValid(bowtie.get()), 0);
    ASSERT_FALSE(NoticeLog().empty());
    EXPECT_THAT(NoticeLog().back(), HasSubstr("ALT:"));

    UniqueWKTReader reader(GEOSWKTReader_create());
    EXPECT_EQ(GEOSWKTReader_read(reader.get(), "NOT VALID WKT"), nullptr);
    ASSERT_FALSE(ErrorLog().empty());
    EXPECT_THAT(ErrorLog().back(), HasSubstr("ALT:"));

    // GEOSFree on nullptr and on allocated strings.
    GEOSFree(nullptr);
    char* wkt = GEOSGeomToWKT(bowtie.get());
    ASSERT_NE(wkt, nullptr);
    GEOSFree(wkt);

    // Interrupt request cancelled before overlay allows completion.
    UniqueGeom poly1(GEOSGeom_createRectangle(0.0, 0.0, 10.0, 10.0));
    UniqueGeom poly2(GEOSGeom_createRectangle(5.0, 5.0, 15.0, 15.0));
    ASSERT_NE(poly1, nullptr);
    ASSERT_NE(poly2, nullptr);

    GEOS_interruptRegisterCallback(nullptr);
    GEOS_interruptRequest();
    GEOS_interruptCancel();
    UniqueGeom inter_ok(GEOSIntersection(poly1.get(), poly2.get()));
    EXPECT_NE(inter_ok, nullptr);

    // Register counting callback; returns cleanly without aborting.
    EXPECT_EQ(GEOS_interruptRegisterCallback(&CountingCallback), nullptr);
    UniqueGeom inter_counted(GEOSIntersection(poly1.get(), poly2.get()));
    EXPECT_NE(inter_counted, nullptr);
    EXPECT_GT(InterruptCallbackCount(), 0);

    // Register requesting callback; aborts OverlayNG during GEOSIntersection.
    ErrorLog().clear();
    EXPECT_EQ(GEOS_interruptRegisterCallback(&RequestingCallback),
              &CountingCallback);
    UniqueGeom inter_aborted(GEOSIntersection(poly1.get(), poly2.get()));
    EXPECT_EQ(inter_aborted, nullptr);
    ASSERT_FALSE(ErrorLog().empty());
    EXPECT_THAT(ErrorLog().back(), HasSubstr("Interrupted"));

    EXPECT_EQ(GEOS_interruptRegisterCallback(nullptr), &RequestingCallback);
    GEOS_interruptCancel();
  }
}

TEST_F(GeosCTest, CoordinateSequenceOperations) {
  // XYZM sequence via GEOSCoordSeq_create(size, dims=4).
  UniqueCoordSeq seq4(GEOSCoordSeq_create(3, 4));
  ASSERT_NE(seq4, nullptr);
  unsigned int size = 0;
  unsigned int dims = 0;
  EXPECT_EQ(GEOSCoordSeq_getSize(seq4.get(), &size), 1);
  EXPECT_EQ(size, 3u);
  EXPECT_EQ(GEOSCoordSeq_getDimensions(seq4.get(), &dims), 1);
  EXPECT_EQ(dims, 4u);
  EXPECT_EQ(GEOSCoordSeq_hasZ(seq4.get()), 1);
  EXPECT_EQ(GEOSCoordSeq_hasM(seq4.get()), 1);

  EXPECT_EQ(GEOSCoordSeq_setX(seq4.get(), 0, 1.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setY(seq4.get(), 0, 2.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setZ(seq4.get(), 0, 3.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setM(seq4.get(), 0, 4.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setXY(seq4.get(), 1, 5.0, 6.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setOrdinate(seq4.get(), 1, 2, 7.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setOrdinate(seq4.get(), 1, 3, 8.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setXYZ(seq4.get(), 2, 9.0, 10.0, 11.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setM(seq4.get(), 2, 12.0), 1);

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double m = 0.0;
  EXPECT_EQ(GEOSCoordSeq_getX(seq4.get(), 0, &x), 1);
  EXPECT_EQ(GEOSCoordSeq_getY(seq4.get(), 0, &y), 1);
  EXPECT_EQ(GEOSCoordSeq_getZ(seq4.get(), 0, &z), 1);
  EXPECT_EQ(GEOSCoordSeq_getM(seq4.get(), 0, &m), 1);
  EXPECT_DOUBLE_EQ(x, 1.0);
  EXPECT_DOUBLE_EQ(y, 2.0);
  EXPECT_DOUBLE_EQ(z, 3.0);
  EXPECT_DOUBLE_EQ(m, 4.0);

  EXPECT_EQ(GEOSCoordSeq_getXY(seq4.get(), 1, &x, &y), 1);
  EXPECT_DOUBLE_EQ(x, 5.0);
  EXPECT_DOUBLE_EQ(y, 6.0);
  EXPECT_EQ(GEOSCoordSeq_getOrdinate(seq4.get(), 1, 3, &m), 1);
  EXPECT_DOUBLE_EQ(m, 8.0);

  EXPECT_EQ(GEOSCoordSeq_getXYZ(seq4.get(), 2, &x, &y, &z), 1);
  EXPECT_DOUBLE_EQ(x, 9.0);
  EXPECT_DOUBLE_EQ(y, 10.0);
  EXPECT_DOUBLE_EQ(z, 11.0);

  // Clone and verify independence.
  UniqueCoordSeq cloned(GEOSCoordSeq_clone(seq4.get()));
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(GEOSCoordSeq_setX(cloned.get(), 0, 99.0), 1);
  EXPECT_EQ(GEOSCoordSeq_getX(seq4.get(), 0, &x), 1);
  EXPECT_DOUBLE_EQ(x, 1.0);

  // Copy to/from buffer in XYZM, XYM, and XY formats.
  double buf_xyzm[12]{};
  EXPECT_EQ(GEOSCoordSeq_copyToBuffer(seq4.get(), buf_xyzm, 1, 1), 1);
  EXPECT_DOUBLE_EQ(buf_xyzm[0], 1.0);
  EXPECT_DOUBLE_EQ(buf_xyzm[3], 4.0);
  EXPECT_DOUBLE_EQ(buf_xyzm[11], 12.0);

  UniqueCoordSeq from_xyzm(GEOSCoordSeq_copyFromBuffer(buf_xyzm, 3, 1, 1));
  ASSERT_NE(from_xyzm, nullptr);
  EXPECT_EQ(GEOSCoordSeq_getM(from_xyzm.get(), 1, &m), 1);
  EXPECT_DOUBLE_EQ(m, 8.0);

  double buf_xym[9]{};
  EXPECT_EQ(GEOSCoordSeq_copyToBuffer(seq4.get(), buf_xym, 0, 1), 1);
  UniqueCoordSeq from_xym(GEOSCoordSeq_copyFromBuffer(buf_xym, 3, 0, 1));
  ASSERT_NE(from_xym, nullptr);
  EXPECT_EQ(GEOSCoordSeq_hasZ(from_xym.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_hasM(from_xym.get()), 1);

  double buf_xy[6]{};
  EXPECT_EQ(GEOSCoordSeq_copyToBuffer(seq4.get(), buf_xy, 0, 0), 1);
  UniqueCoordSeq from_xy(GEOSCoordSeq_copyFromBuffer(buf_xy, 3, 0, 0));
  ASSERT_NE(from_xy, nullptr);
  EXPECT_EQ(GEOSCoordSeq_hasZ(from_xy.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_hasM(from_xy.get()), 0);

  // Copy from/to separate arrays.
  const double xs[2] = {10.0, 20.0};
  const double ys[2] = {30.0, 40.0};
  const double zs[2] = {50.0, 60.0};
  const double ms[2] = {70.0, 80.0};
  UniqueCoordSeq from_arrays(GEOSCoordSeq_copyFromArrays(xs, ys, zs, ms, 2));
  ASSERT_NE(from_arrays, nullptr);
  double ox[2]{};
  double oy[2]{};
  double oz[2]{};
  double om[2]{};
  EXPECT_EQ(GEOSCoordSeq_copyToArrays(from_arrays.get(), ox, oy, oz, om), 1);
  EXPECT_DOUBLE_EQ(ox[1], 20.0);
  EXPECT_DOUBLE_EQ(oy[1], 40.0);
  EXPECT_DOUBLE_EQ(oz[1], 60.0);
  EXPECT_DOUBLE_EQ(om[1], 80.0);

  // createWithDimensions (XY, XYM, XYZ).
  UniqueCoordSeq seq_xym(GEOSCoordSeq_createWithDimensions(2, 0, 1));
  ASSERT_NE(seq_xym, nullptr);
  EXPECT_EQ(GEOSCoordSeq_hasZ(seq_xym.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_hasM(seq_xym.get()), 1);

  // isCCW on CCW vs CW ring and short sequence.
  const double ccw_buf[] = {0.0,  0.0, 10.0, 0.0, 10.0,
                            10.0, 0.0, 10.0, 0.0, 0.0};
  UniqueCoordSeq ccw_seq(GEOSCoordSeq_copyFromBuffer(ccw_buf, 5, 0, 0));
  char is_ccw = 0;
  EXPECT_EQ(GEOSCoordSeq_isCCW(ccw_seq.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 1);

  const double cw_buf[] = {0.0,  0.0,  0.0, 10.0, 10.0,
                           10.0, 10.0, 0.0, 0.0,  0.0};
  UniqueCoordSeq cw_seq(GEOSCoordSeq_copyFromBuffer(cw_buf, 5, 0, 0));
  EXPECT_EQ(GEOSCoordSeq_isCCW(cw_seq.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 0);

  EXPECT_EQ(GEOSCoordSeq_isCCW(from_xy.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 0);

  // Explicit destroy on nullptr is a safe no-op.
  GEOSCoordSeq_destroy(nullptr);
}

TEST_F(GeosCTest, GeometryConstructorsCurvedTypesAndCollectionRelease) {
  // Point from CoordSeq and from XY.
  UniqueCoordSeq pt_cs(GEOSCoordSeq_createWithDimensions(1, 1, 1));
  ASSERT_NE(pt_cs, nullptr);
  EXPECT_EQ(GEOSCoordSeq_setXYZ(pt_cs.get(), 0, 1.5, 2.5, 3.5), 1);
  EXPECT_EQ(GEOSCoordSeq_setM(pt_cs.get(), 0, 4.5), 1);
  UniqueGeom pt4d(GEOSGeom_createPoint(pt_cs.release()));
  ASSERT_NE(pt4d, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(pt4d.get()), GEOS_POINT);
  EXPECT_EQ(GEOSHasZ(pt4d.get()), 1);
  EXPECT_EQ(GEOSHasM(pt4d.get()), 1);

  UniqueGeom pt_xy(GEOSGeom_createPointFromXY(3.0, 4.0));
  ASSERT_NE(pt_xy, nullptr);
  EXPECT_EQ(GEOSHasZ(pt_xy.get()), 0);
  EXPECT_EQ(GEOSHasM(pt_xy.get()), 0);

  // LineString & LinearRing & Polygon with hole.
  const double line_buf[] = {0.0, 0.0, 3.0, 4.0};
  UniqueGeom line(GEOSGeom_createLineString(
      GEOSCoordSeq_copyFromBuffer(line_buf, 2, 0, 0)));
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(line.get()), GEOS_LINESTRING);

  const double outer_buf[] = {0.0,  0.0, 10.0, 0.0, 10.0,
                              10.0, 0.0, 10.0, 0.0, 0.0};
  UniqueGeom shell(GEOSGeom_createLinearRing(
      GEOSCoordSeq_copyFromBuffer(outer_buf, 5, 0, 0)));
  ASSERT_NE(shell, nullptr);
  EXPECT_EQ(GEOSisRing(shell.get()), 1);
  EXPECT_EQ(GEOSisRing(line.get()), 0);

  const double hole_buf[] = {2.0, 2.0, 2.0, 8.0, 8.0, 8.0, 8.0, 2.0, 2.0, 2.0};
  UniqueGeom hole(GEOSGeom_createLinearRing(
      GEOSCoordSeq_copyFromBuffer(hole_buf, 5, 0, 0)));
  ASSERT_NE(hole, nullptr);
  GEOSGeometry* holes[] = {hole.release()};
  UniqueGeom poly(GEOSGeom_createPolygon(shell.release(), holes, 1));
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(poly.get()), GEOS_POLYGON);
  double poly_area = 0.0;
  EXPECT_EQ(GEOSArea(poly.get(), &poly_area), 1);
  EXPECT_DOUBLE_EQ(poly_area, 64.0);

  // Rectangle & clone.
  UniqueGeom rect(GEOSGeom_createRectangle(1.0, 2.0, 5.0, 7.0));
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(rect.get()), GEOS_POLYGON);
  UniqueGeom rect_clone(GEOSGeom_clone(rect.get()));
  ASSERT_NE(rect_clone, nullptr);
  EXPECT_EQ(GEOSEqualsIdentical(rect.get(), rect_clone.get()), 1);

  // CircularString, CompoundCurve, CurvePolygon.
  const double semi_buf[] = {0.0, 0.0, 1.0, 1.0, 2.0, 0.0};
  UniqueGeom arc(GEOSGeom_createCircularString(
      GEOSCoordSeq_copyFromBuffer(semi_buf, 3, 0, 0)));
  ASSERT_NE(arc, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(arc.get()), GEOS_CIRCULARSTRING);

  const double close_seg_buf[] = {2.0, 0.0, 0.0, 0.0};
  UniqueGeom close_seg(GEOSGeom_createLineString(
      GEOSCoordSeq_copyFromBuffer(close_seg_buf, 2, 0, 0)));
  ASSERT_NE(close_seg, nullptr);
  GEOSGeometry* curves[] = {arc.release(), close_seg.release()};
  UniqueGeom comp_curve(GEOSGeom_createCompoundCurve(curves, 2));
  ASSERT_NE(comp_curve, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(comp_curve.get()), GEOS_COMPOUNDCURVE);
  EXPECT_EQ(GEOSisClosed(comp_curve.get()), 1);

  const double outer_circle_buf[] = {4.0, 0.0, 0.0,  4.0, -4.0,
                                     0.0, 0.0, -4.0, 4.0, 0.0};
  const double inner_circle_buf[] = {1.0, 0.0, 0.0, -1.0, -1.0,
                                     0.0, 0.0, 1.0, 1.0,  0.0};
  UniqueGeom curve_shell(GEOSGeom_createCircularString(
      GEOSCoordSeq_copyFromBuffer(outer_circle_buf, 5, 0, 0)));
  UniqueGeom curve_hole(GEOSGeom_createCircularString(
      GEOSCoordSeq_copyFromBuffer(inner_circle_buf, 5, 0, 0)));
  GEOSGeometry* curve_holes[] = {curve_hole.release()};
  UniqueGeom curve_poly(
      GEOSGeom_createCurvePolygon(curve_shell.release(), curve_holes, 1));
  ASSERT_NE(curve_poly, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(curve_poly.get()), GEOS_CURVEPOLYGON);
  EXPECT_EQ(GEOSOrientPolygons(curve_poly.get(), 1), -1);

  // Empty constructors.
  UniqueGeom emp_pt(GEOSGeom_createEmptyPoint());
  UniqueGeom emp_ls(GEOSGeom_createEmptyLineString());
  UniqueGeom emp_poly(GEOSGeom_createEmptyPolygon());
  UniqueGeom emp_cs(GEOSGeom_createEmptyCircularString());
  UniqueGeom emp_cc(GEOSGeom_createEmptyCompoundCurve());
  UniqueGeom emp_cp(GEOSGeom_createEmptyCurvePolygon());
  UniqueGeom emp_col(GEOSGeom_createEmptyCollection(GEOS_MULTICURVE));
  for (const GEOSGeometry* eg :
       {emp_pt.get(), emp_ls.get(), emp_poly.get(), emp_cs.get(), emp_cc.get(),
        emp_cp.get(), emp_col.get()}) {
    ASSERT_NE(eg, nullptr);
    EXPECT_EQ(GEOSisEmpty(eg), 1);
  }
  EXPECT_EQ(GEOSGeomTypeId(emp_cs.get()), GEOS_CIRCULARSTRING);
  EXPECT_EQ(GEOSGeomTypeId(emp_cc.get()), GEOS_COMPOUNDCURVE);
  EXPECT_EQ(GEOSGeomTypeId(emp_cp.get()), GEOS_CURVEPOLYGON);
  EXPECT_EQ(GEOSGeomTypeId(emp_col.get()), GEOS_MULTICURVE);

  // Collection creation & releaseCollection.
  GEOSGeometry* col_items[] = {GEOSGeom_createPointFromXY(1.0, 1.0),
                               GEOSGeom_createPointFromXY(2.0, 2.0)};
  UniqueGeom mp(GEOSGeom_createCollection(GEOS_MULTIPOINT, col_items, 2));
  ASSERT_NE(mp, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(mp.get()), 2);

  unsigned int released_count = 0;
  GEOSGeometry** released =
      GEOSGeom_releaseCollection(mp.get(), &released_count);
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released_count, 2u);
  EXPECT_EQ(GEOSisEmpty(mp.get()), 1);
  for (unsigned int i = 0; i < released_count; ++i) {
    GEOSGeom_destroy(released[i]);
  }
  GEOSFree(released);

  // Release on empty collection returns nullptr and ngeoms = 0.
  EXPECT_EQ(GEOSGeom_releaseCollection(mp.get(), &released_count), nullptr);
  EXPECT_EQ(released_count, 0u);
}

TEST_F(GeosCTest, GeometryAccessorsPrecisionOrientationAndExtent) {
  const double buf[] = {1.0, 2.0, 3.0, 4.0};
  UniqueGeom pt(
      GEOSGeom_createPoint(GEOSCoordSeq_copyFromBuffer(buf, 1, 1, 1)));
  ASSERT_NE(pt, nullptr);

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double m = 0.0;
  EXPECT_EQ(GEOSGeomGetX(pt.get(), &x), 1);
  EXPECT_EQ(GEOSGeomGetY(pt.get(), &y), 1);
  EXPECT_EQ(GEOSGeomGetZ(pt.get(), &z), 1);
  EXPECT_EQ(GEOSGeomGetM(pt.get(), &m), 1);
  EXPECT_DOUBLE_EQ(x, 1.0);
  EXPECT_DOUBLE_EQ(y, 2.0);
  EXPECT_DOUBLE_EQ(z, 3.0);
  EXPECT_DOUBLE_EQ(m, 4.0);
  EXPECT_EQ(GEOSGeom_getDimensions(pt.get()), 0);
  EXPECT_EQ(GEOSGeom_getCoordinateDimension(pt.get()), 4);

  UniqueGeom pt2d(GEOSGeom_createPointFromXY(5.0, 6.0));
  EXPECT_EQ(GEOSGeomGetZ(pt2d.get(), &z), 1);
  EXPECT_TRUE(std::isnan(z));
  EXPECT_EQ(GEOSGeomGetM(pt2d.get(), &m), 1);
  EXPECT_TRUE(std::isnan(m));
  EXPECT_EQ(GEOSGeom_getCoordinateDimension(pt2d.get()), 2);

  // Type name, SRID, UserData, CoordSeq RO accessor.
  UniqueCStr pt_type(GEOSGeomType(pt.get()));
  ASSERT_NE(pt_type, nullptr);
  EXPECT_STREQ(pt_type.get(), "Point");
  EXPECT_EQ(GEOSGetSRID(pt.get()), 0);
  GEOSSetSRID(pt.get(), 4326);
  EXPECT_EQ(GEOSGetSRID(pt.get()), 4326);

  int tag = 77;
  EXPECT_EQ(GEOSGeom_getUserData(pt.get()), nullptr);
  GEOSGeom_setUserData(pt.get(), &tag);
  EXPECT_EQ(GEOSGeom_getUserData(pt.get()), &tag);
  EXPECT_NE(GEOSGeom_getCoordSeq(pt.get()), nullptr);

  // LineString accessors.
  UniqueGeom line(ReadWKT("LINESTRING (0 0, 3 0, 3 4)"));
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(GEOSGeom_getDimensions(line.get()), 1);
  EXPECT_EQ(GEOSGeomGetNumPoints(line.get()), 3);
  EXPECT_EQ(GEOSGetNumCoordinates(line.get()), 3);
  EXPECT_EQ(GEOSisClosed(line.get()), 0);
  double line_len = 0.0;
  EXPECT_EQ(GEOSGeomGetLength(line.get(), &line_len), 1);
  EXPECT_DOUBLE_EQ(line_len, 7.0);
  EXPECT_EQ(GEOSGeomGetX(line.get(), &x), 0);  // Error on non-Point.

  UniqueGeom p_mid(GEOSGeomGetPointN(line.get(), 1));
  UniqueGeom p_start(GEOSGeomGetStartPoint(line.get()));
  UniqueGeom p_end(GEOSGeomGetEndPoint(line.get()));
  ASSERT_NE(p_mid, nullptr);
  ASSERT_NE(p_start, nullptr);
  ASSERT_NE(p_end, nullptr);
  EXPECT_EQ(GEOSGeomGetX(p_mid.get(), &x), 1);
  EXPECT_EQ(GEOSGeomGetY(p_mid.get(), &y), 1);
  EXPECT_DOUBLE_EQ(x, 3.0);
  EXPECT_DOUBLE_EQ(y, 0.0);
  EXPECT_EQ(GEOSGeomGetX(p_start.get(), &x), 1);
  EXPECT_DOUBLE_EQ(x, 0.0);
  EXPECT_EQ(GEOSGeomGetY(p_end.get(), &y), 1);
  EXPECT_DOUBLE_EQ(y, 4.0);

  // Extent accessors.
  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  EXPECT_EQ(GEOSGeom_getXMin(line.get(), &xmin), 1);
  EXPECT_EQ(GEOSGeom_getYMin(line.get(), &ymin), 1);
  EXPECT_EQ(GEOSGeom_getXMax(line.get(), &xmax), 1);
  EXPECT_EQ(GEOSGeom_getYMax(line.get(), &ymax), 1);
  EXPECT_DOUBLE_EQ(xmin, 0.0);
  EXPECT_DOUBLE_EQ(ymin, 0.0);
  EXPECT_DOUBLE_EQ(xmax, 3.0);
  EXPECT_DOUBLE_EQ(ymax, 4.0);
  EXPECT_EQ(GEOSGeom_getExtent(line.get(), &xmin, &ymin, &xmax, &ymax), 1);
  EXPECT_DOUBLE_EQ(xmax, 3.0);
  EXPECT_DOUBLE_EQ(ymax, 4.0);

  UniqueGeom empty_line(GEOSGeom_createEmptyLineString());
  EXPECT_EQ(GEOSGeom_getXMin(empty_line.get(), &xmin), 0);
  EXPECT_EQ(GEOSGeom_getExtent(empty_line.get(), &xmin, &ymin, &xmax, &ymax),
            0);

  // Polygon rings, OrientPolygons, Normalize, Collection subgeometry.
  UniqueGeom poly(ReadWKT(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 4, 2 2))"));
  ASSERT_NE(poly, nullptr);
  EXPECT_EQ(GEOSGeom_getDimensions(poly.get()), 2);
  EXPECT_EQ(GEOSGetNumInteriorRings(poly.get()), 1);
  const GEOSGeometry* ext_ring = GEOSGetExteriorRing(poly.get());
  const GEOSGeometry* int_ring = GEOSGetInteriorRingN(poly.get(), 0);
  ASSERT_NE(ext_ring, nullptr);
  ASSERT_NE(int_ring, nullptr);
  EXPECT_EQ(GEOSGeomGetNumPoints(ext_ring), 5);
  EXPECT_EQ(GEOSGeomGetNumPoints(int_ring), 5);

  EXPECT_EQ(GEOSOrientPolygons(poly.get(), 1), 0);
  char ccw = 0;
  EXPECT_EQ(GEOSCoordSeq_isCCW(GEOSGeom_getCoordSeq(ext_ring), &ccw), 1);
  EXPECT_EQ(ccw, 0);  // Exterior CW
  EXPECT_EQ(GEOSCoordSeq_isCCW(GEOSGeom_getCoordSeq(int_ring), &ccw), 1);
  EXPECT_EQ(ccw, 1);  // Interior CCW

  EXPECT_EQ(GEOSOrientPolygons(poly.get(), 0), 0);
  EXPECT_EQ(GEOSCoordSeq_isCCW(GEOSGeom_getCoordSeq(ext_ring), &ccw), 1);
  EXPECT_EQ(ccw, 1);  // Exterior CCW
  EXPECT_EQ(GEOSNormalize(poly.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_isCCW(GEOSGeom_getCoordSeq(ext_ring), &ccw), 1);
  EXPECT_EQ(ccw, 0);  // Normalized exterior CW

  UniqueGeom mp(ReadWKT("MULTIPOINT ((5 5), (1 1))"));
  EXPECT_EQ(GEOSGetNumGeometries(mp.get()), 2);
  const GEOSGeometry* sub = GEOSGetGeometryN(mp.get(), 1);
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(GEOSGeomGetX(sub, &x), 1);
  EXPECT_DOUBLE_EQ(x, 1.0);

  // Precision get/set.
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision(poly.get()), 0.0);
  UniqueGeom poly_prec(
      GEOSGeom_setPrecision(poly.get(), 0.5, GEOS_PREC_VALID_OUTPUT));
  ASSERT_NE(poly_prec, nullptr);
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision(poly_prec.get()), 0.5);

  UniqueGeom short_line(ReadWKT("LINESTRING (0 0, 0.2 0.2)"));
  UniqueGeom collapsed_removed(
      GEOSGeom_setPrecision(short_line.get(), 1.0, GEOS_PREC_VALID_OUTPUT));
  EXPECT_EQ(GEOSisEmpty(collapsed_removed.get()), 1);
  UniqueGeom collapsed_kept(
      GEOSGeom_setPrecision(short_line.get(), 1.0, GEOS_PREC_KEEP_COLLAPSED));
  EXPECT_EQ(GEOSisEmpty(collapsed_kept.get()), 0);
}

int ScaleXYCallback(double* x, double* y, void* userdata) {
  const double factor = *static_cast<const double*>(userdata);
  if (factor < 0.0) {
    return 0;
  }
  *x *= factor;
  *y *= factor;
  return 1;
}

int OffsetXYZCallback(double* x, double* y, double* z, void* userdata) {
  const double dz = *static_cast<const double*>(userdata);
  if (std::isnan(dz)) {
    return 0;
  }
  *x += 1.0;
  *y += 2.0;
  *z += dz;
  return 1;
}

TEST_F(GeosCTest, SpatialPredicatesEqualsAndRelate) {
  UniqueGeom outer(ReadWKT("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"));
  UniqueGeom inner(ReadWKT("POLYGON ((2 2, 8 2, 8 8, 2 8, 2 2))"));
  UniqueGeom touching(ReadWKT("POLYGON ((10 0, 20 0, 20 10, 10 10, 10 0))"));
  UniqueGeom overlapping(ReadWKT("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))"));
  UniqueGeom disjoint_box(
      ReadWKT("POLYGON ((100 100, 110 100, 110 110, 100 110, 100 100))"));
  UniqueGeom line_cross(ReadWKT("LINESTRING (-1 5, 11 5)"));
  UniqueGeom line_bnd(ReadWKT("LINESTRING (0 0, 10 0)"));

  EXPECT_EQ(GEOSDisjoint(outer.get(), disjoint_box.get()), 1);
  EXPECT_EQ(GEOSDisjoint(outer.get(), inner.get()), 0);

  EXPECT_EQ(GEOSTouches(outer.get(), touching.get()), 1);
  EXPECT_EQ(GEOSTouches(outer.get(), inner.get()), 0);

  EXPECT_EQ(GEOSIntersects(outer.get(), inner.get()), 1);
  EXPECT_EQ(GEOSIntersects(outer.get(), disjoint_box.get()), 0);

  EXPECT_EQ(GEOSCrosses(line_cross.get(), outer.get()), 1);
  EXPECT_EQ(GEOSCrosses(outer.get(), inner.get()), 0);

  EXPECT_EQ(GEOSWithin(inner.get(), outer.get()), 1);
  EXPECT_EQ(GEOSWithin(line_bnd.get(), outer.get()), 0);

  EXPECT_EQ(GEOSContains(outer.get(), inner.get()), 1);
  EXPECT_EQ(GEOSContains(inner.get(), outer.get()), 0);

  EXPECT_EQ(GEOSOverlaps(outer.get(), overlapping.get()), 1);
  EXPECT_EQ(GEOSOverlaps(outer.get(), inner.get()), 0);

  EXPECT_EQ(GEOSCovers(outer.get(), line_bnd.get()), 1);
  EXPECT_EQ(GEOSCoveredBy(line_bnd.get(), outer.get()), 1);
  EXPECT_EQ(GEOSCovers(inner.get(), outer.get()), 0);
  EXPECT_EQ(GEOSCoveredBy(outer.get(), inner.get()), 0);

  // Equality variants.
  UniqueGeom outer_reordered(
      ReadWKT("POLYGON ((10 0, 10 10, 0 10, 0 0, 10 0))"));
  EXPECT_EQ(GEOSEquals(outer.get(), outer_reordered.get()), 1);
  EXPECT_EQ(GEOSEqualsExact(outer.get(), outer_reordered.get(), 1e-6), 0);
  UniqueGeom outer_shifted(
      ReadWKT("POLYGON ((0 0, 10.001 0, 10 10, 0 10, 0 0))"));
  EXPECT_EQ(GEOSEqualsExact(outer.get(), outer_shifted.get(), 0.01), 1);
  EXPECT_EQ(GEOSEqualsExact(outer.get(), outer_shifted.get(), 0.0001), 0);
  EXPECT_EQ(GEOSEqualsIdentical(outer.get(), outer.get()), 1);
  EXPECT_EQ(GEOSEqualsIdentical(outer.get(), outer_reordered.get()), 0);

  UniqueGeom pt_z1(ReadWKT("POINT Z (1 2 3)"));
  UniqueGeom pt_z2(ReadWKT("POINT Z (1 2 9)"));
  EXPECT_EQ(GEOSEqualsExact(pt_z1.get(), pt_z2.get(), 0.0), 1);
  EXPECT_EQ(GEOSEqualsIdentical(pt_z1.get(), pt_z2.get()), 0);

  // DE-9IM Relate & Boundary Node Rules.
  UniqueCStr im(GEOSRelate(inner.get(), outer.get()));
  ASSERT_NE(im, nullptr);
  EXPECT_STREQ(im.get(), "2FF1FF212");
  EXPECT_EQ(GEOSRelatePatternMatch(im.get(), "2FF*FF2*2"), 1);
  EXPECT_EQ(GEOSRelatePatternMatch(im.get(), "FF*FF****"), 0);
  EXPECT_EQ(GEOSRelatePatternMatch(im.get(), "INVALID"), 2);

  EXPECT_EQ(GEOSRelatePattern(inner.get(), outer.get(), "T*F**F***"), 1);
  EXPECT_EQ(GEOSRelatePattern(outer.get(), disjoint_box.get(), "T********"), 0);

  UniqueGeom two_segs(ReadWKT("MULTILINESTRING ((0 0, 5 0), (5 0, 10 0))"));
  UniqueGeom join_pt(ReadWKT("POINT (5 0)"));
  UniqueCStr im_mod2(GEOSRelateBoundaryNodeRule(two_segs.get(), join_pt.get(),
                                                GEOSRELATE_BNR_MOD2));
  UniqueCStr im_endpt(GEOSRelateBoundaryNodeRule(two_segs.get(), join_pt.get(),
                                                 GEOSRELATE_BNR_ENDPOINT));
  UniqueCStr im_multi(GEOSRelateBoundaryNodeRule(
      two_segs.get(), join_pt.get(), GEOSRELATE_BNR_MULTIVALENT_ENDPOINT));
  UniqueCStr im_mono(GEOSRelateBoundaryNodeRule(
      two_segs.get(), join_pt.get(), GEOSRELATE_BNR_MONOVALENT_ENDPOINT));
  ASSERT_NE(im_mod2, nullptr);
  ASSERT_NE(im_endpt, nullptr);
  ASSERT_NE(im_multi, nullptr);
  ASSERT_NE(im_mono, nullptr);
  EXPECT_EQ(im_mod2.get()[0], '0');   // (5,0) is in interior under MOD2.
  EXPECT_EQ(im_endpt.get()[3], '0');  // (5,0) is boundary under ENDPOINT.
  EXPECT_EQ(GEOSRelateBoundaryNodeRule(two_segs.get(), join_pt.get(), 99),
            nullptr);
}

TEST_F(GeosCTest, ValiditySimplicityAndClearance) {
  UniqueGeom valid_poly(ReadWKT("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"));
  UniqueGeom self_touch_hole(
      ReadWKT("POLYGON ((0 0, 5 0, 3 3, 7 3, 5 0, 10 0, 10 10, 0 10, 0 0))"));

  EXPECT_EQ(GEOSisValid(valid_poly.get()), 1);
  EXPECT_EQ(GEOSisValid(self_touch_hole.get()), 0);

  UniqueCStr valid_reason(GEOSisValidReason(valid_poly.get()));
  ASSERT_NE(valid_reason, nullptr);
  EXPECT_STREQ(valid_reason.get(), "Valid Geometry");

  UniqueCStr invalid_reason(GEOSisValidReason(self_touch_hole.get()));
  ASSERT_NE(invalid_reason, nullptr);
  EXPECT_NE(std::string(invalid_reason.get()), "Valid Geometry");

  char* detail_reason_raw = nullptr;
  GEOSGeometry* detail_loc_raw = nullptr;
  EXPECT_EQ(GEOSisValidDetail(self_touch_hole.get(), 0, &detail_reason_raw,
                              &detail_loc_raw),
            0);
  UniqueCStr detail_reason(detail_reason_raw);
  UniqueGeom detail_loc(detail_loc_raw);
  EXPECT_NE(detail_reason, nullptr);
  EXPECT_NE(detail_loc, nullptr);

  char* allowed_reason_raw = nullptr;
  GEOSGeometry* allowed_loc_raw = nullptr;
  EXPECT_EQ(GEOSisValidDetail(self_touch_hole.get(),
                              GEOSVALID_ALLOW_SELFTOUCHING_RING_FORMING_HOLE,
                              &allowed_reason_raw, &allowed_loc_raw),
            1);
  EXPECT_EQ(allowed_reason_raw, nullptr);
  EXPECT_EQ(allowed_loc_raw, nullptr);

  // Simplicity checks.
  UniqueGeom simple_line(ReadWKT("LINESTRING (0 0, 5 5, 10 0)"));
  UniqueGeom nonsimple_lines(
      ReadWKT("MULTILINESTRING ((0 0, 10 10), (0 10, 10 0), (20 0, 30 10), (20 "
              "10, 30 0))"));
  EXPECT_EQ(GEOSisSimple(simple_line.get()), 1);
  EXPECT_EQ(GEOSisSimple(nonsimple_lines.get()), 0);

  GEOSGeometry* simple_loc_raw = nullptr;
  EXPECT_EQ(GEOSisSimpleDetail(simple_line.get(), 1, &simple_loc_raw), 1);
  EXPECT_EQ(simple_loc_raw, nullptr);

  GEOSGeometry* one_loc_raw = nullptr;
  EXPECT_EQ(GEOSisSimpleDetail(nonsimple_lines.get(), 0, &one_loc_raw), 0);
  UniqueGeom one_loc(one_loc_raw);
  ASSERT_NE(one_loc, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(one_loc.get()), GEOS_POINT);

  GEOSGeometry* all_loc_raw = nullptr;
  EXPECT_EQ(GEOSisSimpleDetail(nonsimple_lines.get(), 1, &all_loc_raw), 0);
  UniqueGeom all_loc(all_loc_raw);
  ASSERT_NE(all_loc, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(all_loc.get()), GEOS_MULTIPOINT);
  EXPECT_EQ(GEOSGetNumGeometries(all_loc.get()), 2);

  // Minimum clearance.
  UniqueGeom mp(ReadWKT("MULTIPOINT ((0 0), (3 4), (100 100))"));
  double clearance = 0.0;
  EXPECT_EQ(GEOSMinimumClearance(mp.get(), &clearance), 0);
  EXPECT_DOUBLE_EQ(clearance, 5.0);
  UniqueGeom mc_line(GEOSMinimumClearanceLine(mp.get()));
  ASSERT_NE(mc_line, nullptr);
  double mc_len = 0.0;
  EXPECT_EQ(GEOSLength(mc_line.get(), &mc_len), 1);
  EXPECT_DOUBLE_EQ(mc_len, 5.0);

  UniqueGeom single_pt(GEOSGeom_createPointFromXY(1.0, 2.0));
  EXPECT_EQ(GEOSMinimumClearance(single_pt.get(), &clearance), 0);
  EXPECT_TRUE(std::isinf(clearance));
  UniqueGeom empty_mc(GEOSMinimumClearanceLine(single_pt.get()));
  EXPECT_EQ(GEOSisEmpty(empty_mc.get()), 1);
}

TEST_F(GeosCTest, MetricsDistancesLinearRefAndAlgorithms) {
  UniqueGeom poly(ReadWKT("POLYGON ((0 0, 4 0, 4 3, 0 3, 0 0))"));
  double area = 0.0;
  double length = 0.0;
  EXPECT_EQ(GEOSArea(poly.get(), &area), 1);
  EXPECT_EQ(GEOSLength(poly.get(), &length), 1);
  EXPECT_DOUBLE_EQ(area, 12.0);
  EXPECT_DOUBLE_EQ(length, 14.0);

  UniqueGeom p1(ReadWKT("POINT (0 0)"));
  UniqueGeom p2(ReadWKT("POINT (3 4)"));
  double dist = 0.0;
  EXPECT_EQ(GEOSDistance(p1.get(), p2.get(), &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 5.0);
  EXPECT_EQ(GEOSDistanceWithin(p1.get(), p2.get(), 5.0), 1);
  EXPECT_EQ(GEOSDistanceWithin(p1.get(), p2.get(), 4.9), 0);
  EXPECT_EQ(GEOSDistanceIndexed(p1.get(), p2.get(), &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 5.0);

  UniqueGeom l1(ReadWKT("LINESTRING (0 0, 10 0)"));
  UniqueGeom l2(ReadWKT("LINESTRING (0 2, 10 2)"));
  EXPECT_EQ(GEOSHausdorffDistance(l1.get(), l2.get(), &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 2.0);
  EXPECT_EQ(GEOSHausdorffDistanceDensify(l1.get(), l2.get(), 0.25, &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 2.0);
  EXPECT_EQ(GEOSFrechetDistance(l1.get(), l2.get(), &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 2.0);
  EXPECT_EQ(GEOSFrechetDistanceDensify(l1.get(), l2.get(), 0.25, &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 2.0);

  UniqueCoordSeq nearest(GEOSNearestPoints(l1.get(), l2.get()));
  ASSERT_NE(nearest, nullptr);
  unsigned int nsz = 0;
  EXPECT_EQ(GEOSCoordSeq_getSize(nearest.get(), &nsz), 1);
  EXPECT_EQ(nsz, 2u);

  // Linear referencing.
  UniqueGeom query_pt(ReadWKT("POINT (3 5)"));
  EXPECT_DOUBLE_EQ(GEOSProject(l1.get(), query_pt.get()), 3.0);
  EXPECT_DOUBLE_EQ(GEOSProjectNormalized(l1.get(), query_pt.get()), 0.3);
  EXPECT_DOUBLE_EQ(GEOSProject(l1.get(), l2.get()), -1.0);  // Not a Point.

  UniqueGeom interp(GEOSInterpolate(l1.get(), 7.0));
  ASSERT_NE(interp, nullptr);
  double ix = 0.0;
  double iy = 0.0;
  EXPECT_EQ(GEOSGeomGetX(interp.get(), &ix), 1);
  EXPECT_EQ(GEOSGeomGetY(interp.get(), &iy), 1);
  EXPECT_DOUBLE_EQ(ix, 7.0);
  EXPECT_DOUBLE_EQ(iy, 0.0);

  UniqueGeom interp_norm(GEOSInterpolateNormalized(l1.get(), 0.5));
  ASSERT_NE(interp_norm, nullptr);
  EXPECT_EQ(GEOSGeomGetX(interp_norm.get(), &ix), 1);
  EXPECT_DOUBLE_EQ(ix, 5.0);

  // OrientationIndex and SegmentIntersection.
  EXPECT_EQ(GEOSOrientationIndex(0.0, 0.0, 10.0, 0.0, 5.0, 5.0), 1);    // Left
  EXPECT_EQ(GEOSOrientationIndex(0.0, 0.0, 10.0, 0.0, 5.0, -5.0), -1);  // Right
  EXPECT_EQ(GEOSOrientationIndex(0.0, 0.0, 10.0, 0.0, 5.0, 0.0), 0);  // On line
  EXPECT_EQ(GEOSOrientationIndex(0.0, 0.0, 10.0, 0.0,
                                 std::numeric_limits<double>::quiet_NaN(), 0.0),
            2);

  double cx = 0.0;
  double cy = 0.0;
  EXPECT_EQ(GEOSSegmentIntersection(0.0, 0.0, 10.0, 10.0, 0.0, 10.0, 10.0, 0.0,
                                    &cx, &cy),
            1);
  EXPECT_DOUBLE_EQ(cx, 5.0);
  EXPECT_DOUBLE_EQ(cy, 5.0);
  EXPECT_EQ(
      GEOSSegmentIntersection(0.0, 0.0, 1.0, 0.0, 2.0, 1.0, 3.0, 1.0, &cx, &cy),
      -1);

  // Hilbert code.
  UniqueGeom extent(GEOSGeom_createRectangle(0.0, 0.0, 16.0, 16.0));
  unsigned int hcode0 = 0;
  unsigned int hcode1 = 0;
  UniqueGeom near_origin(GEOSGeom_createPointFromXY(1.0, 1.0));
  UniqueGeom far_corner(GEOSGeom_createPointFromXY(15.0, 1.0));
  EXPECT_EQ(GEOSHilbertCode(near_origin.get(), extent.get(), 4, &hcode0), 1);
  EXPECT_EQ(GEOSHilbertCode(far_corner.get(), extent.get(), 4, &hcode1), 1);
  EXPECT_LT(hcode0, hcode1);

  // transformXY & transformXYZ.
  UniqueGeom pt3d(ReadWKT("POINT ZM (2 3 5 9)"));
  double scale = 3.0;
  UniqueGeom scaled(GEOSGeom_transformXY(pt3d.get(), &ScaleXYCallback, &scale));
  ASSERT_NE(scaled, nullptr);
  double sx = 0.0;
  double sy = 0.0;
  double sz = 0.0;
  double sm = 0.0;
  EXPECT_EQ(GEOSGeomGetX(scaled.get(), &sx), 1);
  EXPECT_EQ(GEOSGeomGetY(scaled.get(), &sy), 1);
  EXPECT_EQ(GEOSGeomGetZ(scaled.get(), &sz), 1);
  EXPECT_EQ(GEOSGeomGetM(scaled.get(), &sm), 1);
  EXPECT_DOUBLE_EQ(sx, 6.0);
  EXPECT_DOUBLE_EQ(sy, 9.0);
  EXPECT_DOUBLE_EQ(sz, 5.0);
  EXPECT_DOUBLE_EQ(sm, 9.0);

  double bad_scale = -1.0;
  EXPECT_EQ(GEOSGeom_transformXY(pt3d.get(), &ScaleXYCallback, &bad_scale),
            nullptr);

  double dz = 10.0;
  UniqueGeom shifted_z(
      GEOSGeom_transformXYZ(pt3d.get(), &OffsetXYZCallback, &dz));
  ASSERT_NE(shifted_z, nullptr);
  EXPECT_EQ(GEOSGeomGetX(shifted_z.get(), &sx), 1);
  EXPECT_EQ(GEOSGeomGetY(shifted_z.get(), &sy), 1);
  EXPECT_EQ(GEOSGeomGetZ(shifted_z.get(), &sz), 1);
  EXPECT_DOUBLE_EQ(sx, 3.0);
  EXPECT_DOUBLE_EQ(sy, 5.0);
  EXPECT_DOUBLE_EQ(sz, 15.0);

  double bad_dz = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(GEOSGeom_transformXYZ(pt3d.get(), &OffsetXYZCallback, &bad_dz),
            nullptr);
}

TEST_F(GeosCTest, ClusteringOperations) {
  UniqueGeom pts(
      ReadWKT("MULTIPOINT ((0 0), (1 0), (20 20), (50 50), (51 50))"));
  UniqueClusterInfo dbscan(GEOSClusterDBSCAN(pts.get(), 2.0, 2));
  ASSERT_NE(dbscan, nullptr);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters(dbscan.get()), 2u);
  EXPECT_EQ(GEOSClusterInfo_getClusterSize(dbscan.get(), 0), 2u);
  const std::size_t* inputs0 =
      GEOSClusterInfo_getInputsForClusterN(dbscan.get(), 0);
  ASSERT_NE(inputs0, nullptr);
  EXPECT_EQ(inputs0[0], 0u);
  EXPECT_EQ(inputs0[1], 1u);

  std::size_t* input_clusters =
      GEOSClusterInfo_getClustersForInputs(dbscan.get());
  ASSERT_NE(input_clusters, nullptr);
  EXPECT_EQ(input_clusters[0], 0u);
  EXPECT_EQ(input_clusters[1], 0u);
  EXPECT_EQ(input_clusters[2], GEOS_CLUSTER_NONE);
  EXPECT_EQ(input_clusters[3], 1u);
  EXPECT_EQ(input_clusters[4], 1u);
  GEOSFree(input_clusters);

  UniqueClusterInfo geom_dist(GEOSClusterGeometryDistance(pts.get(), 2.0));
  ASSERT_NE(geom_dist, nullptr);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters(geom_dist.get()), 3u);

  // L-shaped polygon whose bounding box contains a disjoint inner box.
  UniqueGeom shapes(
      ReadWKT("GEOMETRYCOLLECTION ("
              "POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0)), "
              "POLYGON ((6 6, 8 6, 8 8, 6 8, 6 6)))"));
  UniqueClusterInfo env_isect(GEOSClusterEnvelopeIntersects(shapes.get()));
  ASSERT_NE(env_isect, nullptr);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters(env_isect.get()), 1u);

  UniqueClusterInfo geom_isect(GEOSClusterGeometryIntersects(shapes.get()));
  ASSERT_NE(geom_isect, nullptr);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters(geom_isect.get()), 2u);

  UniqueClusterInfo env_dist(GEOSClusterEnvelopeDistance(shapes.get(), 1.0));
  ASSERT_NE(env_dist, nullptr);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters(env_dist.get()), 1u);
}

TEST_F(GeosCTest, OverlayBufferOffsetCurvesAndGridOperations) {
  UniqueGeom poly1(GEOSGeom_createRectangle(0.0, 0.0, 10.0, 10.0));
  UniqueGeom poly2(GEOSGeom_createRectangle(5.0, 5.0, 15.0, 15.0));
  ASSERT_NE(poly1, nullptr);
  ASSERT_NE(poly2, nullptr);

  // Envelope and Boundary.
  UniqueGeom diag(ReadWKT("LINESTRING (1 2, 4 6)"));
  UniqueGeom env(GEOSEnvelope(diag.get()));
  ASSERT_NE(env, nullptr);
  double area = 0.0;
  EXPECT_EQ(GEOSArea(env.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 12.0);

  UniqueGeom square4(GEOSGeom_createRectangle(0.0, 0.0, 4.0, 4.0));
  UniqueGeom boundary(GEOSBoundary(square4.get()));
  ASSERT_NE(boundary, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(boundary.get()), GEOS_LINESTRING);
  double len = 0.0;
  EXPECT_EQ(GEOSLength(boundary.get(), &len), 1);
  EXPECT_DOUBLE_EQ(len, 16.0);

  // Set operations and *_Prec variants.
  UniqueGeom inter(GEOSIntersection(poly1.get(), poly2.get()));
  ASSERT_NE(inter, nullptr);
  EXPECT_EQ(GEOSArea(inter.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 25.0);

  UniqueGeom inter_prec0(GEOSIntersectionPrec(poly1.get(), poly2.get(), 0.0));
  ASSERT_NE(inter_prec0, nullptr);
  EXPECT_EQ(GEOSArea(inter_prec0.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 25.0);

  UniqueGeom fine_box(GEOSGeom_createRectangle(0.1, 0.1, 1.9, 1.9));
  UniqueGeom inter_prec(GEOSIntersectionPrec(poly1.get(), fine_box.get(), 0.5));
  ASSERT_NE(inter_prec, nullptr);
  EXPECT_EQ(GEOSArea(inter_prec.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 4.0);

  UniqueGeom diff(GEOSDifference(poly1.get(), poly2.get()));
  ASSERT_NE(diff, nullptr);
  EXPECT_EQ(GEOSArea(diff.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 75.0);

  UniqueGeom diff_prec(GEOSDifferencePrec(poly1.get(), poly2.get(), 1.0));
  ASSERT_NE(diff_prec, nullptr);
  EXPECT_EQ(GEOSArea(diff_prec.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 75.0);

  UniqueGeom symdiff(GEOSSymDifference(poly1.get(), poly2.get()));
  ASSERT_NE(symdiff, nullptr);
  EXPECT_EQ(GEOSArea(symdiff.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 150.0);

  UniqueGeom symdiff_prec(GEOSSymDifferencePrec(poly1.get(), poly2.get(), 1.0));
  ASSERT_NE(symdiff_prec, nullptr);
  EXPECT_EQ(GEOSArea(symdiff_prec.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 150.0);

  UniqueGeom uni(GEOSUnion(poly1.get(), poly2.get()));
  ASSERT_NE(uni, nullptr);
  EXPECT_EQ(GEOSArea(uni.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 175.0);

  UniqueGeom uni_prec(GEOSUnionPrec(poly1.get(), poly2.get(), 1.0));
  ASSERT_NE(uni_prec, nullptr);
  EXPECT_EQ(GEOSArea(uni_prec.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 175.0);

  // UnaryUnion, UnaryUnionPrec, UnionCascaded, DisjointSubsetUnion.
  UniqueGeom overlap_mp(
      ReadWKT("MULTIPOLYGON ("
              "((0 0, 10 0, 10 10, 0 10, 0 0)), "
              "((5 0, 15 0, 15 10, 5 10, 5 0)))"));
  UniqueGeom unary(GEOSUnaryUnion(overlap_mp.get()));
  ASSERT_NE(unary, nullptr);
  EXPECT_EQ(GEOSArea(unary.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 150.0);

  UniqueGeom unary_prec(GEOSUnaryUnionPrec(overlap_mp.get(), 1.0));
  ASSERT_NE(unary_prec, nullptr);
  EXPECT_EQ(GEOSArea(unary_prec.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 150.0);

  UniqueGeom cascaded(GEOSUnionCascaded(overlap_mp.get()));
  ASSERT_NE(cascaded, nullptr);
  EXPECT_EQ(GEOSArea(cascaded.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 150.0);
  // GEOSUnionCascaded requires a MultiPolygon.
  EXPECT_EQ(GEOSUnionCascaded(poly1.get()), nullptr);

  UniqueGeom disjoint_mp(
      ReadWKT("MULTIPOLYGON ("
              "((0 0, 4 0, 4 4, 0 4, 0 0)), "
              "((10 10, 14 10, 14 14, 10 14, 10 10)))"));
  UniqueGeom disjoint_union(GEOSDisjointSubsetUnion(disjoint_mp.get()));
  ASSERT_NE(disjoint_union, nullptr);
  EXPECT_EQ(GEOSArea(disjoint_union.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 32.0);

  // PointOnSurface, GetCentroid, MinimumBoundingCircle.
  UniqueGeom lshape(
      ReadWKT("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))"));
  UniqueGeom pos(GEOSPointOnSurface(lshape.get()));
  ASSERT_NE(pos, nullptr);
  EXPECT_EQ(GEOSWithin(pos.get(), lshape.get()), 1);

  UniqueGeom centroid(GEOSGetCentroid(square4.get()));
  ASSERT_NE(centroid, nullptr);
  double cx = 0.0;
  double cy = 0.0;
  EXPECT_EQ(GEOSGeomGetX(centroid.get(), &cx), 1);
  EXPECT_EQ(GEOSGeomGetY(centroid.get(), &cy), 1);
  EXPECT_DOUBLE_EQ(cx, 2.0);
  EXPECT_DOUBLE_EQ(cy, 2.0);

  double mbc_radius = 0.0;
  GEOSGeometry* mbc_center_raw = nullptr;
  UniqueGeom mbc(
      GEOSMinimumBoundingCircle(square4.get(), &mbc_radius, &mbc_center_raw));
  UniqueGeom mbc_center(mbc_center_raw);
  ASSERT_NE(mbc, nullptr);
  ASSERT_NE(mbc_center, nullptr);
  EXPECT_NEAR(mbc_radius, std::sqrt(8.0), 1e-6);
  EXPECT_EQ(GEOSGeomGetX(mbc_center.get(), &cx), 1);
  EXPECT_EQ(GEOSGeomGetY(mbc_center.get(), &cy), 1);
  EXPECT_DOUBLE_EQ(cx, 2.0);
  EXPECT_DOUBLE_EQ(cy, 2.0);

  // ClipByRect and GridIntersectionFractions.
  UniqueGeom clipped(GEOSClipByRect(poly1.get(), 2.0, 3.0, 7.0, 9.0));
  ASSERT_NE(clipped, nullptr);
  EXPECT_EQ(GEOSArea(clipped.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 30.0);

  UniqueGeom left_half(GEOSGeom_createRectangle(0.0, 0.0, 1.0, 2.0));
  std::vector<float> fractions(4, 0.0f);
  EXPECT_EQ(GEOSGridIntersectionFractions(left_half.get(), 0.0, 0.0, 2.0, 2.0,
                                          2, 2, fractions.data()),
            1);
  EXPECT_FLOAT_EQ(fractions[0], 1.0f);
  EXPECT_FLOAT_EQ(fractions[1], 0.0f);
  EXPECT_FLOAT_EQ(fractions[2], 1.0f);
  EXPECT_FLOAT_EQ(fractions[3], 0.0f);

  UniqueGeom pt(GEOSGeom_createPointFromXY(1.0, 1.0));
  EXPECT_EQ(GEOSGridIntersectionFractions(pt.get(), 0.0, 0.0, 2.0, 2.0, 2, 2,
                                          fractions.data()),
            0);

  // ExtractUniquePoints, Node, SharedPaths, Snap.
  UniqueGeom unique_pts(GEOSGeom_extractUniquePoints(square4.get()));
  ASSERT_NE(unique_pts, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(unique_pts.get()), 4);

  UniqueGeom cross(ReadWKT("MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))"));
  UniqueGeom noded(GEOSNode(cross.get()));
  ASSERT_NE(noded, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(noded.get()), 4);

  UniqueGeom line_base(ReadWKT("LINESTRING (0 0, 4 0)"));
  UniqueGeom line_same(ReadWKT("LINESTRING (1 0, 3 0)"));
  UniqueGeom line_opp(ReadWKT("LINESTRING (3 0, 1 0)"));
  UniqueGeom shared_same(GEOSSharedPaths(line_base.get(), line_same.get()));
  ASSERT_NE(shared_same, nullptr);
  ASSERT_EQ(GEOSGetNumGeometries(shared_same.get()), 2);
  EXPECT_EQ(GEOSLength(GEOSGetGeometryN(shared_same.get(), 0), &len), 1);
  EXPECT_DOUBLE_EQ(len, 2.0);
  UniqueGeom shared_opp(GEOSSharedPaths(line_base.get(), line_opp.get()));
  ASSERT_NE(shared_opp, nullptr);
  EXPECT_EQ(GEOSLength(GEOSGetGeometryN(shared_opp.get(), 1), &len), 1);
  EXPECT_DOUBLE_EQ(len, 2.0);

  UniqueGeom off_pt(GEOSGeom_createPointFromXY(0.05, 0.05));
  UniqueGeom origin(GEOSGeom_createPointFromXY(0.0, 0.0));
  UniqueGeom snapped(GEOSSnap(off_pt.get(), origin.get(), 0.2));
  ASSERT_NE(snapped, nullptr);
  EXPECT_EQ(GEOSEqualsExact(snapped.get(), origin.get(), 1e-12), 1);

  // Buffer, BufferWithStyle, SingleSidedBuffer, OffsetCurve, Densify,
  // BufferParams.
  UniqueGeom pt_buf(GEOSBuffer(origin.get(), 2.0, 16));
  ASSERT_NE(pt_buf, nullptr);
  EXPECT_EQ(GEOSArea(pt_buf.get(), &area), 1);
  EXPECT_GT(area, 12.4);
  EXPECT_LT(area, 12.6);

  UniqueGeom styled_buf(GEOSBufferWithStyle(
      line_base.get(), 1.0, 8, GEOSBUF_CAP_SQUARE, GEOSBUF_JOIN_MITRE, 2.0));
  ASSERT_NE(styled_buf, nullptr);
  EXPECT_EQ(GEOSArea(styled_buf.get(), &area), 1);
  EXPECT_NEAR(area, 12.0, 1e-6);
  EXPECT_EQ(
      GEOSBufferWithStyle(line_base.get(), 1.0, 8, 99, GEOSBUF_JOIN_ROUND, 2.0),
      nullptr);

  UniqueGeom ss_buf(GEOSSingleSidedBuffer(line_base.get(), 1.0, 8,
                                          GEOSBUF_JOIN_MITRE, 5.0, 1));
  ASSERT_NE(ss_buf, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(ss_buf.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSLength(ss_buf.get(), &len), 1);
  EXPECT_NEAR(len, 4.0, 1e-6);
  double ss_ymin = 0.0;
  double ss_ymax = 0.0;
  EXPECT_EQ(GEOSGeom_getYMin(ss_buf.get(), &ss_ymin), 1);
  EXPECT_EQ(GEOSGeom_getYMax(ss_buf.get(), &ss_ymax), 1);
  EXPECT_NEAR(ss_ymin, 1.0, 1e-6);
  EXPECT_NEAR(ss_ymax, 1.0, 1e-6);

  UniqueGeom ss_buf_right(GEOSSingleSidedBuffer(line_base.get(), 1.0, 8,
                                                GEOSBUF_JOIN_MITRE, 5.0, 0));
  ASSERT_NE(ss_buf_right, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(ss_buf_right.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSLength(ss_buf_right.get(), &len), 1);
  EXPECT_NEAR(len, 4.0, 1e-6);
  EXPECT_EQ(GEOSGeom_getYMin(ss_buf_right.get(), &ss_ymin), 1);
  EXPECT_NEAR(ss_ymin, -1.0, 1e-6);

  UniqueGeom offset(
      GEOSOffsetCurve(line_base.get(), 1.0, 8, GEOSBUF_JOIN_BEVEL, 5.0));
  ASSERT_NE(offset, nullptr);
  EXPECT_EQ(GEOSLength(offset.get(), &len), 1);
  EXPECT_NEAR(len, 4.0, 1e-6);

  UniqueGeom densified(GEOSDensify(line_base.get(), 1.0));
  ASSERT_NE(densified, nullptr);
  EXPECT_GE(GEOSGeomGetNumPoints(densified.get()), 5);

  UniqueBufferParams bparams(GEOSBufferParams_create());
  ASSERT_NE(bparams, nullptr);
  EXPECT_EQ(GEOSBufferParams_setEndCapStyle(bparams.get(), GEOSBUF_CAP_FLAT),
            1);
  EXPECT_EQ(GEOSBufferParams_setEndCapStyle(bparams.get(), 99), 0);
  EXPECT_EQ(GEOSBufferParams_setJoinStyle(bparams.get(), GEOSBUF_JOIN_ROUND),
            1);
  EXPECT_EQ(GEOSBufferParams_setJoinStyle(bparams.get(), 99), 0);
  EXPECT_EQ(GEOSBufferParams_setMitreLimit(bparams.get(), 3.0), 1);
  EXPECT_EQ(GEOSBufferParams_setQuadrantSegments(bparams.get(), 8), 1);
  EXPECT_EQ(GEOSBufferParams_setSingleSided(bparams.get(), 1), 1);
  UniqueGeom param_buf(
      GEOSBufferWithParams(line_base.get(), bparams.get(), 1.0));
  ASSERT_NE(param_buf, nullptr);
  EXPECT_EQ(GEOSArea(param_buf.get(), &area), 1);
  EXPECT_NEAR(area, 4.0, 1e-6);
}

TEST_F(GeosCTest, HullsSimplifyPolygonizeValidCoverageAndTriangulation) {
  UniqueGeom lshape(
      ReadWKT("POLYGON ((0 0, 10 0, 10 2, 2 2, 2 10, 0 10, 0 0))"));
  double area = 0.0;
  double len = 0.0;

  // ConvexHull, ConcaveHull, ConcaveHullByLength.
  UniqueGeom ch(GEOSConvexHull(lshape.get()));
  ASSERT_NE(ch, nullptr);
  EXPECT_EQ(GEOSArea(ch.get(), &area), 1);
  EXPECT_NEAR(area, 68.0, 1e-6);

  UniqueGeom hull_pts(
      ReadWKT("MULTIPOINT ((0 0), (10 0), (10 2), (2 2), (2 10), (0 10))"));
  UniqueGeom concave(GEOSConcaveHull(hull_pts.get(), 0.3, 0));
  ASSERT_NE(concave, nullptr);
  EXPECT_EQ(GEOSArea(concave.get(), &area), 1);
  EXPECT_GT(area, 0.0);

  UniqueGeom concave_len(GEOSConcaveHullByLength(hull_pts.get(), 5.0, 1));
  ASSERT_NE(concave_len, nullptr);
  EXPECT_EQ(GEOSisValid(concave_len.get()), 1);

  // PolygonHullSimplify, PolygonHullSimplifyMode, ConcaveHullOfPolygons.
  UniqueGeom bumpy(ReadWKT(
      "POLYGON ((0 0, 5 0, 5.2 1, 10 0, 10 10, 5 10, 5 9, 0 10, 0 0))"));
  UniqueGeom outer_hull(GEOSPolygonHullSimplify(bumpy.get(), 1, 0.5));
  ASSERT_NE(outer_hull, nullptr);
  EXPECT_EQ(GEOSCovers(outer_hull.get(), bumpy.get()), 1);

  UniqueGeom inner_hull(GEOSPolygonHullSimplifyMode(
      bumpy.get(), 0, GEOSHULL_PARAM_VERTEX_RATIO, 0.5));
  ASSERT_NE(inner_hull, nullptr);
  EXPECT_EQ(GEOSCovers(bumpy.get(), inner_hull.get()), 1);

  UniqueGeom area_hull(GEOSPolygonHullSimplifyMode(
      bumpy.get(), 1, GEOSHULL_PARAM_AREA_RATIO, 0.1));
  ASSERT_NE(area_hull, nullptr);
  EXPECT_EQ(GEOSCovers(area_hull.get(), bumpy.get()), 1);

  EXPECT_EQ(GEOSPolygonHullSimplifyMode(bumpy.get(), 1, 99, 0.5), nullptr);

  UniqueGeom two_boxes(
      ReadWKT("MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
              "((3 0, 5 0, 5 2, 3 2, 3 0)))"));
  UniqueGeom poly_hull(GEOSConcaveHullOfPolygons(two_boxes.get(), 0.5, 1, 0));
  ASSERT_NE(poly_hull, nullptr);
  EXPECT_EQ(GEOSArea(poly_hull.get(), &area), 1);
  EXPECT_GE(area, 8.0);

  // MinimumRotatedRectangle, MaximumInscribedCircle, LargestEmptyCircle,
  // MinimumWidth, Simplify, TopologyPreserveSimplify.
  UniqueGeom diamond(ReadWKT("POLYGON ((0 1, 1 0, 2 1, 1 2, 0 1))"));
  UniqueGeom mrr(GEOSMinimumRotatedRectangle(diamond.get()));
  ASSERT_NE(mrr, nullptr);
  EXPECT_EQ(GEOSArea(mrr.get(), &area), 1);
  EXPECT_NEAR(area, 2.0, 1e-6);

  UniqueGeom square4(GEOSGeom_createRectangle(0.0, 0.0, 4.0, 4.0));
  UniqueGeom mic(GEOSMaximumInscribedCircle(square4.get(), 0.01));
  ASSERT_NE(mic, nullptr);
  EXPECT_EQ(GEOSLength(mic.get(), &len), 1);
  EXPECT_NEAR(len, 2.0, 0.02);

  UniqueGeom corners(ReadWKT("MULTIPOINT ((0 0), (4 0), (4 4), (0 4))"));
  UniqueGeom lec(GEOSLargestEmptyCircle(corners.get(), square4.get(), 0.01));
  ASSERT_NE(lec, nullptr);
  EXPECT_EQ(GEOSLength(lec.get(), &len), 1);
  EXPECT_NEAR(len, std::sqrt(8.0), 0.02);

  UniqueGeom rect10x3(GEOSGeom_createRectangle(0.0, 0.0, 10.0, 3.0));
  UniqueGeom min_width(GEOSMinimumWidth(rect10x3.get()));
  ASSERT_NE(min_width, nullptr);
  EXPECT_EQ(GEOSLength(min_width.get(), &len), 1);
  EXPECT_NEAR(len, 3.0, 1e-6);

  UniqueGeom bendy(ReadWKT("LINESTRING (0 0, 1 0.1, 2 0)"));
  UniqueGeom simplified(GEOSSimplify(bendy.get(), 0.5));
  ASSERT_NE(simplified, nullptr);
  EXPECT_EQ(GEOSGeomGetNumPoints(simplified.get()), 2);

  UniqueGeom tp_poly(ReadWKT("POLYGON ((0 0, 5 0.1, 10 0, 10 10, 0 10, 0 0))"));
  UniqueGeom tp_simp(GEOSTopologyPreserveSimplify(tp_poly.get(), 0.5));
  ASSERT_NE(tp_simp, nullptr);
  EXPECT_EQ(GEOSisValid(tp_simp.get()), 1);

  // Polygonize, Polygonize_valid, Polygonizer_getCutEdges, Polygonize_full,
  // BuildArea.
  UniqueGeom edge1(ReadWKT("LINESTRING (0 0, 4 0, 4 2)"));
  UniqueGeom edge2(ReadWKT("LINESTRING (4 2, 4 4, 0 4, 0 0)"));
  UniqueGeom cut_bridge(ReadWKT("LINESTRING (4 2, 6 2)"));
  UniqueGeom loop2(ReadWKT("LINESTRING (6 2, 8 2, 8 4, 6 4, 6 2)"));
  UniqueGeom dangle(ReadWKT("LINESTRING (0 0, -2 0)"));
  UniqueGeom invalid_ring(
      ReadWKT("LINESTRING (10 10, 12 12, 10 12, 12 10, 10 10)"));

  const GEOSGeometry* box_edges[] = {edge1.get(), edge2.get()};
  UniqueGeom polygonized(GEOSPolygonize(box_edges, 2));
  ASSERT_NE(polygonized, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(polygonized.get()), 1);
  EXPECT_EQ(GEOSArea(polygonized.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 16.0);

  UniqueGeom polygonized_valid(GEOSPolygonize_valid(box_edges, 2));
  ASSERT_NE(polygonized_valid, nullptr);
  EXPECT_EQ(GEOSArea(polygonized_valid.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 16.0);

  const GEOSGeometry* cut_inputs[] = {edge1.get(), edge2.get(),
                                      cut_bridge.get(), loop2.get()};
  UniqueGeom cuts_only(GEOSPolygonizer_getCutEdges(cut_inputs, 4));
  ASSERT_NE(cuts_only, nullptr);
  EXPECT_GE(GEOSGetNumGeometries(cuts_only.get()), 1);

  GEOSGeometry* full_inputs_raw[] = {
      GEOSGeom_clone(edge1.get()),      GEOSGeom_clone(edge2.get()),
      GEOSGeom_clone(cut_bridge.get()), GEOSGeom_clone(loop2.get()),
      GEOSGeom_clone(dangle.get()),     GEOSGeom_clone(invalid_ring.get())};
  UniqueGeom full_collection(
      GEOSGeom_createCollection(GEOS_GEOMETRYCOLLECTION, full_inputs_raw, 6));
  ASSERT_NE(full_collection, nullptr);
  GEOSGeometry* cuts_raw = nullptr;
  GEOSGeometry* dangles_raw = nullptr;
  GEOSGeometry* invalid_raw = nullptr;
  UniqueGeom full_polys(GEOSPolygonize_full(full_collection.get(), &cuts_raw,
                                            &dangles_raw, &invalid_raw));
  UniqueGeom cuts(cuts_raw);
  UniqueGeom dangles(dangles_raw);
  UniqueGeom invalid(invalid_raw);
  ASSERT_NE(full_polys, nullptr);
  ASSERT_NE(cuts, nullptr);
  ASSERT_NE(dangles, nullptr);
  ASSERT_NE(invalid, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(full_polys.get()), 2);
  EXPECT_GE(GEOSGetNumGeometries(cuts.get()), 1);
  EXPECT_GE(GEOSGetNumGeometries(dangles.get()), 1);

  UniqueGeom donut_lines(
      ReadWKT("MULTILINESTRING ((0 0, 4 0, 4 4, 0 4, 0 0), "
              "(1 1, 3 1, 3 3, 1 3, 1 1))"));
  UniqueGeom built_area(GEOSBuildArea(donut_lines.get()));
  ASSERT_NE(built_area, nullptr);
  EXPECT_EQ(GEOSArea(built_area.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 12.0);

  // MakeValid, MakeValidParams, MakeValidWithParams.
  UniqueGeom bowtie(ReadWKT("POLYGON ((0 0, 4 4, 0 4, 4 0, 0 0))"));
  UniqueGeom fixed_default(GEOSMakeValid(bowtie.get()));
  ASSERT_NE(fixed_default, nullptr);
  EXPECT_EQ(GEOSisValid(fixed_default.get()), 1);
  EXPECT_EQ(GEOSArea(fixed_default.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 8.0);

  UniqueMakeValidParams mv_params(GEOSMakeValidParams_create());
  ASSERT_NE(mv_params, nullptr);
  EXPECT_EQ(
      GEOSMakeValidParams_setMethod(mv_params.get(), GEOS_MAKE_VALID_STRUCTURE),
      1);
  EXPECT_EQ(GEOSMakeValidParams_setKeepCollapsed(mv_params.get(), 1), 1);
  UniqueGeom fixed_struct(
      GEOSMakeValidWithParams(bowtie.get(), mv_params.get()));
  ASSERT_NE(fixed_struct, nullptr);
  EXPECT_EQ(GEOSisValid(fixed_struct.get()), 1);

  EXPECT_EQ(
      GEOSMakeValidParams_setMethod(mv_params.get(), GEOS_MAKE_VALID_LINEWORK),
      1);
  EXPECT_EQ(GEOSMakeValidParams_setKeepCollapsed(mv_params.get(), 0), 1);
  UniqueGeom fixed_linework(
      GEOSMakeValidWithParams(bowtie.get(), mv_params.get()));
  ASSERT_NE(fixed_linework, nullptr);
  EXPECT_EQ(GEOSisValid(fixed_linework.get()), 1);
  EXPECT_EQ(GEOSArea(fixed_linework.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 8.0);
  EXPECT_EQ(GEOSMakeValidWithParams(bowtie.get(), nullptr), nullptr);

  // RemoveRepeatedPoints, LineMerge, LineMergeDirected, LineSubstring, Reverse.
  UniqueGeom dup_line(ReadWKT("LINESTRING (0 0, 0 0, 2 0, 2.05 0, 4 0)"));
  UniqueGeom deduped(GEOSRemoveRepeatedPoints(dup_line.get(), 0.1));
  ASSERT_NE(deduped, nullptr);
  EXPECT_EQ(GEOSGeomGetNumPoints(deduped.get()), 3);

  UniqueGeom opp_lines(ReadWKT("MULTILINESTRING ((0 0, 2 0), (4 0, 2 0))"));
  UniqueGeom merged_undirected(GEOSLineMerge(opp_lines.get()));
  ASSERT_NE(merged_undirected, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(merged_undirected.get()), GEOS_LINESTRING);

  UniqueGeom merged_directed(GEOSLineMergeDirected(opp_lines.get()));
  ASSERT_NE(merged_directed, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(merged_directed.get()), 2);

  UniqueGeom long_line(ReadWKT("LINESTRING (0 0, 10 0)"));
  UniqueGeom sub(GEOSLineSubstring(long_line.get(), 0.25, 0.75));
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(GEOSLength(sub.get(), &len), 1);
  EXPECT_DOUBLE_EQ(len, 5.0);
  EXPECT_EQ(GEOSLineSubstring(long_line.get(), -0.1, 0.5), nullptr);

  UniqueGeom reversed(GEOSReverse(long_line.get()));
  ASSERT_NE(reversed, nullptr);
  UniqueGeom rev_start(GEOSGeomGetStartPoint(reversed.get()));
  double sx = 0.0;
  EXPECT_EQ(GEOSGeomGetX(rev_start.get(), &sx), 1);
  EXPECT_DOUBLE_EQ(sx, 10.0);

  // DelaunayTriangulation, ConstrainedDelaunayTriangulation, VoronoiDiagram.
  UniqueGeom del_polys(GEOSDelaunayTriangulation(corners.get(), 0.0, 0));
  ASSERT_NE(del_polys, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(del_polys.get()), 2);
  EXPECT_EQ(GEOSArea(del_polys.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 16.0);

  UniqueGeom del_edges(GEOSDelaunayTriangulation(corners.get(), 0.0, 1));
  ASSERT_NE(del_edges, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(del_edges.get()), 5);

  UniqueGeom cdt(GEOSConstrainedDelaunayTriangulation(lshape.get()));
  ASSERT_NE(cdt, nullptr);
  EXPECT_EQ(GEOSArea(cdt.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 36.0);

  // VoronoiDiagramBuilder expands the sites envelope [0,4]x[0,4] by
  // max(width, height)=4 to [-4,8]x[-4,8] (area 144), and clips to the union
  // of that expanded envelope and clip_env.
  UniqueGeom clip_env(GEOSGeom_createRectangle(-2.0, -2.0, 6.0, 6.0));
  UniqueGeom vor_polys(
      GEOSVoronoiDiagram(corners.get(), clip_env.get(), 0.0, 0));
  ASSERT_NE(vor_polys, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(vor_polys.get()), 4);
  EXPECT_EQ(GEOSArea(vor_polys.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 144.0);

  UniqueGeom large_env(GEOSGeom_createRectangle(-10.0, -10.0, 10.0, 10.0));
  UniqueGeom vor_polys_large(
      GEOSVoronoiDiagram(corners.get(), large_env.get(), 0.0, 0));
  ASSERT_NE(vor_polys_large, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(vor_polys_large.get()), 4);
  EXPECT_EQ(GEOSArea(vor_polys_large.get(), &area), 1);
  EXPECT_NEAR(area, 400.0, 1e-6);

  UniqueGeom vor_edges(
      GEOSVoronoiDiagram(corners.get(), nullptr, 0.0, GEOS_VORONOI_ONLY_EDGES));
  ASSERT_NE(vor_edges, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(vor_edges.get()), GEOS_MULTILINESTRING);

  UniqueGeom dup_sites(ReadWKT("MULTIPOINT ((0 0), (0 0), (4 0), (0 4))"));
  EXPECT_EQ(GEOSVoronoiDiagram(dup_sites.get(), clip_env.get(), 0.0,
                               GEOS_VORONOI_PRESERVE_ORDER),
            nullptr);

  // Coverage operations.
  UniqueGeom valid_cov(
      ReadWKT("GEOMETRYCOLLECTION ("
              "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
              "POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0)))"));
  GEOSGeometry* invalid_edges_raw = nullptr;
  EXPECT_EQ(GEOSCoverageIsValid(valid_cov.get(), 0.0, &invalid_edges_raw), 1);
  UniqueGeom invalid_edges_ok(invalid_edges_raw);
  EXPECT_NE(invalid_edges_ok, nullptr);

  UniqueGeom cov_union(GEOSCoverageUnion(valid_cov.get()));
  ASSERT_NE(cov_union, nullptr);
  EXPECT_EQ(GEOSArea(cov_union.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 8.0);

  UniqueGeom cov_simp(GEOSCoverageSimplifyVW(valid_cov.get(), 0.5, 1));
  ASSERT_NE(cov_simp, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(cov_simp.get()), 2);
  UniqueGeom cov_simp_outer(GEOSCoverageSimplifyVW(valid_cov.get(), 0.5, 0));
  ASSERT_NE(cov_simp_outer, nullptr);
  EXPECT_EQ(GEOSCoverageSimplifyVW(valid_cov.get(), 0.5, 2), nullptr);

  UniqueGeom dirty_cov(
      ReadWKT("GEOMETRYCOLLECTION ("
              "POLYGON ((0 0, 2.2 0, 2.2 2, 0 2, 0 0)), "
              "POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0)))"));
  GEOSGeometry* dirty_edges_raw = nullptr;
  EXPECT_EQ(GEOSCoverageIsValid(dirty_cov.get(), 0.0, &dirty_edges_raw), 0);
  UniqueGeom dirty_edges(dirty_edges_raw);
  EXPECT_NE(dirty_edges, nullptr);

  UniqueGeom pt(GEOSGeom_createPointFromXY(0.0, 0.0));
  GEOSGeometry* bad_cov_edges = nullptr;
  EXPECT_EQ(GEOSCoverageIsValid(pt.get(), 0.0, &bad_cov_edges), 2);
  EXPECT_EQ(bad_cov_edges, nullptr);

  UniqueGeom cleaned(GEOSCoverageClean(dirty_cov.get()));
  ASSERT_NE(cleaned, nullptr);
  EXPECT_EQ(GEOSCoverageIsValid(cleaned.get(), 0.0, nullptr), 1);

  UniqueCoverageCleanParams cc_params(GEOSCoverageCleanParams_create());
  ASSERT_NE(cc_params, nullptr);
  EXPECT_EQ(GEOSCoverageCleanParams_setSnappingDistance(cc_params.get(), 0.05),
            1);
  EXPECT_EQ(GEOSCoverageCleanParams_setGapMaximumWidth(cc_params.get(), 0.2),
            1);
  EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy(
                cc_params.get(), GEOS_MERGE_MAX_AREA),
            1);
  EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy(
                cc_params.get(), GEOS_MERGE_MIN_AREA),
            1);
  EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy(
                cc_params.get(), GEOS_MERGE_MIN_INDEX),
            1);
  EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy(
                cc_params.get(), GEOS_MERGE_LONGEST_BORDER),
            1);
  EXPECT_EQ(
      GEOSCoverageCleanParams_setOverlapMergeStrategy(cc_params.get(), 99), 0);

  UniqueGeom cleaned_params(
      GEOSCoverageCleanWithParams(dirty_cov.get(), cc_params.get()));
  ASSERT_NE(cleaned_params, nullptr);
  EXPECT_EQ(GEOSCoverageIsValid(cleaned_params.get(), 0.0, nullptr), 1);
  EXPECT_EQ(GEOSCoverageCleanWithParams(pt.get(), cc_params.get()), nullptr);
}

void CollectGeomCallback(void* item, void* userdata) {
  auto* items = static_cast<std::vector<const GEOSGeometry*>*>(userdata);
  items->push_back(static_cast<const GEOSGeometry*>(item));
}

struct CustomBox {
  double x;
  double y;
  int id;
};

int CustomBoxDistance(const void* item1, const void* item2, double* distance,
                      void* userdata) {
  const auto* a = static_cast<const CustomBox*>(item1);
  const auto* b = static_cast<const CustomBox*>(item2);
  const double scale = userdata ? *static_cast<const double*>(userdata) : 1.0;
  const double dx = a->x - b->x;
  const double dy = a->y - b->y;
  *distance = std::hypot(dx, dy) * scale;
  return 1;
}

int FailingDistance(const void* /*item1*/, const void* /*item2*/,
                    double* /*distance*/, void* /*userdata*/) {
  return 0;
}

TEST_F(GeosCTest, WktWkbHexGeoJsonReadersWritersAndPrintDouble) {
  // Version string.
  const char* ver = GEOSversion();
  ASSERT_NE(ver, nullptr);
  EXPECT_GT(std::strlen(ver), 0u);

  // Legacy WKT functions: GEOSGeomToWKT forces 2D and trim=false.
  UniqueGeom pt3d(GEOSGeomFromWKT("POINT Z (1.5 2.5 3.5)"));
  ASSERT_NE(pt3d, nullptr);
  UniqueCStr legacy_wkt(GEOSGeomToWKT(pt3d.get()));
  ASSERT_NE(legacy_wkt, nullptr);
  EXPECT_THAT(std::string(legacy_wkt.get()), HasSubstr("POINT (1.500"));
  EXPECT_EQ(GEOSGeomFromWKT("INVALID WKT"), nullptr);

  // Global WKB output dims and byte order getters/setters.
  const int orig_dims = GEOS_getWKBOutputDims();
  const int orig_order = GEOS_getWKBByteOrder();
  EXPECT_GE(orig_dims, 2);

  EXPECT_EQ(GEOS_setWKBOutputDims(3), orig_dims);
  EXPECT_EQ(GEOS_getWKBOutputDims(), 3);
  EXPECT_EQ(GEOS_setWKBByteOrder(GEOS_WKB_XDR), orig_order);
  EXPECT_EQ(GEOS_getWKBByteOrder(), GEOS_WKB_XDR);

  std::size_t wkb_size = 0;
  UniqueBytes wkb_buf(GEOSGeomToWKB_buf(pt3d.get(), &wkb_size));
  ASSERT_NE(wkb_buf, nullptr);
  EXPECT_GT(wkb_size, 0u);
  EXPECT_EQ(wkb_buf.get()[0], 0);  // Big-endian XDR byte order marker.

  UniqueGeom from_wkb(GEOSGeomFromWKB_buf(wkb_buf.get(), wkb_size));
  ASSERT_NE(from_wkb, nullptr);
  double z = 0.0;
  EXPECT_EQ(GEOSGeomGetZ(from_wkb.get(), &z), 1);
  EXPECT_DOUBLE_EQ(z, 3.5);

  std::size_t hex_size = 0;
  UniqueBytes hex_buf(GEOSGeomToHEX_buf(pt3d.get(), &hex_size));
  ASSERT_NE(hex_buf, nullptr);
  EXPECT_GT(hex_size, 0u);
  UniqueGeom from_hex(GEOSGeomFromHEX_buf(hex_buf.get(), hex_size));
  ASSERT_NE(from_hex, nullptr);
  EXPECT_EQ(GEOSEqualsIdentical(from_wkb.get(), from_hex.get()), 1);

  // Restore global WKB settings (and test out-of-range dimension error log).
  ErrorLog().clear();
  EXPECT_EQ(GEOS_setWKBOutputDims(5), 3);
  EXPECT_FALSE(ErrorLog().empty());
  GEOS_setWKBOutputDims(orig_dims);
  GEOS_setWKBByteOrder(orig_order);

  // WKTReader and setFixStructure.
  UniqueWKTReader wkt_reader(GEOSWKTReader_create());
  ASSERT_NE(wkt_reader, nullptr);
  const char* unclosed_wkt = "POLYGON ((0 0, 1 0, 1 1, 0 1))";
  EXPECT_EQ(GEOSWKTReader_read(wkt_reader.get(), unclosed_wkt), nullptr);
  GEOSWKTReader_setFixStructure(wkt_reader.get(), 1);
  UniqueGeom fixed_wkt_poly(GEOSWKTReader_read(wkt_reader.get(), unclosed_wkt));
  ASSERT_NE(fixed_wkt_poly, nullptr);
  double area = 0.0;
  EXPECT_EQ(GEOSArea(fixed_wkt_poly.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 1.0);
  GEOSWKTReader_setFixStructure(wkt_reader.get(), 0);
  EXPECT_EQ(GEOSWKTReader_read(wkt_reader.get(), unclosed_wkt), nullptr);

  // WKTWriter options.
  UniqueWKTWriter wkt_writer(GEOSWKTWriter_create());
  ASSERT_NE(wkt_writer, nullptr);
  EXPECT_EQ(GEOSWKTWriter_getOutputDimension(wkt_writer.get()), 4);
  GEOSWKTWriter_setOutputDimension(wkt_writer.get(), 3);
  EXPECT_EQ(GEOSWKTWriter_getOutputDimension(wkt_writer.get()), 3);

  UniqueCStr iso_3d(GEOSWKTWriter_write(wkt_writer.get(), pt3d.get()));
  ASSERT_NE(iso_3d, nullptr);
  EXPECT_EQ(std::string(iso_3d.get()), "POINT Z (1.5 2.5 3.5)");

  GEOSWKTWriter_setOld3D(wkt_writer.get(), 1);
  UniqueCStr old_3d(GEOSWKTWriter_write(wkt_writer.get(), pt3d.get()));
  ASSERT_NE(old_3d, nullptr);
  EXPECT_EQ(std::string(old_3d.get()), "POINT (1.5 2.5 3.5)");

  GEOSWKTWriter_setOld3D(wkt_writer.get(), 0);
  GEOSWKTWriter_setOutputDimension(wkt_writer.get(), 2);
  UniqueGeom approx_pt(GEOSGeom_createPointFromXY(1.234, 2.5));
  GEOSWKTWriter_setRoundingPrecision(wkt_writer.get(), 2);
  GEOSWKTWriter_setTrim(wkt_writer.get(), 0);
  UniqueCStr untrimmed_wkt(
      GEOSWKTWriter_write(wkt_writer.get(), approx_pt.get()));
  ASSERT_NE(untrimmed_wkt, nullptr);
  EXPECT_EQ(std::string(untrimmed_wkt.get()), "POINT (1.23 2.50)");

  GEOSWKTWriter_setTrim(wkt_writer.get(), 1);
  UniqueCStr trimmed_wkt(
      GEOSWKTWriter_write(wkt_writer.get(), approx_pt.get()));
  ASSERT_NE(trimmed_wkt, nullptr);
  EXPECT_EQ(std::string(trimmed_wkt.get()), "POINT (1.23 2.5)");

  // WKBWriter and WKBReader options.
  UniqueWKBWriter wkb_writer(GEOSWKBWriter_create());
  ASSERT_NE(wkb_writer, nullptr);
  EXPECT_EQ(GEOSWKBWriter_getOutputDimension(wkb_writer.get()), 4);
  GEOSWKBWriter_setOutputDimension(wkb_writer.get(), 3);
  EXPECT_EQ(GEOSWKBWriter_getOutputDimension(wkb_writer.get()), 3);

  GEOSWKBWriter_setByteOrder(wkb_writer.get(), GEOS_WKB_XDR);
  EXPECT_EQ(GEOSWKBWriter_getByteOrder(wkb_writer.get()), GEOS_WKB_XDR);
  GEOSWKBWriter_setByteOrder(wkb_writer.get(), GEOS_WKB_NDR);
  EXPECT_EQ(GEOSWKBWriter_getByteOrder(wkb_writer.get()), GEOS_WKB_NDR);

  EXPECT_EQ(GEOSWKBWriter_getFlavor(wkb_writer.get()), GEOS_WKB_EXTENDED);
  EXPECT_EQ(GEOSWKBWriter_getIncludeSRID(wkb_writer.get()), 0);
  GEOSWKBWriter_setIncludeSRID(wkb_writer.get(), 1);
  EXPECT_EQ(GEOSWKBWriter_getIncludeSRID(wkb_writer.get()), 1);

  UniqueGeom srid_pt(GEOSGeom_clone(pt3d.get()));
  GEOSSetSRID(srid_pt.get(), 4326);
  std::size_t writer_wkb_len = 0;
  UniqueBytes ext_wkb(
      GEOSWKBWriter_write(wkb_writer.get(), srid_pt.get(), &writer_wkb_len));
  ASSERT_NE(ext_wkb, nullptr);

  UniqueWKBReader wkb_reader(GEOSWKBReader_create());
  ASSERT_NE(wkb_reader, nullptr);
  UniqueGeom read_ext(
      GEOSWKBReader_read(wkb_reader.get(), ext_wkb.get(), writer_wkb_len));
  ASSERT_NE(read_ext, nullptr);
  EXPECT_EQ(GEOSGetSRID(read_ext.get()), 4326);

  // Switch to ISO flavor; ISO does not embed SRID.
  GEOSWKBWriter_setFlavor(wkb_writer.get(), GEOS_WKB_ISO);
  EXPECT_EQ(GEOSWKBWriter_getFlavor(wkb_writer.get()), GEOS_WKB_ISO);
  std::size_t iso_hex_len = 0;
  UniqueBytes iso_hex(
      GEOSWKBWriter_writeHEX(wkb_writer.get(), srid_pt.get(), &iso_hex_len));
  ASSERT_NE(iso_hex, nullptr);
  UniqueGeom read_iso(
      GEOSWKBReader_readHEX(wkb_reader.get(), iso_hex.get(), iso_hex_len));
  ASSERT_NE(read_iso, nullptr);
  EXPECT_EQ(GEOSGetSRID(read_iso.get()), 0);
  EXPECT_EQ(GEOSGeomGetZ(read_iso.get(), &z), 1);
  EXPECT_DOUBLE_EQ(z, 3.5);

  // WKBReader_setFixStructure on NDR WKB hex for unclosed square ring.
  const std::string unclosed_wkb_hex =
      "01030000000100000004000000"
      "00000000000000000000000000000000"
      "000000000000F03F0000000000000000"
      "000000000000F03F000000000000F03F"
      "0000000000000000000000000000F03F";
  const auto* unclosed_hex_bytes =
      reinterpret_cast<const unsigned char*>(unclosed_wkb_hex.data());
  EXPECT_EQ(GEOSWKBReader_readHEX(wkb_reader.get(), unclosed_hex_bytes,
                                  unclosed_wkb_hex.size()),
            nullptr);
  GEOSWKBReader_setFixStructure(wkb_reader.get(), 1);
  UniqueGeom fixed_wkb_poly(GEOSWKBReader_readHEX(
      wkb_reader.get(), unclosed_hex_bytes, unclosed_wkb_hex.size()));
  ASSERT_NE(fixed_wkb_poly, nullptr);
  EXPECT_EQ(GEOSArea(fixed_wkb_poly.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 1.0);

  // GEOS_printDouble (does not null-terminate).
  char dbuf[64]{};
  int dlen = GEOS_printDouble(12.3456, 2, dbuf);
  ASSERT_GT(dlen, 0);
  EXPECT_EQ(std::string(dbuf, static_cast<std::size_t>(dlen)), "12.35");

  std::memset(dbuf, 0, sizeof(dbuf));
  dlen = GEOS_printDouble(0.0, 4, dbuf);
  ASSERT_GT(dlen, 0);
  EXPECT_EQ(std::string(dbuf, static_cast<std::size_t>(dlen)), "0");

  std::memset(dbuf, 0, sizeof(dbuf));
  dlen = GEOS_printDouble(1e-6, 3, dbuf);
  ASSERT_GT(dlen, 0);
  EXPECT_THAT(std::string(dbuf, static_cast<std::size_t>(dlen)),
              HasSubstr("e-"));

  // GeoJSON Reader & Writer.
  UniqueGeoJSONReader json_reader(GEOSGeoJSONReader_create());
  ASSERT_NE(json_reader, nullptr);
  UniqueGeom json_pt(GEOSGeoJSONReader_readGeometry(
      json_reader.get(), R"({"type":"Point","coordinates":[1.5,2.5,3.5]})"));
  ASSERT_NE(json_pt, nullptr);
  EXPECT_EQ(GEOSGeomTypeId(json_pt.get()), GEOS_POINT);

  UniqueGeom json_fc(GEOSGeoJSONReader_readGeometry(
      json_reader.get(),
      R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[1,2]},"properties":{}},{"type":"Feature","geometry":{"type":"Point","coordinates":[3,4]},"properties":{}}]})"));
  ASSERT_NE(json_fc, nullptr);
  EXPECT_EQ(GEOSGetNumGeometries(json_fc.get()), 2);
  EXPECT_EQ(GEOSGeoJSONReader_readGeometry(json_reader.get(), "{invalid"),
            nullptr);

  UniqueGeoJSONWriter json_writer(GEOSGeoJSONWriter_create());
  ASSERT_NE(json_writer, nullptr);
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension(json_writer.get()), 3);
  GEOSGeoJSONWriter_setOutputDimension(json_writer.get(), 2);
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension(json_writer.get()), 2);

  UniqueCStr compact_json(
      GEOSGeoJSONWriter_writeGeometry(json_writer.get(), json_pt.get(), -1));
  ASSERT_NE(compact_json, nullptr);
  EXPECT_EQ(std::string(compact_json.get()),
            R"({"type":"Point","coordinates":[1.5,2.5]})");

  UniqueCStr pretty_json(
      GEOSGeoJSONWriter_writeGeometry(json_writer.get(), json_pt.get(), 2));
  ASSERT_NE(pretty_json, nullptr);
  EXPECT_THAT(std::string(pretty_json.get()), HasSubstr("\n"));

  // Dimension 4 throws IllegalArgumentException on GeoJSONWriter.
  ErrorLog().clear();
  GEOSGeoJSONWriter_setOutputDimension(json_writer.get(), 4);
  EXPECT_FALSE(ErrorLog().empty());
}

TEST_F(GeosCTest, PreparedGeometryAllPredicatesAndMetrics) {
  UniqueGeom poly(GEOSGeom_createRectangle(0.0, 0.0, 10.0, 10.0));
  UniqueGeom inside_pt(GEOSGeom_createPointFromXY(5.0, 5.0));
  UniqueGeom boundary_pt(GEOSGeom_createPointFromXY(0.0, 5.0));
  UniqueGeom out_pt(GEOSGeom_createPointFromXY(13.0, 14.0));
  UniqueGeom inside_box(GEOSGeom_createRectangle(2.0, 2.0, 8.0, 8.0));
  UniqueGeom edge_box(GEOSGeom_createRectangle(0.0, 2.0, 5.0, 8.0));
  UniqueGeom overlap_box(GEOSGeom_createRectangle(5.0, 5.0, 15.0, 15.0));
  UniqueGeom touch_box(GEOSGeom_createRectangle(10.0, 0.0, 15.0, 10.0));
  UniqueGeom line_thru(ReadWKT("LINESTRING (-2 5, 12 5)"));
  UniqueGeom line_inside(ReadWKT("LINESTRING (2 5, 8 5)"));

  // Destroy UniquePreparedGeom before the underlying GEOSGeometry objects.
  UniquePreparedGeom pg_poly(GEOSPrepare(poly.get()));
  UniquePreparedGeom pg_inside(GEOSPrepare(inside_box.get()));
  ASSERT_NE(pg_poly, nullptr);
  ASSERT_NE(pg_inside, nullptr);

  EXPECT_EQ(GEOSPreparedContains(pg_poly.get(), inside_pt.get()), 1);
  EXPECT_EQ(GEOSPreparedContains(pg_poly.get(), out_pt.get()), 0);
  EXPECT_EQ(GEOSPreparedContainsXY(pg_poly.get(), 5.0, 5.0), 1);
  EXPECT_EQ(GEOSPreparedContainsXY(pg_poly.get(), 0.0, 0.0), 0);

  EXPECT_EQ(GEOSPreparedContainsProperly(pg_poly.get(), inside_box.get()), 1);
  EXPECT_EQ(GEOSPreparedContainsProperly(pg_poly.get(), edge_box.get()), 0);

  EXPECT_EQ(GEOSPreparedCovers(pg_poly.get(), boundary_pt.get()), 1);
  EXPECT_EQ(GEOSPreparedCovers(pg_poly.get(), out_pt.get()), 0);

  EXPECT_EQ(GEOSPreparedCoveredBy(pg_inside.get(), poly.get()), 1);
  EXPECT_EQ(GEOSPreparedCoveredBy(pg_poly.get(), inside_box.get()), 0);

  EXPECT_EQ(GEOSPreparedCrosses(pg_poly.get(), line_thru.get()), 1);
  EXPECT_EQ(GEOSPreparedCrosses(pg_poly.get(), line_inside.get()), 0);

  EXPECT_EQ(GEOSPreparedDisjoint(pg_poly.get(), out_pt.get()), 1);
  EXPECT_EQ(GEOSPreparedDisjoint(pg_poly.get(), inside_pt.get()), 0);

  EXPECT_EQ(GEOSPreparedIntersects(pg_poly.get(), boundary_pt.get()), 1);
  EXPECT_EQ(GEOSPreparedIntersects(pg_poly.get(), out_pt.get()), 0);
  EXPECT_EQ(GEOSPreparedIntersectsXY(pg_poly.get(), 0.0, 0.0), 1);
  EXPECT_EQ(GEOSPreparedIntersectsXY(pg_poly.get(), 20.0, 20.0), 0);

  EXPECT_EQ(GEOSPreparedOverlaps(pg_poly.get(), overlap_box.get()), 1);
  EXPECT_EQ(GEOSPreparedOverlaps(pg_poly.get(), inside_box.get()), 0);

  EXPECT_EQ(GEOSPreparedTouches(pg_poly.get(), touch_box.get()), 1);
  EXPECT_EQ(GEOSPreparedTouches(pg_poly.get(), overlap_box.get()), 0);

  EXPECT_EQ(GEOSPreparedWithin(pg_inside.get(), poly.get()), 1);
  EXPECT_EQ(GEOSPreparedWithin(pg_poly.get(), out_pt.get()), 0);

  UniqueCStr rel(GEOSPreparedRelate(pg_poly.get(), inside_pt.get()));
  ASSERT_NE(rel, nullptr);
  EXPECT_EQ(std::strlen(rel.get()), 9u);
  EXPECT_EQ(GEOSRelatePatternMatch(rel.get(), "T*****FF*"), 1);

  EXPECT_EQ(
      GEOSPreparedRelatePattern(pg_poly.get(), inside_pt.get(), "T*****FF*"),
      1);
  EXPECT_EQ(GEOSPreparedRelatePattern(pg_poly.get(), out_pt.get(), "T********"),
            0);
  EXPECT_EQ(GEOSPreparedRelatePattern(pg_poly.get(), inside_pt.get(), "BAD"),
            2);

  UniqueCoordSeq nearest(
      GEOSPreparedNearestPoints(pg_poly.get(), out_pt.get()));
  ASSERT_NE(nearest, nullptr);
  double x = 0.0;
  double y = 0.0;
  EXPECT_EQ(GEOSCoordSeq_getXY(nearest.get(), 0, &x, &y), 1);
  EXPECT_DOUBLE_EQ(x, 10.0);
  EXPECT_DOUBLE_EQ(y, 10.0);

  double dist = 0.0;
  EXPECT_EQ(GEOSPreparedDistance(pg_poly.get(), out_pt.get(), &dist), 1);
  EXPECT_DOUBLE_EQ(dist, 5.0);
  EXPECT_EQ(GEOSPreparedDistanceWithin(pg_poly.get(), out_pt.get(), 5.1), 1);
  EXPECT_EQ(GEOSPreparedDistanceWithin(pg_poly.get(), out_pt.get(), 4.9), 0);
}

TEST_F(GeosCTest, STRtreeAllOperations) {
  UniqueGeom box1(GEOSGeom_createRectangle(0.0, 0.0, 2.0, 2.0));
  UniqueGeom box2(GEOSGeom_createRectangle(5.0, 5.0, 7.0, 7.0));
  UniqueGeom box3(GEOSGeom_createRectangle(10.0, 10.0, 12.0, 12.0));

  UniqueSTRtree tree(GEOSSTRtree_create(4));
  ASSERT_NE(tree, nullptr);
  GEOSSTRtree_insert(tree.get(), box1.get(), box1.get());
  GEOSSTRtree_insert(tree.get(), box2.get(), box2.get());
  GEOSSTRtree_insert(tree.get(), box3.get(), box3.get());
  EXPECT_EQ(GEOSSTRtree_build(tree.get()), 1);

  std::vector<const GEOSGeometry*> all_items;
  GEOSSTRtree_iterate(tree.get(), &CollectGeomCallback, &all_items);
  EXPECT_EQ(all_items.size(), 3u);

  UniqueGeom query_env(GEOSGeom_createRectangle(4.5, 4.5, 7.5, 7.5));
  std::vector<const GEOSGeometry*> hits;
  GEOSSTRtree_query(tree.get(), query_env.get(), &CollectGeomCallback, &hits);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0], box2.get());

  UniqueGeom query_pt(GEOSGeom_createPointFromXY(0.5, 0.5));
  EXPECT_EQ(GEOSSTRtree_nearest(tree.get(), query_pt.get()), box1.get());

  EXPECT_EQ(GEOSSTRtree_remove(tree.get(), box2.get(), box2.get()), 1);
  EXPECT_EQ(GEOSSTRtree_remove(tree.get(), box2.get(), box2.get()), 0);
  hits.clear();
  GEOSSTRtree_query(tree.get(), query_env.get(), &CollectGeomCallback, &hits);
  EXPECT_TRUE(hits.empty());

  // Custom item distance tree via GEOSSTRtree_nearest_generic.
  // GEOS requires item distance <= Cartesian distance between item envelopes.
  CustomBox cboxes[3] = {{0.0, 0.0, 1}, {10.0, 0.0, 2}, {20.0, 0.0, 3}};
  UniqueGeom cenv0(GEOSGeom_createPointFromXY(0.0, 0.0));
  UniqueGeom cenv1(GEOSGeom_createPointFromXY(10.0, 0.0));
  UniqueGeom cenv2(GEOSGeom_createPointFromXY(20.0, 0.0));

  UniqueSTRtree custom_tree(GEOSSTRtree_create(2));
  ASSERT_NE(custom_tree, nullptr);
  GEOSSTRtree_insert(custom_tree.get(), cenv0.get(), &cboxes[0]);
  GEOSSTRtree_insert(custom_tree.get(), cenv1.get(), &cboxes[1]);
  GEOSSTRtree_insert(custom_tree.get(), cenv2.get(), &cboxes[2]);

  CustomBox target{9.2, 0.3, 99};
  UniqueGeom target_env(GEOSGeom_createPointFromXY(9.2, 0.3));
  double scale = 0.5;
  const void* nearest = GEOSSTRtree_nearest_generic(
      custom_tree.get(), &target, target_env.get(), &CustomBoxDistance, &scale);
  ASSERT_NE(nearest, nullptr);
  EXPECT_EQ(static_cast<const CustomBox*>(nearest)->id, 2);

  EXPECT_EQ(
      GEOSSTRtree_nearest_generic(custom_tree.get(), &target, target_env.get(),
                                  &FailingDistance, nullptr),
      nullptr);
}

}  // namespace
}  // namespace geos_google
