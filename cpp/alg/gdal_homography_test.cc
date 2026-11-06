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
// Tests homography transformer (alg/gdal_homography.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_homography.cpp

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

extern "C" {
CPLXMLNode* GDALSerializeHomographyTransformer(void* pTransformArg);
void* GDALDeserializeHomographyTransformer(CPLXMLNode* psTree);
}

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

GDAL_GCP MakeGCP(double pixel, double line, double x, double y) {
  GDAL_GCP gcp;
  gcp.pszId = const_cast<char*>("");
  gcp.pszInfo = const_cast<char*>("");
  gcp.dfGCPPixel = pixel;
  gcp.dfGCPLine = line;
  gcp.dfGCPX = x;
  gcp.dfGCPY = y;
  gcp.dfGCPZ = 0.0;
  return gcp;
}

class GdalHomographyTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalHomographyTest, NullPointerValidation) {
  // 1. Similar transformer with null argument returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Homography transform with null transformer argument returns 0.
  {
    double x = 0.0;
    double y = 0.0;
    int success = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, GDALHomographyTransform(nullptr, FALSE, 1, &x, &y, nullptr,
                                         &success));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GDALHomographyTransform"));
    CPLErrorReset();
  }

  // 3. Serialize with null transformer argument returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeHomographyTransformer(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALSerializeHomographyTransformer"));
    CPLErrorReset();
  }

  // 4. Destroy null pointer succeeds cleanly.
  GDALDestroyHomographyTransformer(nullptr);
}

TEST_F(GdalHomographyTest, CreateAndDestroyHomographyTransformer) {
  // 1. Valid homography matrix.
  double homography[9] = {10.0, 1.0, 0.0, 20.0, 0.0, 1.0, 1.0, 0.0, 0.0};
  void* hTransform = GDALCreateHomographyTransformer(homography);
  ASSERT_NE(nullptr, hTransform);
  GDALDestroyHomographyTransformer(hTransform);

  // 2. Singular (non-invertible) matrix fails creation.
  double singular[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateHomographyTransformer(singular));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALCreateHomographyTransformer() failed, because "
                          "GDALInvHomography() failed"));
    CPLErrorReset();
  }
}

TEST_F(GdalHomographyTest, SimilarTransformer) {
  std::array<double, 9> homography = {100.0, 2.0, 0.0, 200.0, 0.0,
                                      3.0,   1.0, 0.0, 0.0};
  void* hTransform = GDALCreateHomographyTransformer(homography.data());
  absl::Cleanup cleanup = [hTransform] {
    if (hTransform != nullptr) GDALDestroyHomographyTransformer(hTransform);
  };
  ASSERT_NE(nullptr, hTransform);

  // 1. Ratio (1.0, 1.0) increments reference count and returns same pointer.
  void* hSimilar = GDALCreateSimilarTransformer(hTransform, 1.0, 1.0);
  ASSERT_NE(nullptr, hSimilar);
  EXPECT_EQ(hTransform, hSimilar);

  // Destroying similar decrements ref count; original remains valid.
  GDALDestroyHomographyTransformer(hSimilar);

  double x = 10.0;
  double y = 20.0;
  int success = 0;
  EXPECT_EQ(TRUE, GDALHomographyTransform(hTransform, FALSE, 1, &x, &y, nullptr,
                                          &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_DOUBLE_EQ(120.0, x);
  EXPECT_DOUBLE_EQ(260.0, y);

  // 2. Scaled ratio (2.0, 3.0) creates a new transformed instance.
  void* hScaled = GDALCreateSimilarTransformer(hTransform, 2.0, 3.0);
  ASSERT_NE(nullptr, hScaled);
  EXPECT_NE(hTransform, hScaled);
  absl::Cleanup cleanup_scaled = [hScaled] {
    GDALDestroyHomographyTransformer(hScaled);
  };

  // With ratio 2.0 along X and 3.0 along Y, forward coefficients are divided by
  // ratios: x' = 100.0 + (2.0 / 2.0) * x = 100.0 + x y' = 200.0 + (3.0 / 3.0) *
  // y = 200.0 + y
  double sx = 10.0;
  double sy = 20.0;
  int s_success = 0;
  EXPECT_EQ(TRUE, GDALHomographyTransform(hScaled, FALSE, 1, &sx, &sy, nullptr,
                                          &s_success));
  EXPECT_EQ(TRUE, s_success);
  EXPECT_DOUBLE_EQ(110.0, sx);
  EXPECT_DOUBLE_EQ(220.0, sy);

  GDALDestroyHomographyTransformer(hTransform);
}

TEST_F(GdalHomographyTest, ApplyHomography) {
  // 1. Valid homography point transformation.
  double homography[9] = {100.0, 2.0, 0.5, 200.0, -0.5, 3.0, 1.0, 0.001, 0.002};
  double geo_x = 0.0;
  double geo_y = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(homography, 10.0, 20.0, &geo_x, &geo_y));

  double expected_w = 1.0 + 10.0 * 0.001 + 20.0 * 0.002;
  double expected_wx = 100.0 + 10.0 * 2.0 + 20.0 * 0.5;
  double expected_wy = 200.0 - 10.0 * 0.5 + 20.0 * 3.0;
  EXPECT_NEAR(expected_wx / expected_w, geo_x, 1e-9);
  EXPECT_NEAR(expected_wy / expected_w, geo_y, 1e-9);

  // 2. Denominator w near zero (|w| < 1e-15) returns FALSE.
  double homography_zero_w[9] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
  // At (pixel=0, line=10), w = 0.0 + 0.0*1.0 + 10.0*0.0 = 0.0.
  EXPECT_EQ(FALSE,
            GDALApplyHomography(homography_zero_w, 0.0, 10.0, &geo_x, &geo_y));
}

TEST_F(GdalHomographyTest, InvHomographySpecialCaseNoRotation) {
  // Special case: no rotation (padfHIn[2] == 0, padfHIn[4] == 0, padfHIn[7] ==
  // 0, padfHIn[8] == 0).
  double homography[9] = {50.0, 2.0, 0.0, 100.0, 0.0, -4.0, 1.0, 0.0, 0.0};
  double inv_homography[9] = {};
  EXPECT_EQ(TRUE, GDALInvHomography(homography, inv_homography));

  // Verify analytically computed inverse.
  EXPECT_DOUBLE_EQ(-25.0, inv_homography[0]);
  EXPECT_DOUBLE_EQ(0.5, inv_homography[1]);
  EXPECT_DOUBLE_EQ(0.0, inv_homography[2]);
  EXPECT_DOUBLE_EQ(25.0, inv_homography[3]);
  EXPECT_DOUBLE_EQ(0.0, inv_homography[4]);
  EXPECT_DOUBLE_EQ(-0.25, inv_homography[5]);
  EXPECT_DOUBLE_EQ(1.0, inv_homography[6]);
  EXPECT_DOUBLE_EQ(0.0, inv_homography[7]);
  EXPECT_DOUBLE_EQ(0.0, inv_homography[8]);

  // Round-trip transformation on point (10.0, 5.0).
  double gx = 0.0;
  double gy = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(homography, 10.0, 5.0, &gx, &gy));
  double px = 0.0;
  double py = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(inv_homography, gx, gy, &px, &py));
  EXPECT_NEAR(10.0, px, 1e-9);
  EXPECT_NEAR(5.0, py, 1e-9);
}

TEST_F(GdalHomographyTest, InvHomographyGeneralCase) {
  // 1. General invertible homography with shear and perspective components.
  double homography[9] = {10.0, 1.5, 0.5, 20.0, -0.3, 2.0, 1.0, 0.001, -0.002};
  double inv_homography[9] = {};
  EXPECT_EQ(TRUE, GDALInvHomography(homography, inv_homography));

  // Round-trip on point (30.0, 40.0).
  double gx = 0.0;
  double gy = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(homography, 30.0, 40.0, &gx, &gy));
  double px = 0.0;
  double py = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(inv_homography, gx, gy, &px, &py));
  EXPECT_NEAR(30.0, px, 1e-6);
  EXPECT_NEAR(40.0, py, 1e-6);

  // 2. Singular matrix in general case (rows 1 and 2 are linearly dependent).
  double singular[9] = {1.0, 2.0, 3.0, 2.0, 4.0, 6.0, 1.0, 0.01, 0.02};
  double out[9] = {};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALInvHomography(singular, out));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("null determinant"));
    CPLErrorReset();
  }
}

TEST_F(GdalHomographyTest, ComposeHomographies) {
  // H1: translation by (10, 20).
  double h1[9] = {10.0, 1.0, 0.0, 20.0, 0.0, 1.0, 1.0, 0.0, 0.0};
  // H2: scale by (2, 3).
  double h2[9] = {0.0, 2.0, 0.0, 0.0, 0.0, 3.0, 1.0, 0.0, 0.0};
  double h_out[9] = {};
  GDALComposeHomographies(h1, h2, h_out);

  // Applying H1 then H2 to point (5, 5): (5+10)*2 = 30, (5+20)*3 = 75.
  double x = 0.0;
  double y = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(h_out, 5.0, 5.0, &x, &y));
  EXPECT_DOUBLE_EQ(30.0, x);
  EXPECT_DOUBLE_EQ(75.0, y);

  // In-place composition: output array same as first or second argument.
  double h1_copy[9];
  std::memcpy(h1_copy, h1, sizeof(h1));
  GDALComposeHomographies(h1_copy, h2, h1_copy);
  for (int i = 0; i < 9; ++i) {
    EXPECT_DOUBLE_EQ(h_out[i], h1_copy[i]);
  }

  double h2_copy[9];
  std::memcpy(h2_copy, h2, sizeof(h2));
  GDALComposeHomographies(h1, h2_copy, h2_copy);
  for (int i = 0; i < 9; ++i) {
    EXPECT_DOUBLE_EQ(h_out[i], h2_copy[i]);
  }
}

TEST_F(GdalHomographyTest, GCPsToHomographyLessThanFour) {
  double homography[9] = {};

  // 1. 0 GCPs -> sets [6]=1, [7]=0, [8]=0, delegates to GDALGCPsToGeoTransform,
  // returns FALSE.
  EXPECT_EQ(FALSE, GDALGCPsToHomography(0, nullptr, homography));
  EXPECT_DOUBLE_EQ(1.0, homography[6]);
  EXPECT_DOUBLE_EQ(0.0, homography[7]);
  EXPECT_DOUBLE_EQ(0.0, homography[8]);

  // 2. 1 GCP -> returns FALSE.
  GDAL_GCP gcp1 = MakeGCP(0.0, 0.0, 10.0, 20.0);
  EXPECT_EQ(FALSE, GDALGCPsToHomography(1, &gcp1, homography));

  // 3. 2 GCPs -> returns TRUE (affine fit from GDALGCPsToGeoTransform).
  std::vector<GDAL_GCP> gcps2 = {
      MakeGCP(0.0, 0.0, 10.0, 20.0),
      MakeGCP(1.0, 1.0, 30.0, 40.0),
  };
  EXPECT_EQ(TRUE, GDALGCPsToHomography(2, gcps2.data(), homography));

  // 4. 3 GCPs non-collinear -> returns TRUE (affine fit).
  std::vector<GDAL_GCP> gcps3 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(1.0, 0.0, 110.0, 200.0),
      MakeGCP(0.0, 1.0, 100.0, 220.0),
  };
  EXPECT_EQ(TRUE, GDALGCPsToHomography(3, gcps3.data(), homography));
  EXPECT_DOUBLE_EQ(1.0, homography[6]);
  EXPECT_DOUBLE_EQ(0.0, homography[7]);
  EXPECT_DOUBLE_EQ(0.0, homography[8]);
  double gx = 0.0;
  double gy = 0.0;
  EXPECT_EQ(TRUE, GDALApplyHomography(homography, 2.0, 3.0, &gx, &gy));
  EXPECT_NEAR(120.0, gx, 1e-6);
  EXPECT_NEAR(260.0, gy, 1e-6);
}

TEST_F(GdalHomographyTest, GCPsToHomographyDegenerateDimensions) {
  double homography[9] = {};

  // 1. All GCPs have same pixel value.
  {
    std::vector<GDAL_GCP> gcps = {
        MakeGCP(5.0, 0.0, 10.0, 10.0),
        MakeGCP(5.0, 10.0, 20.0, 10.0),
        MakeGCP(5.0, 20.0, 20.0, 20.0),
        MakeGCP(5.0, 30.0, 10.0, 20.0),
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALGCPsToHomography(4, gcps.data(), homography));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GCPs degenerate in at least one dimension"));
    CPLErrorReset();
  }

  // 2. All GCPs have same line value.
  {
    std::vector<GDAL_GCP> gcps = {
        MakeGCP(0.0, 5.0, 10.0, 10.0),
        MakeGCP(10.0, 5.0, 20.0, 10.0),
        MakeGCP(20.0, 5.0, 20.0, 20.0),
        MakeGCP(30.0, 5.0, 10.0, 20.0),
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALGCPsToHomography(4, gcps.data(), homography));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GCPs degenerate in at least one dimension"));
    CPLErrorReset();
  }

  // 3. All GCPs have same geox value.
  {
    std::vector<GDAL_GCP> gcps = {
        MakeGCP(0.0, 0.0, 50.0, 10.0),
        MakeGCP(10.0, 0.0, 50.0, 20.0),
        MakeGCP(10.0, 10.0, 50.0, 30.0),
        MakeGCP(0.0, 10.0, 50.0, 40.0),
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALGCPsToHomography(4, gcps.data(), homography));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GCPs degenerate in at least one dimension"));
    CPLErrorReset();
  }

  // 4. All GCPs have same geoy value.
  {
    std::vector<GDAL_GCP> gcps = {
        MakeGCP(0.0, 0.0, 10.0, 50.0),
        MakeGCP(10.0, 0.0, 20.0, 50.0),
        MakeGCP(10.0, 10.0, 30.0, 50.0),
        MakeGCP(0.0, 10.0, 40.0, 50.0),
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALGCPsToHomography(4, gcps.data(), homography));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GCPs degenerate in at least one dimension"));
    CPLErrorReset();
  }
}

TEST_F(GdalHomographyTest, GCPsToHomographyNonConvexOrCrossed) {
  // Twisted/crossed polygon where cross12 * cross23 <= 0.0.
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 0.0, 0.0),
      MakeGCP(100.0, 0.0, 100.0, 100.0),  // Crossed corner
      MakeGCP(100.0, 100.0, 100.0, 0.0),  // Crossed corner
      MakeGCP(0.0, 100.0, 0.0, 100.0),
  };
  double homography[9] = {};
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(FALSE, GDALGCPsToHomography(4, gcps.data(), homography));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("cross12 * cross23 <= 0.0"));
  CPLErrorReset();
}

TEST_F(GdalHomographyTest, GCPsToHomographyValidFourPoints) {
  // Perspective mapping of 4 corners to a non-affine convex quadrilateral.
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),
      MakeGCP(100.0, 0.0, 220.0, 105.0),
      MakeGCP(100.0, 100.0, 210.0, 195.0),
      MakeGCP(0.0, 100.0, 105.0, 200.0),
  };
  double homography[9] = {};
  EXPECT_EQ(TRUE, GDALGCPsToHomography(4, gcps.data(), homography));

  for (const auto& gcp : gcps) {
    double x = 0.0;
    double y = 0.0;
    EXPECT_EQ(TRUE, GDALApplyHomography(homography, gcp.dfGCPPixel,
                                        gcp.dfGCPLine, &x, &y));
    EXPECT_NEAR(gcp.dfGCPX, x, 1e-3);
    EXPECT_NEAR(gcp.dfGCPY, y, 1e-3);
  }
}

TEST_F(GdalHomographyTest, GCPsToHomographyOverdetermined) {
  // 5 GCPs: 4 corners and center point.
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),     MakeGCP(100.0, 0.0, 200.0, 100.0),
      MakeGCP(100.0, 100.0, 200.0, 200.0), MakeGCP(0.0, 100.0, 100.0, 200.0),
      MakeGCP(50.0, 50.0, 150.0, 150.0),
  };
  double homography[9] = {};
  EXPECT_EQ(TRUE, GDALGCPsToHomography(5, gcps.data(), homography));

  for (const auto& gcp : gcps) {
    double x = 0.0;
    double y = 0.0;
    EXPECT_EQ(TRUE, GDALApplyHomography(homography, gcp.dfGCPPixel,
                                        gcp.dfGCPLine, &x, &y));
    EXPECT_NEAR(gcp.dfGCPX, x, 1e-3);
    EXPECT_NEAR(gcp.dfGCPY, y, 1e-3);
  }
}

TEST_F(GdalHomographyTest, CreateHomographyTransformerFromGCPs) {
  // 1. Valid GCPs produce non-null transformer.
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 100.0, 100.0),
      MakeGCP(100.0, 0.0, 200.0, 100.0),
      MakeGCP(100.0, 100.0, 200.0, 200.0),
      MakeGCP(0.0, 100.0, 100.0, 200.0),
  };
  void* hTransform = GDALCreateHomographyTransformerFromGCPs(4, gcps.data());
  ASSERT_NE(nullptr, hTransform);
  GDALDestroyHomographyTransformer(hTransform);

  // 2. Degenerate GCPs produce null transformer.
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALCreateHomographyTransformerFromGCPs(0, nullptr));
  CPLErrorReset();
}

TEST_F(GdalHomographyTest, HomographyTransformForwardAndReverse) {
  double homography[9] = {100.0, 2.0, 0.0, 200.0, 0.0, 3.0, 1.0, 0.0, 0.0};
  void* hTransform = GDALCreateHomographyTransformer(homography);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyHomographyTransformer(hTransform);
  };

  // 1. Forward transform on multiple points.
  std::vector<double> x = {0.0, 10.0, 50.0};
  std::vector<double> y = {0.0, 20.0, 30.0};
  std::vector<int> success(3, 0);

  EXPECT_EQ(TRUE, GDALHomographyTransform(hTransform, FALSE, 3, x.data(),
                                          y.data(), nullptr, success.data()));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);
  EXPECT_EQ(TRUE, success[2]);
  EXPECT_DOUBLE_EQ(100.0, x[0]);
  EXPECT_DOUBLE_EQ(200.0, y[0]);
  EXPECT_DOUBLE_EQ(120.0, x[1]);
  EXPECT_DOUBLE_EQ(260.0, y[1]);
  EXPECT_DOUBLE_EQ(200.0, x[2]);
  EXPECT_DOUBLE_EQ(290.0, y[2]);

  // 2. Reverse transform back to pixel/line.
  EXPECT_EQ(TRUE, GDALHomographyTransform(hTransform, TRUE, 3, x.data(),
                                          y.data(), nullptr, success.data()));
  EXPECT_EQ(TRUE, success[0]);
  EXPECT_EQ(TRUE, success[1]);
  EXPECT_EQ(TRUE, success[2]);
  EXPECT_NEAR(0.0, x[0], 1e-6);
  EXPECT_NEAR(0.0, y[0], 1e-6);
  EXPECT_NEAR(10.0, x[1], 1e-6);
  EXPECT_NEAR(20.0, y[1], 1e-6);
  EXPECT_NEAR(50.0, x[2], 1e-6);
  EXPECT_NEAR(30.0, y[2], 1e-6);
}

TEST_F(GdalHomographyTest, HomographyTransformSingularPoint) {
  // An invertible projective homography where a specific input point maps to
  // w=0. det = 1.0 - (-0.1) = 1.1 != 0, so transformer creation succeeds.
  double homography[9] = {10.0, 1.0, 0.0, 20.0, 0.0, 1.0, 1.0, -0.01, 0.0};
  void* hTransform = GDALCreateHomographyTransformer(homography);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyHomographyTransformer(hTransform);
  };

  // Point 0 at (100.0, 0.0): w = 1.0 + 100.0 * (-0.01) = 0.0 < 1e-15 ->
  // failure. Point 1 at (0.0, 0.0): w = 1.0 -> success.
  std::vector<double> x = {100.0, 0.0};
  std::vector<double> y = {0.0, 0.0};
  std::vector<int> success(2, 0);

  EXPECT_EQ(FALSE, GDALHomographyTransform(hTransform, FALSE, 2, x.data(),
                                           y.data(), nullptr, success.data()));
  EXPECT_EQ(FALSE, success[0]);
  EXPECT_EQ(TRUE, success[1]);
  EXPECT_DOUBLE_EQ(10.0, x[1]);
  EXPECT_DOUBLE_EQ(20.0, y[1]);
}

TEST_F(GdalHomographyTest, SerializationAndDeserialization) {
  double homography[9] = {10.0, 1.5, 0.5, 20.0, -0.3, 2.0, 1.0, 0.001, -0.002};
  void* hTransform = GDALCreateHomographyTransformer(homography);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyHomographyTransformer(hTransform);
  };

  // 1. Direct serialization and deserialization.
  CPLXMLTreeCloser tree(GDALSerializeHomographyTransformer(hTransform));
  ASSERT_NE(nullptr, tree.get());

  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(tree.get()));
  ASSERT_NE(nullptr, xml_str);
  EXPECT_THAT(xml_str.get(), HasSubstr("<HomographyTransformer>"));
  EXPECT_THAT(xml_str.get(), HasSubstr("<Homography>"));

  void* hRestored = GDALDeserializeHomographyTransformer(tree.get());
  ASSERT_NE(nullptr, hRestored);
  absl::Cleanup cleanup_restored = [hRestored] {
    GDALDestroyHomographyTransformer(hRestored);
  };

  double orig_x = 15.0;
  double orig_y = 25.0;
  int orig_success = 0;
  EXPECT_EQ(TRUE, GDALHomographyTransform(hTransform, FALSE, 1, &orig_x,
                                          &orig_y, nullptr, &orig_success));

  double rest_x = 15.0;
  double rest_y = 25.0;
  int rest_success = 0;
  EXPECT_EQ(TRUE, GDALHomographyTransform(hRestored, FALSE, 1, &rest_x, &rest_y,
                                          nullptr, &rest_success));

  EXPECT_DOUBLE_EQ(orig_x, rest_x);
  EXPECT_DOUBLE_EQ(orig_y, rest_y);

  // 2. Deserialize tree lacking <Homography> element returns null.
  CPLXMLTreeCloser empty_tree(
      CPLCreateXMLNode(nullptr, CXT_Element, "InvalidRoot"));
  ASSERT_NE(nullptr, empty_tree.get());
  EXPECT_EQ(nullptr, GDALDeserializeHomographyTransformer(empty_tree.get()));

  // 3. Generic GDALSerializeTransformer / GDALDeserializeTransformer.
  CPLXMLTreeCloser generic_tree(
      GDALSerializeTransformer(GDALHomographyTransform, hTransform));
  ASSERT_NE(nullptr, generic_tree.get());

  void* hGenericRestored = nullptr;
  GDALTransformerFunc pfnGeneric = nullptr;
  EXPECT_EQ(CE_None, GDALDeserializeTransformer(generic_tree.get(), &pfnGeneric,
                                                &hGenericRestored));
  ASSERT_NE(nullptr, hGenericRestored);
  absl::Cleanup cleanup_generic_restored = [hGenericRestored] {
    GDALDestroyHomographyTransformer(hGenericRestored);
  };
  EXPECT_EQ(GDALHomographyTransform, pfnGeneric);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Homography decomposition and affine degenerate cases.
