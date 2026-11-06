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
// Tests gdal_tps (alg/gdal_tps.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_tps.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/thinplatespline.h

#include <cmath>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/alg/thinplatespline.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

CPL_C_START
CPLXMLNode* GDALSerializeTPSTransformer(void* pTransformArg);
void* GDALDeserializeTPSTransformer(CPLXMLNode* psTree);
CPL_C_END

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

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

TEST(GdalTpsTest, NullPointerSafety) {
  // 1. Destroying a null transformer is a safe no-op.
  GDALDestroyTPSTransformer(nullptr);

  // 2. Serializing a null transformer returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALSerializeTPSTransformer(nullptr));
    CPLErrorReset();
  }

  // 3. Transforming with a null transformer returns 0 (failure).
  {
    double x = 0.0;
    double y = 0.0;
    int success = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0,
              GDALTPSTransform(nullptr, FALSE, 1, &x, &y, nullptr, &success));
    CPLErrorReset();
  }

  // 4. Creating similar transformer from null returns null.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateSimilarTransformer(nullptr, 1.0, 1.0));
    CPLErrorReset();
  }
}

TEST(GdalTpsTest, DegenerateGCPCounts) {
  // 1. 0 GCPs -> creation fails.
  EXPECT_EQ(nullptr, GDALCreateTPSTransformer(0, nullptr, FALSE));
  EXPECT_EQ(nullptr, GDALCreateTPSTransformerInt(0, nullptr, FALSE, nullptr));

  // 2. 1 GCP -> constant translation.
  GDAL_GCP gcp1 = MakeGCP(10.0, 20.0, 100.0, 200.0);
  void* hTransform1 = GDALCreateTPSTransformer(1, &gcp1, FALSE);
  ASSERT_NE(nullptr, hTransform1);
  absl::Cleanup cleanup1 = [hTransform1] {
    GDALDestroyTPSTransformer(hTransform1);
  };

  // Forward transform (pixel/line -> georef).
  double x = 10.0;
  double y = 20.0;
  int success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform1, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(100.0, x, 1e-5);
  EXPECT_NEAR(200.0, y, 1e-5);

  // Reverse transform (georef -> pixel/line).
  x = 100.0;
  y = 200.0;
  success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform1, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(10.0, x, 1e-5);
  EXPECT_NEAR(20.0, y, 1e-5);

  // 3. 2 GCPs -> 1D interpolation along segment.
  std::vector<GDAL_GCP> gcps2 = {
      MakeGCP(0.0, 0.0, 100.0, 200.0),
      MakeGCP(10.0, 0.0, 110.0, 200.0),
  };
  void* hTransform2 = GDALCreateTPSTransformer(2, gcps2.data(), FALSE);
  ASSERT_NE(nullptr, hTransform2);
  absl::Cleanup cleanup2 = [hTransform2] {
    GDALDestroyTPSTransformer(hTransform2);
  };

  x = 5.0;
  y = 0.0;
  success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform2, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(105.0, x, 1e-5);
  EXPECT_NEAR(200.0, y, 1e-5);

  x = 105.0;
  y = 200.0;
  success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform2, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(5.0, x, 1e-5);
  EXPECT_NEAR(0.0, y, 1e-5);

  // 4. Collinear GCPs (>= 3 collinear points -> 1D spline case).
  std::vector<GDAL_GCP> gcps_collinear = {
      MakeGCP(1.0, 1.0, 10.0, 10.0),
      MakeGCP(2.0, 2.0, 20.0, 20.0),
      MakeGCP(3.0, 3.0, 30.0, 30.0),
  };
  void* hTransformCollinear =
      GDALCreateTPSTransformer(3, gcps_collinear.data(), FALSE);
  ASSERT_NE(nullptr, hTransformCollinear);
  absl::Cleanup cleanup_collinear = [hTransformCollinear] {
    GDALDestroyTPSTransformer(hTransformCollinear);
  };

  x = 2.0;
  y = 2.0;
  success = 0;
  EXPECT_EQ(TRUE, GDALTPSTransform(hTransformCollinear, FALSE, 1, &x, &y,
                                   nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(20.0, x, 1e-5);
  EXPECT_NEAR(20.0, y, 1e-5);

  // 5. Degenerate collinear system where SSxx * SSyy == 0 (all identical X).
  std::vector<GDAL_GCP> gcps_degenerate = {
      MakeGCP(0.0, 0.0, 5.0, 10.0),
      MakeGCP(0.0, 1.0, 5.0, 20.0),
      MakeGCP(0.0, 2.0, 5.0, 30.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALCreateTPSTransformer(3, gcps_degenerate.data(), FALSE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Degenerate system"));
    CPLErrorReset();
  }
}

TEST(GdalTpsTest, GCPDuplicateHandling) {
  // 1. Exact duplicate GCPs (same pixel/line AND same X/Y) are silently
  // skipped.
  std::vector<GDAL_GCP> gcps_exact_dup = {
      MakeGCP(0.0, 0.0, 10.0, 20.0),
      MakeGCP(0.0, 0.0, 10.0, 20.0),  // duplicate, skipped
      MakeGCP(10.0, 0.0, 20.0, 20.0),
      MakeGCP(0.0, 10.0, 10.0, 30.0),
      MakeGCP(10.0, 10.0, 20.0, 30.0),
  };
  void* hTransformDup =
      GDALCreateTPSTransformer(5, gcps_exact_dup.data(), FALSE);
  ASSERT_NE(nullptr, hTransformDup);
  absl::Cleanup cleanup_dup = [hTransformDup] {
    GDALDestroyTPSTransformer(hTransformDup);
  };

  // 2. Same pixel/line but different X/Y fails to solve and returns nullptr.
  std::vector<GDAL_GCP> gcps_same_pl_diff_xy = {
      MakeGCP(0.0, 0.0, 10.0, 20.0),
      MakeGCP(0.0, 0.0, 99.0, 99.0),
      MakeGCP(10.0, 0.0, 20.0, 20.0),
      MakeGCP(0.0, 10.0, 10.0, 30.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* h = GDALCreateTPSTransformer(4, gcps_same_pl_diff_xy.data(), FALSE);
    EXPECT_EQ(nullptr, h);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    if (h) {
      GDALDestroyTPSTransformer(h);
    }
    CPLErrorReset();
  }

  // 3. Same X/Y but different pixel/line fails to solve and returns nullptr.
  std::vector<GDAL_GCP> gcps_same_xy_diff_pl = {
      MakeGCP(0.0, 0.0, 10.0, 20.0),
      MakeGCP(99.0, 99.0, 10.0, 20.0),
      MakeGCP(10.0, 0.0, 20.0, 20.0),
      MakeGCP(0.0, 10.0, 10.0, 30.0),
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    void* h = GDALCreateTPSTransformer(4, gcps_same_xy_diff_pl.data(), FALSE);
    EXPECT_EQ(nullptr, h);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    if (h) {
      GDALDestroyTPSTransformer(h);
    }
    CPLErrorReset();
  }
}

TEST(GdalTpsTest, Standard2DTransformForwardAndReverse) {
  // 4 non-collinear GCPs forming a rectangle:
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 1000.0, 2000.0),
      MakeGCP(100.0, 0.0, 1100.0, 2000.0),
      MakeGCP(0.0, 100.0, 1000.0, 2100.0),
      MakeGCP(100.0, 100.0, 1100.0, 2100.0),
  };

  void* hTransform = GDALCreateTPSTransformer(4, gcps.data(), FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyTPSTransformer(hTransform);
  };

  // Forward transform (bDstToSrc = FALSE): (50, 50) -> (1050, 2050).
  double x = 50.0;
  double y = 50.0;
  int success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1050.0, x, 1e-4);
  EXPECT_NEAR(2050.0, y, 1e-4);

  // Reverse transform (bDstToSrc = TRUE): (1050, 2050) -> (50, 50).
  x = 1050.0;
  y = 2050.0;
  success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(50.0, x, 1e-4);
  EXPECT_NEAR(50.0, y, 1e-4);
}

TEST(GdalTpsTest, ReversedTransformer) {
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 1000.0, 2000.0),
      MakeGCP(100.0, 0.0, 1100.0, 2000.0),
      MakeGCP(0.0, 100.0, 1000.0, 2100.0),
      MakeGCP(100.0, 100.0, 1100.0, 2100.0),
  };

  // When bReversed = TRUE, forward direction maps (georef) -> (pixel/line).
  void* hTransform = GDALCreateTPSTransformer(4, gcps.data(), TRUE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyTPSTransformer(hTransform);
  };

  // Forward transform with bReversed = TRUE: (1050, 2050) -> (50, 50).
  double x = 1050.0;
  double y = 2050.0;
  int success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(50.0, x, 1e-4);
  EXPECT_NEAR(50.0, y, 1e-4);

  // Reverse transform with bReversed = TRUE: (50, 50) -> (1050, 2050).
  x = 50.0;
  y = 50.0;
  success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1050.0, x, 1e-4);
  EXPECT_NEAR(2050.0, y, 1e-4);
}

TEST(GdalTpsTest, OptionsAndApproxError) {
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 1000.0, 2000.0),
      MakeGCP(100.0, 0.0, 1100.0, 2000.0),
      MakeGCP(0.0, 100.0, 1000.0, 2100.0),
      MakeGCP(100.0, 100.0, 1100.0, 2100.0),
  };

  CPLStringList options;
  options.SetNameValue("SRC_APPROX_ERROR_IN_PIXEL", "0.25");

  void* hTransform =
      GDALCreateTPSTransformerInt(4, gcps.data(), FALSE, options.List());
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyTPSTransformer(hTransform);
  };

  // Reverse transform uses SRC_APPROX_ERROR_IN_PIXEL in GDALGenericInverse2D.
  double x = 1050.0;
  double y = 2050.0;
  int success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, TRUE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(50.0, x, 1e-3);
  EXPECT_NEAR(50.0, y, 1e-3);
}

TEST(GdalTpsTest, MultithreadedSolvingPath) {
  // Create 105 GCPs on a 15x7 grid to exceed the 100 GCP threshold.
  std::vector<GDAL_GCP> gcps;
  gcps.reserve(105);
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 15; ++c) {
      gcps.push_back(
          MakeGCP(c * 10.0, r * 10.0, 100.0 + c * 20.0, 200.0 + r * 20.0));
    }
  }

  // 1. Explicit NUM_THREADS = 2 exercises CPLCreateJoinableThread path.
  {
    CPLStringList options;
    options.SetNameValue("NUM_THREADS", "2");

    void* hTransform = GDALCreateTPSTransformerInt(
        static_cast<int>(gcps.size()), gcps.data(), FALSE, options.List());
    ASSERT_NE(nullptr, hTransform);
    absl::Cleanup cleanup = [hTransform] {
      GDALDestroyTPSTransformer(hTransform);
    };

    double x = 50.0;
    double y = 30.0;
    int success = 0;
    EXPECT_EQ(TRUE, GDALTPSTransform(hTransform, FALSE, 1, &x, &y, nullptr,
                                     &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(200.0, x, 1e-4);
    EXPECT_NEAR(260.0, y, 1e-4);
  }

  // 2. NUM_THREADS = ALL_CPUS.
  {
    CPLStringList options;
    options.SetNameValue("NUM_THREADS", "ALL_CPUS");

    void* hTransform = GDALCreateTPSTransformerInt(
        static_cast<int>(gcps.size()), gcps.data(), FALSE, options.List());
    ASSERT_NE(nullptr, hTransform);
    absl::Cleanup cleanup = [hTransform] {
      GDALDestroyTPSTransformer(hTransform);
    };

    double x = 50.0;
    double y = 30.0;
    int success = 0;
    EXPECT_EQ(TRUE, GDALTPSTransform(hTransform, FALSE, 1, &x, &y, nullptr,
                                     &success));
    EXPECT_EQ(TRUE, success);
    EXPECT_NEAR(200.0, x, 1e-4);
    EXPECT_NEAR(260.0, y, 1e-4);
  }
}

TEST(GdalTpsTest, CreateSimilarTPSTransformer) {
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 1000.0, 2000.0),
      MakeGCP(100.0, 0.0, 1100.0, 2000.0),
      MakeGCP(0.0, 100.0, 1000.0, 2100.0),
      MakeGCP(100.0, 100.0, 1100.0, 2100.0),
  };

  void* hTransform = GDALCreateTPSTransformer(4, gcps.data(), FALSE);
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyTPSTransformer(hTransform);
  };

  // 1. Ratio (1.0, 1.0) -> increments ref count and returns same pointer.
  void* hSimilar = GDALCreateSimilarTransformer(hTransform, 1.0, 1.0);
  ASSERT_NE(nullptr, hSimilar);
  EXPECT_EQ(hTransform, hSimilar);

  // Destroying the similar transformer decrements the ref count back to 1.
  GDALDestroyTPSTransformer(hSimilar);

  // Original transformer remains valid and usable.
  double x = 50.0;
  double y = 50.0;
  int success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hTransform, FALSE, 1, &x, &y, nullptr, &success));
  EXPECT_EQ(TRUE, success);
  EXPECT_NEAR(1050.0, x, 1e-4);
  EXPECT_NEAR(2050.0, y, 1e-4);

  // 2. Ratio (2.0, 2.0) -> creates a new transformer with scaled GCP
  // pixel/line.
  void* hScaled = GDALCreateSimilarTransformer(hTransform, 2.0, 2.0);
  ASSERT_NE(nullptr, hScaled);
  EXPECT_NE(hTransform, hScaled);
  absl::Cleanup cleanup_scaled = [hScaled] {
    GDALDestroyTPSTransformer(hScaled);
  };

  // (25, 25) in scaled pixel/line corresponds to (50, 50) in original.
  double sx = 25.0;
  double sy = 25.0;
  int s_success = 0;
  EXPECT_EQ(TRUE,
            GDALTPSTransform(hScaled, FALSE, 1, &sx, &sy, nullptr, &s_success));
  EXPECT_EQ(TRUE, s_success);
  EXPECT_NEAR(1050.0, sx, 1e-4);
  EXPECT_NEAR(2050.0, sy, 1e-4);
}

TEST(GdalTpsTest, SerializationAndDeserialization) {
  std::vector<GDAL_GCP> gcps = {
      MakeGCP(0.0, 0.0, 1000.0, 2000.0),
      MakeGCP(100.0, 0.0, 1100.0, 2000.0),
      MakeGCP(0.0, 100.0, 1000.0, 2100.0),
      MakeGCP(100.0, 100.0, 1100.0, 2100.0),
  };

  CPLStringList options;
  options.SetNameValue("SRC_APPROX_ERROR_IN_PIXEL", "0.5");

  void* hTransform =
      GDALCreateTPSTransformerInt(4, gcps.data(), FALSE, options.List());
  ASSERT_NE(nullptr, hTransform);
  absl::Cleanup cleanup = [hTransform] {
    GDALDestroyTPSTransformer(hTransform);
  };

  // 1. Serialize transformer to XML.
  CPLXMLTreeCloser tree(GDALSerializeTPSTransformer(hTransform));
  ASSERT_NE(nullptr, tree.get());

  EXPECT_STREQ("TPSTransformer", tree->pszValue);
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "Reversed"));
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "GCPList"));
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "SrcApproxErrorInPixel"));

  // 2. Deserialize transformer from XML and verify round-trip equivalence.
  void* hDeserialized = GDALDeserializeTPSTransformer(tree.get());
  ASSERT_NE(nullptr, hDeserialized);
  absl::Cleanup des_cleanup = [hDeserialized] {
    GDALDestroyTPSTransformer(hDeserialized);
  };

  double x_orig = 50.0;
  double y_orig = 50.0;
  int succ_orig = 0;
  double x_des = 50.0;
  double y_des = 50.0;
  int succ_des = 0;

  EXPECT_EQ(TRUE, GDALTPSTransform(hTransform, FALSE, 1, &x_orig, &y_orig,
                                   nullptr, &succ_orig));
  EXPECT_EQ(TRUE, GDALTPSTransform(hDeserialized, FALSE, 1, &x_des, &y_des,
                                   nullptr, &succ_des));
  EXPECT_EQ(TRUE, succ_orig);
  EXPECT_EQ(TRUE, succ_des);
  EXPECT_DOUBLE_EQ(x_orig, x_des);
  EXPECT_DOUBLE_EQ(y_orig, y_des);

  // 3. Deserializing XML with missing/empty GCPList returns null.
  CPLXMLTreeCloser empty_tree(
      CPLCreateXMLNode(nullptr, CXT_Element, "TPSTransformer"));
  ASSERT_NE(nullptr, empty_tree.get());
  EXPECT_EQ(nullptr, GDALDeserializeTPSTransformer(empty_tree.get()));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Thin plate spline numerical conditioning and extreme
// point sets.
