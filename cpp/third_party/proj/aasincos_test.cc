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

#include <cmath>
#include <limits>
#include <memory>

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
// Order matters for the PROJ includes.
#include "src/projects.h"
#include "src/proj_api.h"

namespace {

class ProjCtxTest : public ::testing::Test {
  void SetUp() override { ctx_ = pj_ctx_alloc(); }
  void TearDown() override { pj_ctx_free(ctx_); }

 protected:
  projCtx_t *ctx_ = nullptr;
};

TEST_F(ProjCtxTest, AsinNegOne) {
  EXPECT_DOUBLE_EQ(-M_PI_2, aasin(ctx_, -1.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinZero) {
  EXPECT_DOUBLE_EQ(0.0, aasin(ctx_, 0.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinOne) {
  EXPECT_DOUBLE_EQ(M_PI_2, aasin(ctx_, 1.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinNegOneWithinTolerance) {
  EXPECT_DOUBLE_EQ(-M_PI_2, aasin(ctx_, -1.000000000000005));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinOneWithinTolerance) {
  EXPECT_DOUBLE_EQ(M_PI_2, aasin(ctx_, 1.000000000000005));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinTooNegative) {
  EXPECT_DOUBLE_EQ(-M_PI_2, aasin(ctx_, -2.0));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinTooPositive) {
  EXPECT_DOUBLE_EQ(M_PI_2, aasin(ctx_, 2.0));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinNegInf) {
  const auto inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(-M_PI_2, aasin(ctx_, -inf));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinPosInf) {
  const auto inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(M_PI_2, aasin(ctx_, inf));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AsinNan) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(isnan(aasin(ctx_, nan)));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosNegOne) {
  EXPECT_DOUBLE_EQ(M_PI, aacos(ctx_, -1.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosZero) {
  EXPECT_DOUBLE_EQ(M_PI_2, aacos(ctx_, 0.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosOne) {
  EXPECT_DOUBLE_EQ(0.0, aacos(ctx_, 1.0));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosNegWithinTolerance) {
  EXPECT_DOUBLE_EQ(M_PI, aacos(ctx_, -1.000000000000005));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosWithinTolerance) {
  EXPECT_DOUBLE_EQ(0.0, aacos(ctx_, 1.000000000000005));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosTooNegative) {
  EXPECT_DOUBLE_EQ(M_PI, aacos(ctx_, -2.0));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosTooPositive) {
  EXPECT_DOUBLE_EQ(0.0, aacos(ctx_, 2.0));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosInf) {
  const auto inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(M_PI, aacos(ctx_, -inf));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosNegInf) {
  const auto inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(0.0, aacos(ctx_, inf));
  EXPECT_EQ(PJD_ERR_ACOS_ASIN_ARG_TOO_LARGE, pj_ctx_get_errno(ctx_));
}

TEST_F(ProjCtxTest, AcosNan) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(isnan(aacos(ctx_, nan)));
  EXPECT_EQ(0, pj_ctx_get_errno(ctx_));
}

TEST(AaSinCosTest, Asqrt) {
  // asqrt does not use a ctx or report error conditions.
  EXPECT_DOUBLE_EQ(0.0, asqrt(-0.1));

  EXPECT_DOUBLE_EQ(0.0, asqrt(0.0));
  EXPECT_DOUBLE_EQ(2.0, asqrt(4.0));

  constexpr auto inf = std::numeric_limits<double>::infinity();
  EXPECT_DOUBLE_EQ(0.0, asqrt(-inf));
  EXPECT_TRUE(isinf(asqrt(inf)));

  const auto nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(isnan(asqrt(nan)));
}

// TODO(schwehr): Test aatan2.

}  // namespace
