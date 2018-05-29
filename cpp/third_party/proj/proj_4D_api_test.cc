// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gmock.h"
#include "gunit.h"
#include "src/proj.h"
#include "util/gtl/cleanup.h"

namespace {

TEST(Proj4dApiTest, ProjCoord) {
  double x = 1.2;
  double y = 3.4;
  double z = 5.6;
  double t = -7.8;
  PJ_COORD c = proj_coord(x, y, z, t);
  EXPECT_DOUBLE_EQ(x, c.xyzt.x);
  EXPECT_DOUBLE_EQ(y, c.xyzt.y);
  EXPECT_DOUBLE_EQ(z, c.xyzt.z);
  EXPECT_DOUBLE_EQ(t, c.xyzt.t);
}

TEST(Proj4dApiTest, BasicContext) {
  // https://proj4.org/development/quickstart.html
  // Create a thread context and projection.
  // Use those to forward and reverse project a point.

  PJ_CONTEXT *ctx = proj_context_create();
  ASSERT_THAT(ctx, ::testing::NotNull());
  auto ctx_cleaner = gtl::MakeCleanup([ctx] { proj_context_destroy(ctx); });

  PJ *pj = proj_create(ctx, "+proj=utm +zone=32 +ellps=GRS80");
  ASSERT_THAT(pj, ::testing::NotNull());
  auto pj_cleaner = gtl::MakeCleanup([pj] { proj_destroy(pj); });

  // Degrees.
  const double lat = 55.0;
  const double lon = 12.0;
  const PJ_COORD a = proj_coord(proj_torad(lon), proj_torad(lat), 0, 0);

  const PJ_COORD b = proj_trans(pj, PJ_FWD, a);
  EXPECT_DOUBLE_EQ(691875.63213966065, b.enu.e);
  EXPECT_DOUBLE_EQ(6098907.8250050116, b.enu.n);

  const PJ_COORD c = proj_trans(pj, PJ_INV, b);
  EXPECT_DOUBLE_EQ(0.20943951023931953, c.lp.lam);
  EXPECT_DOUBLE_EQ(0.95993108859688114, c.lp.phi);
}

// TODO(schwehr): Test proj_angular_input
// TODO(schwehr): Test proj_angular_output
// TODO(schwehr): Test proj_geod
// TODO(schwehr): Test proj_lp_dist
// TODO(schwehr): Test proj_lpz_dist
// TODO(schwehr): Test proj_xy_dist
// TODO(schwehr): Test proj_xyz_dist
// TODO(schwehr): Test proj_roundtrip
// TODO(schwehr): Test proj_trans
// TODO(schwehr): Test proj_trans_array
// TODO(schwehr): Test proj_trans_generic
// TODO(schwehr): Test proj_geocentric_latitude
// TODO(schwehr): Test proj_torad
// TODO(schwehr): Test proj_todeg
// TODO(schwehr): Test proj_dmstor
// TODO(schwehr): Test proj_rtodms
// TODO(schwehr): Test proj_create
// TODO(schwehr): Test proj_create_argv
// TODO(schwehr): Test proj_create_crs_to_crs
// TODO(schwehr): Test proj_destroy
// TODO(schwehr): Test proj_errno
// TODO(schwehr): Test proj_context_errno
// TODO(schwehr): Test proj_errno_set
// TODO(schwehr): Test proj_errno_restore
// TODO(schwehr): Test proj_errno_reset
// TODO(schwehr): Test proj_context_create
// TODO(schwehr): Test proj_context_destroy

TEST(Proj4dApiTest, ProjInfo) {
  const PJ_INFO i = proj_info();
  EXPECT_LE(5, i.major);
  EXPECT_LE(0, i.minor);
  EXPECT_LE(0, i.patch);

  EXPECT_THAT(i.release, testing::HasSubstr("Rel"));
  EXPECT_THAT(i.version, testing::HasSubstr("."));
  EXPECT_THAT(i.searchpath, testing::HasSubstr("/"));

  EXPECT_THAT(i.paths, ::testing::IsNull());
  EXPECT_EQ(0, i.path_count);
}

// TODO(schwehr): Test proj_pj_info
// TODO(schwehr): Test proj_grid_info
// TODO(schwehr): Test proj_init_info
// TODO(schwehr): Test proj_factors

}  // namespace
