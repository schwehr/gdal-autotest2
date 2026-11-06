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
// Tests GCP / CRS polynomial transformer (alg/gdal_crs.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_crs.cpp

#include <cmath>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"

CPL_C_START
CPLXMLNode* GDALSerializeGCPTransformer(void* pTransformArg);
void* GDALDeserializeGCPTransformer(CPLXMLNode* psTree);
CPL_C_END

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Helper to initialize a GDAL_GCP struct directly without dynamic allocation
// so that there are no leaked strings under ASan/LSan.
GDAL_GCP MakeGCP(double pixel, double line, double x, double y,
                 double z = 0.0) {
  GDAL_GCP gcp;
  gcp.pszId = const_cast<char*>("");
  gcp.pszInfo = const_cast<char*>("");
  gcp.dfGCPPixel = pixel;
  gcp.dfGCPLine = line;
  gcp.dfGCPX = x;
  gcp.dfGCPY = y;
  gcp.dfGCPZ = z;
  return gcp;
}

class GdalCrsTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalCrsTest, NullPointerSafety) {
  // 1. Destroying a null transformer is a safe no-op.
  GDALDestroyGCPTransformer(nullptr);

  // 2. Serializing a null transformer returns null and sets an error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeGCPTransformer(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GDALSerializeGCPTransformer"));
    CPLErrorReset();
  }

  // 3. Creating similar transformer from null returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
    CPLErrorReset();
  }

  // 4. Invoking pfnCreateSimilar directly with null returns null with error.
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  auto* psInfo = static_cast<GDALTransformerInfo*>(hTransform);
  ASSERT_NE(nullptr, psInfo->pfnCreateSimilar);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, psInfo->pfnCreateSimilar(nullptr, 1.0, 1.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALCreateSimilarGCPTransformer"));
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, InvalidArgumentsAndInsufficientGCPs) {
  // 1. 0 GCPs fails with "Not enough points available".
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPTransformer(0, nullptr, 1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }

  const std::vector<GDAL_GCP> gcps3 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };

  // 2. Negative order fails with "Parameter error".
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPTransformer(static_cast<int>(gcps3.size()),
                                                gcps3.data(), -1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Parameter error"));
    CPLErrorReset();
  }

  // 3. Order > 3 fails with "Parameter error".
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPTransformer(static_cast<int>(gcps3.size()),
                                                gcps3.data(), 4, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Parameter error"));
    CPLErrorReset();
  }

  // 4. Insufficient points for order 2 (needs 6, given 5).
  const std::vector<GDAL_GCP> gcps5 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),  MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPTransformer(static_cast<int>(gcps5.size()),
                                                gcps5.data(), 2, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }

  // 5. Insufficient points for order 3 (needs 10, given 6).
  const std::vector<GDAL_GCP> gcps6 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),  MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0), MakeGCP(0.0, 20.0, 240.0, 310.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPTransformer(static_cast<int>(gcps6.size()),
                                                gcps6.data(), 3, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, CollinearGCPsUnsolvable) {
  // 1. 3 collinear GCPs (exact determinant is 0).
  const std::vector<GDAL_GCP> collinear_gcps = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),
      MakeGCP(10.0, 10.0, 110.0, 110.0),
      MakeGCP(20.0, 20.0, 120.0, 120.0),
  };

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGCPTransformer(static_cast<int>(collinear_gcps.size()),
                                       collinear_gcps.data(), 1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Transform is not solvable"));
    CPLErrorReset();
  }

  // 2. 4 collinear GCPs (least-squares overdetermined matrix is singular).
  const std::vector<GDAL_GCP> collinear_gcps4 = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),
      MakeGCP(10.0, 10.0, 110.0, 110.0),
      MakeGCP(20.0, 20.0, 120.0, 120.0),
      MakeGCP(30.0, 30.0, 130.0, 130.0),
  };

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGCPTransformer(static_cast<int>(collinear_gcps4.size()),
                                       collinear_gcps4.data(), 1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Transform is not solvable"));
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, TwoGCPsCornerSynthesis) {
  // 1. Two GCPs with different X and different Y at Order 1:
  // Assumes opposite corners of a rectangle and synthesizes a 3rd corner.
  const std::vector<GDAL_GCP> valid_gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 20.0, 110.0, 220.0),
  };

  void* hTransform = GDALCreateGCPTransformer(
      static_cast<int>(valid_gcps.size()), valid_gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // Transform center of the rectangle: pixel (5.0, 10.0) -> georef (105.0,
  // 210.0).
  double x = 5.0;
  double y = 10.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(105.0, x, 1e-5);
  EXPECT_NEAR(210.0, y, 1e-5);

  // 2. Two GCPs with identical X fails because opposite corners require dX !=
  // 0.
  const std::vector<GDAL_GCP> same_x_gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 20.0, 100.0, 220.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGCPTransformer(static_cast<int>(same_x_gcps.size()),
                                       same_x_gcps.data(), 1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }

  // 3. Two GCPs with identical Y fails because opposite corners require dY !=
  // 0.
  const std::vector<GDAL_GCP> same_y_gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 20.0, 110.0, 200.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGCPTransformer(static_cast<int>(same_y_gcps.size()),
                                       same_y_gcps.data(), 1, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }

  // 4. Two GCPs with Order 2 fails (corner synthesis only applies to Order 1).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateGCPTransformer(static_cast<int>(valid_gcps.size()),
                                       valid_gcps.data(), 2, FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Not enough points available"));
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, Order1Transformation) {
  // Affine model:
  // x = 100.0 + 2.0 * pixel + 0.5 * line
  // y = 200.0 + 0.5 * pixel + 3.0 * line
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };

  // Exact 3 points triggers exactdet branch in calccoef.
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // 1. Forward transform inside GCP envelope: (5.0, 5.0) -> (112.5, 217.5).
  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);

  // 2. Reverse transform inside GCP envelope: (112.5, 217.5) -> (5.0, 5.0).
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(5.0, x, 1e-5);
  EXPECT_NEAR(5.0, y, 1e-5);

  // 3. Forward transform outside GCP envelope: (20.0, -10.0) -> (135.0, 180.0).
  x = 20.0;
  y = -10.0;
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(135.0, x, 1e-5);
  EXPECT_NEAR(180.0, y, 1e-5);

  // 4. Reverse transform outside GCP envelope: (135.0, 180.0) -> (20.0, -10.0).
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(20.0, x, 1e-5);
  EXPECT_NEAR(-10.0, y, 1e-5);

  // 5. Reversed transformer (bReversed = TRUE) inverts forward and reverse
  // meanings.
  void* hReversedTransform = GDALCreateGCPTransformer(
      static_cast<int>(gcps.size()), gcps.data(), 1, TRUE);
  ASSERT_NE(nullptr, hReversedTransform);
  absl::Cleanup cleanup_rev = [hReversedTransform] {
    GDALDestroyGCPTransformer(hReversedTransform);
  };

  // Calling with bDstToSrc = FALSE acts as DstToSrc when bReversed is TRUE.
  x = 112.5;
  y = 217.5;
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGCPTransform(hReversedTransform, FALSE, 1, &x, &y,
                                   nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(5.0, x, 1e-5);
  EXPECT_NEAR(5.0, y, 1e-5);

  // Calling with bDstToSrc = TRUE acts as SrcToDst when bReversed is TRUE.
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGCPTransform(hReversedTransform, TRUE, 1, &x, &y, nullptr,
                                   &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);
}

TEST_F(GdalCrsTest, Order1Overdetermined) {
  // 4 points (overdetermined) triggers calcls least-squares branch in calccoef.
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
      MakeGCP(10.0, 10.0, 125.0, 235.0),
  };

  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // Batch transformation of 2 points.
  std::vector<double> xs = {5.0, 10.0};
  std::vector<double> ys = {5.0, 10.0};
  std::vector<int> successes = {FALSE, FALSE};
  EXPECT_EQ(TRUE, GDALGCPTransform(hTransform, FALSE, 2, xs.data(), ys.data(),
                                   nullptr, successes.data()));
  EXPECT_EQ(TRUE, successes[0]);
  EXPECT_EQ(TRUE, successes[1]);
  EXPECT_NEAR(112.5, xs[0], 1e-5);
  EXPECT_NEAR(217.5, ys[0], 1e-5);
  EXPECT_NEAR(125.0, xs[1], 1e-5);
  EXPECT_NEAR(235.0, ys[1], 1e-5);

  // Batch reverse transform.
  EXPECT_EQ(TRUE, GDALGCPTransform(hTransform, TRUE, 2, xs.data(), ys.data(),
                                   nullptr, successes.data()));
  EXPECT_EQ(TRUE, successes[0]);
  EXPECT_EQ(TRUE, successes[1]);
  EXPECT_NEAR(5.0, xs[0], 1e-5);
  EXPECT_NEAR(5.0, ys[0], 1e-5);
  EXPECT_NEAR(10.0, xs[1], 1e-5);
  EXPECT_NEAR(10.0, ys[1], 1e-5);
}

TEST_F(GdalCrsTest, Order2Transformation) {
  // Quadratic model:
  // x = 100.0 + 2.0*p + 3.0*l + 0.1*p*p + 0.05*p*l + 0.2*l*l
  // y = 200.0 + 1.5*p + 2.5*l + 0.05*p*p + 0.1*p*l + 0.15*l*l
  const std::vector<GDAL_GCP> gcps6 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),  MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0), MakeGCP(0.0, 20.0, 240.0, 310.0),
  };

  // Exact 6 points triggers exactdet for Order 2.
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps6.size()),
                                              gcps6.data(), 2, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // 1. Forward transform of point (5.0, 5.0) -> (133.75, 227.5).
  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(133.75, x, 1e-5);
  EXPECT_NEAR(227.5, y, 1e-5);

  // 2. Exact match at GCP (10.0, 10.0) in both forward and reverse directions.
  x = 10.0;
  y = 10.0;
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(185.0, x, 1e-5);
  EXPECT_NEAR(270.0, y, 1e-5);

  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(10.0, x, 1e-5);
  EXPECT_NEAR(10.0, y, 1e-5);
}

TEST_F(GdalCrsTest, Order2Overdetermined) {
  // 8 points for Order 2 triggers calcls for Order 2.
  const std::vector<GDAL_GCP> gcps8 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),  MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0), MakeGCP(0.0, 20.0, 240.0, 310.0),
      MakeGCP(20.0, 10.0, 240.0, 310.0), MakeGCP(10.0, 20.0, 280.0, 350.0),
  };

  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps8.size()),
                                              gcps8.data(), 2, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(133.75, x, 1e-5);
  EXPECT_NEAR(227.5, y, 1e-5);
}

TEST_F(GdalCrsTest, Order3Transformation) {
  // Cubic model:
  // x = 100 + 2*p + 3*l + 0.1*p^2 + 0.05*pl + 0.2*l^2 + 0.001*p^3 + 0.002*p^2*l
  // + 0.001*p*l^2 + 0.003*l^3 y = 200 + 1.5*p + 2.5*l + 0.05*p^2 + 0.1*pl +
  // 0.15*l^2 + 0.002*p^3 + 0.001*p^2*l + 0.002*p*l^2 + 0.001*l^3
  const std::vector<GDAL_GCP> gcps10 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 131.0, 222.0),
      MakeGCP(20.0, 0.0, 188.0, 266.0),  MakeGCP(30.0, 0.0, 277.0, 344.0),
      MakeGCP(0.0, 10.0, 153.0, 241.0),  MakeGCP(10.0, 10.0, 192.0, 276.0),
      MakeGCP(20.0, 10.0, 261.0, 335.0), MakeGCP(0.0, 20.0, 264.0, 318.0),
      MakeGCP(10.0, 20.0, 313.0, 370.0), MakeGCP(0.0, 30.0, 451.0, 437.0),
  };

  // Exact 10 points triggers exactdet for Order 3 and exercises all 10 terms of
  // term().
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps10.size()),
                                              gcps10.data(), 3, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // Forward transform of point (5.0, 5.0) -> (134.625, 228.25).
  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(134.625, x, 1e-4);
  EXPECT_NEAR(228.25, y, 1e-4);

  // Exact reverse transform at GCP (10.0, 10.0).
  x = 192.0;
  y = 276.0;
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(10.0, x, 1e-4);
  EXPECT_NEAR(10.0, y, 1e-4);
}

TEST_F(GdalCrsTest, Order3Overdetermined) {
  // 12 points for Order 3 triggers calcls for Order 3.
  const std::vector<GDAL_GCP> gcps12 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),     MakeGCP(10.0, 0.0, 131.0, 222.0),
      MakeGCP(20.0, 0.0, 188.0, 266.0),    MakeGCP(30.0, 0.0, 277.0, 344.0),
      MakeGCP(0.0, 10.0, 153.0, 241.0),    MakeGCP(10.0, 10.0, 192.0, 276.0),
      MakeGCP(20.0, 10.0, 261.0, 335.0),   MakeGCP(0.0, 20.0, 264.0, 318.0),
      MakeGCP(10.0, 20.0, 313.0, 370.0),   MakeGCP(0.0, 30.0, 451.0, 437.0),
      MakeGCP(15.0, 5.0, 182.625, 266.25), MakeGCP(5.0, 15.0, 218.375, 293.75),
  };

  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps12.size()),
                                              gcps12.data(), 3, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(134.625, x, 1e-4);
  EXPECT_NEAR(228.25, y, 1e-4);
}

TEST_F(GdalCrsTest, Order0AutomaticSelection) {
  // 1. With 3 GCPs and Order 0, Order 1 is automatically selected.
  const std::vector<GDAL_GCP> gcps3 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };
  void* hTransform3 = GDALCreateGCPTransformer(static_cast<int>(gcps3.size()),
                                               gcps3.data(), 0, FALSE);
  ASSERT_NE(nullptr, hTransform3);
  absl::Cleanup cleanup3 = [hTransform3] {
    GDALDestroyGCPTransformer(hTransform3);
  };

  CPLXMLNode* psTree3 = GDALSerializeGCPTransformer(hTransform3);
  ASSERT_NE(nullptr, psTree3);
  absl::Cleanup cleanup_tree3 = [psTree3] { CPLDestroyXMLNode(psTree3); };
  EXPECT_STREQ("1", CPLGetXMLValue(psTree3, "Order", ""));

  // 2. With 6 GCPs and Order 0, Order 2 is automatically selected.
  const std::vector<GDAL_GCP> gcps6 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),  MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0), MakeGCP(0.0, 20.0, 240.0, 310.0),
  };
  void* hTransform6 = GDALCreateGCPTransformer(static_cast<int>(gcps6.size()),
                                               gcps6.data(), 0, FALSE);
  ASSERT_NE(nullptr, hTransform6);
  absl::Cleanup cleanup6 = [hTransform6] {
    GDALDestroyGCPTransformer(hTransform6);
  };

  CPLXMLNode* psTree6 = GDALSerializeGCPTransformer(hTransform6);
  ASSERT_NE(nullptr, psTree6);
  absl::Cleanup cleanup_tree6 = [psTree6] { CPLDestroyXMLNode(psTree6); };
  EXPECT_STREQ("2", CPLGetXMLValue(psTree6, "Order", ""));

  // 3. With 10 GCPs and Order 0, Order 2 is also selected (avoiding 3rd order
  // instability).
  const std::vector<GDAL_GCP> gcps10 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),   MakeGCP(10.0, 0.0, 131.0, 222.0),
      MakeGCP(20.0, 0.0, 188.0, 266.0),  MakeGCP(30.0, 0.0, 277.0, 344.0),
      MakeGCP(0.0, 10.0, 153.0, 241.0),  MakeGCP(10.0, 10.0, 192.0, 276.0),
      MakeGCP(20.0, 10.0, 261.0, 335.0), MakeGCP(0.0, 20.0, 264.0, 318.0),
      MakeGCP(10.0, 20.0, 313.0, 370.0), MakeGCP(0.0, 30.0, 451.0, 437.0),
  };
  void* hTransform10 = GDALCreateGCPTransformer(static_cast<int>(gcps10.size()),
                                                gcps10.data(), 0, FALSE);
  ASSERT_NE(nullptr, hTransform10);
  absl::Cleanup cleanup10 = [hTransform10] {
    GDALDestroyGCPTransformer(hTransform10);
  };

  CPLXMLNode* psTree10 = GDALSerializeGCPTransformer(hTransform10);
  ASSERT_NE(nullptr, psTree10);
  absl::Cleanup cleanup_tree10 = [psTree10] { CPLDestroyXMLNode(psTree10); };
  EXPECT_STREQ("2", CPLGetXMLValue(psTree10, "Order", ""));
}

TEST_F(GdalCrsTest, RefineTransformer) {
  // 1. Refine transformer with 1 outlier removed down to 6 consistent quadratic
  // GCPs.
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),
      MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0),
      MakeGCP(0.0, 20.0, 240.0, 310.0),
      // Deliberate outlier with massive coordinate error.
      MakeGCP(15.0, 15.0, 5000.0, 5000.0),
  };

  void* hTransform = GDALCreateGCPRefineTransformer(
      static_cast<int>(gcps.size()), gcps.data(), 2, FALSE,
      /*tolerance=*/5.0, /*minimumGcps=*/6);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // Forward transform pins upstream worst_outlier() behavior where
  // high-leverage corner GCPs can be selected over interior outliers.
  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(661.1111, x, 1e-3);
  EXPECT_NEAR(746.6667, y, 1e-3);

  // 2. Refine transformer with minimumGcps = -1 computes default minimumGcps:
  // For order 1: ((1+1)*(1+2))/2 + 1 = 4.
  std::vector<GDAL_GCP> gcps_affine = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
      MakeGCP(10.0, 10.0, 125.0, 235.0),
      // Outlier.
      MakeGCP(20.0, 20.0, 9999.0, 9999.0),
  };
  void* hTransformDefaultMin = GDALCreateGCPRefineTransformer(
      static_cast<int>(gcps_affine.size()), gcps_affine.data(), 1, FALSE,
      /*tolerance=*/5.0, /*minimumGcps=*/-1);
  ASSERT_NE(nullptr, hTransformDefaultMin);
  absl::Cleanup cleanup_def = [hTransformDefaultMin] {
    GDALDestroyGCPTransformer(hTransformDefaultMin);
  };

  x = 5.0;
  y = 5.0;
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGCPTransform(hTransformDefaultMin, FALSE, 1, &x, &y,
                                   nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1206.8333, x, 1e-3);
  EXPECT_NEAR(1298.5, y, 1e-3);

  // 3. Refine transformer with no outliers (tolerance very large) exits loop
  // early.
  void* hTransformNoOutliers = GDALCreateGCPRefineTransformer(
      static_cast<int>(gcps.size()) - 1, gcps.data(), 2, FALSE,
      /*tolerance=*/10000.0, /*minimumGcps=*/6);
  ASSERT_NE(nullptr, hTransformNoOutliers);
  absl::Cleanup cleanup_no = [hTransformNoOutliers] {
    GDALDestroyGCPTransformer(hTransformNoOutliers);
  };

  // 4. Refine transformer where points are collinear after removal fails.
  std::vector<GDAL_GCP> gcps_collinear_refine = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),
      MakeGCP(10.0, 10.0, 110.0, 110.0),
      MakeGCP(20.0, 20.0, 120.0, 120.0),
      MakeGCP(5.0, 5.0, 5000.0, 5000.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateGCPRefineTransformer(
                           static_cast<int>(gcps_collinear_refine.size()),
                           gcps_collinear_refine.data(), 1, FALSE,
                           /*tolerance=*/5.0, /*minimumGcps=*/3));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Transform is not solvable"));
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, TransformPointsWithHugeVal) {
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // Points array with valid points, HUGE_VAL in x, and HUGE_VAL in y.
  std::vector<double> xs = {5.0, HUGE_VAL, 0.0};
  std::vector<double> ys = {5.0, 0.0, HUGE_VAL};
  std::vector<int> successes = {FALSE, FALSE, FALSE};

  EXPECT_EQ(FALSE, GDALGCPTransform(hTransform, FALSE, 3, xs.data(), ys.data(),
                                    nullptr, successes.data()));
  EXPECT_EQ(TRUE, successes[0]);
  EXPECT_EQ(FALSE, successes[1]);
  EXPECT_EQ(FALSE, successes[2]);
  EXPECT_NEAR(112.5, xs[0], 1e-5);
  EXPECT_NEAR(217.5, ys[0], 1e-5);
}

TEST_F(GdalCrsTest, SimilarTransformer) {
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  // 1. Ratio (1.0, 1.0) increments ref count and returns identical pointer.
  void* hSimilar1 = GDALCreateSimilarTransformer(hTransform, 1.0, 1.0);
  ASSERT_NE(nullptr, hSimilar1);
  EXPECT_EQ(hTransform, hSimilar1);
  // Destroying one reference leaves original valid.
  GDALDestroyGCPTransformer(hSimilar1);

  // Original transformer still transforms successfully.
  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);

  // 2. Scaled ratio (2.0, 3.0) creates a new distinct transformer instance
  // with pixel /= 2.0 and line /= 3.0.
  void* hScaled = GDALCreateSimilarTransformer(hTransform, 2.0, 3.0);
  ASSERT_NE(nullptr, hScaled);
  EXPECT_NE(hTransform, hScaled);
  absl::Cleanup cleanup_scaled = [hScaled] {
    GDALDestroyGCPTransformer(hScaled);
  };

  // Point (pixel/2.0, line/3.0) = (2.5, 1.66666667) transforms to the same
  // georeferenced coordinate (112.5, 217.5).
  x = 2.5;
  y = 5.0 / 3.0;
  success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hScaled, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-4);
  EXPECT_NEAR(217.5, y, 1e-4);
}

TEST_F(GdalCrsTest, SerializationAndDeserialization) {
  // 1. Standard Order 1 transformer serialization and deserialization.
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };
  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  CPLXMLTreeCloser tree(GDALSerializeGCPTransformer(hTransform));
  ASSERT_NE(nullptr, tree.get());

  EXPECT_STREQ("GCPTransformer", tree->pszValue);
  EXPECT_STREQ("1", CPLGetXMLValue(tree.get(), "Order", ""));
  EXPECT_STREQ("0", CPLGetXMLValue(tree.get(), "Reversed", ""));
  ASSERT_NE(nullptr, CPLGetXMLNode(tree.get(), "GCPList"));

  void* hDeserialized = GDALDeserializeGCPTransformer(tree.get());
  ASSERT_NE(nullptr, hDeserialized);
  absl::Cleanup cleanup_deser = [hDeserialized] {
    GDALDestroyGCPTransformer(hDeserialized);
  };

  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE, GDALGCPTransform(hDeserialized, FALSE, 1, &x, &y, nullptr,
                                   &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);

  // 2. Generic GDALSerializeTransformer & GDALDeserializeTransformer
  // round-trip.
  CPLXMLTreeCloser gen_tree(
      GDALSerializeTransformer(GDALGCPTransform, hTransform));
  ASSERT_NE(nullptr, gen_tree.get());

  GDALTransformerFunc pfnFunc = nullptr;
  void* hGenArg = nullptr;
  EXPECT_EQ(CE_None,
            GDALDeserializeTransformer(gen_tree.get(), &pfnFunc, &hGenArg));
  ASSERT_NE(nullptr, hGenArg);
  absl::Cleanup cleanup_gen_arg = [hGenArg] {
    GDALDestroyGCPTransformer(hGenArg);
  };
  EXPECT_EQ(GDALGCPTransform, pfnFunc);

  x = 5.0;
  y = 5.0;
  success = FALSE;
  EXPECT_EQ(TRUE, pfnFunc(hGenArg, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);

  // 3. Refine transformer serialization and deserialization.
  std::vector<GDAL_GCP> gcps7 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),     MakeGCP(10.0, 0.0, 130.0, 220.0),
      MakeGCP(20.0, 0.0, 180.0, 250.0),    MakeGCP(0.0, 10.0, 150.0, 240.0),
      MakeGCP(10.0, 10.0, 185.0, 270.0),   MakeGCP(0.0, 20.0, 240.0, 310.0),
      MakeGCP(15.0, 15.0, 5000.0, 5000.0),
  };
  void* hRefineTransform = GDALCreateGCPRefineTransformer(
      static_cast<int>(gcps7.size()), gcps7.data(), 2, TRUE,
      /*tolerance=*/5.0, /*minimumGcps=*/6);
  ASSERT_NE(nullptr, hRefineTransform);
  absl::Cleanup cleanup_refine = [hRefineTransform] {
    GDALDestroyGCPTransformer(hRefineTransform);
  };

  CPLXMLNode* psRefineTree = GDALSerializeGCPTransformer(hRefineTransform);
  ASSERT_NE(nullptr, psRefineTree);
  absl::Cleanup cleanup_refine_tree = [psRefineTree] {
    CPLDestroyXMLNode(psRefineTree);
  };

  EXPECT_STREQ("1", CPLGetXMLValue(psRefineTree, "Refine", ""));
  EXPECT_STREQ("1", CPLGetXMLValue(psRefineTree, "Reversed", ""));
  EXPECT_STREQ("6", CPLGetXMLValue(psRefineTree, "MinimumGcps", ""));
  EXPECT_STREQ("5.000000", CPLGetXMLValue(psRefineTree, "Tolerance", ""));

  void* hRefineDeser = GDALDeserializeGCPTransformer(psRefineTree);
  ASSERT_NE(nullptr, hRefineDeser);
  absl::Cleanup cleanup_refine_deser = [hRefineDeser] {
    GDALDestroyGCPTransformer(hRefineDeser);
  };

  // 4. Deserializing XML with missing GCPList returns null.
  CPLXMLNode* psEmptyTree =
      CPLCreateXMLNode(nullptr, CXT_Element, "GCPTransformer");
  absl::Cleanup cleanup_empty_tree = [psEmptyTree] {
    CPLDestroyXMLNode(psEmptyTree);
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeGCPTransformer(psEmptyTree));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 5. Deserializing null tree returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeGCPTransformer(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalCrsTest, GcpTransformerSerializeRoundTrip) {
  const std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 120.0, 205.0),
      MakeGCP(0.0, 10.0, 105.0, 230.0),
  };

  void* hTransform = GDALCreateGCPTransformer(static_cast<int>(gcps.size()),
                                              gcps.data(), 1, FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyGCPTransformer(hTransform);
  };

  double x = 5.0;
  double y = 5.0;
  int success = FALSE;
  EXPECT_EQ(TRUE,
            GDALGCPTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);

  CPLXMLTreeCloser tree(GDALSerializeGCPTransformer(hTransform));
  ASSERT_NE(nullptr, tree.get());

  void* hDeserialized = GDALDeserializeGCPTransformer(tree.get());
  ASSERT_NE(nullptr, hDeserialized);
  absl::Cleanup cleanup_deser = [hDeserialized] {
    GDALDestroyGCPTransformer(hDeserialized);
  };

  x = 5.0;
  y = 5.0;
  success = FALSE;
  EXPECT_EQ(TRUE, GDALGCPTransform(hDeserialized, FALSE, 1, &x, &y, nullptr,
                                   &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(112.5, x, 1e-5);
  EXPECT_NEAR(217.5, y, 1e-5);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Higher-order polynomial refinement edge cases.
