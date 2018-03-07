// Copyright 2017 Google Inc. All Rights Reserved.
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
//
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2016, Thomas Knudsen
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Based on pj_utm_selftest and pj_run_selftests.c:
//   https://github.com/OSGeo/proj.4/blob/ad7a7c1b1d54c69/src/proj_etmerc.c#L449
//   https://github.com/OSGeo/proj.4/blob/b288ee6bbc345ba/src/pj_run_selftests.c

#include <math.h>

#include "base/logging.h"
#include "gunit.h"
#include "third_party/absl/base/macros.h"
// TODO(schwehr): Add proj.h after cl/162737615 and change .u and .v below.
// #include "proj.h"  // Avoid type pruning.
#include "projects.h"
#include "util/gtl/cleanup.h"

namespace {

// TODO(schwehr): Factor the helpers out into a library for other tests.

// Determine whether two XYs deviate by more than <tolerance>.
//
// The test material ("expected" values) may contain coordinates that
// are indeterminate. For those cases, we test the other coordinate
// only by forcing expected and actual ("got") coordinates to 0.
bool deviates_xy(XY expected, XY got, double tolerance) {
  if (HUGE_VAL == expected.u || HUGE_VAL == expected.v) return false;
  const double delta = hypot(expected.u - got.u, expected.v - got.v);
  if (delta > tolerance) {
    LOG(ERROR) << delta << " > " << tolerance;
    return true;
  }
  return false;
}

// Determine whether two LPs deviate by more than <tolerance>.
//
// This one is slightly tricky, since the <expected> LP is
// supposed to be represented as degrees (since it was at some
// time written down by a real human), whereas the <got> LP is
// represented in radians (since it is supposed to be the result
// output from pj_inv).
bool deviates_lp(LP expected, LP got, double tolerance) {
  if (HUGE_VAL == expected.u || HUGE_VAL == expected.v) return false;
  const double delta =
      hypot(DEG_TO_RAD * expected.u - got.u, DEG_TO_RAD * expected.v - got.v);
  if (delta > tolerance) {
    LOG(ERROR) << delta << " > " << tolerance;
    return true;
  }
  return false;
}

// Wrapper for pj_fwd, accepting input in degrees.
XY pj_fwd_deg(LP in, PJ *P) {
  const LP in_rad = {DEG_TO_RAD * in.u, DEG_TO_RAD * in.v};
  return pj_fwd(in_rad, P);
}

class ProjTest : public ::testing::Test {
 public:
  void SetProjection(const char *projection) {
    pj_ = pj_init_plus(projection);
    ASSERT_NE(nullptr, pj_);
  }
  ~ProjTest() override {
    if (pj_ != nullptr) pj_free(pj_);
  }

 protected:
  PJ *pj_ = nullptr;
};

constexpr char kProjection[] =
    "+proj=utm +ellps=GRS80 +lat_1=0.5 +lat_2=2 +n=0.5 +zone=30";

TEST_F(ProjTest, UtmForward) {
  SetProjection(kProjection);

  // TODO(schwehr): After update use: 1e-7;
  constexpr double tolerance_xy = 3e-4;
  constexpr LP fwd_in[] = {{2, 1}, {2, -1}, {-2, 1}, {-2, -1}};
  constexpr XY e_fwd_expect[] = {
      {1057002.4054912981, 110955.14117594929},
      {1057002.4054912981, -110955.14117594929},
      {611263.81227890507, 110547.10569680421},
      {611263.81227890507, -110547.10569680421},
  };

  for (int i = 0; i < ABSL_ARRAYSIZE(e_fwd_expect); i++) {
    EXPECT_FALSE(
        deviates_xy(e_fwd_expect[i], pj_fwd_deg(fwd_in[i], pj_), tolerance_xy));
  }
}

TEST_F(ProjTest, UtmInverse) {
  SetProjection(kProjection);

  constexpr double tolerance_lp = 1e-10;
  constexpr XY inv_in[] = {{200, 100}, {200, -100}, {-200, 100}, {-200, -100}};
  constexpr LP e_inv_expect[] = {
      {-7.4869520833902357, 0.00090193980983462605},
      {-7.4869520833902357, -0.00090193980983462605},
      {-7.4905356820622613, 0.00090193535121489081},
      {-7.4905356820622613, -0.00090193535121489081},
  };

  for (int i = 0; i < ABSL_ARRAYSIZE(inv_in); i++) {
    EXPECT_FALSE(
        deviates_lp(e_inv_expect[i], pj_inv(inv_in[i], pj_), tolerance_lp));
  }
}

}  // namespace
