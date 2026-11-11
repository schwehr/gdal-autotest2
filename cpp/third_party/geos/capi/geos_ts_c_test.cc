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

// Unit tests for third_party/geos/capi/geos_ts_c.cpp (reentrant GEOS C API).
//
// See also:
//   https://github.com/libgeos/geos/blob/main/capi/geos_ts_c.cpp

#define GEOS_USE_ONLY_R_API
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/capi/geos_c.h"
#include "third_party/geos/google/util/scoped_geos_c.h"

namespace geos_google {
namespace {

using ::testing::AnyOf;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::StartsWith;

thread_local std::vector<std::string>* g_tl_old_notices = nullptr;
thread_local std::vector<std::string>* g_tl_old_errors = nullptr;

void CaptureOldNotice(const char* fmt, ...) GEOS_PRINTF_FORMAT_ATTR(1, 2);
void CaptureOldNotice(const char* fmt, ...) {
  if (g_tl_old_notices == nullptr) return;
  std::array<char, 1024> buf{};
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf.data(), buf.size(), fmt, args);
  va_end(args);
  g_tl_old_notices->emplace_back(buf.data());
}

void CaptureOldError(const char* fmt, ...) GEOS_PRINTF_FORMAT_ATTR(1, 2);
void CaptureOldError(const char* fmt, ...) {
  if (g_tl_old_errors == nullptr) return;
  std::array<char, 1024> buf{};
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf.data(), buf.size(), fmt, args);
  va_end(args);
  g_tl_old_errors->emplace_back(buf.data());
}

ScopedGeom ReadWkt(GEOSContextHandle_t ctx, const char* wkt) {
  ScopedWKTReader reader(ctx, GEOSWKTReader_create_r(ctx));
  return ScopedGeom(ctx, GEOSWKTReader_read_r(ctx, reader.get(), wkt));
}

std::string WriteWkt(GEOSContextHandle_t ctx, const GEOSGeometry* g) {
  ScopedWKTWriter writer(ctx, GEOSWKTWriter_create_r(ctx));
  ScopedCString s(ctx, GEOSWKTWriter_write_r(ctx, writer.get(), g));
  return s ? std::string(s.get()) : std::string();
}

void AppendLE32(std::vector<unsigned char>& buf, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    buf.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFF));
  }
}

void AppendLE64Double(std::vector<unsigned char>& buf, double value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<unsigned char>((bits >> (8 * i)) & 0xFF));
  }
}

class GeosTsCTest : public ::testing::Test {
 protected:
  ScopedGeom Wkt(const char* wkt) const { return ReadWkt(ctx_, wkt); }
  std::string ToWkt(const GEOSGeometry* g) const { return WriteWkt(ctx_, g); }

  ScopedContext ctx_;
};

// ============================================================================
// Context lifecycle, message handlers, interrupts, execute guards, concurrency
// ============================================================================

TEST(GeosTsCStandaloneTest, VersionReturnsNonEmptyCapiString) {
  const char* ver = GEOSversion();
  ASSERT_NE(ver, nullptr);
  EXPECT_THAT(std::string(ver), StartsWith(GEOS_VERSION));
  EXPECT_STREQ(ver, GEOS_CAPI_VERSION);
}

TEST(GeosTsCStandaloneTest, FreshContextWithoutHandlersIgnoresMessages) {
  ScopedContext ctx(GEOS_init_r());
  ASSERT_TRUE(ctx.valid());

  // Destroying null objects and freeing null buffers must be safe no-ops.
  GEOSGeom_destroy_r(ctx, nullptr);
  GEOSCoordSeq_destroy_r(ctx, nullptr);
  GEOSPreparedGeom_destroy_r(ctx, nullptr);
  GEOSClusterInfo_destroy_r(ctx, nullptr);
  GEOSFree_r(ctx, nullptr);
  GEOSBufferParams_destroy_r(ctx, nullptr);
  GEOSMakeValidParams_destroy_r(ctx, nullptr);
  GEOSCoverageCleanParams_destroy_r(ctx, nullptr);
  GEOSSTRtree_destroy_r(ctx, nullptr);
  GEOSWKTReader_destroy_r(ctx, nullptr);
  GEOSWKTWriter_destroy_r(ctx, nullptr);
  GEOSWKBReader_destroy_r(ctx, nullptr);
  GEOSWKBWriter_destroy_r(ctx, nullptr);
  GEOSGeoJSONReader_destroy_r(ctx, nullptr);
  GEOSGeoJSONWriter_destroy_r(ctx, nullptr);
  GEOS_finish_r(nullptr);

  // Triggers NOTICE_MESSAGE with both handlers unset.
  ScopedGeom bowtie = ReadWkt(ctx, "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  ASSERT_TRUE(bowtie);
  EXPECT_EQ(GEOSisValid_r(ctx, bowtie.get()), 0);

  // Triggers ERROR_MESSAGE with both handlers unset.
  ScopedGeom bad(ctx, GEOSGeomFromWKT_r(ctx, "NOT A WKT"));
  EXPECT_FALSE(bad);
}

TEST(GeosTsCStandaloneTest, MessageHandlersAndOldHandlersCaptureAndChain) {
  ScopedContext ctx;
  ASSERT_TRUE(ctx.valid());

  std::vector<std::string> notices2;
  std::vector<std::string> errors2;
  auto prev_n = GEOSContext_setNoticeMessageHandler_r(
      ctx,
      [](const char* msg, void* ud) {
        static_cast<std::vector<std::string>*>(ud)->emplace_back(msg);
      },
      &notices2);
  auto prev_e = GEOSContext_setErrorMessageHandler_r(
      ctx,
      [](const char* msg, void* ud) {
        static_cast<std::vector<std::string>*>(ud)->emplace_back(msg);
      },
      &errors2);
  EXPECT_NE(prev_n, nullptr);
  EXPECT_NE(prev_e, nullptr);

  ScopedGeom bowtie = ReadWkt(ctx, "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  ASSERT_TRUE(bowtie);
  EXPECT_EQ(GEOSisValid_r(ctx, bowtie.get()), 0);
  EXPECT_THAT(notices2, Not(IsEmpty()));

  ScopedGeom bad(ctx, GEOSGeomFromWKT_r(ctx, "POINT (1"));
  EXPECT_FALSE(bad);
  EXPECT_THAT(errors2, Not(IsEmpty()));

  // Switch to legacy varargs GEOSMessageHandler on the context.
  std::vector<std::string> old_notices;
  std::vector<std::string> old_errors;
  g_tl_old_notices = &old_notices;
  g_tl_old_errors = &old_errors;
  absl::Cleanup reset_tl = [] {
    g_tl_old_notices = nullptr;
    g_tl_old_errors = nullptr;
  };

  EXPECT_EQ(GEOSContext_setNoticeHandler_r(ctx, &CaptureOldNotice), nullptr);
  EXPECT_EQ(GEOSContext_setErrorHandler_r(ctx, &CaptureOldError), nullptr);

  EXPECT_EQ(GEOSisValid_r(ctx, bowtie.get()), 0);
  EXPECT_THAT(old_notices, Contains(HasSubstr("Self-intersection")));

  ScopedGeom bad2(ctx, GEOSGeomFromWKT_r(ctx, "LINESTRING (0"));
  EXPECT_FALSE(bad2);
  EXPECT_THAT(old_errors, Not(IsEmpty()));

  // Replacing old handlers returns the previously configured old handlers.
  EXPECT_EQ(GEOSContext_setNoticeHandler_r(ctx, nullptr), &CaptureOldNotice);
  EXPECT_EQ(GEOSContext_setErrorHandler_r(ctx, nullptr), &CaptureOldError);
}

TEST(GeosTsCStandaloneTest, DeprecatedInitAndFinishGeosRWorkWithOldHandlers) {
  std::vector<std::string> notices;
  std::vector<std::string> errors;
  g_tl_old_notices = &notices;
  g_tl_old_errors = &errors;
  absl::Cleanup reset_tl = [] {
    g_tl_old_notices = nullptr;
    g_tl_old_errors = nullptr;
  };

  GEOSContextHandle_t raw_ctx = initGEOS_r(&CaptureOldNotice, &CaptureOldError);
  ASSERT_NE(raw_ctx, nullptr);
  absl::Cleanup finish_guard = [&] { finishGEOS_r(raw_ctx); };

  ScopedGeom bowtie = ReadWkt(raw_ctx, "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  ASSERT_TRUE(bowtie);
  EXPECT_EQ(GEOSisValid_r(raw_ctx, bowtie.get()), 0);
  EXPECT_THAT(notices, Not(IsEmpty()));

  ScopedGeom bad(raw_ctx, GEOSGeomFromWKT_r(raw_ctx, "BOGUS"));
  EXPECT_FALSE(bad);
  EXPECT_THAT(errors, Not(IsEmpty()));
}

TEST(GeosTsCStandaloneTest, NullContextHandleThrowsStdRuntimeError) {
  ScopedContext ctx;
  ScopedGeom pt(ctx, GEOSGeom_createPointFromXY_r(ctx, 1.0, 2.0));
  ASSERT_TRUE(pt);

  GEOSGeom_destroy_r(nullptr, nullptr);
  EXPECT_THROW(GEOSDisjoint_r(nullptr, pt.get(), pt.get()), std::runtime_error);
  EXPECT_THROW(GEOSGeom_clone_r(nullptr, pt.get()), std::runtime_error);
  EXPECT_EQ(GEOSSharedPaths_r(nullptr, pt.get(), pt.get()), nullptr);
}

TEST(GeosTsCStandaloneTest, InterruptCallbackCancelsOperationAndCanBeCleared) {
  ScopedContext ctx;
  ScopedGeom poly = ReadWkt(ctx, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(poly);

  struct InterruptState {
    int calls = 0;
    int interrupt_after = 1000000;
  };
  InterruptState state;
  auto cb = [](void* ud) -> int {
    auto* s = static_cast<InterruptState*>(ud);
    ++s->calls;
    return s->calls >= s->interrupt_after ? 1 : 0;
  };

  // Callback returning 0 permits completion.
  EXPECT_EQ(GEOSContext_setInterruptCallback_r(ctx, cb, &state), nullptr);
  {
    ScopedGeom buffered(ctx, GEOSBuffer_r(ctx, poly.get(), 1.0, 8));
    ASSERT_TRUE(buffered);
    EXPECT_GT(state.calls, 0);
    EXPECT_THAT(ctx.errors(), IsEmpty());
  }

  // Callback returning 1 interrupts and triggers error message.
  state.calls = 0;
  state.interrupt_after = 1;
  {
    ScopedGeom interrupted(ctx, GEOSBuffer_r(ctx, poly.get(), 1.0, 16));
    EXPECT_FALSE(interrupted);
    EXPECT_THAT(ctx.errors(), Contains(HasSubstr("Interrupted")));
  }

  // Also test errval overload interruption via GEOSFrechetDistance_r.
  ctx.ClearMessages();
  state.calls = 0;
  state.interrupt_after = 1;
  double dist = -1.0;
  EXPECT_EQ(GEOSFrechetDistance_r(ctx, poly.get(), poly.get(), &dist), 0);
  EXPECT_THAT(ctx.errors(), Contains(HasSubstr("Interrupted")));

  // Clearing the interrupt callback returns previous callback and restores
  // runs.
  EXPECT_EQ(GEOSContext_setInterruptCallback_r(ctx, nullptr, nullptr), cb);
  ctx.ClearMessages();
  ScopedGeom after(ctx, GEOSBuffer_r(ctx, poly.get(), 1.0, 4));
  EXPECT_TRUE(after);
  EXPECT_THAT(ctx.errors(), IsEmpty());
}

TEST(GeosTsCStandaloneTest, ConcurrentPerThreadContextsAreThreadSafe) {
  // Initialize contexts and default-factory geometries serially on the main
  // thread so GeometryFactory::getDefaultInstance()'s non-atomic _refCount is
  // never mutated concurrently across worker threads under TSan.
  constexpr int kThreads = 4;
  struct WorkerData {
    ScopedContext ctx;
    ScopedGeom poly;
    ScopedGeom pt_in;
    ScopedGeom pt_out;
    ScopedPreparedGeom prep;
  };
  std::vector<WorkerData> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    WorkerData w;
    const double base = static_cast<double>(i * 10);
    w.poly = ScopedGeom(
        w.ctx, GEOSGeom_createRectangle_r(w.ctx, base, 0.0, base + 4.0, 4.0));
    w.pt_in =
        ScopedGeom(w.ctx, GEOSGeom_createPointFromXY_r(w.ctx, base + 2.0, 2.0));
    w.pt_out =
        ScopedGeom(w.ctx, GEOSGeom_createPointFromXY_r(w.ctx, base + 6.0, 2.0));
    w.prep = ScopedPreparedGeom(w.ctx, GEOSPrepare_r(w.ctx, w.poly.get()));
    ASSERT_TRUE(w.poly && w.pt_in && w.pt_out && w.prep);
    workers.push_back(std::move(w));
  }

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&w = workers[i], i]() {
      const double base = static_cast<double>(i * 10);
      for (int iter = 0; iter < 50; ++iter) {
        EXPECT_EQ(
            GEOSPreparedContainsXY_r(w.ctx, w.prep.get(), base + 2.0, 2.0), 1);
        EXPECT_EQ(
            GEOSPreparedContainsXY_r(w.ctx, w.prep.get(), base + 6.0, 2.0), 0);
        EXPECT_EQ(GEOSPreparedIntersectsXY_r(w.ctx, w.prep.get(), base, 0.0),
                  1);
        EXPECT_EQ(GEOSPreparedIntersects_r(w.ctx, w.prep.get(), w.pt_in.get()),
                  1);

        double dist = -1.0;
        EXPECT_EQ(
            GEOSPreparedDistance_r(w.ctx, w.prep.get(), w.pt_out.get(), &dist),
            1);
        EXPECT_DOUBLE_EQ(dist, 2.0);

        double area = 0.0;
        EXPECT_EQ(GEOSArea_r(w.ctx, w.poly.get(), &area), 1);
        EXPECT_DOUBLE_EQ(area, 16.0);

        EXPECT_EQ(GEOSisValid_r(w.ctx, w.poly.get()), 1);
        EXPECT_EQ(GEOSOrientationIndex_r(w.ctx, 0.0, 0.0, 1.0, 0.0, 2.0, 0.0),
                  0);

        double ix = 0.0;
        double iy = 0.0;
        EXPECT_EQ(GEOSSegmentIntersection_r(w.ctx, 0.0, 0.0, 2.0, 2.0, 0.0, 2.0,
                                            2.0, 0.0, &ix, &iy),
                  1);
        EXPECT_DOUBLE_EQ(ix, 1.0);
        EXPECT_DOUBLE_EQ(iy, 1.0);

        ScopedCoordSeq cs(w.ctx, GEOSCoordSeq_create_r(w.ctx, 2, 2));
        ASSERT_TRUE(cs);
        EXPECT_EQ(GEOSCoordSeq_setXY_r(w.ctx, cs.get(), 0, base, 1.0), 1);
        double x = 0.0;
        double y = 0.0;
        EXPECT_EQ(GEOSCoordSeq_getXY_r(w.ctx, cs.get(), 0, &x, &y), 1);
        EXPECT_DOUBLE_EQ(x, base);
        EXPECT_DOUBLE_EQ(y, 1.0);
      }
      EXPECT_THAT(w.ctx.errors(), IsEmpty());
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

// ============================================================================
// Section 2: Coordinate sequences
// ============================================================================

TEST_F(GeosTsCTest, CoordSeqCreateHasZHasMSizeAndDimensions) {
  {
    ScopedCoordSeq cs(ctx_, GEOSCoordSeq_create_r(ctx_, 3, 2));
    ASSERT_TRUE(cs);
    unsigned int size = 0;
    unsigned int dims = 0;
    EXPECT_EQ(GEOSCoordSeq_getSize_r(ctx_, cs.get(), &size), 1);
    EXPECT_EQ(GEOSCoordSeq_getDimensions_r(ctx_, cs.get(), &dims), 1);
    EXPECT_EQ(size, 3u);
    EXPECT_EQ(dims, 2u);
    EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs.get()), 0);
    EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs.get()), 0);
  }
  {
    ScopedCoordSeq cs3(ctx_, GEOSCoordSeq_create_r(ctx_, 2, 3));
    ScopedCoordSeq cs4(ctx_, GEOSCoordSeq_create_r(ctx_, 2, 4));
    ASSERT_TRUE(cs3 && cs4);
    unsigned int dims = 0;
    EXPECT_EQ(GEOSCoordSeq_getDimensions_r(ctx_, cs3.get(), &dims), 1);
    EXPECT_EQ(dims, 3u);
    EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs3.get()), 1);
    EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs3.get()), 0);

    EXPECT_EQ(GEOSCoordSeq_getDimensions_r(ctx_, cs4.get(), &dims), 1);
    EXPECT_EQ(dims, 4u);
    EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs4.get()), 1);
    EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs4.get()), 1);
  }

  // Invalid declared dimensions (1 and 5).
  EXPECT_EQ(GEOSCoordSeq_create_r(ctx_, 2, 1), nullptr);
  EXPECT_EQ(GEOSCoordSeq_create_r(ctx_, 2, 5), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  // All 4 combinations of (hasZ, hasM) via createWithDimensions_r.
  struct DimCase {
    int has_z;
    int has_m;
    unsigned int expected_dims;
  };
  constexpr std::array<DimCase, 4> kCases = {{
      {0, 0, 2},
      {1, 0, 3},
      {0, 1, 3},
      {1, 1, 4},
  }};
  for (const DimCase& c : kCases) {
    ScopedCoordSeq cs(
        ctx_, GEOSCoordSeq_createWithDimensions_r(ctx_, 2, c.has_z, c.has_m));
    ASSERT_TRUE(cs);
    EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs.get()), c.has_z);
    EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs.get()), c.has_m);
    unsigned int dims = 0;
    EXPECT_EQ(GEOSCoordSeq_getDimensions_r(ctx_, cs.get(), &dims), 1);
    EXPECT_EQ(dims, c.expected_dims);
  }
}

TEST_F(GeosTsCTest, CoordSeqOrdinateSettersGettersAndClone) {
  ScopedCoordSeq cs(ctx_, GEOSCoordSeq_create_r(ctx_, 2, 4));
  ASSERT_TRUE(cs);

  EXPECT_EQ(GEOSCoordSeq_setX_r(ctx_, cs.get(), 0, 1.5), 1);
  EXPECT_EQ(GEOSCoordSeq_setY_r(ctx_, cs.get(), 0, 2.5), 1);
  EXPECT_EQ(GEOSCoordSeq_setZ_r(ctx_, cs.get(), 0, 3.5), 1);
  EXPECT_EQ(GEOSCoordSeq_setM_r(ctx_, cs.get(), 0, 4.5), 1);
  EXPECT_EQ(GEOSCoordSeq_setXY_r(ctx_, cs.get(), 1, 10.0, 20.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setXYZ_r(ctx_, cs.get(), 1, 11.0, 22.0, 33.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setOrdinate_r(ctx_, cs.get(), 1, 3, 44.0), 1);

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double m = 0.0;
  double ord = 0.0;
  EXPECT_EQ(GEOSCoordSeq_getX_r(ctx_, cs.get(), 0, &x), 1);
  EXPECT_EQ(GEOSCoordSeq_getY_r(ctx_, cs.get(), 0, &y), 1);
  EXPECT_EQ(GEOSCoordSeq_getZ_r(ctx_, cs.get(), 0, &z), 1);
  EXPECT_EQ(GEOSCoordSeq_getM_r(ctx_, cs.get(), 0, &m), 1);
  EXPECT_DOUBLE_EQ(x, 1.5);
  EXPECT_DOUBLE_EQ(y, 2.5);
  EXPECT_DOUBLE_EQ(z, 3.5);
  EXPECT_DOUBLE_EQ(m, 4.5);

  EXPECT_EQ(GEOSCoordSeq_getXY_r(ctx_, cs.get(), 1, &x, &y), 1);
  EXPECT_DOUBLE_EQ(x, 11.0);
  EXPECT_DOUBLE_EQ(y, 22.0);
  EXPECT_EQ(GEOSCoordSeq_getXYZ_r(ctx_, cs.get(), 1, &x, &y, &z), 1);
  EXPECT_DOUBLE_EQ(x, 11.0);
  EXPECT_DOUBLE_EQ(y, 22.0);
  EXPECT_DOUBLE_EQ(z, 33.0);
  EXPECT_EQ(GEOSCoordSeq_getOrdinate_r(ctx_, cs.get(), 1, 3, &ord), 1);
  EXPECT_DOUBLE_EQ(ord, 44.0);

  // Invalid ordinate index in getOrdinate returns NaN, setOrdinate fails.
  EXPECT_EQ(GEOSCoordSeq_getOrdinate_r(ctx_, cs.get(), 0, 99, &ord), 1);
  EXPECT_TRUE(std::isnan(ord));
  EXPECT_EQ(GEOSCoordSeq_setOrdinate_r(ctx_, cs.get(), 0, 99, 1.0), 0);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  // Clone creates an independent copy.
  ScopedCoordSeq clone(ctx_, GEOSCoordSeq_clone_r(ctx_, cs.get()));
  ASSERT_TRUE(clone);
  EXPECT_EQ(GEOSCoordSeq_setX_r(ctx_, clone.get(), 0, 999.0), 1);
  EXPECT_EQ(GEOSCoordSeq_getX_r(ctx_, cs.get(), 0, &x), 1);
  EXPECT_EQ(GEOSCoordSeq_getX_r(ctx_, clone.get(), 0, &ord), 1);
  EXPECT_DOUBLE_EQ(x, 1.5);
  EXPECT_DOUBLE_EQ(ord, 999.0);

  // XY sequence: setting Z or M throws, getting Z or M yields NaN.
  ScopedCoordSeq cs2(ctx_, GEOSCoordSeq_create_r(ctx_, 1, 2));
  ASSERT_TRUE(cs2);
  EXPECT_EQ(GEOSCoordSeq_setXY_r(ctx_, cs2.get(), 0, 5.0, 6.0), 1);
  EXPECT_EQ(GEOSCoordSeq_setZ_r(ctx_, cs2.get(), 0, 7.0), 0);
  EXPECT_EQ(GEOSCoordSeq_setM_r(ctx_, cs2.get(), 0, 8.0), 0);
  EXPECT_EQ(ctx_.errors().size(), 2u);
  ctx_.ClearMessages();

  EXPECT_EQ(GEOSCoordSeq_getZ_r(ctx_, cs2.get(), 0, &z), 1);
  EXPECT_EQ(GEOSCoordSeq_getM_r(ctx_, cs2.get(), 0, &m), 1);
  EXPECT_TRUE(std::isnan(z));
  EXPECT_TRUE(std::isnan(m));
}

TEST_F(GeosTsCTest, CoordSeqCopyToFromBuffersAndArrays) {
  // XY buffer
  const std::array<double, 4> xy_buf = {1.0, 2.0, 3.0, 4.0};
  ScopedCoordSeq cs_xy(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, xy_buf.data(), 2, 0, 0));
  ASSERT_TRUE(cs_xy);
  EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs_xy.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs_xy.get()), 0);

  // XYZ buffer (memcpy path)
  const std::array<double, 6> xyz_buf = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  ScopedCoordSeq cs_xyz(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, xyz_buf.data(), 2, 1, 0));
  ASSERT_TRUE(cs_xyz);
  EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs_xyz.get()), 1);
  EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs_xyz.get()), 0);

  // XYM buffer
  const std::array<double, 6> xym_buf = {1.0, 2.0, 10.0, 4.0, 5.0, 20.0};
  ScopedCoordSeq cs_xym(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, xym_buf.data(), 2, 0, 1));
  ASSERT_TRUE(cs_xym);
  EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs_xym.get()), 0);
  EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs_xym.get()), 1);
  double m_val = 0.0;
  EXPECT_EQ(GEOSCoordSeq_getM_r(ctx_, cs_xym.get(), 1, &m_val), 1);
  EXPECT_DOUBLE_EQ(m_val, 20.0);

  // XYZM buffer (memcpy path)
  const std::array<double, 8> xyzm_buf = {1.0, 2.0, 3.0, 4.0,
                                          5.0, 6.0, 7.0, 8.0};
  ScopedCoordSeq cs_xyzm(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, xyzm_buf.data(), 2, 1, 1));
  ASSERT_TRUE(cs_xyzm);
  EXPECT_EQ(GEOSCoordSeq_hasZ_r(ctx_, cs_xyzm.get()), 1);
  EXPECT_EQ(GEOSCoordSeq_hasM_r(ctx_, cs_xyzm.get()), 1);

  // copyToBuffer same-type memcpy fast paths (XY, XYZ, XYM, XYZM).
  std::array<double, 4> out_xy_same{};
  std::array<double, 6> out_xyz{};
  std::array<double, 6> out_xym_same{};
  std::array<double, 8> out_xyzm{};
  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xy.get(), out_xy_same.data(), 0, 0),
      1);
  EXPECT_EQ(out_xy_same, xy_buf);
  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyz.get(), out_xyz.data(), 1, 0), 1);
  EXPECT_EQ(out_xyz, xyz_buf);
  EXPECT_EQ(GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xym.get(), out_xym_same.data(),
                                        0, 1),
            1);
  EXPECT_EQ(out_xym_same, xym_buf);
  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyzm.get(), out_xyzm.data(), 1, 1),
      1);
  EXPECT_EQ(out_xyzm, xyzm_buf);

  // copyToBuffer cross-type conversion paths.
  std::array<double, 4> out_xy{};
  std::array<double, 6> out_conv3{};
  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyzm.get(), out_xy.data(), 0, 0), 1);
  EXPECT_EQ(out_xy, (std::array<double, 4>{1.0, 2.0, 5.0, 6.0}));

  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyzm.get(), out_conv3.data(), 1, 0),
      1);
  EXPECT_EQ(out_conv3, (std::array<double, 6>{1.0, 2.0, 3.0, 5.0, 6.0, 7.0}));

  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyzm.get(), out_conv3.data(), 0, 1),
      1);
  EXPECT_EQ(out_conv3, (std::array<double, 6>{1.0, 2.0, 4.0, 5.0, 6.0, 8.0}));

  EXPECT_EQ(
      GEOSCoordSeq_copyToBuffer_r(ctx_, cs_xyz.get(), out_xyzm.data(), 1, 1),
      1);
  EXPECT_DOUBLE_EQ(out_xyzm[0], 1.0);
  EXPECT_DOUBLE_EQ(out_xyzm[1], 2.0);
  EXPECT_DOUBLE_EQ(out_xyzm[2], 3.0);
  EXPECT_TRUE(std::isnan(out_xyzm[3]));
  EXPECT_DOUBLE_EQ(out_xyzm[4], 4.0);
  EXPECT_DOUBLE_EQ(out_xyzm[5], 5.0);
  EXPECT_DOUBLE_EQ(out_xyzm[6], 6.0);
  EXPECT_TRUE(std::isnan(out_xyzm[7]));

  // copyFromArrays / copyToArrays across null/non-null z and m.
  const std::array<double, 2> ax = {7.0, 8.0};
  const std::array<double, 2> ay = {9.0, 10.0};
  const std::array<double, 2> az = {11.0, 12.0};
  const std::array<double, 2> am = {13.0, 14.0};

  ScopedCoordSeq from_arr_xy(
      ctx_, GEOSCoordSeq_copyFromArrays_r(ctx_, ax.data(), ay.data(), nullptr,
                                          nullptr, 2));
  ScopedCoordSeq from_arr_xyzm(
      ctx_, GEOSCoordSeq_copyFromArrays_r(ctx_, ax.data(), ay.data(), az.data(),
                                          am.data(), 2));
  ASSERT_TRUE(from_arr_xy && from_arr_xyzm);

  std::array<double, 2> ox{};
  std::array<double, 2> oy{};
  std::array<double, 2> oz{};
  std::array<double, 2> om{};
  EXPECT_EQ(GEOSCoordSeq_copyToArrays_r(ctx_, from_arr_xyzm.get(), ox.data(),
                                        oy.data(), oz.data(), om.data()),
            1);
  EXPECT_EQ(ox, ax);
  EXPECT_EQ(oy, ay);
  EXPECT_EQ(oz, az);
  EXPECT_EQ(om, am);

  ox = {};
  oy = {};
  EXPECT_EQ(GEOSCoordSeq_copyToArrays_r(ctx_, from_arr_xy.get(), ox.data(),
                                        oy.data(), nullptr, nullptr),
            1);
  EXPECT_EQ(ox, ax);
  EXPECT_EQ(oy, ay);
}

TEST_F(GeosTsCTest, CoordSeqIsCCWChecksOrientationAndShortSequences) {
  const std::array<double, 10> ccw = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0};
  const std::array<double, 10> cw = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0};
  const std::array<double, 4> short_seq = {0, 0, 1, 1};

  ScopedCoordSeq cs_ccw(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, ccw.data(), 5, 0, 0));
  ScopedCoordSeq cs_cw(ctx_,
                       GEOSCoordSeq_copyFromBuffer_r(ctx_, cw.data(), 5, 0, 0));
  ScopedCoordSeq cs_short(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, short_seq.data(), 2, 0, 0));
  ASSERT_TRUE(cs_ccw && cs_cw && cs_short);

  char is_ccw = -1;
  EXPECT_EQ(GEOSCoordSeq_isCCW_r(ctx_, cs_ccw.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 1);

  EXPECT_EQ(GEOSCoordSeq_isCCW_r(ctx_, cs_cw.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 0);

  EXPECT_EQ(GEOSCoordSeq_isCCW_r(ctx_, cs_short.get(), &is_ccw), 1);
  EXPECT_EQ(is_ccw, 0);
}

// ============================================================================
// Section 3: Constructors, curved types, collections, metadata, and mutators
// ============================================================================

TEST_F(GeosTsCTest, PointConstructorsAccessorsAndDimensionChecks) {
  ScopedGeom empty_pt(ctx_, GEOSGeom_createEmptyPoint_r(ctx_));
  ASSERT_TRUE(empty_pt);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, empty_pt.get()), GEOS_POINT);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, empty_pt.get()), 1);

  // Null CoordSeq creates empty Point.
  ScopedGeom empty_from_null(ctx_, GEOSGeom_createPoint_r(ctx_, nullptr));
  ASSERT_TRUE(empty_from_null);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, empty_from_null.get()), 1);

  ScopedGeom pt_xy(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 3.0, 4.0));
  ASSERT_TRUE(pt_xy);
  EXPECT_EQ(GEOSHasZ_r(ctx_, pt_xy.get()), 0);
  EXPECT_EQ(GEOSHasM_r(ctx_, pt_xy.get()), 0);

  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double m = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, pt_xy.get(), &x), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, pt_xy.get(), &y), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, pt_xy.get(), &z), 1);
  EXPECT_EQ(GEOSGeomGetM_r(ctx_, pt_xy.get(), &m), 1);
  EXPECT_DOUBLE_EQ(x, 3.0);
  EXPECT_DOUBLE_EQ(y, 4.0);
  EXPECT_TRUE(std::isnan(z));
  EXPECT_TRUE(std::isnan(m));

  // XYZM point preserves all four ordinates.
  const std::array<double, 4> xyzm = {1.0, 2.0, 3.0, 4.0};
  ScopedCoordSeq cs4(ctx_,
                     GEOSCoordSeq_copyFromBuffer_r(ctx_, xyzm.data(), 1, 1, 1));
  ASSERT_TRUE(cs4);
  ScopedGeom pt4(ctx_, GEOSGeom_createPoint_r(ctx_, cs4.release()));
  ASSERT_TRUE(pt4);
  EXPECT_EQ(GEOSHasZ_r(ctx_, pt4.get()), 1);
  EXPECT_EQ(GEOSHasM_r(ctx_, pt4.get()), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, pt4.get(), &z), 1);
  EXPECT_EQ(GEOSGeomGetM_r(ctx_, pt4.get(), &m), 1);
  EXPECT_DOUBLE_EQ(z, 3.0);
  EXPECT_DOUBLE_EQ(m, 4.0);

  // Error cases: empty point or non-point geometry.
  ScopedGeom line = Wkt("LINESTRING (0 0, 1 1)");
  ASSERT_TRUE(line);
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, empty_pt.get(), &x), 0);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, empty_pt.get(), &y), 0);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, line.get(), &z), 0);
  EXPECT_EQ(GEOSGeomGetM_r(ctx_, line.get(), &m), 0);
  EXPECT_EQ(ctx_.errors().size(), 4u);
}

TEST_F(GeosTsCTest, LineStringLinearRingAndRectangleConstructors) {
  ScopedGeom empty_line(ctx_, GEOSGeom_createEmptyLineString_r(ctx_));
  ASSERT_TRUE(empty_line);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, empty_line.get()), 1);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, empty_line.get()), GEOS_LINESTRING);

  // 1-point LineString must fail and still consume its CoordSeq without leak.
  ScopedCoordSeq bad_line_cs(ctx_, GEOSCoordSeq_create_r(ctx_, 1, 2));
  ASSERT_TRUE(bad_line_cs);
  EXPECT_EQ(GEOSCoordSeq_setXY_r(ctx_, bad_line_cs.get(), 0, 1.0, 2.0), 1);
  EXPECT_EQ(GEOSGeom_createLineString_r(ctx_, bad_line_cs.release()), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  const std::array<double, 6> line_coords = {0, 0, 3, 4, 3, 0};
  ScopedCoordSeq line_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, line_coords.data(), 3, 0, 0));
  ASSERT_TRUE(line_cs);
  ScopedGeom line(ctx_, GEOSGeom_createLineString_r(ctx_, line_cs.release()));
  ASSERT_TRUE(line);

  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, line.get()), 3);
  double len = 0.0;
  EXPECT_EQ(GEOSGeomGetLength_r(ctx_, line.get(), &len), 1);
  EXPECT_DOUBLE_EQ(len, 9.0);

  ScopedGeom p0(ctx_, GEOSGeomGetPointN_r(ctx_, line.get(), 1));
  ScopedGeom p_start(ctx_, GEOSGeomGetStartPoint_r(ctx_, line.get()));
  ScopedGeom p_end(ctx_, GEOSGeomGetEndPoint_r(ctx_, line.get()));
  ASSERT_TRUE(p0 && p_start && p_end);
  double x = 0.0;
  double y = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, p0.get(), &x), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, p0.get(), &y), 1);
  EXPECT_DOUBLE_EQ(x, 3.0);
  EXPECT_DOUBLE_EQ(y, 4.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, p_end.get(), p_start.get()), 0);

  const GEOSCoordSequence* ro_cs = GEOSGeom_getCoordSeq_r(ctx_, line.get());
  ASSERT_NE(ro_cs, nullptr);
  unsigned int ro_sz = 0;
  EXPECT_EQ(GEOSCoordSeq_getSize_r(ctx_, ro_cs, &ro_sz), 1);
  EXPECT_EQ(ro_sz, 3u);

  const GEOSCoordSequence* pt_cs = GEOSGeom_getCoordSeq_r(ctx_, p_start.get());
  EXPECT_NE(pt_cs, nullptr);

  // Error paths on non-curves / negative index.
  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, p_start.get()), -1);
  EXPECT_EQ(GEOSGeomGetPointN_r(ctx_, line.get(), -1), nullptr);
  EXPECT_EQ(GEOSGeomGetPointN_r(ctx_, p_start.get(), 0), nullptr);
  EXPECT_EQ(GEOSGeomGetStartPoint_r(ctx_, p_start.get()), nullptr);
  EXPECT_EQ(GEOSGeomGetEndPoint_r(ctx_, p_start.get()), nullptr);
  EXPECT_EQ(GEOSGeomGetLength_r(ctx_, p_start.get(), &len), 0);
  ctx_.ClearMessages();

  // LinearRing: unclosed throws, closed succeeds.
  ScopedCoordSeq bad_ring_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, line_coords.data(), 3, 0, 0));
  ASSERT_TRUE(bad_ring_cs);
  EXPECT_EQ(GEOSGeom_createLinearRing_r(ctx_, bad_ring_cs.release()), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  const std::array<double, 8> ring_coords = {0, 0, 2, 0, 0, 2, 0, 0};
  ScopedCoordSeq ring_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, ring_coords.data(), 4, 0, 0));
  ASSERT_TRUE(ring_cs);
  ScopedGeom ring(ctx_, GEOSGeom_createLinearRing_r(ctx_, ring_cs.release()));
  ASSERT_TRUE(ring);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, ring.get()), GEOS_LINEARRING);

  // createRectangle: normal box and degenerate point.
  ScopedGeom rect(ctx_, GEOSGeom_createRectangle_r(ctx_, 1.0, 2.0, 4.0, 6.0));
  ScopedGeom pt_rect(ctx_,
                     GEOSGeom_createRectangle_r(ctx_, 2.0, 3.0, 2.0, 3.0));
  ASSERT_TRUE(rect && pt_rect);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, rect.get()), GEOS_POLYGON);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, pt_rect.get()), GEOS_POINT);
  double area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, rect.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 12.0);
}

TEST_F(GeosTsCTest, PolygonConstructorsAndRingAccessors) {
  ScopedGeom empty_poly(ctx_, GEOSGeom_createEmptyPolygon_r(ctx_));
  ASSERT_TRUE(empty_poly);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, empty_poly.get()), 1);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, empty_poly.get()), GEOS_POLYGON);

  const std::array<double, 10> shell_coords = {0,  0, 10, 0, 10,
                                               10, 0, 10, 0, 0};
  const std::array<double, 10> hole_coords = {2, 2, 4, 2, 4, 4, 2, 4, 2, 2};
  ScopedCoordSeq shell_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, shell_coords.data(), 5, 0, 0));
  ScopedCoordSeq hole_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, hole_coords.data(), 5, 0, 0));
  ASSERT_TRUE(shell_cs && hole_cs);

  ScopedGeom shell(ctx_, GEOSGeom_createLinearRing_r(ctx_, shell_cs.release()));
  ScopedGeom hole(ctx_, GEOSGeom_createLinearRing_r(ctx_, hole_cs.release()));
  ASSERT_TRUE(shell && hole);

  // Polygon without holes.
  ScopedGeom cloned_shell(ctx_, GEOSGeom_clone_r(ctx_, shell.get()));
  ASSERT_TRUE(cloned_shell);
  ScopedGeom poly_no_holes(
      ctx_, GEOSGeom_createPolygon_r(ctx_, cloned_shell.release(), nullptr, 0));
  ASSERT_TRUE(poly_no_holes);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, poly_no_holes.get()), 0);
  EXPECT_EQ(GEOSGeom_getCoordSeq_r(ctx_, poly_no_holes.get()), nullptr);
  ctx_.ClearMessages();

  // Polygon with 1 hole.
  std::array<GEOSGeometry*, 1> holes = {hole.release()};
  ScopedGeom poly(
      ctx_, GEOSGeom_createPolygon_r(ctx_, shell.release(), holes.data(), 1));
  ASSERT_TRUE(poly);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, poly.get()), 1);
  const GEOSGeometry* ext_ring = GEOSGetExteriorRing_r(ctx_, poly.get());
  const GEOSGeometry* int_ring = GEOSGetInteriorRingN_r(ctx_, poly.get(), 0);
  ASSERT_NE(ext_ring, nullptr);
  ASSERT_NE(int_ring, nullptr);
  double poly_area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, poly.get(), &poly_area), 1);
  EXPECT_DOUBLE_EQ(poly_area, 96.0);

  // Accessor error paths.
  ScopedGeom pt(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 0.0, 0.0));
  ASSERT_TRUE(pt);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, pt.get()), -1);
  EXPECT_EQ(GEOSGetExteriorRing_r(ctx_, pt.get()), nullptr);
  EXPECT_EQ(GEOSGetInteriorRingN_r(ctx_, pt.get(), 0), nullptr);
  EXPECT_EQ(GEOSGetInteriorRingN_r(ctx_, poly.get(), -1), nullptr);
  ctx_.ClearMessages();

  // Invalid shell and invalid hole both free all transferred arguments.
  ScopedGeom bad_shell(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 1.0, 1.0));
  ScopedGeom good_hole_cs(
      ctx_, GEOSGeom_createLinearRing_r(
                ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, hole_coords.data(), 5,
                                                    0, 0)));
  ASSERT_TRUE(bad_shell && good_hole_cs);
  std::array<GEOSGeometry*, 1> h1 = {good_hole_cs.release()};
  EXPECT_EQ(GEOSGeom_createPolygon_r(ctx_, bad_shell.release(), h1.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Shell")));
  ctx_.ClearMessages();

  ScopedGeom good_shell(
      ctx_, GEOSGeom_createLinearRing_r(
                ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, shell_coords.data(),
                                                    5, 0, 0)));
  ScopedGeom bad_hole(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 2.0, 2.0));
  ASSERT_TRUE(good_shell && bad_hole);
  std::array<GEOSGeometry*, 1> h2 = {bad_hole.release()};
  EXPECT_EQ(GEOSGeom_createPolygon_r(ctx_, good_shell.release(), h2.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Hole")));
  ctx_.ClearMessages();

  // nholes > 0 with nullptr array or nullptr hole entry still cleans up shell.
  ScopedGeom shell_null_arr(
      ctx_, GEOSGeom_createLinearRing_r(
                ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, shell_coords.data(),
                                                    5, 0, 0)));
  ASSERT_TRUE(shell_null_arr);
  EXPECT_EQ(
      GEOSGeom_createPolygon_r(ctx_, shell_null_arr.release(), nullptr, 1),
      nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Hole")));
  ctx_.ClearMessages();

  ScopedGeom shell_null_elem(
      ctx_, GEOSGeom_createLinearRing_r(
                ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, shell_coords.data(),
                                                    5, 0, 0)));
  ASSERT_TRUE(shell_null_elem);
  std::array<GEOSGeometry*, 1> h_null = {nullptr};
  EXPECT_EQ(GEOSGeom_createPolygon_r(ctx_, shell_null_elem.release(),
                                     h_null.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Hole")));
}

TEST_F(GeosTsCTest, CurvedGeometriesAndAllGeomTypes) {
  ScopedGeom empty_cs_geom(ctx_, GEOSGeom_createEmptyCircularString_r(ctx_));
  ScopedGeom empty_cc(ctx_, GEOSGeom_createEmptyCompoundCurve_r(ctx_));
  ScopedGeom empty_cp(ctx_, GEOSGeom_createEmptyCurvePolygon_r(ctx_));
  ASSERT_TRUE(empty_cs_geom && empty_cc && empty_cp);

  const std::array<double, 10> arc_coords = {0, 0, 1, 1, 2, 0, 1, -1, 0, 0};
  ScopedCoordSeq arc_cs(
      ctx_, GEOSCoordSeq_copyFromBuffer_r(ctx_, arc_coords.data(), 5, 0, 0));
  ASSERT_TRUE(arc_cs);
  ScopedGeom circ(ctx_,
                  GEOSGeom_createCircularString_r(ctx_, arc_cs.release()));
  ASSERT_TRUE(circ);
  EXPECT_NE(GEOSGeom_getCoordSeq_r(ctx_, circ.get()), nullptr);
  EXPECT_EQ(GEOSisClosed_r(ctx_, circ.get()), 1);

  // CompoundCurve from LineString + CircularString.
  ScopedGeom seg = Wkt("LINESTRING (0 0, 2 0)");
  ScopedGeom arc = Wkt("CIRCULARSTRING (2 0, 1 1, 0 0)");
  ASSERT_TRUE(seg && arc);
  std::array<GEOSGeometry*, 2> cc_parts = {seg.release(), arc.release()};
  ScopedGeom cc(ctx_, GEOSGeom_createCompoundCurve_r(ctx_, cc_parts.data(), 2));
  ASSERT_TRUE(cc);
  EXPECT_EQ(GEOSisClosed_r(ctx_, cc.get()), 1);

  // CompoundCurve error path: non-curve cleans all elements.
  ScopedGeom seg2 = Wkt("LINESTRING (0 0, 1 0)");
  ScopedGeom bad_cc_pt(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 5.0, 5.0));
  ASSERT_TRUE(seg2 && bad_cc_pt);
  std::array<GEOSGeometry*, 2> bad_cc_parts = {seg2.release(),
                                               bad_cc_pt.release()};
  EXPECT_EQ(GEOSGeom_createCompoundCurve_r(ctx_, bad_cc_parts.data(), 2),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("SimpleCurve")));
  ctx_.ClearMessages();

  // CurvePolygon with closed CircularString shell and hole.
  const std::array<double, 10> inner_arc = {0.5, 0, 1,    0.5, 1.5,
                                            0,   1, -0.5, 0.5, 0};
  ScopedGeom hole_circ(ctx_, GEOSGeom_createCircularString_r(
                                 ctx_, GEOSCoordSeq_copyFromBuffer_r(
                                           ctx_, inner_arc.data(), 5, 0, 0)));
  ASSERT_TRUE(hole_circ);
  std::array<GEOSGeometry*, 1> cp_holes = {hole_circ.release()};
  ScopedGeom cp(ctx_, GEOSGeom_createCurvePolygon_r(ctx_, circ.release(),
                                                    cp_holes.data(), 1));
  ASSERT_TRUE(cp);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, cp.get()), 1);
  EXPECT_NE(GEOSGetExteriorRing_r(ctx_, cp.get()), nullptr);

  ScopedGeom cp_shell_only = Wkt("CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)");
  ASSERT_TRUE(cp_shell_only);
  ScopedGeom cp_no_holes(ctx_, GEOSGeom_createCurvePolygon_r(
                                   ctx_, cp_shell_only.release(), nullptr, 0));
  ASSERT_TRUE(cp_no_holes);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, cp_no_holes.get()), 0);

  // CurvePolygon error paths: bad shell and bad hole free all transferred args.
  ScopedGeom bad_shell(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 0.0, 0.0));
  ScopedGeom hole_curve = Wkt("CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)");
  ASSERT_TRUE(bad_shell && hole_curve);
  std::array<GEOSGeometry*, 1> bad_cp_holes = {hole_curve.release()};
  EXPECT_EQ(GEOSGeom_createCurvePolygon_r(ctx_, bad_shell.release(),
                                          bad_cp_holes.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedGeom ok_shell = Wkt("CIRCULARSTRING (0 0, 1 1, 2 0, 1 -1, 0 0)");
  ScopedGeom bad_cp_hole(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 1.0, 0.0));
  ASSERT_TRUE(ok_shell && bad_cp_hole);
  std::array<GEOSGeometry*, 1> bad_cp_holes2 = {bad_cp_hole.release()};
  EXPECT_EQ(GEOSGeom_createCurvePolygon_r(ctx_, ok_shell.release(),
                                          bad_cp_holes2.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  // Verify GEOSGeomType_r and GEOSGeomTypeId_r for all 13 geometry types.
  ScopedGeom g_pt = Wkt("POINT (0 0)");
  ScopedGeom g_ls = Wkt("LINESTRING (0 0, 1 1)");
  const std::array<double, 8> tri = {0, 0, 1, 0, 0, 1, 0, 0};
  ScopedGeom ring_geom(
      ctx_, GEOSGeom_createLinearRing_r(ctx_, GEOSCoordSeq_copyFromBuffer_r(
                                                  ctx_, tri.data(), 4, 0, 0)));
  ScopedGeom g_poly = Wkt("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  ScopedGeom g_mp = Wkt("MULTIPOINT ((0 0), (1 1))");
  ScopedGeom g_mls = Wkt("MULTILINESTRING ((0 0, 1 1))");
  ScopedGeom g_mpoly = Wkt("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 0)))");
  ScopedGeom g_gc = Wkt("GEOMETRYCOLLECTION (POINT (0 0))");
  ScopedGeom g_circ = Wkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ScopedGeom g_mc = Wkt("MULTICURVE ((0 0, 1 1))");
  ScopedGeom g_ms = Wkt("MULTISURFACE (((0 0, 1 0, 1 1, 0 0)))");
  ASSERT_TRUE(ring_geom);

  struct TypeCase {
    const GEOSGeometry* geom;
    int id;
    const char* name;
  };
  const std::array<TypeCase, 13> cases = {{
      {g_pt.get(), GEOS_POINT, "Point"},
      {g_ls.get(), GEOS_LINESTRING, "LineString"},
      {ring_geom.get(), GEOS_LINEARRING, "LinearRing"},
      {g_poly.get(), GEOS_POLYGON, "Polygon"},
      {g_mp.get(), GEOS_MULTIPOINT, "MultiPoint"},
      {g_mls.get(), GEOS_MULTILINESTRING, "MultiLineString"},
      {g_mpoly.get(), GEOS_MULTIPOLYGON, "MultiPolygon"},
      {g_gc.get(), GEOS_GEOMETRYCOLLECTION, "GeometryCollection"},
      {g_circ.get(), GEOS_CIRCULARSTRING, "CircularString"},
      {cc.get(), GEOS_COMPOUNDCURVE, "CompoundCurve"},
      {cp.get(), GEOS_CURVEPOLYGON, "CurvePolygon"},
      {g_mc.get(), GEOS_MULTICURVE, "MultiCurve"},
      {g_ms.get(), GEOS_MULTISURFACE, "MultiSurface"},
  }};
  for (const TypeCase& tc : cases) {
    ASSERT_NE(tc.geom, nullptr);
    EXPECT_EQ(GEOSGeomTypeId_r(ctx_, tc.geom), tc.id);
    ScopedCString tname(ctx_, GEOSGeomType_r(ctx_, tc.geom));
    ASSERT_TRUE(tname);
    EXPECT_STREQ(tname.get(), tc.name);
  }
}

TEST_F(GeosTsCTest, CollectionsCreateReleaseAndAccessors) {
  constexpr std::array<int, 6> kCollectionTypes = {
      GEOS_GEOMETRYCOLLECTION, GEOS_MULTIPOINT, GEOS_MULTILINESTRING,
      GEOS_MULTIPOLYGON,       GEOS_MULTICURVE, GEOS_MULTISURFACE,
  };
  for (int t : kCollectionTypes) {
    ScopedGeom empty_col(ctx_, GEOSGeom_createEmptyCollection_r(ctx_, t));
    ASSERT_TRUE(empty_col);
    EXPECT_EQ(GEOSGeomTypeId_r(ctx_, empty_col.get()), t);
    EXPECT_EQ(GEOSisEmpty_r(ctx_, empty_col.get()), 1);
    EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, empty_col.get()), 0);
  }

  // Unsupported type error paths.
  EXPECT_EQ(GEOSGeom_createEmptyCollection_r(ctx_, GEOS_POINT), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedGeom orphan_pt(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 1.0, 2.0));
  ASSERT_TRUE(orphan_pt);
  std::array<GEOSGeometry*, 1> orphan_arr = {orphan_pt.release()};
  EXPECT_EQ(GEOSGeom_createCollection_r(ctx_, GEOS_POINT, orphan_arr.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Unsupported type")));
  ctx_.ClearMessages();

  // Populate all 6 collection types.
  ScopedGeom gc_pt = Wkt("POINT (0 0)");
  ScopedGeom gc_line = Wkt("LINESTRING (1 1, 2 2)");
  ASSERT_TRUE(gc_pt && gc_line);
  std::array<GEOSGeometry*, 2> gc_elems = {gc_pt.release(), gc_line.release()};
  ScopedGeom gc(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_GEOMETRYCOLLECTION,
                                                  gc_elems.data(), 2));
  ASSERT_TRUE(gc);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, gc.get()), 2);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, GEOSGetGeometryN_r(ctx_, gc.get(), 1)),
            GEOS_LINESTRING);

  ScopedGeom mp_pt = Wkt("POINT (1 2)");
  ASSERT_TRUE(mp_pt);
  std::array<GEOSGeometry*, 1> mp_elems = {mp_pt.release()};
  ScopedGeom mp(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_MULTIPOINT,
                                                  mp_elems.data(), 1));
  ASSERT_TRUE(mp);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, mp.get()), GEOS_MULTIPOINT);

  ScopedGeom mls_line = Wkt("LINESTRING (0 0, 2 0)");
  ASSERT_TRUE(mls_line);
  std::array<GEOSGeometry*, 1> mls_elems = {mls_line.release()};
  ScopedGeom mls(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_MULTILINESTRING,
                                                   mls_elems.data(), 1));
  ASSERT_TRUE(mls);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, mls.get()), GEOS_MULTILINESTRING);

  ScopedGeom mpoly_p = Wkt("POLYGON ((0 0, 1 0, 1 1, 0 0))");
  ASSERT_TRUE(mpoly_p);
  std::array<GEOSGeometry*, 1> mpoly_elems = {mpoly_p.release()};
  ScopedGeom mpoly(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_MULTIPOLYGON,
                                                     mpoly_elems.data(), 1));
  ASSERT_TRUE(mpoly);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, mpoly.get()), GEOS_MULTIPOLYGON);

  // Mismatched child type throws and consumes child without leaking.
  ScopedGeom mismatch_pt = Wkt("POINT (0 0)");
  ASSERT_TRUE(mismatch_pt);
  std::array<GEOSGeometry*, 1> mismatch_elems = {mismatch_pt.release()};
  EXPECT_EQ(GEOSGeom_createCollection_r(ctx_, GEOS_MULTICURVE,
                                        mismatch_elems.data(), 1),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("MultiCurve")));
  ctx_.ClearMessages();

  ScopedGeom mc_line = Wkt("LINESTRING (0 0, 1 1)");
  ScopedGeom mc_arc = Wkt("CIRCULARSTRING (1 1, 2 2, 3 1)");
  ASSERT_TRUE(mc_line && mc_arc);
  std::array<GEOSGeometry*, 2> mc_elems = {mc_line.release(), mc_arc.release()};
  ScopedGeom mc(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_MULTICURVE,
                                                  mc_elems.data(), 2));
  ASSERT_TRUE(mc);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, mc.get()), 2);

  ScopedGeom ms_poly = Wkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ScopedGeom ms_cp = Wkt("CURVEPOLYGON ((10 0, 11 1, 12 0, 11 -1, 10 0))");
  ASSERT_TRUE(ms_poly && ms_cp);
  std::array<GEOSGeometry*, 2> ms_elems = {ms_poly.release(), ms_cp.release()};
  ScopedGeom ms(ctx_, GEOSGeom_createCollection_r(ctx_, GEOS_MULTISURFACE,
                                                  ms_elems.data(), 2));
  ASSERT_TRUE(ms);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, ms.get()), 2);

  // Release collection transfers child ownership cleanly.
  unsigned int released_count = 0;
  ScopedGeosBuffer<GEOSGeometry*> released_buf(
      ctx_, GEOSGeom_releaseCollection_r(ctx_, ms.get(), &released_count));
  ASSERT_TRUE(released_buf);
  ASSERT_EQ(released_count, 2u);
  std::vector<ScopedGeom> owned_children;
  owned_children.reserve(released_count);
  for (unsigned int i = 0; i < released_count; ++i) {
    owned_children.emplace_back(ctx_, released_buf.get()[i]);
  }
  EXPECT_EQ(GEOSisEmpty_r(ctx_, ms.get()), 1);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, owned_children[0].get()), GEOS_POLYGON);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, owned_children[1].get()), GEOS_CURVEPOLYGON);

  // Releasing an empty collection returns nullptr with ngeoms = 0 and no error.
  released_count = 99;
  EXPECT_EQ(GEOSGeom_releaseCollection_r(ctx_, ms.get(), &released_count),
            nullptr);
  EXPECT_EQ(released_count, 0u);
  EXPECT_THAT(ctx_.errors(), IsEmpty());

  // Error cases for releaseCollection and GetGeometryN.
  ScopedGeom pt(ctx_, GEOSGeom_createPointFromXY_r(ctx_, 0.0, 0.0));
  ASSERT_TRUE(pt);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, pt.get()), 1);
  EXPECT_EQ(GEOSGetGeometryN_r(ctx_, mc.get(), -1), nullptr);
  EXPECT_EQ(GEOSGeom_releaseCollection_r(ctx_, pt.get(), &released_count),
            nullptr);
  EXPECT_EQ(GEOSGeom_releaseCollection_r(ctx_, pt.get(), nullptr), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, MetadataMutatorsPrecisionAndTransforms) {
  ScopedGeom poly = Wkt("POLYGON ((0 0, 4 0, 4 2, 0 2, 0 0))");
  ASSERT_TRUE(poly);

  GEOSSetSRID_r(ctx_, poly.get(), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, poly.get()), 4326);

  int tag = 77;
  GEOSGeom_setUserData_r(ctx_, poly.get(), &tag);
  EXPECT_EQ(GEOSGeom_getUserData_r(ctx_, poly.get()), &tag);

  EXPECT_EQ(GEOSGetNumCoordinates_r(ctx_, poly.get()), 5);
  EXPECT_EQ(GEOSGeom_getDimensions_r(ctx_, poly.get()), 2);
  EXPECT_EQ(GEOSGeom_getCoordinateDimension_r(ctx_, poly.get()), 2);

  double xmin = 0.0;
  double ymin = 0.0;
  double xmax = 0.0;
  double ymax = 0.0;
  double single = 0.0;
  EXPECT_EQ(GEOSGeom_getXMin_r(ctx_, poly.get(), &single), 1);
  EXPECT_DOUBLE_EQ(single, 0.0);
  EXPECT_EQ(GEOSGeom_getXMax_r(ctx_, poly.get(), &single), 1);
  EXPECT_DOUBLE_EQ(single, 4.0);
  EXPECT_EQ(GEOSGeom_getYMin_r(ctx_, poly.get(), &single), 1);
  EXPECT_DOUBLE_EQ(single, 0.0);
  EXPECT_EQ(GEOSGeom_getYMax_r(ctx_, poly.get(), &single), 1);
  EXPECT_DOUBLE_EQ(single, 2.0);
  EXPECT_EQ(GEOSGeom_getExtent_r(ctx_, poly.get(), &xmin, &ymin, &xmax, &ymax),
            1);
  EXPECT_DOUBLE_EQ(xmin, 0.0);
  EXPECT_DOUBLE_EQ(ymin, 0.0);
  EXPECT_DOUBLE_EQ(xmax, 4.0);
  EXPECT_DOUBLE_EQ(ymax, 2.0);

  ScopedGeom empty_poly(ctx_, GEOSGeom_createEmptyPolygon_r(ctx_));
  ASSERT_TRUE(empty_poly);
  EXPECT_EQ(GEOSGeom_getXMin_r(ctx_, empty_poly.get(), &single), 0);
  EXPECT_EQ(GEOSGeom_getXMax_r(ctx_, empty_poly.get(), &single), 0);
  EXPECT_EQ(GEOSGeom_getYMin_r(ctx_, empty_poly.get(), &single), 0);
  EXPECT_EQ(GEOSGeom_getYMax_r(ctx_, empty_poly.get(), &single), 0);
  EXPECT_EQ(
      GEOSGeom_getExtent_r(ctx_, empty_poly.get(), &xmin, &ymin, &xmax, &ymax),
      0);

  // Normalize and OrientPolygons.
  EXPECT_EQ(GEOSNormalize_r(ctx_, poly.get()), 0);
  EXPECT_EQ(GEOSOrientPolygons_r(ctx_, poly.get(), 1), 0);
  char is_ccw = -1;
  const GEOSGeometry* ext = GEOSGetExteriorRing_r(ctx_, poly.get());
  ASSERT_NE(ext, nullptr);
  EXPECT_EQ(
      GEOSCoordSeq_isCCW_r(ctx_, GEOSGeom_getCoordSeq_r(ctx_, ext), &is_ccw),
      1);
  EXPECT_EQ(is_ccw, 0);
  EXPECT_EQ(GEOSOrientPolygons_r(ctx_, poly.get(), 0), 0);
  EXPECT_EQ(
      GEOSCoordSeq_isCCW_r(ctx_, GEOSGeom_getCoordSeq_r(ctx_, ext), &is_ccw),
      1);
  EXPECT_EQ(is_ccw, 1);

  ScopedGeom cp = Wkt("CURVEPOLYGON ((0 0, 1 1, 2 0, 1 -1, 0 0))");
  ASSERT_TRUE(cp);
  EXPECT_EQ(GEOSOrientPolygons_r(ctx_, cp.get(), 1), -1);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Curved")));
  ctx_.ClearMessages();

  // Precision reduction and unchanged-gridSize fast path.
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision_r(ctx_, poly.get()), 0.0);
  ScopedGeom sliver =
      Wkt("POLYGON ((0.1 0.1, 4.2 0.1, 4.2 0.3, 0.1 0.3, 0.1 0.1))");
  ASSERT_TRUE(sliver);
  GEOSSetSRID_r(ctx_, sliver.get(), 3857);
  ScopedGeom reduced(ctx_, GEOSGeom_setPrecision_r(ctx_, sliver.get(), 0.5,
                                                   GEOS_PREC_VALID_OUTPUT));
  ASSERT_TRUE(reduced);
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision_r(ctx_, reduced.get()), 0.5);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, reduced.get()), 0);

  // Same gridSize hits factory-change branch (preserving SRID); gridSize 0
  // restores floating PM.
  GEOSSetSRID_r(ctx_, reduced.get(), 3857);
  ScopedGeom same_prec(ctx_,
                       GEOSGeom_setPrecision_r(ctx_, reduced.get(), 0.5, 0));
  ScopedGeom floating(ctx_,
                      GEOSGeom_setPrecision_r(ctx_, reduced.get(), 0.0, 0));
  ASSERT_TRUE(same_prec && floating);
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision_r(ctx_, same_prec.get()), 0.5);
  EXPECT_DOUBLE_EQ(GEOSGeom_getPrecision_r(ctx_, floating.get()), 0.0);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, same_prec.get()), 3857);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, floating.get()), 3857);

  ScopedGeom kept_collapsed(
      ctx_,
      GEOSGeom_setPrecision_r(ctx_, sliver.get(), 1.0,
                              GEOS_PREC_NO_TOPO | GEOS_PREC_KEEP_COLLAPSED));
  EXPECT_TRUE(kept_collapsed);

  // Coordinate transforms (transformXY preserves Z/M, transformXYZ updates Z).
  const std::array<double, 4> pt4_buf = {1.0, 2.0, 3.0, 4.0};
  ScopedGeom pt4(
      ctx_, GEOSGeom_createPoint_r(ctx_, GEOSCoordSeq_copyFromBuffer_r(
                                             ctx_, pt4_buf.data(), 1, 1, 1)));
  ASSERT_TRUE(pt4);
  double shift = 10.0;
  ScopedGeom shifted_xy(ctx_, GEOSGeom_transformXY_r(
                                  ctx_, pt4.get(),
                                  [](double* px, double* py, void* ud) -> int {
                                    const double d = *static_cast<double*>(ud);
                                    *px += d;
                                    *py += 2.0 * d;
                                    return 1;
                                  },
                                  &shift));
  ASSERT_TRUE(shifted_xy);
  double ox = 0.0;
  double oy = 0.0;
  double oz = 0.0;
  double om = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, shifted_xy.get(), &ox), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, shifted_xy.get(), &oy), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, shifted_xy.get(), &oz), 1);
  EXPECT_EQ(GEOSGeomGetM_r(ctx_, shifted_xy.get(), &om), 1);
  EXPECT_DOUBLE_EQ(ox, 11.0);
  EXPECT_DOUBLE_EQ(oy, 22.0);
  EXPECT_DOUBLE_EQ(oz, 3.0);
  EXPECT_DOUBLE_EQ(om, 4.0);

  ScopedGeom shifted_xyz(
      ctx_, GEOSGeom_transformXYZ_r(
                ctx_, pt4.get(),
                [](double* px, double* py, double* pz, void* ud) -> int {
                  const double d = *static_cast<double*>(ud);
                  *px += d;
                  *py -= d;
                  *pz *= 2.0;
                  return 1;
                },
                &shift));
  ASSERT_TRUE(shifted_xyz);
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, shifted_xyz.get(), &ox), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, shifted_xyz.get(), &oy), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, shifted_xyz.get(), &oz), 1);
  EXPECT_EQ(GEOSGeomGetM_r(ctx_, shifted_xyz.get(), &om), 1);
  EXPECT_DOUBLE_EQ(ox, 11.0);
  EXPECT_DOUBLE_EQ(oy, -8.0);
  EXPECT_DOUBLE_EQ(oz, 6.0);
  EXPECT_DOUBLE_EQ(om, 4.0);

  // Transform callbacks returning 0 trigger error and return nullptr.
  EXPECT_EQ(GEOSGeom_transformXY_r(
                ctx_, pt4.get(),
                [](double*, double*, void*) -> int { return 0; }, nullptr),
            nullptr);
  EXPECT_EQ(
      GEOSGeom_transformXYZ_r(
          ctx_, pt4.get(),
          [](double*, double*, double*, void*) -> int { return 0; }, nullptr),
      nullptr);
  EXPECT_EQ(ctx_.errors().size(), 2u);
}

// ============================================================================
// Section 4: Binary predicates, DE-9IM relate, validity, and repair
// ============================================================================

TEST_F(GeosTsCTest, BinaryPredicatesTrueAndFalseCases) {
  ScopedGeom box1 = Wkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ScopedGeom box1_dup = Wkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ScopedGeom box1_rev = Wkt("POLYGON ((0 0, 0 4, 4 4, 4 0, 0 0))");
  ScopedGeom box_near = Wkt("POLYGON ((0 0, 4.05 0, 4.05 4, 0 4, 0 0))");
  ScopedGeom box_touch = Wkt("POLYGON ((4 0, 8 0, 8 4, 4 4, 4 0))");
  ScopedGeom box_overlap = Wkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");
  ScopedGeom box_disjoint =
      Wkt("POLYGON ((10 10, 12 10, 12 12, 10 12, 10 10))");
  ScopedGeom box_inside = Wkt("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  ScopedGeom line_cross = Wkt("LINESTRING (-1 2, 5 2)");
  ScopedGeom line_in = Wkt("LINESTRING (1 1, 3 3)");
  ScopedGeom edge_line = Wkt("LINESTRING (0 0, 4 0)");
  ASSERT_TRUE(box1 && box1_dup && box1_rev && box_near && box_touch &&
              box_overlap && box_disjoint && box_inside && line_cross &&
              line_in && edge_line);

  EXPECT_EQ(GEOSDisjoint_r(ctx_, box1.get(), box_disjoint.get()), 1);
  EXPECT_EQ(GEOSDisjoint_r(ctx_, box1.get(), box_overlap.get()), 0);

  EXPECT_EQ(GEOSTouches_r(ctx_, box1.get(), box_touch.get()), 1);
  EXPECT_EQ(GEOSTouches_r(ctx_, box1.get(), box_overlap.get()), 0);

  EXPECT_EQ(GEOSIntersects_r(ctx_, box1.get(), box_overlap.get()), 1);
  EXPECT_EQ(GEOSIntersects_r(ctx_, box1.get(), box_disjoint.get()), 0);

  EXPECT_EQ(GEOSCrosses_r(ctx_, line_cross.get(), box1.get()), 1);
  EXPECT_EQ(GEOSCrosses_r(ctx_, line_in.get(), box1.get()), 0);

  EXPECT_EQ(GEOSWithin_r(ctx_, box_inside.get(), box1.get()), 1);
  EXPECT_EQ(GEOSWithin_r(ctx_, box_overlap.get(), box1.get()), 0);

  EXPECT_EQ(GEOSContains_r(ctx_, box1.get(), box_inside.get()), 1);
  EXPECT_EQ(GEOSContains_r(ctx_, box1.get(), edge_line.get()), 0);

  EXPECT_EQ(GEOSOverlaps_r(ctx_, box1.get(), box_overlap.get()), 1);
  EXPECT_EQ(GEOSOverlaps_r(ctx_, box1.get(), box_inside.get()), 0);

  EXPECT_EQ(GEOSCovers_r(ctx_, box1.get(), edge_line.get()), 1);
  EXPECT_EQ(GEOSCovers_r(ctx_, box1.get(), box_overlap.get()), 0);

  EXPECT_EQ(GEOSCoveredBy_r(ctx_, edge_line.get(), box1.get()), 1);
  EXPECT_EQ(GEOSCoveredBy_r(ctx_, line_cross.get(), box1.get()), 0);

  EXPECT_EQ(GEOSEquals_r(ctx_, box1.get(), box1_rev.get()), 1);
  EXPECT_EQ(GEOSEquals_r(ctx_, box1.get(), box_inside.get()), 0);

  EXPECT_EQ(GEOSEqualsExact_r(ctx_, box1.get(), box_near.get(), 0.1), 1);
  EXPECT_EQ(GEOSEqualsExact_r(ctx_, box1.get(), box_near.get(), 0.01), 0);
  EXPECT_EQ(GEOSEqualsExact_r(ctx_, box1.get(), box1_rev.get(), 0.0), 0);

  EXPECT_EQ(GEOSEqualsIdentical_r(ctx_, box1.get(), box1_dup.get()), 1);
  EXPECT_EQ(GEOSEqualsIdentical_r(ctx_, box1.get(), box1_rev.get()), 0);

  const std::array<double, 4> p_xyzm = {1.0, 2.0, 3.0, 4.0};
  const std::array<double, 4> p_xyzm2 = {1.0, 2.0, 3.0, 9.0};
  ScopedGeom g_xyzm1(
      ctx_, GEOSGeom_createPoint_r(ctx_, GEOSCoordSeq_copyFromBuffer_r(
                                             ctx_, p_xyzm.data(), 1, 1, 1)));
  ScopedGeom g_xyzm2(
      ctx_, GEOSGeom_createPoint_r(ctx_, GEOSCoordSeq_copyFromBuffer_r(
                                             ctx_, p_xyzm2.data(), 1, 1, 1)));
  ASSERT_TRUE(g_xyzm1 && g_xyzm2);
  EXPECT_EQ(GEOSEqualsIdentical_r(ctx_, g_xyzm1.get(), g_xyzm1.get()), 1);
  EXPECT_EQ(GEOSEqualsIdentical_r(ctx_, g_xyzm1.get(), g_xyzm2.get()), 0);
}

TEST_F(GeosTsCTest, RelateMatrixPatternAndBoundaryNodeRules) {
  ScopedGeom a = Wkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ScopedGeom b = Wkt("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  ASSERT_TRUE(a && b);

  ScopedCString matrix(ctx_, GEOSRelate_r(ctx_, a.get(), b.get()));
  ASSERT_TRUE(matrix);
  EXPECT_STREQ(matrix.get(), "212101212");

  EXPECT_EQ(GEOSRelatePattern_r(ctx_, a.get(), b.get(), "2*2*0*212"), 1);
  EXPECT_EQ(GEOSRelatePattern_r(ctx_, a.get(), b.get(), "FF*FF****"), 0);
  EXPECT_EQ(GEOSRelatePattern_r(ctx_, a.get(), b.get(), "BAD"), 2);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, matrix.get(), "2*2*0*212"), 1);
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, matrix.get(), "FF*FF****"), 0);
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, "SHORT", "T********"), 2);
  ctx_.ClearMessages();

  // At (1, 0), l_split has a degree-2 (bivalent) endpoint, while l_touch ends
  // at (1, 0). Boundary node rules classify (1, 0) differently.
  ScopedGeom l_split = Wkt("MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))");
  ScopedGeom l_touch = Wkt("LINESTRING (1 0, 1 2)");
  ASSERT_TRUE(l_split && l_touch);

  ScopedCString bnr_mod2(
      ctx_, GEOSRelateBoundaryNodeRule_r(ctx_, l_split.get(), l_touch.get(),
                                         GEOSRELATE_BNR_MOD2));
  ScopedCString bnr_ep(
      ctx_, GEOSRelateBoundaryNodeRule_r(ctx_, l_split.get(), l_touch.get(),
                                         GEOSRELATE_BNR_ENDPOINT));
  ScopedCString bnr_multi(
      ctx_, GEOSRelateBoundaryNodeRule_r(ctx_, l_split.get(), l_touch.get(),
                                         GEOSRELATE_BNR_MULTIVALENT_ENDPOINT));
  ScopedCString bnr_mono(
      ctx_, GEOSRelateBoundaryNodeRule_r(ctx_, l_split.get(), l_touch.get(),
                                         GEOSRELATE_BNR_MONOVALENT_ENDPOINT));
  ASSERT_TRUE(bnr_mod2 && bnr_ep && bnr_multi && bnr_mono);

  // MOD2 and MONOVALENT_ENDPOINT treat degree-2 (1, 0) in l_split as interior
  // while degree-1 (1, 0) in l_touch is boundary (I-B = 0).
  EXPECT_STREQ(bnr_mod2.get(), "F01FF0102");
  EXPECT_STREQ(bnr_mono.get(), "F01FF0102");
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, bnr_mod2.get(), "F0*******"), 1);
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, bnr_mono.get(), "F0*******"), 1);
  // ENDPOINT treats both endpoints as boundary (B-B = 0).
  EXPECT_STREQ(bnr_ep.get(), "FF1F00102");
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, bnr_ep.get(), "FF**0****"), 1);
  // MULTIVALENT_ENDPOINT requires degree > 1 for boundary: degree-2 (1, 0) in
  // l_split is boundary while degree-1 (1, 0) in l_touch is interior (B-I = 0).
  EXPECT_STREQ(bnr_multi.get(), "FF10FF1F2");
  EXPECT_EQ(GEOSRelatePatternMatch_r(ctx_, bnr_multi.get(), "FF*0F****"), 1);
  EXPECT_STRNE(bnr_mod2.get(), bnr_ep.get());
  EXPECT_STRNE(bnr_ep.get(), bnr_multi.get());

  EXPECT_EQ(
      GEOSRelateBoundaryNodeRule_r(ctx_, l_split.get(), l_touch.get(), 99),
      nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Invalid boundary node rule")));
}

TEST_F(GeosTsCTest, ValiditySimplicityRingAndClosedPredicates) {
  ScopedGeom pt = Wkt("POINT (1 2)");
  ScopedGeom open_line = Wkt("LINESTRING (0 0, 2 0, 2 2)");
  ScopedGeom closed_line = Wkt("LINESTRING (0 0, 2 0, 2 2, 0 0)");
  ScopedGeom fig8_line = Wkt("LINESTRING (0 0, 2 2, 0 2, 2 0, 0 0)");
  ScopedGeom double_cross =
      Wkt("LINESTRING (0 0, 2 2, 2 0, 0 2, -1 3, 7 3, 6 2, 4 0, 4 2, 6 0)");
  ScopedGeom mls_closed =
      Wkt("MULTILINESTRING ((0 0, 1 0, 0 1, 0 0), (2 0, 3 0, 2 1, 2 0))");
  ScopedGeom mls_open =
      Wkt("MULTILINESTRING ((0 0, 1 0, 0 1, 0 0), (2 0, 3 0))");
  ScopedGeom mc_closed = Wkt("MULTICURVE ((0 0, 1 0, 0 1, 0 0))");
  ASSERT_TRUE(pt && open_line && closed_line && fig8_line && double_cross &&
              mls_closed && mls_open && mc_closed);

  EXPECT_EQ(GEOSisEmpty_r(ctx_, pt.get()), 0);
  EXPECT_EQ(GEOSisSimple_r(ctx_, open_line.get()), 1);
  EXPECT_EQ(GEOSisSimple_r(ctx_, fig8_line.get()), 0);

  EXPECT_EQ(GEOSisRing_r(ctx_, closed_line.get()), 1);
  EXPECT_EQ(GEOSisRing_r(ctx_, open_line.get()), 0);
  EXPECT_EQ(GEOSisRing_r(ctx_, fig8_line.get()), 0);
  EXPECT_EQ(GEOSisRing_r(ctx_, pt.get()), 0);

  EXPECT_EQ(GEOSisClosed_r(ctx_, closed_line.get()), 1);
  EXPECT_EQ(GEOSisClosed_r(ctx_, open_line.get()), 0);
  EXPECT_EQ(GEOSisClosed_r(ctx_, mls_closed.get()), 1);
  EXPECT_EQ(GEOSisClosed_r(ctx_, mls_open.get()), 0);
  EXPECT_EQ(GEOSisClosed_r(ctx_, mc_closed.get()), 1);
  EXPECT_EQ(GEOSisClosed_r(ctx_, pt.get()), 2);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  // GEOSisSimpleDetail_r for simple, 1 crossing, and 2 crossings.
  {
    GEOSGeometry* raw_detail = nullptr;
    EXPECT_EQ(GEOSisSimpleDetail_r(ctx_, open_line.get(), 0, &raw_detail), 1);
    EXPECT_EQ(raw_detail, nullptr);
  }
  {
    GEOSGeometry* raw_detail = nullptr;
    EXPECT_EQ(GEOSisSimpleDetail_r(ctx_, double_cross.get(), 0, &raw_detail),
              0);
    ScopedGeom detail(ctx_, raw_detail);
    ASSERT_TRUE(detail);
    EXPECT_EQ(GEOSGeomTypeId_r(ctx_, detail.get()), GEOS_POINT);
  }
  {
    GEOSGeometry* raw_detail = nullptr;
    EXPECT_EQ(GEOSisSimpleDetail_r(ctx_, double_cross.get(), 1, &raw_detail),
              0);
    ScopedGeom detail(ctx_, raw_detail);
    ASSERT_TRUE(detail);
    EXPECT_EQ(GEOSGeomTypeId_r(ctx_, detail.get()), GEOS_MULTIPOINT);
    EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, detail.get()), 2);
  }

  // isValid, isValidReason, isValidDetail.
  ScopedGeom valid_poly = Wkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ScopedGeom bowtie = Wkt("POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  ScopedGeom pinched_hole =
      Wkt("POLYGON ((0 0, 5 0, 3 5, 7 5, 5 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(valid_poly && bowtie && pinched_hole);

  EXPECT_EQ(GEOSisValid_r(ctx_, valid_poly.get()), 1);
  EXPECT_EQ(GEOSisValid_r(ctx_, bowtie.get()), 0);
  EXPECT_THAT(ctx_.notices(), Contains(HasSubstr("Self-intersection")));

  {
    ScopedCString ok_reason(ctx_, GEOSisValidReason_r(ctx_, valid_poly.get()));
    ScopedCString bad_reason(ctx_, GEOSisValidReason_r(ctx_, bowtie.get()));
    ASSERT_TRUE(ok_reason && bad_reason);
    EXPECT_STREQ(ok_reason.get(), "Valid Geometry");
    EXPECT_THAT(std::string(bad_reason.get()), HasSubstr("Self-intersection"));
  }
  {
    char* raw_reason = nullptr;
    GEOSGeometry* raw_loc = nullptr;
    EXPECT_EQ(
        GEOSisValidDetail_r(ctx_, valid_poly.get(), 0, &raw_reason, &raw_loc),
        1);
    EXPECT_EQ(raw_reason, nullptr);
    EXPECT_EQ(raw_loc, nullptr);
  }
  {
    char* raw_reason = nullptr;
    GEOSGeometry* raw_loc = nullptr;
    EXPECT_EQ(GEOSisValidDetail_r(ctx_, bowtie.get(), 0, &raw_reason, &raw_loc),
              0);
    ScopedCString reason(ctx_, raw_reason);
    ScopedGeom loc(ctx_, raw_loc);
    ASSERT_TRUE(reason && loc);
    EXPECT_THAT(std::string(reason.get()), HasSubstr("Self-intersection"));
    EXPECT_EQ(GEOSGeomTypeId_r(ctx_, loc.get()), GEOS_POINT);
    double lx = 0.0;
    double ly = 0.0;
    EXPECT_EQ(GEOSGeomGetX_r(ctx_, loc.get(), &lx), 1);
    EXPECT_EQ(GEOSGeomGetY_r(ctx_, loc.get(), &ly), 1);
    EXPECT_DOUBLE_EQ(lx, 1.0);
    EXPECT_DOUBLE_EQ(ly, 1.0);
  }
  // Null optional out-parameters on valid and invalid geometries.
  EXPECT_EQ(GEOSisValidDetail_r(ctx_, valid_poly.get(), 0, nullptr, nullptr),
            1);
  EXPECT_EQ(GEOSisValidDetail_r(ctx_, bowtie.get(), 0, nullptr, nullptr), 0);

  // Pinched self-touching hole allowed only when flag is set.
  EXPECT_EQ(GEOSisValidDetail_r(ctx_, pinched_hole.get(), 0, nullptr, nullptr),
            0);
  EXPECT_EQ(GEOSisValidDetail_r(ctx_, pinched_hole.get(),
                                GEOSVALID_ALLOW_SELFTOUCHING_RING_FORMING_HOLE,
                                nullptr, nullptr),
            1);
}

TEST_F(GeosTsCTest, MakeValidAndRemoveRepeatedPoints) {
  ScopedGeom bowtie = Wkt("POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))");
  ASSERT_TRUE(bowtie);
  GEOSSetSRID_r(ctx_, bowtie.get(), 4326);

  ScopedGeom fixed_default(ctx_, GEOSMakeValid_r(ctx_, bowtie.get()));
  ASSERT_TRUE(fixed_default);
  EXPECT_EQ(GEOSisValid_r(ctx_, fixed_default.get()), 1);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, fixed_default.get()), 4326);
  double area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, fixed_default.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 2.0);

  // MakeValidParams: LINEWORK and STRUCTURE with keepCollapsed 0 vs 1.
  ScopedGeom spiked = Wkt("POLYGON ((0 0, 2 0, 2 2, 5 2, 2 2, 0 2, 0 0))");
  ASSERT_TRUE(spiked);
  GEOSSetSRID_r(ctx_, spiked.get(), 32610);

  ScopedMakeValidParams params(ctx_, GEOSMakeValidParams_create_r(ctx_));
  ASSERT_TRUE(params);
  EXPECT_EQ(GEOSMakeValidParams_setMethod_r(ctx_, params.get(),
                                            GEOS_MAKE_VALID_LINEWORK),
            1);
  ScopedGeom lw_fixed(
      ctx_, GEOSMakeValidWithParams_r(ctx_, spiked.get(), params.get()));
  ASSERT_TRUE(lw_fixed);
  EXPECT_EQ(GEOSisValid_r(ctx_, lw_fixed.get()), 1);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, lw_fixed.get()), 32610);

  EXPECT_EQ(GEOSMakeValidParams_setMethod_r(ctx_, params.get(),
                                            GEOS_MAKE_VALID_STRUCTURE),
            1);
  EXPECT_EQ(GEOSMakeValidParams_setKeepCollapsed_r(ctx_, params.get(), 0), 1);
  ScopedGeom struct_drop(
      ctx_, GEOSMakeValidWithParams_r(ctx_, spiked.get(), params.get()));
  ASSERT_TRUE(struct_drop);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, struct_drop.get()), GEOS_POLYGON);
  EXPECT_EQ(GEOSisValid_r(ctx_, struct_drop.get()), 1);
  double struct_area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, struct_drop.get(), &struct_area), 1);
  EXPECT_DOUBLE_EQ(struct_area, 4.0);

  // keepCollapsed controls whether a completely collapsed ring drops to an
  // empty polygon (0) or degrades to a lower-dimension valid geometry (1).
  ScopedGeom collapsed = Wkt("POLYGON ((0 0, 2 0, 1 0, 0 0))");
  ASSERT_TRUE(collapsed);
  GEOSSetSRID_r(ctx_, collapsed.get(), 32610);

  ScopedGeom collapsed_drop(
      ctx_, GEOSMakeValidWithParams_r(ctx_, collapsed.get(), params.get()));
  ASSERT_TRUE(collapsed_drop);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, collapsed_drop.get()), GEOS_POLYGON);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, collapsed_drop.get()), 1);

  EXPECT_EQ(GEOSMakeValidParams_setKeepCollapsed_r(ctx_, params.get(), 1), 1);
  ScopedGeom struct_keep(
      ctx_, GEOSMakeValidWithParams_r(ctx_, collapsed.get(), params.get()));
  ASSERT_TRUE(struct_keep);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, struct_keep.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, struct_keep.get()), 0);
  EXPECT_EQ(GEOSisValid_r(ctx_, struct_keep.get()), 1);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, struct_keep.get()), 32610);

  // Null params triggers the unknown-method error branch cleanly without UB.
  EXPECT_EQ(GEOSMakeValidWithParams_r(ctx_, spiked.get(), nullptr), nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Unknown method")));
  ctx_.ClearMessages();

  // GEOSRemoveRepeatedPoints_r with exact 0.0 and non-zero tolerance.
  ScopedGeom dup_line = Wkt("LINESTRING (0 0, 0 0, 1 0, 1.1 0, 2 0)");
  ASSERT_TRUE(dup_line);
  GEOSSetSRID_r(ctx_, dup_line.get(), 4269);

  ScopedGeom dedup0(ctx_,
                    GEOSRemoveRepeatedPoints_r(ctx_, dup_line.get(), 0.0));
  ScopedGeom dedup_tol(ctx_,
                       GEOSRemoveRepeatedPoints_r(ctx_, dup_line.get(), 0.25));
  ASSERT_TRUE(dedup0 && dedup_tol);
  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, dedup0.get()), 4);
  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, dedup_tol.get()), 3);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, dedup_tol.get()), 4269);
}

// ============================================================================
// Section 5: Metrics, overlay, buffer, hulls, triangulation, noding, coverages
// ============================================================================

TEST_F(GeosTsCTest, MetricsDistancesAndNearestPoints) {
  ScopedGeom rect = Wkt("POLYGON ((0 0, 3 0, 3 4, 0 4, 0 0))");
  ScopedGeom seg_a = Wkt("LINESTRING (0 0, 0 4)");
  ScopedGeom seg_b = Wkt("LINESTRING (3 0, 3 4)");
  ScopedGeom empty_pt(ctx_, GEOSGeom_createEmptyPoint_r(ctx_));
  ASSERT_TRUE(rect && seg_a && seg_b && empty_pt);

  double val = -1.0;
  EXPECT_EQ(GEOSArea_r(ctx_, rect.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 12.0);
  EXPECT_EQ(GEOSLength_r(ctx_, rect.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 14.0);

  EXPECT_EQ(GEOSDistance_r(ctx_, seg_a.get(), seg_b.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 3.0);
  EXPECT_EQ(GEOSDistanceIndexed_r(ctx_, seg_a.get(), seg_b.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 3.0);
  EXPECT_EQ(GEOSDistanceWithin_r(ctx_, seg_a.get(), seg_b.get(), 3.1), 1);
  EXPECT_EQ(GEOSDistanceWithin_r(ctx_, seg_a.get(), seg_b.get(), 2.9), 0);

  EXPECT_EQ(GEOSHausdorffDistance_r(ctx_, seg_a.get(), seg_b.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 3.0);
  EXPECT_EQ(GEOSHausdorffDistanceDensify_r(ctx_, seg_a.get(), seg_b.get(), 0.25,
                                           &val),
            1);
  EXPECT_DOUBLE_EQ(val, 3.0);

  EXPECT_EQ(GEOSFrechetDistance_r(ctx_, seg_a.get(), seg_b.get(), &val), 1);
  EXPECT_DOUBLE_EQ(val, 3.0);
  EXPECT_EQ(
      GEOSFrechetDistanceDensify_r(ctx_, seg_a.get(), seg_b.get(), 0.25, &val),
      1);
  EXPECT_DOUBLE_EQ(val, 3.0);

  // Invalid densify fraction must return 0 and record error.
  EXPECT_EQ(
      GEOSHausdorffDistanceDensify_r(ctx_, seg_a.get(), seg_b.get(), 2.0, &val),
      0);
  EXPECT_EQ(
      GEOSFrechetDistanceDensify_r(ctx_, seg_a.get(), seg_b.get(), 2.0, &val),
      0);
  EXPECT_EQ(ctx_.errors().size(), 2u);
  ctx_.ClearMessages();

  ScopedCoordSeq nearest(ctx_,
                         GEOSNearestPoints_r(ctx_, seg_a.get(), seg_b.get()));
  ASSERT_TRUE(nearest);
  unsigned int nsz = 0;
  EXPECT_EQ(GEOSCoordSeq_getSize_r(ctx_, nearest.get(), &nsz), 1);
  EXPECT_EQ(nsz, 2u);

  // NearestPoints with empty input (first or second arg) returns nullptr
  // cleanly.
  EXPECT_EQ(GEOSNearestPoints_r(ctx_, empty_pt.get(), seg_b.get()), nullptr);
  EXPECT_EQ(GEOSNearestPoints_r(ctx_, seg_a.get(), empty_pt.get()), nullptr);
  EXPECT_THAT(ctx_.errors(), IsEmpty());
}

TEST_F(GeosTsCTest, EnvelopeBoundaryOverlayClipAndGrid) {
  ScopedGeom tri = Wkt("POLYGON ((0 0, 4 0, 2 3, 0 0))");
  GEOSSetSRID_r(ctx_, tri.get(), 4326);
  ScopedGeom box_a = Wkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  GEOSSetSRID_r(ctx_, box_a.get(), 4326);
  ScopedGeom box_b = Wkt("POLYGON ((2 0, 6 0, 6 4, 2 4, 2 0))");
  ASSERT_TRUE(tri && box_a && box_b);

  ScopedGeom env(ctx_, GEOSEnvelope_r(ctx_, tri.get()));
  ScopedGeom bnd(ctx_, GEOSBoundary_r(ctx_, tri.get()));
  ASSERT_TRUE(env && bnd);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, env.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, bnd.get()), 4326);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, env.get()), GEOS_POLYGON);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, bnd.get()), GEOS_LINESTRING);

  double area = 0.0;
  ScopedGeom isect(ctx_, GEOSIntersection_r(ctx_, box_a.get(), box_b.get()));
  ScopedGeom isect_p0(
      ctx_, GEOSIntersectionPrec_r(ctx_, box_a.get(), box_b.get(), 0.0));
  ScopedGeom isect_p1(
      ctx_, GEOSIntersectionPrec_r(ctx_, box_a.get(), box_b.get(), 0.5));
  ASSERT_TRUE(isect && isect_p0 && isect_p1);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, isect.get()), 4326);
  EXPECT_EQ(GEOSArea_r(ctx_, isect.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 8.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, isect.get(), isect_p0.get()), 1);
  EXPECT_EQ(GEOSEquals_r(ctx_, isect.get(), isect_p1.get()), 1);

  ScopedGeom diff(ctx_, GEOSDifference_r(ctx_, box_a.get(), box_b.get()));
  ScopedGeom diff_p0(ctx_,
                     GEOSDifferencePrec_r(ctx_, box_a.get(), box_b.get(), 0.0));
  ScopedGeom diff_p1(ctx_,
                     GEOSDifferencePrec_r(ctx_, box_a.get(), box_b.get(), 0.5));
  ASSERT_TRUE(diff && diff_p0 && diff_p1);
  EXPECT_EQ(GEOSArea_r(ctx_, diff.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 8.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, diff.get(), diff_p1.get()), 1);

  ScopedGeom sym(ctx_, GEOSSymDifference_r(ctx_, box_a.get(), box_b.get()));
  ScopedGeom sym_p0(
      ctx_, GEOSSymDifferencePrec_r(ctx_, box_a.get(), box_b.get(), 0.0));
  ScopedGeom sym_p1(
      ctx_, GEOSSymDifferencePrec_r(ctx_, box_a.get(), box_b.get(), 0.5));
  ASSERT_TRUE(sym && sym_p0 && sym_p1);
  EXPECT_EQ(GEOSArea_r(ctx_, sym.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 16.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, sym.get(), sym_p1.get()), 1);

  ScopedGeom uni(ctx_, GEOSUnion_r(ctx_, box_a.get(), box_b.get()));
  ScopedGeom uni_p0(ctx_, GEOSUnionPrec_r(ctx_, box_a.get(), box_b.get(), 0.0));
  ScopedGeom uni_p1(ctx_, GEOSUnionPrec_r(ctx_, box_a.get(), box_b.get(), 0.5));
  ASSERT_TRUE(uni && uni_p0 && uni_p1);
  EXPECT_EQ(GEOSArea_r(ctx_, uni.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 24.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, uni.get(), uni_p1.get()), 1);

  ScopedGeom mpoly_cov =
      Wkt("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((1 0, 2 0, 2 1, 1 1, 1 "
          "0)))");
  ScopedGeom mpoly_disj =
      Wkt("MULTIPOLYGON (((0 0, 1 0, 1 1, 0 1, 0 0)), ((3 0, 4 0, 4 1, 3 1, 3 "
          "0)))");
  ASSERT_TRUE(mpoly_cov && mpoly_disj);
  GEOSSetSRID_r(ctx_, mpoly_cov.get(), 4326);

  ScopedGeom u_unary(ctx_, GEOSUnaryUnion_r(ctx_, mpoly_cov.get()));
  ScopedGeom u_unary_p0(ctx_, GEOSUnaryUnionPrec_r(ctx_, mpoly_cov.get(), 0.0));
  ScopedGeom u_unary_p1(ctx_, GEOSUnaryUnionPrec_r(ctx_, mpoly_cov.get(), 0.5));
  ScopedGeom u_casc(ctx_, GEOSUnionCascaded_r(ctx_, mpoly_cov.get()));
  ScopedGeom u_cov(ctx_, GEOSCoverageUnion_r(ctx_, mpoly_cov.get()));
  ScopedGeom u_disj(ctx_, GEOSDisjointSubsetUnion_r(ctx_, mpoly_disj.get()));
  ASSERT_TRUE(u_unary && u_unary_p0 && u_unary_p1 && u_casc && u_cov && u_disj);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, u_cov.get()), 4326);
  EXPECT_EQ(GEOSArea_r(ctx_, u_cov.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 2.0);
  EXPECT_EQ(GEOSArea_r(ctx_, u_disj.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 2.0);
  EXPECT_EQ(GEOSEquals_r(ctx_, u_unary.get(), u_casc.get()), 1);

  // UnionCascaded rejects non-MultiPolygon.
  EXPECT_EQ(GEOSUnionCascaded_r(ctx_, box_a.get()), nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("MultiPolygon")));
  ctx_.ClearMessages();

  // ClipByRect and GridIntersectionFractions.
  ScopedGeom clipped(ctx_,
                     GEOSClipByRect_r(ctx_, box_a.get(), 1.0, 1.0, 3.0, 4.0));
  ASSERT_TRUE(clipped);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, clipped.get()), 4326);
  EXPECT_EQ(GEOSArea_r(ctx_, clipped.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 6.0);

  ScopedGeom unit_sq = Wkt("POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))");
  ASSERT_TRUE(unit_sq);
  std::array<float, 4> fractions{};
  EXPECT_EQ(GEOSGridIntersectionFractions_r(ctx_, unit_sq.get(), 0.0, 0.0, 2.0,
                                            2.0, 2, 2, fractions.data()),
            1);
  float sum = 0.0f;
  int ones = 0;
  for (float f : fractions) {
    sum += f;
    if (f == 1.0f) ++ones;
  }
  EXPECT_FLOAT_EQ(sum, 1.0f);
  EXPECT_EQ(ones, 1);

  ScopedGeom pt_err = Wkt("POINT (0.5 0.5)");
  ASSERT_TRUE(pt_err);
  EXPECT_EQ(GEOSGridIntersectionFractions_r(ctx_, pt_err.get(), 0.0, 0.0, 2.0,
                                            2.0, 2, 2, fractions.data()),
            0);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, BufferSimplifyDensifyAndSnap) {
  ScopedGeom line = Wkt("LINESTRING (0 0, 4 0)");
  ASSERT_TRUE(line);
  GEOSSetSRID_r(ctx_, line.get(), 3857);

  ScopedGeom buf(ctx_, GEOSBuffer_r(ctx_, line.get(), 1.0, 8));
  ASSERT_TRUE(buf);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, buf.get()), 3857);
  double area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, buf.get(), &area), 1);
  EXPECT_GT(area, 8.0);

  ScopedGeom buf_style(
      ctx_, GEOSBufferWithStyle_r(ctx_, line.get(), 1.0, 4, GEOSBUF_CAP_SQUARE,
                                  GEOSBUF_JOIN_MITRE, 5.0));
  ASSERT_TRUE(buf_style);
  EXPECT_EQ(GEOSArea_r(ctx_, buf_style.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 12.0);

  EXPECT_EQ(GEOSBufferWithStyle_r(ctx_, line.get(), 1.0, 4, 99,
                                  GEOSBUF_JOIN_ROUND, 5.0),
            nullptr);
  EXPECT_EQ(GEOSBufferWithStyle_r(ctx_, line.get(), 1.0, 4, GEOSBUF_CAP_ROUND,
                                  99, 5.0),
            nullptr);
  EXPECT_EQ(ctx_.errors().size(), 2u);
  ctx_.ClearMessages();

  ScopedBufferParams bp(ctx_, GEOSBufferParams_create_r(ctx_));
  ASSERT_TRUE(bp);
  EXPECT_EQ(GEOSBufferParams_setEndCapStyle_r(ctx_, bp.get(), GEOSBUF_CAP_FLAT),
            1);
  EXPECT_EQ(GEOSBufferParams_setJoinStyle_r(ctx_, bp.get(), GEOSBUF_JOIN_BEVEL),
            1);
  EXPECT_EQ(GEOSBufferParams_setMitreLimit_r(ctx_, bp.get(), 2.0), 1);
  EXPECT_EQ(GEOSBufferParams_setQuadrantSegments_r(ctx_, bp.get(), 4), 1);
  EXPECT_EQ(GEOSBufferParams_setSingleSided_r(ctx_, bp.get(), 1), 1);
  ScopedGeom buf_ss(ctx_,
                    GEOSBufferWithParams_r(ctx_, line.get(), bp.get(), 1.0));
  ASSERT_TRUE(buf_ss);
  EXPECT_EQ(GEOSArea_r(ctx_, buf_ss.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 4.0);

  EXPECT_EQ(GEOSBufferParams_setSingleSided_r(ctx_, bp.get(), 0), 1);
  EXPECT_EQ(GEOSBufferParams_setEndCapStyle_r(ctx_, bp.get(), 99), 0);
  EXPECT_EQ(GEOSBufferParams_setJoinStyle_r(ctx_, bp.get(), 99), 0);
  EXPECT_EQ(ctx_.errors().size(), 2u);
  ctx_.ClearMessages();

  // OffsetCurve and SingleSidedBuffer (including invalid join styles).
  ScopedGeom off_left(ctx_, GEOSOffsetCurve_r(ctx_, line.get(), 1.0, 4,
                                              GEOSBUF_JOIN_ROUND, 5.0));
  ScopedGeom off_right(ctx_, GEOSOffsetCurve_r(ctx_, line.get(), -1.0, 4,
                                               GEOSBUF_JOIN_ROUND, 5.0));
  ASSERT_TRUE(off_left && off_right);
  double miny = 0.0;
  double maxy = 0.0;
  EXPECT_EQ(GEOSGeom_getYMin_r(ctx_, off_left.get(), &miny), 1);
  EXPECT_DOUBLE_EQ(miny, 1.0);
  EXPECT_EQ(GEOSGeom_getYMax_r(ctx_, off_right.get(), &maxy), 1);
  EXPECT_DOUBLE_EQ(maxy, -1.0);
  EXPECT_EQ(GEOSOffsetCurve_r(ctx_, line.get(), 1.0, 4, 99, 5.0), nullptr);
  ctx_.ClearMessages();

  // GEOSSingleSidedBuffer_r returns a 1D offset LineString on left/right side.
  ScopedGeom ss_left(ctx_, GEOSSingleSidedBuffer_r(ctx_, line.get(), 1.0, 4,
                                                   GEOSBUF_JOIN_MITRE, 5.0, 1));
  ScopedGeom ss_right(ctx_,
                      GEOSSingleSidedBuffer_r(ctx_, line.get(), 1.0, 4,
                                              GEOSBUF_JOIN_MITRE, 5.0, 0));
  ASSERT_TRUE(ss_left && ss_right);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, ss_left.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, ss_right.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, ss_left.get()), 3857);
  double ss_len = 0.0;
  EXPECT_EQ(GEOSLength_r(ctx_, ss_left.get(), &ss_len), 1);
  EXPECT_DOUBLE_EQ(ss_len, 4.0);
  EXPECT_EQ(GEOSLength_r(ctx_, ss_right.get(), &ss_len), 1);
  EXPECT_DOUBLE_EQ(ss_len, 4.0);
  EXPECT_EQ(GEOSGeom_getYMin_r(ctx_, ss_left.get(), &miny), 1);
  EXPECT_DOUBLE_EQ(miny, 1.0);
  EXPECT_EQ(GEOSGeom_getYMax_r(ctx_, ss_right.get(), &maxy), 1);
  EXPECT_DOUBLE_EQ(maxy, -1.0);
  EXPECT_EQ(GEOSSingleSidedBuffer_r(ctx_, line.get(), 1.0, 4, 99, 5.0, 1),
            nullptr);
  ctx_.ClearMessages();

  // Densify, Simplify, TopologyPreserveSimplify, and Snap.
  ScopedGeom dense(ctx_, GEOSDensify_r(ctx_, line.get(), 1.5));
  ASSERT_TRUE(dense);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, dense.get()), 3857);
  EXPECT_GT(GEOSGeomGetNumPoints_r(ctx_, dense.get()), 2);
  EXPECT_EQ(GEOSDensify_r(ctx_, line.get(), -1.0), nullptr);
  ctx_.ClearMessages();

  ScopedGeom zig = Wkt("LINESTRING (0 0, 1 0.05, 2 -0.05, 3 0)");
  ASSERT_TRUE(zig);
  GEOSSetSRID_r(ctx_, zig.get(), 4326);
  ScopedGeom simp(ctx_, GEOSSimplify_r(ctx_, zig.get(), 0.2));
  ScopedGeom tpsimp(ctx_, GEOSTopologyPreserveSimplify_r(ctx_, zig.get(), 0.2));
  ASSERT_TRUE(simp && tpsimp);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, simp.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, tpsimp.get()), 4326);
  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, simp.get()), 2);
  EXPECT_EQ(GEOSGeomGetNumPoints_r(ctx_, tpsimp.get()), 2);

  ScopedGeom near_pt = Wkt("POINT (0.05 0.05)");
  ASSERT_TRUE(near_pt);
  ScopedGeom snapped(ctx_, GEOSSnap_r(ctx_, near_pt.get(), line.get(), 0.2));
  ASSERT_TRUE(snapped);
  double sx = 0.0;
  double sy = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, snapped.get(), &sx), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, snapped.get(), &sy), 1);
  EXPECT_DOUBLE_EQ(sx, 0.0);
  EXPECT_DOUBLE_EQ(sy, 0.0);
}

TEST_F(GeosTsCTest, HullsCentersClearanceAndTriangulation) {
  ScopedGeom pts =
      Wkt("MULTIPOINT ((0 0), (4 0), (4 4), (0 4), (2 0.5), (1 2), (3 2), (2 "
          "3.5))");
  ScopedGeom poly = Wkt("POLYGON ((0 0, 4 0, 4 2, 2 2, 2 4, 0 4, 0 0))");
  GEOSSetSRID_r(ctx_, poly.get(), 4326);
  ScopedGeom mpoly =
      Wkt("MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), ((3 0, 5 0, 5 2, 3 2, 3 "
          "0)))");
  ScopedGeom complex_poly =
      Wkt("POLYGON ((0 0, 1 0.2, 2 0, 3 0.2, 4 0, 4 4, 3 3.8, 2 4, 1 3.8, 0 4, "
          "0 0))");
  ASSERT_TRUE(pts && poly && mpoly && complex_poly);

  ScopedGeom convex(ctx_, GEOSConvexHull_r(ctx_, poly.get()));
  ScopedGeom concave(ctx_, GEOSConcaveHull_r(ctx_, pts.get(), 0.5, 1));
  ScopedGeom concave_len(ctx_,
                         GEOSConcaveHullByLength_r(ctx_, pts.get(), 2.5, 0));
  ScopedGeom concave_polys(
      ctx_, GEOSConcaveHullOfPolygons_r(ctx_, mpoly.get(), 0.6, 1, 1));
  ASSERT_TRUE(convex && concave && concave_len && concave_polys);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, convex.get()), 4326);

  // Upstream geos_c.h declares enum GEOSPolygonHullParameterModes outside the
  // GEOS_USE_ONLY_R_API guard while GEOSPolygonHullSimplifyMode_r accepts
  // unsigned int directly (1 = vertex ratio, 2 = area ratio).
  constexpr unsigned int kHullParamVertexRatio = 1;
  constexpr unsigned int kHullParamAreaRatio = 2;

  ScopedGeom hull_simp(
      ctx_, GEOSPolygonHullSimplify_r(ctx_, complex_poly.get(), 1, 0.5));
  ScopedGeom hull_area(
      ctx_, GEOSPolygonHullSimplifyMode_r(ctx_, complex_poly.get(), 1,
                                          kHullParamAreaRatio, 0.1));
  ScopedGeom hull_vert(
      ctx_, GEOSPolygonHullSimplifyMode_r(ctx_, complex_poly.get(), 0,
                                          kHullParamVertexRatio, 0.5));
  ASSERT_TRUE(hull_simp && hull_area && hull_vert);
  EXPECT_EQ(GEOSisValid_r(ctx_, hull_area.get()), 1);
  EXPECT_EQ(GEOSPolygonHullSimplifyMode_r(ctx_, complex_poly.get(), 1, 99, 0.5),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Unknown parameterMode")));
  ctx_.ClearMessages();

  ScopedGeom mrr(ctx_, GEOSMinimumRotatedRectangle_r(ctx_, poly.get()));
  ScopedGeom mic(ctx_, GEOSMaximumInscribedCircle_r(ctx_, poly.get(), 0.05));
  ScopedGeom lec_nobnd(
      ctx_, GEOSLargestEmptyCircle_r(ctx_, pts.get(), nullptr, 0.05));
  ScopedGeom bnd_box = Wkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ASSERT_TRUE(bnd_box);
  ScopedGeom lec_bnd(
      ctx_, GEOSLargestEmptyCircle_r(ctx_, pts.get(), bnd_box.get(), 0.05));
  ScopedGeom min_width(ctx_, GEOSMinimumWidth_r(ctx_, poly.get()));
  ScopedGeom min_clear_line(ctx_, GEOSMinimumClearanceLine_r(ctx_, poly.get()));
  ASSERT_TRUE(mrr && mic && lec_nobnd && lec_bnd && min_width &&
              min_clear_line);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, mrr.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, mic.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, min_width.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, min_clear_line.get()), 4326);

  double clearance = -1.0;
  // MinimumClearance returns 0 on success and 2 on error.
  EXPECT_EQ(GEOSMinimumClearance_r(ctx_, poly.get(), &clearance), 0);
  EXPECT_DOUBLE_EQ(clearance, 2.0);
  ScopedGeom single_pt = Wkt("POINT (0 0)");
  ScopedGeom curve = Wkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
  ASSERT_TRUE(single_pt && curve);
  EXPECT_EQ(GEOSMinimumClearance_r(ctx_, single_pt.get(), &clearance), 0);
  EXPECT_TRUE(std::isinf(clearance));
  EXPECT_EQ(GEOSMinimumClearance_r(ctx_, curve.get(), &clearance), 2);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedGeom pos(ctx_, GEOSPointOnSurface_r(ctx_, poly.get()));
  ScopedGeom centroid(ctx_, GEOSGetCentroid_r(ctx_, poly.get()));
  ASSERT_TRUE(pos && centroid);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, pos.get()), 4326);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, centroid.get()), 4326);
  EXPECT_EQ(GEOSContains_r(ctx_, poly.get(), pos.get()), 1);

  double mbc_radius = 0.0;
  GEOSGeometry* raw_center = nullptr;
  ScopedGeom mbc(ctx_, GEOSMinimumBoundingCircle_r(ctx_, poly.get(),
                                                   &mbc_radius, &raw_center));
  ScopedGeom center(ctx_, raw_center);
  ASSERT_TRUE(mbc && center);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, mbc.get()), 4326);
  EXPECT_GT(mbc_radius, 2.0);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, center.get()), GEOS_POINT);

  ScopedGeom mbc_no_out(
      ctx_, GEOSMinimumBoundingCircle_r(ctx_, poly.get(), nullptr, nullptr));
  EXPECT_TRUE(mbc_no_out);

  // Delaunay, ConstrainedDelaunay, and VoronoiDiagram.
  ScopedGeom dt_tris(ctx_,
                     GEOSDelaunayTriangulation_r(ctx_, pts.get(), 0.0, 0));
  ScopedGeom dt_edges(ctx_,
                      GEOSDelaunayTriangulation_r(ctx_, pts.get(), 0.0, 1));
  ScopedGeom cdt(ctx_,
                 GEOSConstrainedDelaunayTriangulation_r(ctx_, poly.get()));
  ASSERT_TRUE(dt_tris && dt_edges && cdt);
  EXPECT_GT(GEOSGetNumGeometries_r(ctx_, dt_tris.get()), 1);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, dt_edges.get()), GEOS_MULTILINESTRING);
  EXPECT_GE(GEOSGetNumGeometries_r(ctx_, cdt.get()), 2);

  // Upstream geos_c.h declares enum GEOSVoronoiFlags outside the
  // GEOS_USE_ONLY_R_API guard while GEOSVoronoiDiagram_r takes int flags
  // (1 = only edges, 2 = preserve order).
  constexpr int kVoronoiOnlyEdges = 1;
  constexpr int kVoronoiPreserveOrder = 2;

  ScopedGeom vor_polys(ctx_,
                       GEOSVoronoiDiagram_r(ctx_, pts.get(), nullptr, 0.0, 0));
  ScopedGeom vor_ordered(ctx_,
                         GEOSVoronoiDiagram_r(ctx_, pts.get(), nullptr, 0.0,
                                              kVoronoiPreserveOrder));
  ScopedGeom vor_edges_only(
      ctx_, GEOSVoronoiDiagram_r(ctx_, pts.get(), bnd_box.get(), 0.0,
                                 kVoronoiOnlyEdges));
  ScopedGeom vor_edges(
      ctx_, GEOSVoronoiDiagram_r(ctx_, pts.get(), bnd_box.get(), 0.0,
                                 kVoronoiOnlyEdges | kVoronoiPreserveOrder));
  ASSERT_TRUE(vor_polys && vor_ordered && vor_edges_only && vor_edges);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, vor_polys.get()), 8);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, vor_ordered.get()), 8);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, vor_edges_only.get()), GEOS_MULTILINESTRING);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, vor_edges.get()), GEOS_MULTILINESTRING);

  EXPECT_EQ(GEOSVoronoiDiagram_r(ctx_, curve.get(), nullptr, 0.0, 0), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, NodingPolygonizeLineMergeAndAlgorithms) {
  // Node crossing lines.
  ScopedGeom cross = Wkt("MULTILINESTRING ((0 0, 2 2), (0 2, 2 0))");
  ASSERT_TRUE(cross);
  GEOSSetSRID_r(ctx_, cross.get(), 4326);
  ScopedGeom noded(ctx_, GEOSNode_r(ctx_, cross.get()));
  ASSERT_TRUE(noded);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, noded.get()), 4326);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, noded.get()), 4);

  // Polygonize & Polygonize_valid across 0, 1, and 2 polygons.
  ScopedGeom open_line = Wkt("LINESTRING (0 0, 1 1)");
  GEOSSetSRID_r(ctx_, open_line.get(), 3857);
  ScopedGeom sq1 = Wkt("LINEARRING (0 0, 1 0, 1 1, 0 1, 0 0)");
  ScopedGeom sq2 = Wkt("LINEARRING (2 0, 3 0, 3 1, 2 1, 2 0)");
  ASSERT_TRUE(open_line && sq1 && sq2);

  const std::array<const GEOSGeometry*, 1> arr_open = {open_line.get()};
  ScopedGeom p_none(ctx_, GEOSPolygonize_valid_r(ctx_, arr_open.data(), 1));
  ASSERT_TRUE(p_none);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, p_none.get()), 1);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, p_none.get()), 3857);

  const std::array<const GEOSGeometry*, 1> arr_one = {sq1.get()};
  ScopedGeom p_raw(ctx_, GEOSPolygonize_r(ctx_, arr_one.data(), 1));
  ScopedGeom p_one(ctx_, GEOSPolygonize_valid_r(ctx_, arr_one.data(), 1));
  ASSERT_TRUE(p_raw && p_one);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, p_raw.get()), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, p_raw.get()), 1);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, p_one.get()), GEOS_POLYGON);

  const std::array<const GEOSGeometry*, 2> arr_two = {sq1.get(), sq2.get()};
  ScopedGeom p_two(ctx_, GEOSPolygonize_valid_r(ctx_, arr_two.data(), 2));
  ASSERT_TRUE(p_two);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, p_two.get()), GEOS_MULTIPOLYGON);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, p_two.get()), 2);

  // Polygonizer_getCutEdges & Polygonize_full with cuts, dangles, invalid ring.
  ScopedGeom outer = Wkt("LINESTRING (0 0, 4 0, 4 4, 0 4, 0 0)");
  ScopedGeom inner = Wkt("LINESTRING (1 1, 2 1, 2 2, 1 2, 1 1)");
  ScopedGeom bridge = Wkt("LINESTRING (0 0, 1 1)");
  ScopedGeom dangle = Wkt("LINESTRING (0 0, -1 -1)");
  ScopedGeom invalid_ring = Wkt("LINESTRING (10 0, 12 2, 12 0, 10 2, 10 0)");
  ASSERT_TRUE(outer && inner && bridge && dangle && invalid_ring);

  const std::array<const GEOSGeometry*, 3> cut_inputs = {
      outer.get(), inner.get(), bridge.get()};
  ScopedGeom cut_only(
      ctx_, GEOSPolygonizer_getCutEdges_r(ctx_, cut_inputs.data(), 3));
  ASSERT_TRUE(cut_only);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, cut_only.get()), 1);

  ScopedGeom full_input =
      Wkt("MULTILINESTRING ((0 0, 4 0, 4 4, 0 4, 0 0), "
          "(1 1, 2 1, 2 2, 1 2, 1 1), (0 0, 1 1), (0 0, -1 -1), "
          "(10 0, 12 2, 12 0, 10 2, 10 0))");
  ASSERT_TRUE(full_input);
  GEOSGeometry* raw_cuts = nullptr;
  GEOSGeometry* raw_dangles = nullptr;
  GEOSGeometry* raw_invalid = nullptr;
  ScopedGeom full_polys(ctx_,
                        GEOSPolygonize_full_r(ctx_, full_input.get(), &raw_cuts,
                                              &raw_dangles, &raw_invalid));
  ScopedGeom cuts(ctx_, raw_cuts);
  ScopedGeom dangles(ctx_, raw_dangles);
  ScopedGeom invalid(ctx_, raw_invalid);
  ASSERT_TRUE(full_polys && cuts && dangles && invalid);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, full_polys.get()), 2);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, cuts.get()), 1);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, dangles.get()), 1);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, invalid.get()), 1);

  ScopedGeom full_null_outs(
      ctx_,
      GEOSPolygonize_full_r(ctx_, full_input.get(), nullptr, nullptr, nullptr));
  ASSERT_TRUE(full_null_outs);

  // BuildArea, LineMerge, LineMergeDirected, Reverse, extractUniquePoints.
  ScopedGeom shells = Wkt(
      "MULTILINESTRING ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 2 1, 2 2, 1 2, 1 1))");
  ASSERT_TRUE(shells);
  GEOSSetSRID_r(ctx_, shells.get(), 4326);
  ScopedGeom built(ctx_, GEOSBuildArea_r(ctx_, shells.get()));
  ASSERT_TRUE(built);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, built.get()), 4326);
  EXPECT_EQ(GEOSGetNumInteriorRings_r(ctx_, built.get()), 1);

  ScopedGeom merge_in = Wkt("MULTILINESTRING ((0 0, 1 0), (2 0, 1 0))");
  ASSERT_TRUE(merge_in);
  ScopedGeom merged(ctx_, GEOSLineMerge_r(ctx_, merge_in.get()));
  ScopedGeom merged_dir(ctx_, GEOSLineMergeDirected_r(ctx_, merge_in.get()));
  ASSERT_TRUE(merged && merged_dir);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, merged.get()), GEOS_LINESTRING);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, merged_dir.get()), 2);

  ScopedGeom rev(ctx_, GEOSReverse_r(ctx_, open_line.get()));
  ASSERT_TRUE(rev);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, rev.get()), 3857);

  ScopedGeom repeated_pts_geom = Wkt("LINESTRING (0 0, 1 1, 0 0, 2 2)");
  ASSERT_TRUE(repeated_pts_geom);
  ScopedGeom unique_pts(
      ctx_, GEOSGeom_extractUniquePoints_r(ctx_, repeated_pts_geom.get()));
  ASSERT_TRUE(unique_pts);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, unique_pts.get()), 3);

  // Linear referencing: Project, Interpolate, ProjectNormalized,
  // InterpolateNormalized, LineSubstring.
  ScopedGeom seg4 = Wkt("LINESTRING (0 0, 4 0)");
  ScopedGeom zero_seg = Wkt("LINESTRING (0 0, 0 0)");
  ScopedGeom q_pt = Wkt("POINT (1 5)");
  ScopedGeom p0 = Wkt("POINT (0 0)");
  ScopedGeom empty_line(ctx_, GEOSGeom_createEmptyLineString_r(ctx_));
  ASSERT_TRUE(seg4 && zero_seg && q_pt && p0 && empty_line);

  EXPECT_DOUBLE_EQ(GEOSProject_r(ctx_, seg4.get(), q_pt.get()), 1.0);
  EXPECT_DOUBLE_EQ(GEOSProjectNormalized_r(ctx_, seg4.get(), q_pt.get()), 0.25);
  EXPECT_DOUBLE_EQ(GEOSProjectNormalized_r(ctx_, zero_seg.get(), p0.get()),
                   0.0);
  EXPECT_DOUBLE_EQ(GEOSProject_r(ctx_, seg4.get(), seg4.get()), -1.0);
  EXPECT_DOUBLE_EQ(GEOSProjectNormalized_r(ctx_, seg4.get(), seg4.get()), -1.0);
  ctx_.ClearMessages();

  ScopedGeom interp(ctx_, GEOSInterpolate_r(ctx_, seg4.get(), 3.0));
  ScopedGeom interp_norm(ctx_,
                         GEOSInterpolateNormalized_r(ctx_, seg4.get(), 0.75));
  ScopedGeom interp_empty(ctx_, GEOSInterpolate_r(ctx_, empty_line.get(), 1.0));
  ASSERT_TRUE(interp && interp_norm && interp_empty);
  double ix = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, interp.get(), &ix), 1);
  EXPECT_DOUBLE_EQ(ix, 3.0);
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, interp_norm.get(), &ix), 1);
  EXPECT_DOUBLE_EQ(ix, 3.0);
  EXPECT_EQ(GEOSisEmpty_r(ctx_, interp_empty.get()), 1);

  ScopedGeom sub(ctx_, GEOSLineSubstring_r(ctx_, seg4.get(), 0.25, 0.75));
  ASSERT_TRUE(sub);
  double sub_len = 0.0;
  EXPECT_EQ(GEOSLength_r(ctx_, sub.get(), &sub_len), 1);
  EXPECT_DOUBLE_EQ(sub_len, 2.0);
  EXPECT_EQ(GEOSLineSubstring_r(ctx_, seg4.get(), -0.1, 0.5), nullptr);
  EXPECT_EQ(GEOSLineSubstring_r(ctx_, seg4.get(), 0.2, -0.1), nullptr);
  EXPECT_EQ(GEOSLineSubstring_r(ctx_, seg4.get(), 1.5, 0.5), nullptr);
  EXPECT_EQ(GEOSLineSubstring_r(ctx_, seg4.get(), 0.2, 1.5), nullptr);
  ctx_.ClearMessages();

  // SharedPaths, OrientationIndex, SegmentIntersection, HilbertCode.
  ScopedGeom sp_a = Wkt("LINESTRING (0 0, 2 0, 4 0)");
  ScopedGeom sp_b = Wkt("MULTILINESTRING ((0 0, 2 0), (4 0, 2 0))");
  ASSERT_TRUE(sp_a && sp_b);
  ScopedGeom shared(ctx_, GEOSSharedPaths_r(ctx_, sp_a.get(), sp_b.get()));
  ASSERT_TRUE(shared);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, shared.get()), 2);
  EXPECT_EQ(
      GEOSGetNumGeometries_r(ctx_, GEOSGetGeometryN_r(ctx_, shared.get(), 0)),
      1);
  EXPECT_EQ(
      GEOSGetNumGeometries_r(ctx_, GEOSGetGeometryN_r(ctx_, shared.get(), 1)),
      1);

  ScopedGeom poly_err = Wkt("POLYGON ((0 0, 1 0, 1 1, 0 0))");
  ASSERT_TRUE(poly_err);
  EXPECT_EQ(GEOSSharedPaths_r(ctx_, poly_err.get(), sp_b.get()), nullptr);
  ctx_.ClearMessages();

  const int col = GEOSOrientationIndex_r(ctx_, 0.0, 0.0, 1.0, 0.0, 2.0, 0.0);
  const int left = GEOSOrientationIndex_r(ctx_, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
  const int right = GEOSOrientationIndex_r(ctx_, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0);
  EXPECT_EQ(col, 0);
  EXPECT_THAT(left, AnyOf(Eq(1), Eq(-1)));
  EXPECT_EQ(right, -left);

  double cx = 0.0;
  double cy = 0.0;
  EXPECT_EQ(GEOSSegmentIntersection_r(ctx_, 0.0, 0.0, 2.0, 2.0, 0.0, 2.0, 2.0,
                                      0.0, &cx, &cy),
            1);
  EXPECT_DOUBLE_EQ(cx, 1.0);
  EXPECT_DOUBLE_EQ(cy, 1.0);
  EXPECT_EQ(GEOSSegmentIntersection_r(ctx_, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0,
                                      1.0, &cx, &cy),
            -1);

  ScopedGeom extent = Wkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  ASSERT_TRUE(extent);
  unsigned int code_sw = 999;
  unsigned int code_ne = 0;
  EXPECT_EQ(GEOSHilbertCode_r(ctx_, p0.get(), extent.get(), 4, &code_sw), 1);
  ScopedGeom p_ne = Wkt("POINT (10 10)");
  ASSERT_TRUE(p_ne);
  EXPECT_EQ(GEOSHilbertCode_r(ctx_, p_ne.get(), extent.get(), 4, &code_ne), 1);
  EXPECT_EQ(code_sw, 0u);
  EXPECT_GT(code_ne, 0u);
  EXPECT_EQ(GEOSHilbertCode_r(ctx_, p0.get(), extent.get(), 17, &code_sw), 0);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, CoverageValidationSimplificationAndCleaning) {
  ScopedGeom valid_cov =
      Wkt("GEOMETRYCOLLECTION ("
          "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)), "
          "POLYGON ((1 0, 2 0, 2 1, 1 1, 1 0)))");
  ScopedGeom overlap_cov =
      Wkt("GEOMETRYCOLLECTION ("
          "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0)), "
          "POLYGON ((1 0, 3 0, 3 2, 1 2, 1 0)))");
  // Shared boundary segments at y in [0,1] and [3,4] with a 0.2-wide
  // diamond gap in [1,3] kept clear of the horizontal outer edges y=0, y=4.
  ScopedGeom gappy_cov =
      Wkt("GEOMETRYCOLLECTION ("
          "POLYGON ((0 0, 3 0, 3 1, 2.9 2, 3 3, 3 4, 0 4, 0 0)), "
          "POLYGON ((3 0, 6 0, 6 4, 3 4, 3 3, 3.1 2, 3 1, 3 0)))");
  ScopedGeom non_cov_pt = Wkt("POINT (0 0)");
  ASSERT_TRUE(valid_cov && overlap_cov && gappy_cov && non_cov_pt);

  GEOSGeometry* raw_invalid = nullptr;
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, valid_cov.get(), 0.0, &raw_invalid), 1);
  {
    ScopedGeom invalid_edges(ctx_, raw_invalid);
    ASSERT_TRUE(invalid_edges);
    EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, invalid_edges.get()), 2);
    EXPECT_EQ(GEOSisEmpty_r(ctx_, invalid_edges.get()), 1);
  }

  raw_invalid = nullptr;
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, overlap_cov.get(), 0.0, &raw_invalid),
            0);
  {
    ScopedGeom invalid_edges(ctx_, raw_invalid);
    ASSERT_TRUE(invalid_edges);
    EXPECT_EQ(GEOSisEmpty_r(ctx_, invalid_edges.get()), 0);
  }

  // Gap width detection with both non-null and nullptr invalidEdges out-param.
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, gappy_cov.get(), 0.0, nullptr), 1);
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, gappy_cov.get(), 0.5, nullptr), 0);

  // Non-collection throws and returns 2.
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, non_cov_pt.get(), 0.0, nullptr), 2);
  ctx_.ClearMessages();

  // CoverageSimplifyVW.
  ScopedGeom bumpy_cov =
      Wkt("GEOMETRYCOLLECTION ("
          "POLYGON ((0 0, 1 0.01, 2 0, 2 2, 1 1.99, 0 2, 0 0)), "
          "POLYGON ((2 0, 4 0, 4 2, 2 2, 2 0)))");
  ASSERT_TRUE(bumpy_cov);
  ScopedGeom simp_free(ctx_,
                       GEOSCoverageSimplifyVW_r(ctx_, bumpy_cov.get(), 0.5, 0));
  ScopedGeom simp_inner(
      ctx_, GEOSCoverageSimplifyVW_r(ctx_, bumpy_cov.get(), 0.5, 1));
  ASSERT_TRUE(simp_free && simp_inner);
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, simp_free.get(), 0.0, nullptr), 1);
  EXPECT_EQ(GEOSCoverageSimplifyVW_r(ctx_, bumpy_cov.get(), 0.5, 2), nullptr);
  EXPECT_EQ(GEOSCoverageSimplifyVW_r(ctx_, non_cov_pt.get(), 0.5, 0), nullptr);

  // CoverageClean & CoverageCleanParams across all merge strategies.
  ScopedGeom cleaned_default(ctx_,
                             GEOSCoverageClean_r(ctx_, overlap_cov.get()));
  ASSERT_TRUE(cleaned_default);
  EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, cleaned_default.get(), 0.0, nullptr),
            1);

  // Two unequal-area polygons partitioning [0,7]x[0,6] apart from a 0.2-wide
  // gap diamond in [1,3] and a 0.4-wide overlap diamond in [3,5].
  ScopedGeom dirty_cov =
      Wkt("GEOMETRYCOLLECTION ("
          "POLYGON ((0 0, 3 0, 3 1, 2.9 2, 3 3, 3.2 4, 3 5, 3 6, 0 6, 0 0)), "
          "POLYGON ((3 0, 7 0, 7 6, 3 6, 3 5, 2.8 4, 3 3, 3.1 2, 3 1, 3 0)))");
  ASSERT_TRUE(dirty_cov);

  ScopedCoverageCleanParams cparams(ctx_,
                                    GEOSCoverageCleanParams_create_r(ctx_));
  ASSERT_TRUE(cparams);
  EXPECT_EQ(
      GEOSCoverageCleanParams_setSnappingDistance_r(ctx_, cparams.get(), 0.01),
      1);
  EXPECT_EQ(
      GEOSCoverageCleanParams_setGapMaximumWidth_r(ctx_, cparams.get(), 0.5),
      1);
  constexpr std::array<int, 4> kMergeStrategies = {
      GEOS_MERGE_LONGEST_BORDER,
      GEOS_MERGE_MAX_AREA,
      GEOS_MERGE_MIN_AREA,
      GEOS_MERGE_MIN_INDEX,
  };
  for (int strat : kMergeStrategies) {
    EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy_r(
                  ctx_, cparams.get(), strat),
              1);
    ScopedGeom cleaned(ctx_, GEOSCoverageCleanWithParams_r(
                                 ctx_, dirty_cov.get(), cparams.get()));
    ASSERT_TRUE(cleaned);
    EXPECT_EQ(GEOSCoverageIsValid_r(ctx_, cleaned.get(), 0.5, nullptr), 1);
    double cleaned_area = 0.0;
    EXPECT_EQ(GEOSArea_r(ctx_, cleaned.get(), &cleaned_area), 1);
    EXPECT_NEAR(cleaned_area, 42.0, 1e-9);
  }
  EXPECT_EQ(GEOSCoverageCleanParams_setOverlapMergeStrategy_r(
                ctx_, cparams.get(), 99),
            0);
  EXPECT_THAT(ctx_.errors(),
              Contains(HasSubstr("Invalid overlapMergeStrategy")));
  EXPECT_EQ(
      GEOSCoverageCleanWithParams_r(ctx_, non_cov_pt.get(), cparams.get()),
      nullptr);
}

// ============================================================================
// Section 6: Clustering, PreparedGeometry, STRtree, and WKT/WKB/GeoJSON I/O
// ============================================================================

TEST_F(GeosTsCTest, ClusteringAlgorithmsAndClusterInfo) {
  ScopedGeom dbscan_pts = Wkt("MULTIPOINT ((0 0), (0.5 0), (0 0.5), (10 10))");
  ASSERT_TRUE(dbscan_pts);

  ScopedClusterInfo dbscan(ctx_,
                           GEOSClusterDBSCAN_r(ctx_, dbscan_pts.get(), 1.0, 2));
  ASSERT_TRUE(dbscan);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, dbscan.get()), 1u);
  EXPECT_EQ(GEOSClusterInfo_getClusterSize_r(ctx_, dbscan.get(), 0), 3u);

  const std::size_t* members =
      GEOSClusterInfo_getInputsForClusterN_r(ctx_, dbscan.get(), 0);
  ASSERT_NE(members, nullptr);
  std::vector<std::size_t> cluster0(members, members + 3);
  std::sort(cluster0.begin(), cluster0.end());
  EXPECT_EQ(cluster0, (std::vector<std::size_t>{0u, 1u, 2u}));

  // Upstream geos_c.h defines GEOS_CLUSTER_NONE = (size_t)-1 outside the
  // GEOS_USE_ONLY_R_API guard; GEOSClusterInfo_getClustersForInputs_r still
  // uses that sentinel for unclustered noise inputs.
  constexpr std::size_t kClusterNone = std::numeric_limits<std::size_t>::max();
  ScopedGeosBuffer<std::size_t> input_clusters(
      ctx_, GEOSClusterInfo_getClustersForInputs_r(ctx_, dbscan.get()));
  ASSERT_TRUE(input_clusters);
  EXPECT_EQ(input_clusters.get()[0], 0u);
  EXPECT_EQ(input_clusters.get()[1], 0u);
  EXPECT_EQ(input_clusters.get()[2], 0u);
  EXPECT_EQ(input_clusters.get()[3], kClusterNone);

  // Diagonal line [0,2]x[0,2] and corner segment [0,0.2]x[1.8,2] have
  // intersecting envelopes without intersecting geometries.
  ScopedGeom lines = Wkt("MULTILINESTRING ((0 0, 2 2), (0 1.8, 0.2 2))");
  ASSERT_TRUE(lines);

  ScopedClusterInfo geom_isect(
      ctx_, GEOSClusterGeometryIntersects_r(ctx_, lines.get()));
  ScopedClusterInfo env_isect(
      ctx_, GEOSClusterEnvelopeIntersects_r(ctx_, lines.get()));
  ScopedClusterInfo env_dist(
      ctx_, GEOSClusterEnvelopeDistance_r(ctx_, lines.get(), 0.05));
  ScopedClusterInfo geom_dist_small(
      ctx_, GEOSClusterGeometryDistance_r(ctx_, lines.get(), 0.05));
  ScopedClusterInfo geom_dist_large(
      ctx_, GEOSClusterGeometryDistance_r(ctx_, lines.get(), 2.0));
  ASSERT_TRUE(geom_isect && env_isect && env_dist && geom_dist_small &&
              geom_dist_large);

  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, geom_isect.get()), 2u);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, env_isect.get()), 1u);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, env_dist.get()), 1u);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, geom_dist_small.get()), 2u);
  EXPECT_EQ(GEOSClusterInfo_getNumClusters_r(ctx_, geom_dist_large.get()), 1u);
}

TEST_F(GeosTsCTest, PreparedGeometryPredicatesAndMetrics) {
  ScopedGeom box = Wkt("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ScopedGeom outer_box = Wkt("POLYGON ((-1 -1, 5 -1, 5 5, -1 5, -1 -1))");
  ScopedGeom inner_box = Wkt("POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))");
  ScopedGeom touch_edge_inner = Wkt("POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  ScopedGeom touch_ext = Wkt("POLYGON ((4 0, 6 0, 6 4, 4 4, 4 0))");
  ScopedGeom overlap_box = Wkt("POLYGON ((2 2, 6 2, 6 6, 2 6, 2 2))");
  ScopedGeom disjoint_pt = Wkt("POINT (10 10)");
  ScopedGeom inside_pt = Wkt("POINT (2 2)");
  ScopedGeom cross_line = Wkt("LINESTRING (-1 2, 5 2)");
  ScopedGeom inside_line = Wkt("LINESTRING (1 2, 3 2)");
  ScopedGeom border_line = Wkt("LINESTRING (0 0, 4 0)");
  ASSERT_TRUE(box && outer_box && inner_box && touch_edge_inner && touch_ext &&
              overlap_box && disjoint_pt && inside_pt && cross_line &&
              inside_line && border_line);

  // `prep` borrows `box` and is destroyed first.
  ScopedPreparedGeom prep(ctx_, GEOSPrepare_r(ctx_, box.get()));
  ASSERT_TRUE(prep);

  EXPECT_EQ(GEOSPreparedContains_r(ctx_, prep.get(), inner_box.get()), 1);
  EXPECT_EQ(GEOSPreparedContains_r(ctx_, prep.get(), border_line.get()), 0);

  EXPECT_EQ(GEOSPreparedContainsXY_r(ctx_, prep.get(), 2.0, 2.0), 1);
  EXPECT_EQ(GEOSPreparedContainsXY_r(ctx_, prep.get(), 0.0, 0.0), 0);

  EXPECT_EQ(GEOSPreparedContainsProperly_r(ctx_, prep.get(), inner_box.get()),
            1);
  EXPECT_EQ(
      GEOSPreparedContainsProperly_r(ctx_, prep.get(), touch_edge_inner.get()),
      0);

  EXPECT_EQ(GEOSPreparedCoveredBy_r(ctx_, prep.get(), outer_box.get()), 1);
  EXPECT_EQ(GEOSPreparedCoveredBy_r(ctx_, prep.get(), overlap_box.get()), 0);

  EXPECT_EQ(GEOSPreparedCovers_r(ctx_, prep.get(), border_line.get()), 1);
  EXPECT_EQ(GEOSPreparedCovers_r(ctx_, prep.get(), overlap_box.get()), 0);

  EXPECT_EQ(GEOSPreparedCrosses_r(ctx_, prep.get(), cross_line.get()), 1);
  EXPECT_EQ(GEOSPreparedCrosses_r(ctx_, prep.get(), inside_line.get()), 0);

  EXPECT_EQ(GEOSPreparedDisjoint_r(ctx_, prep.get(), disjoint_pt.get()), 1);
  EXPECT_EQ(GEOSPreparedDisjoint_r(ctx_, prep.get(), overlap_box.get()), 0);

  EXPECT_EQ(GEOSPreparedIntersects_r(ctx_, prep.get(), overlap_box.get()), 1);
  EXPECT_EQ(GEOSPreparedIntersects_r(ctx_, prep.get(), disjoint_pt.get()), 0);

  EXPECT_EQ(GEOSPreparedIntersectsXY_r(ctx_, prep.get(), 0.0, 0.0), 1);
  EXPECT_EQ(GEOSPreparedIntersectsXY_r(ctx_, prep.get(), 10.0, 10.0), 0);

  EXPECT_EQ(GEOSPreparedOverlaps_r(ctx_, prep.get(), overlap_box.get()), 1);
  EXPECT_EQ(GEOSPreparedOverlaps_r(ctx_, prep.get(), inner_box.get()), 0);

  EXPECT_EQ(GEOSPreparedTouches_r(ctx_, prep.get(), touch_ext.get()), 1);
  EXPECT_EQ(GEOSPreparedTouches_r(ctx_, prep.get(), overlap_box.get()), 0);

  EXPECT_EQ(GEOSPreparedWithin_r(ctx_, prep.get(), outer_box.get()), 1);
  EXPECT_EQ(GEOSPreparedWithin_r(ctx_, prep.get(), overlap_box.get()), 0);

  ScopedCString rel(ctx_, GEOSPreparedRelate_r(ctx_, prep.get(), box.get()));
  ASSERT_TRUE(rel);
  EXPECT_STREQ(rel.get(), "2FFF1FFF2");
  EXPECT_EQ(
      GEOSPreparedRelatePattern_r(ctx_, prep.get(), box.get(), "T*F**FFF2"), 1);
  EXPECT_EQ(
      GEOSPreparedRelatePattern_r(ctx_, prep.get(), box.get(), "FF*FF****"), 0);
  EXPECT_EQ(GEOSPreparedRelatePattern_r(ctx_, prep.get(), box.get(), "BAD"), 2);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedCoordSeq np(
      ctx_, GEOSPreparedNearestPoints_r(ctx_, prep.get(), disjoint_pt.get()));
  ASSERT_TRUE(np);
  unsigned int nsz = 0;
  EXPECT_EQ(GEOSCoordSeq_getSize_r(ctx_, np.get(), &nsz), 1);
  EXPECT_EQ(nsz, 2u);

  double dist = -1.0;
  EXPECT_EQ(GEOSPreparedDistance_r(ctx_, prep.get(), disjoint_pt.get(), &dist),
            1);
  EXPECT_NEAR(dist, std::hypot(6.0, 6.0), 1e-12);
  EXPECT_EQ(
      GEOSPreparedDistanceWithin_r(ctx_, prep.get(), disjoint_pt.get(), 9.0),
      1);
  EXPECT_EQ(
      GEOSPreparedDistanceWithin_r(ctx_, prep.get(), disjoint_pt.get(), 8.0),
      0);
}

TEST_F(GeosTsCTest, SpatialIndexSTRtreeOperations) {
  EXPECT_EQ(GEOSSTRtree_create_r(ctx_, 1), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedSTRtree empty_tree(ctx_, GEOSSTRtree_create_r(ctx_, 4));
  ASSERT_TRUE(empty_tree);
  EXPECT_EQ(GEOSSTRtree_build_r(ctx_, empty_tree.get()), 1);
  ScopedGeom probe_pt = Wkt("POINT (0 0)");
  ASSERT_TRUE(probe_pt);
  EXPECT_EQ(GEOSSTRtree_nearest_r(ctx_, empty_tree.get(), probe_pt.get()),
            nullptr);

  // Geometry-item tree for iterate, build, query, remove, and nearest_r.
  ScopedGeom p1 = Wkt("POINT (0 0)");
  ScopedGeom p2 = Wkt("POINT (10 0)");
  ScopedGeom p3 = Wkt("POINT (5 1)");
  ScopedGeom p4 = Wkt("POINT (20 20)");
  ASSERT_TRUE(p1 && p2 && p3 && p4);

  ScopedSTRtree tree(ctx_, GEOSSTRtree_create_r(ctx_, 2));
  ASSERT_TRUE(tree);
  GEOSSTRtree_insert_r(ctx_, tree.get(), p1.get(), p1.get());
  GEOSSTRtree_insert_r(ctx_, tree.get(), p2.get(), p2.get());
  GEOSSTRtree_insert_r(ctx_, tree.get(), p3.get(), p3.get());
  GEOSSTRtree_insert_r(ctx_, tree.get(), p4.get(), p4.get());

  // Iterate before building visits all 4 items.
  std::vector<const GEOSGeometry*> iterated;
  GEOSSTRtree_iterate_r(
      ctx_, tree.get(),
      [](void* item, void* ud) {
        static_cast<std::vector<const GEOSGeometry*>*>(ud)->push_back(
            static_cast<const GEOSGeometry*>(item));
      },
      &iterated);
  EXPECT_EQ(iterated.size(), 4u);

  // Remove p4 (present -> 1) and remove again (absent -> 0), then build.
  EXPECT_EQ(GEOSSTRtree_remove_r(ctx_, tree.get(), p4.get(), p4.get()), 1);
  EXPECT_EQ(GEOSSTRtree_remove_r(ctx_, tree.get(), p4.get(), p4.get()), 0);
  EXPECT_EQ(GEOSSTRtree_build_r(ctx_, tree.get()), 1);

  ScopedGeom query_rect(ctx_,
                        GEOSGeom_createRectangle_r(ctx_, -1.0, -1.0, 6.0, 2.0));
  ASSERT_TRUE(query_rect);
  std::vector<const GEOSGeometry*> hits;
  GEOSSTRtree_query_r(
      ctx_, tree.get(), query_rect.get(),
      [](void* item, void* ud) {
        static_cast<std::vector<const GEOSGeometry*>*>(ud)->push_back(
            static_cast<const GEOSGeometry*>(item));
      },
      &hits);
  EXPECT_EQ(hits.size(), 2u);

  ScopedGeom query_pt = Wkt("POINT (9.5 0.1)");
  ASSERT_TRUE(query_pt);
  EXPECT_EQ(GEOSSTRtree_nearest_r(ctx_, tree.get(), query_pt.get()), p2.get());

  // Custom item type with GEOSSTRtree_nearest_generic_r.
  struct TaggedPoint {
    int id;
    double x;
    double y;
    ScopedGeom geom;
  };
  TaggedPoint t1{1, 0.0, 0.0, Wkt("POINT (0 0)")};
  TaggedPoint t2{2, 4.0, 3.0, Wkt("POINT (4 3)")};
  TaggedPoint query_tag{99, 3.9, 3.1, Wkt("POINT (3.9 3.1)")};
  ASSERT_TRUE(t1.geom && t2.geom && query_tag.geom);

  ScopedSTRtree generic_tree(ctx_, GEOSSTRtree_create_r(ctx_, 4));
  ASSERT_TRUE(generic_tree);
  GEOSSTRtree_insert_r(ctx_, generic_tree.get(), t1.geom.get(), &t1);
  GEOSSTRtree_insert_r(ctx_, generic_tree.get(), t2.geom.get(), &t2);

  const void* nearest_item = GEOSSTRtree_nearest_generic_r(
      ctx_, generic_tree.get(), &query_tag, query_tag.geom.get(),
      [](const void* a, const void* b, double* d, void*) -> int {
        const auto* ta = static_cast<const TaggedPoint*>(a);
        const auto* tb = static_cast<const TaggedPoint*>(b);
        *d = std::hypot(ta->x - tb->x, ta->y - tb->y);
        return 1;
      },
      nullptr);
  EXPECT_EQ(nearest_item, &t2);

  // Distance callback returning 0 propagates failure cleanly as nullptr.
  EXPECT_EQ(
      GEOSSTRtree_nearest_generic_r(
          ctx_, generic_tree.get(), &query_tag, query_tag.geom.get(),
          [](const void*, const void*, double*, void*) -> int { return 0; },
          nullptr),
      nullptr);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("Failed to compute distance")));
}

TEST_F(GeosTsCTest, WktReaderWriterAndDeprecatedWkt) {
  ScopedWKTReader reader(ctx_, GEOSWKTReader_create_r(ctx_));
  ASSERT_TRUE(reader);

  // Unclosed Polygon ring fails with fixStructure=0 and closes with =1.
  constexpr const char* kUnclosedPoly = "POLYGON ((0 0, 2 0, 2 2, 0 2))";
  GEOSWKTReader_setFixStructure_r(ctx_, reader.get(), 0);
  EXPECT_EQ(GEOSWKTReader_read_r(ctx_, reader.get(), kUnclosedPoly), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  GEOSWKTReader_setFixStructure_r(ctx_, reader.get(), 1);
  ScopedGeom fixed_poly(
      ctx_, GEOSWKTReader_read_r(ctx_, reader.get(), kUnclosedPoly));
  ASSERT_TRUE(fixed_poly);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, fixed_poly.get()), GEOS_POLYGON);
  EXPECT_EQ(ToWkt(fixed_poly.get()), "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))");
  double area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, fixed_poly.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 4.0);

  EXPECT_EQ(GEOSWKTReader_read_r(ctx_, reader.get(), "NOT A WKT"), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  // WKTWriter options: output dimension, old3D, trim, rounding precision.
  ScopedWKTWriter writer(ctx_, GEOSWKTWriter_create_r(ctx_));
  ASSERT_TRUE(writer);
  EXPECT_EQ(GEOSWKTWriter_getOutputDimension_r(ctx_, writer.get()), 4);

  GEOSWKTWriter_setOutputDimension_r(ctx_, writer.get(), 5);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("WKT output dimension")));
  ctx_.ClearMessages();
  EXPECT_EQ(GEOSWKTWriter_getOutputDimension_r(ctx_, writer.get()), 4);

  ScopedGeom pt3d = Wkt("POINT Z (1 2 3)");
  ASSERT_TRUE(pt3d);

  GEOSWKTWriter_setOutputDimension_r(ctx_, writer.get(), 2);
  EXPECT_EQ(GEOSWKTWriter_getOutputDimension_r(ctx_, writer.get()), 2);
  {
    ScopedCString s(ctx_,
                    GEOSWKTWriter_write_r(ctx_, writer.get(), pt3d.get()));
    ASSERT_TRUE(s);
    EXPECT_STREQ(s.get(), "POINT (1 2)");
  }

  GEOSWKTWriter_setOutputDimension_r(ctx_, writer.get(), 3);
  GEOSWKTWriter_setOld3D_r(ctx_, writer.get(), 0);
  {
    ScopedCString s(ctx_,
                    GEOSWKTWriter_write_r(ctx_, writer.get(), pt3d.get()));
    ASSERT_TRUE(s);
    EXPECT_STREQ(s.get(), "POINT Z (1 2 3)");
  }
  GEOSWKTWriter_setOld3D_r(ctx_, writer.get(), 1);
  {
    ScopedCString s(ctx_,
                    GEOSWKTWriter_write_r(ctx_, writer.get(), pt3d.get()));
    ASSERT_TRUE(s);
    EXPECT_STREQ(s.get(), "POINT (1 2 3)");
  }

  ScopedGeom dec_pt = Wkt("POINT (1.12345 2.5)");
  ASSERT_TRUE(dec_pt);
  GEOSWKTWriter_setTrim_r(ctx_, writer.get(), 0);
  {
    ScopedCString s(ctx_,
                    GEOSWKTWriter_write_r(ctx_, writer.get(), dec_pt.get()));
    ASSERT_TRUE(s);
    EXPECT_THAT(std::string(s.get()), HasSubstr("2.500000"));
  }
  GEOSWKTWriter_setTrim_r(ctx_, writer.get(), 1);
  GEOSWKTWriter_setRoundingPrecision_r(ctx_, writer.get(), 2);
  {
    ScopedCString s(ctx_,
                    GEOSWKTWriter_write_r(ctx_, writer.get(), dec_pt.get()));
    ASSERT_TRUE(s);
    EXPECT_STREQ(s.get(), "POINT (1.12 2.5)");
  }

  // Deprecated GEOSGeomFromWKT_r / GEOSGeomToWKT_r emit untrimmed 2D WKT.
  ScopedGeom dep_pt(ctx_, GEOSGeomFromWKT_r(ctx_, "POINT Z (1 2 3)"));
  ASSERT_TRUE(dep_pt);
  EXPECT_EQ(GEOSHasZ_r(ctx_, dep_pt.get()), 1);
  ScopedCString dep_wkt(ctx_, GEOSGeomToWKT_r(ctx_, dep_pt.get()));
  ASSERT_TRUE(dep_wkt);
  EXPECT_THAT(std::string(dep_wkt.get()), StartsWith("POINT (1."));
  EXPECT_THAT(std::string(dep_wkt.get()), Not(HasSubstr(" Z ")));
  EXPECT_EQ(GEOSGeomFromWKT_r(ctx_, "INVALID WKT"), nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, WkbReaderWriterAndDeprecatedWkbContext) {
  // Unclosed 4-point NDR Polygon WKB buffer.
  std::vector<unsigned char> unclosed_wkb;
  unclosed_wkb.push_back(1);    // NDR (little-endian)
  AppendLE32(unclosed_wkb, 3);  // Polygon
  AppendLE32(unclosed_wkb, 1);  // numRings
  AppendLE32(unclosed_wkb, 4);  // numPoints (missing closing point)
  AppendLE64Double(unclosed_wkb, 0.0);
  AppendLE64Double(unclosed_wkb, 0.0);
  AppendLE64Double(unclosed_wkb, 2.0);
  AppendLE64Double(unclosed_wkb, 0.0);
  AppendLE64Double(unclosed_wkb, 2.0);
  AppendLE64Double(unclosed_wkb, 2.0);
  AppendLE64Double(unclosed_wkb, 0.0);
  AppendLE64Double(unclosed_wkb, 2.0);

  ScopedWKBReader reader(ctx_, GEOSWKBReader_create_r(ctx_));
  ASSERT_TRUE(reader);

  GEOSWKBReader_setFixStructure_r(ctx_, reader.get(), 0);
  EXPECT_EQ(GEOSWKBReader_read_r(ctx_, reader.get(), unclosed_wkb.data(),
                                 unclosed_wkb.size()),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  GEOSWKBReader_setFixStructure_r(ctx_, reader.get(), 1);
  ScopedGeom fixed_wkb_poly(
      ctx_, GEOSWKBReader_read_r(ctx_, reader.get(), unclosed_wkb.data(),
                                 unclosed_wkb.size()));
  ASSERT_TRUE(fixed_wkb_poly);
  double area = 0.0;
  EXPECT_EQ(GEOSArea_r(ctx_, fixed_wkb_poly.get(), &area), 1);
  EXPECT_DOUBLE_EQ(area, 4.0);

  const std::array<unsigned char, 3> bad_wkb = {0x01, 0x01, 0x00};
  EXPECT_EQ(
      GEOSWKBReader_read_r(ctx_, reader.get(), bad_wkb.data(), bad_wkb.size()),
      nullptr);
  const unsigned char bad_hex[] = "ZZZZ";
  EXPECT_EQ(GEOSWKBReader_readHEX_r(ctx_, reader.get(), bad_hex, 4), nullptr);
  EXPECT_EQ(ctx_.errors().size(), 2u);
  ctx_.ClearMessages();

  // WKBWriter properties, extended SRID embedding, ISO Z encoding, and errors.
  ScopedWKBWriter writer(ctx_, GEOSWKBWriter_create_r(ctx_));
  ASSERT_TRUE(writer);
  EXPECT_EQ(GEOSWKBWriter_getOutputDimension_r(ctx_, writer.get()), 4);
  EXPECT_THAT(GEOSWKBWriter_getByteOrder_r(ctx_, writer.get()),
              AnyOf(Eq(GEOS_WKB_XDR), Eq(GEOS_WKB_NDR)));
  EXPECT_EQ(GEOSWKBWriter_getIncludeSRID_r(ctx_, writer.get()), 0);
  EXPECT_EQ(GEOSWKBWriter_getFlavor_r(ctx_, writer.get()), GEOS_WKB_EXTENDED);

  GEOSWKBWriter_setOutputDimension_r(ctx_, writer.get(), 5);
  GEOSWKBWriter_setByteOrder_r(ctx_, writer.get(), 99);
  GEOSWKBWriter_setFlavor_r(ctx_, writer.get(), 99);
  EXPECT_EQ(ctx_.errors().size(), 3u);
  ctx_.ClearMessages();

  ScopedGeom pt3d = Wkt("POINT Z (1 2 3)");
  ASSERT_TRUE(pt3d);
  GEOSSetSRID_r(ctx_, pt3d.get(), 4326);

  // Extended WKB + XDR + includeSRID preserves SRID and Z in both WKB and HEX.
  GEOSWKBWriter_setOutputDimension_r(ctx_, writer.get(), 3);
  GEOSWKBWriter_setByteOrder_r(ctx_, writer.get(), GEOS_WKB_XDR);
  GEOSWKBWriter_setIncludeSRID_r(ctx_, writer.get(), 1);
  GEOSWKBWriter_setFlavor_r(ctx_, writer.get(), GEOS_WKB_EXTENDED);
  EXPECT_EQ(GEOSWKBWriter_getOutputDimension_r(ctx_, writer.get()), 3);
  EXPECT_EQ(GEOSWKBWriter_getByteOrder_r(ctx_, writer.get()), GEOS_WKB_XDR);
  EXPECT_EQ(GEOSWKBWriter_getIncludeSRID_r(ctx_, writer.get()), 1);
  EXPECT_EQ(GEOSWKBWriter_getFlavor_r(ctx_, writer.get()), GEOS_WKB_EXTENDED);

  std::size_t wkb_sz = 0;
  ScopedBytes wkb_buf(
      ctx_, GEOSWKBWriter_write_r(ctx_, writer.get(), pt3d.get(), &wkb_sz));
  ASSERT_TRUE(wkb_buf);
  ASSERT_GT(wkb_sz, 0u);
  EXPECT_EQ(wkb_buf.get()[0], 0x00);

  ScopedGeom from_wkb(
      ctx_, GEOSWKBReader_read_r(ctx_, reader.get(), wkb_buf.get(), wkb_sz));
  ASSERT_TRUE(from_wkb);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, from_wkb.get()), 4326);
  EXPECT_EQ(GEOSHasZ_r(ctx_, from_wkb.get()), 1);
  double z = 0.0;
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, from_wkb.get(), &z), 1);
  EXPECT_DOUBLE_EQ(z, 3.0);

  // ISO + NDR without SRID encodes as NDR hex starting with "01" and SRID 0.
  GEOSWKBWriter_setOutputDimension_r(ctx_, writer.get(), 2);
  GEOSWKBWriter_setByteOrder_r(ctx_, writer.get(), GEOS_WKB_NDR);
  GEOSWKBWriter_setIncludeSRID_r(ctx_, writer.get(), 0);
  GEOSWKBWriter_setFlavor_r(ctx_, writer.get(), GEOS_WKB_ISO);
  EXPECT_EQ(GEOSWKBWriter_getFlavor_r(ctx_, writer.get()), GEOS_WKB_ISO);

  std::size_t hex_sz = 0;
  ScopedBytes hex_buf(
      ctx_, GEOSWKBWriter_writeHEX_r(ctx_, writer.get(), pt3d.get(), &hex_sz));
  ASSERT_TRUE(hex_buf);
  ASSERT_GT(hex_sz, 2u);
  EXPECT_EQ(hex_buf.get()[0], '0');
  EXPECT_EQ(hex_buf.get()[1], '1');

  ScopedGeom from_hex(
      ctx_, GEOSWKBReader_readHEX_r(ctx_, reader.get(), hex_buf.get(), hex_sz));
  ASSERT_TRUE(from_hex);
  EXPECT_EQ(GEOSGetSRID_r(ctx_, from_hex.get()), 0);
  EXPECT_EQ(GEOSHasZ_r(ctx_, from_hex.get()), 0);

  // Deprecated context-level WKB output dims, byte order, and buffer helpers.
  const int orig_dims = GEOS_getWKBOutputDims_r(ctx_);
  const int orig_order = GEOS_getWKBByteOrder_r(ctx_);
  absl::Cleanup restore_wkb_ctx = [&] {
    GEOS_setWKBOutputDims_r(ctx_, orig_dims);
    GEOS_setWKBByteOrder_r(ctx_, orig_order);
  };

  EXPECT_EQ(orig_dims, 2);
  EXPECT_EQ(GEOS_setWKBOutputDims_r(ctx_, 1), 2);
  EXPECT_EQ(GEOS_setWKBOutputDims_r(ctx_, 5), 1);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("out of range 2..4")));
  ctx_.ClearMessages();

  EXPECT_EQ(GEOS_setWKBOutputDims_r(ctx_, 3), 5);
  EXPECT_EQ(GEOS_getWKBOutputDims_r(ctx_), 3);

  EXPECT_EQ(GEOS_setWKBByteOrder_r(ctx_, GEOS_WKB_XDR), orig_order);
  EXPECT_EQ(GEOS_getWKBByteOrder_r(ctx_), GEOS_WKB_XDR);

  std::size_t dep_wkb_sz = 0;
  ScopedBytes dep_wkb(ctx_, GEOSGeomToWKB_buf_r(ctx_, pt3d.get(), &dep_wkb_sz));
  ASSERT_TRUE(dep_wkb);
  ASSERT_GT(dep_wkb_sz, 0u);
  EXPECT_EQ(dep_wkb.get()[0], 0x00);

  ScopedGeom dep_from_wkb(
      ctx_, GEOSGeomFromWKB_buf_r(ctx_, dep_wkb.get(), dep_wkb_sz));
  ASSERT_TRUE(dep_from_wkb);
  EXPECT_EQ(GEOSHasZ_r(ctx_, dep_from_wkb.get()), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, dep_from_wkb.get(), &z), 1);
  EXPECT_DOUBLE_EQ(z, 3.0);

  std::size_t dep_hex_sz = 0;
  ScopedBytes dep_hex(ctx_, GEOSGeomToHEX_buf_r(ctx_, pt3d.get(), &dep_hex_sz));
  ASSERT_TRUE(dep_hex);
  ASSERT_GT(dep_hex_sz, 2u);
  EXPECT_EQ(dep_hex.get()[0], '0');
  EXPECT_EQ(dep_hex.get()[1], '0');

  ScopedGeom dep_from_hex(
      ctx_, GEOSGeomFromHEX_buf_r(ctx_, dep_hex.get(), dep_hex_sz));
  ASSERT_TRUE(dep_from_hex);
  EXPECT_EQ(GEOSHasZ_r(ctx_, dep_from_hex.get()), 1);
  EXPECT_EQ(GEOSGeomFromHEX_buf_r(
                ctx_, reinterpret_cast<const unsigned char*>("ZZ"), 2),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
}

TEST_F(GeosTsCTest, GeoJsonReaderWriterRoundTrips) {
  ScopedGeoJSONReader reader(ctx_, GEOSGeoJSONReader_create_r(ctx_));
  ASSERT_TRUE(reader);

  ScopedGeom pt3d(ctx_, GEOSGeoJSONReader_readGeometry_r(
                            ctx_, reader.get(),
                            R"({"type":"Point","coordinates":[1.5,2.5,3.5]})"));
  ASSERT_TRUE(pt3d);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, pt3d.get()), GEOS_POINT);
  EXPECT_EQ(GEOSHasZ_r(ctx_, pt3d.get()), 1);
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  EXPECT_EQ(GEOSGeomGetX_r(ctx_, pt3d.get(), &x), 1);
  EXPECT_EQ(GEOSGeomGetY_r(ctx_, pt3d.get(), &y), 1);
  EXPECT_EQ(GEOSGeomGetZ_r(ctx_, pt3d.get(), &z), 1);
  EXPECT_DOUBLE_EQ(x, 1.5);
  EXPECT_DOUBLE_EQ(y, 2.5);
  EXPECT_DOUBLE_EQ(z, 3.5);

  ScopedGeom feat(
      ctx_,
      GEOSGeoJSONReader_readGeometry_r(
          ctx_, reader.get(),
          R"({"type":"Feature","geometry":{"type":"Point","coordinates":[4,5]},"properties":{"k":"v"}})"));
  ASSERT_TRUE(feat);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, feat.get()), GEOS_POINT);

  ScopedGeom fc(
      ctx_,
      GEOSGeoJSONReader_readGeometry_r(
          ctx_, reader.get(),
          R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]},"properties":{}},{"type":"Feature","geometry":{"type":"Point","coordinates":[1,1]},"properties":{}}]})"));
  ASSERT_TRUE(fc);
  EXPECT_EQ(GEOSGeomTypeId_r(ctx_, fc.get()), GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(GEOSGetNumGeometries_r(ctx_, fc.get()), 2);

  EXPECT_EQ(GEOSGeoJSONReader_readGeometry_r(ctx_, reader.get(), "{bad json"),
            nullptr);
  EXPECT_THAT(ctx_.errors(), Not(IsEmpty()));
  ctx_.ClearMessages();

  ScopedGeoJSONWriter writer(ctx_, GEOSGeoJSONWriter_create_r(ctx_));
  ASSERT_TRUE(writer);
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension_r(ctx_, writer.get()), 3);

  GEOSGeoJSONWriter_setOutputDimension_r(ctx_, writer.get(), 4);
  EXPECT_THAT(ctx_.errors(), Contains(HasSubstr("GeoJSON output dimension")));
  ctx_.ClearMessages();
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension_r(ctx_, writer.get()), 3);

  GEOSGeoJSONWriter_setOutputDimension_r(ctx_, writer.get(), 2);
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension_r(ctx_, writer.get()), 2);
  {
    ScopedCString compact_2d(ctx_, GEOSGeoJSONWriter_writeGeometry_r(
                                       ctx_, writer.get(), pt3d.get(), -1));
    ASSERT_TRUE(compact_2d);
    const std::string text(compact_2d.get());
    EXPECT_THAT(text, HasSubstr("[1.5,2.5]"));
    EXPECT_THAT(text, Not(HasSubstr("3.5")));
    EXPECT_THAT(text, Not(HasSubstr("\n")));
  }

  GEOSGeoJSONWriter_setOutputDimension_r(ctx_, writer.get(), 3);
  EXPECT_EQ(GEOSGeoJSONWriter_getOutputDimension_r(ctx_, writer.get()), 3);
  {
    ScopedCString pretty_3d(ctx_, GEOSGeoJSONWriter_writeGeometry_r(
                                      ctx_, writer.get(), pt3d.get(), 2));
    ASSERT_TRUE(pretty_3d);
    const std::string text(pretty_3d.get());
    EXPECT_THAT(text, HasSubstr("3.5"));
    EXPECT_THAT(text, HasSubstr("\n"));

    ScopedGeom round_trip(ctx_, GEOSGeoJSONReader_readGeometry_r(
                                    ctx_, reader.get(), text.c_str()));
    ASSERT_TRUE(round_trip);
    EXPECT_EQ(GEOSHasZ_r(ctx_, round_trip.get()), 1);
  }
}

}  // namespace
}  // namespace geos_google
