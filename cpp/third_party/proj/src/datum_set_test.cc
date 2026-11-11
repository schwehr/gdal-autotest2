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
// Unit tests for built-in datum expansion, nadgrids, and towgs84 parsing
// (proj/src/datum_set.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/datum_set.cpp

#include <initializer_list>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kArcsecToRad = std::numbers::pi / (180.0 * 3600.0);

// Allocates a PJ wrapped in ScopedPj whose P->params holds the supplied list;
// pj_default_destructor frees both initial and pj_datum_set-appended nodes.
ScopedPj MakePjWithParams(PJ_CONTEXT* ctx,
                          std::initializer_list<const char*> params) {
  ScopedPj pj(pj_new());
  if (!pj) {
    return nullptr;
  }
  pj->ctx = ctx;
  paralist* tail = nullptr;
  for (const char* p : params) {
    paralist* node = pj_mkparam(p);
    if (!node) {
      return nullptr;
    }
    if (pj->params == nullptr) {
      pj->params = node;
    } else {
      tail->next = node;
    }
    tail = node;
  }
  return pj;
}

TEST(DatumSetTest, NoDatumParametersSetsPjdUnknown) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj empty_pj = MakePjWithParams(ctx.get(), {});
  ASSERT_NE(nullptr, empty_pj.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), empty_pj->params, empty_pj.get()));
  EXPECT_EQ(PJD_UNKNOWN, empty_pj->datum_type);

  ScopedPj plain_pj = MakePjWithParams(ctx.get(), {"proj=merc", "ellps=WGS84"});
  ASSERT_NE(nullptr, plain_pj.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), plain_pj->params, plain_pj.get()));
  EXPECT_EQ(PJD_UNKNOWN, plain_pj->datum_type);
}

TEST(DatumSetTest, BuiltInDatumsExpandEllipsoidAndTransformParameters) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Single-node paralist exercises curr->next == nullptr end-of-list path.
  ScopedPj wgs84 = MakePjWithParams(ctx.get(), {"datum=WGS84"});
  ASSERT_NE(nullptr, wgs84.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), wgs84->params, wgs84.get()));
  EXPECT_EQ(PJD_3PARAM, wgs84->datum_type);
  EXPECT_STREQ("WGS84", pj_param(ctx.get(), wgs84->params, "sellps").s);
  EXPECT_DOUBLE_EQ(0.0, wgs84->datum_params[0]);

  // 2. Multi-node paralist traverses to tail before appending GGRS87 3-param.
  ScopedPj ggrs87 = MakePjWithParams(ctx.get(), {"proj=tmerc", "datum=GGRS87"});
  ASSERT_NE(nullptr, ggrs87.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), ggrs87->params, ggrs87.get()));
  EXPECT_EQ(PJD_3PARAM, ggrs87->datum_type);
  EXPECT_STREQ("GRS80", pj_param(ctx.get(), ggrs87->params, "sellps").s);
  EXPECT_NEAR(-199.87, ggrs87->datum_params[0], 1e-12);
  EXPECT_NEAR(74.79, ggrs87->datum_params[1], 1e-12);
  EXPECT_NEAR(246.62, ggrs87->datum_params[2], 1e-12);

  // 3. Built-in gridshift datum (NAD27) appends clrk66 + nadgrids list.
  ScopedPj nad27 = MakePjWithParams(ctx.get(), {"proj=tmerc", "datum=NAD27"});
  ASSERT_NE(nullptr, nad27.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), nad27->params, nad27.get()));
  EXPECT_EQ(PJD_GRIDSHIFT, nad27->datum_type);
  EXPECT_STREQ("clrk66", pj_param(ctx.get(), nad27->params, "sellps").s);
  EXPECT_STREQ("@conus,@alaska,@ntv2_0.gsb,@ntv1_can.dat",
               pj_param(ctx.get(), nad27->params, "snadgrids").s);

  // 4. Built-in 7-parameter datum (OSGB36) converts arcsec -> rad and ppm.
  ScopedPj osgb36 = MakePjWithParams(ctx.get(), {"proj=tmerc", "datum=OSGB36"});
  ASSERT_NE(nullptr, osgb36.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), osgb36->params, osgb36.get()));
  EXPECT_EQ(PJD_7PARAM, osgb36->datum_type);
  EXPECT_STREQ("airy", pj_param(ctx.get(), osgb36->params, "sellps").s);
  EXPECT_NEAR(446.448, osgb36->datum_params[0], 1e-12);
  EXPECT_NEAR(-125.157, osgb36->datum_params[1], 1e-12);
  EXPECT_NEAR(542.060, osgb36->datum_params[2], 1e-12);
  EXPECT_NEAR(0.1502 * kArcsecToRad, osgb36->datum_params[3], 1e-15);
  EXPECT_NEAR(1.0 - 20.4894 / 1e6, osgb36->datum_params[6], 1e-15);
}

TEST(DatumSetTest, UnknownDatumLogsErrorAndSetsIllegalArgErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  ScopedPj bad =
      MakePjWithParams(ctx.get(), {"proj=merc", "datum=nonexistent"});
  ASSERT_NE(nullptr, bad.get());
  EXPECT_EQ(1, pj_datum_set(ctx.get(), bad->params, bad.get()));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(capture.ContainsMessage("Unknown value for datum"));
}

TEST(DatumSetTest, NadgridsTakesPrecedenceOverTowgs84) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj grid_only = MakePjWithParams(ctx.get(), {"nadgrids=@null"});
  ASSERT_NE(nullptr, grid_only.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), grid_only->params, grid_only.get()));
  EXPECT_EQ(PJD_GRIDSHIFT, grid_only->datum_type);

  ScopedPj both =
      MakePjWithParams(ctx.get(), {"nadgrids=@null", "towgs84=1,2,3"});
  ASSERT_NE(nullptr, both.get());
  EXPECT_EQ(0, pj_datum_set(ctx.get(), both->params, both.get()));
  EXPECT_EQ(PJD_GRIDSHIFT, both->datum_type);
}

TEST(DatumSetTest, Towgs84ThreeAndSevenParameterParsingBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct Towgs84Case {
    const char* param;
    int expected_type;
    double expected_p0;
    double expected_p1;
    double expected_p2;
    double expected_p3_rad;
    double expected_p4_rad;
    double expected_p5_rad;
    double expected_p6_scale;
  };
  constexpr Towgs84Case kCases[] = {
      {.param = "towgs84=",
       .expected_type = PJD_3PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 0.0},
      {.param = "towgs84=10,-20",
       .expected_type = PJD_3PARAM,
       .expected_p0 = 10.0,
       .expected_p1 = -20.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 0.0},
      {.param = "towgs84=100,-200,300",
       .expected_type = PJD_3PARAM,
       .expected_p0 = 100.0,
       .expected_p1 = -200.0,
       .expected_p2 = 300.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 0.0},
      {.param = "towgs84=0,0,0,0,0,0,0",
       .expected_type = PJD_3PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 0.0},
      // Exercise each individual clause of the 7-parameter non-zero check.
      {.param = "towgs84=0,0,0,1.5,0,0,0",
       .expected_type = PJD_7PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 1.5 * kArcsecToRad,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 1.0},
      {.param = "towgs84=0,0,0,0,-2.0,0,0",
       .expected_type = PJD_7PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = -2.0 * kArcsecToRad,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 1.0},
      {.param = "towgs84=0,0,0,0,0,3.0,0",
       .expected_type = PJD_7PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 3.0 * kArcsecToRad,
       .expected_p6_scale = 1.0},
      {.param = "towgs84=0,0,0,0,0,0,15.0",
       .expected_type = PJD_7PARAM,
       .expected_p0 = 0.0,
       .expected_p1 = 0.0,
       .expected_p2 = 0.0,
       .expected_p3_rad = 0.0,
       .expected_p4_rad = 0.0,
       .expected_p5_rad = 0.0,
       .expected_p6_scale = 1.0 + 15.0 / 1e6},
      // Extra values beyond 7 are ignored without overflowing datum_params.
      {.param = "towgs84=1,2,3,4,5,6,7,8,9",
       .expected_type = PJD_7PARAM,
       .expected_p0 = 1.0,
       .expected_p1 = 2.0,
       .expected_p2 = 3.0,
       .expected_p3_rad = 4.0 * kArcsecToRad,
       .expected_p4_rad = 5.0 * kArcsecToRad,
       .expected_p5_rad = 6.0 * kArcsecToRad,
       .expected_p6_scale = 1.0 + 7.0 / 1e6},
  };

  for (const auto& tc : kCases) {
    ScopedPj pj = MakePjWithParams(ctx.get(), {tc.param});
    ASSERT_NE(nullptr, pj.get()) << tc.param;
    EXPECT_EQ(0, pj_datum_set(ctx.get(), pj->params, pj.get())) << tc.param;
    EXPECT_EQ(tc.expected_type, pj->datum_type) << tc.param;
    EXPECT_NEAR(tc.expected_p0, pj->datum_params[0], 1e-12) << tc.param;
    EXPECT_NEAR(tc.expected_p1, pj->datum_params[1], 1e-12) << tc.param;
    EXPECT_NEAR(tc.expected_p2, pj->datum_params[2], 1e-12) << tc.param;
    EXPECT_NEAR(tc.expected_p3_rad, pj->datum_params[3], 1e-15) << tc.param;
    EXPECT_NEAR(tc.expected_p4_rad, pj->datum_params[4], 1e-15) << tc.param;
    EXPECT_NEAR(tc.expected_p5_rad, pj->datum_params[5], 1e-15) << tc.param;
    EXPECT_NEAR(tc.expected_p6_scale, pj->datum_params[6], 1e-15) << tc.param;
  }
}

TEST(DatumSetTest, ProjCreateIntegratesDatumTypes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. pj_init promotes zero-shift towgs84 on WGS84 ellipsoid to PJD_WGS84.
  ScopedPj wgs84(proj_create(ctx.get(), "+proj=merc +datum=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());
  EXPECT_EQ(PJD_WGS84, wgs84->datum_type);

  ScopedPj ggrs87(proj_create(ctx.get(), "+proj=merc +datum=GGRS87"));
  ASSERT_NE(nullptr, ggrs87.get());
  EXPECT_EQ(PJD_3PARAM, ggrs87->datum_type);

  ScopedPj osgb36(proj_create(ctx.get(), "+proj=merc +datum=OSGB36"));
  ASSERT_NE(nullptr, osgb36.get());
  EXPECT_EQ(PJD_7PARAM, osgb36->datum_type);

  ScopedPj grid(proj_create(ctx.get(), "+proj=merc +nadgrids=@null"));
  ASSERT_NE(nullptr, grid.get());
  EXPECT_EQ(PJD_GRIDSHIFT, grid->datum_type);

  ScopedPj invalid(proj_create(ctx.get(), "+proj=merc +datum=no_such_datum"));
  EXPECT_EQ(nullptr, invalid.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

// Note: defensive unreachable guards in datum_set.cpp (curr == nullptr after
// matching "sdatum" and pj_mkparam OOM branches) cannot be triggered via normal
// allocation paths.

}  // namespace
}  // namespace proj
