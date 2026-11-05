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

#include "third_party/gdal/ogr/ogrvrtgeometrytypes.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"

namespace autotest2 {
namespace {

TEST(OgrVrtGeometryTypesTest, GetGeometryTypeBasic) {
  int err = 0;
  EXPECT_EQ(wkbPoint, OGRVRTGetGeometryType("wkbPoint", &err));
  EXPECT_FALSE(err);

  EXPECT_EQ(wkbPoint, OGRVRTGetGeometryType("wkbPoint", nullptr));

  EXPECT_EQ(wkbUnknown, OGRVRTGetGeometryType("wkbInvalidType", &err));
  EXPECT_TRUE(err);

  EXPECT_EQ(wkbUnknown, OGRVRTGetGeometryType("wkbInvalidType", nullptr));
}

TEST(OgrVrtGeometryTypesTest, GetGeometryTypeAllTypes) {
  const struct {
    const char* name;
    OGRwkbGeometryType expected;
  } kTestCases[] = {
      {"wkbUnknown", wkbUnknown},
      {"wkbPoint", wkbPoint},
      {"wkbLineString", wkbLineString},
      {"wkbPolygon", wkbPolygon},
      {"wkbMultiPoint", wkbMultiPoint},
      {"wkbMultiLineString", wkbMultiLineString},
      {"wkbMultiPolygon", wkbMultiPolygon},
      {"wkbGeometryCollection", wkbGeometryCollection},
      {"wkbCircularString", wkbCircularString},
      {"wkbCompoundCurve", wkbCompoundCurve},
      {"wkbCurvePolygon", wkbCurvePolygon},
      {"wkbMultiCurve", wkbMultiCurve},
      {"wkbMultiSurface", wkbMultiSurface},
      {"wkbCurve", wkbCurve},
      {"wkbSurface", wkbSurface},
      {"wkbPolyhedralSurface", wkbPolyhedralSurface},
      {"wkbTIN", wkbTIN},
      {"wkbTriangle", wkbTriangle},
      {"wkbNone", wkbNone},
      {"wkbLinearRing", wkbLinearRing},
  };

  for (const auto& tc : kTestCases) {
    int err = 1;
    EXPECT_EQ(tc.expected, OGRVRTGetGeometryType(tc.name, &err)) << tc.name;
    EXPECT_FALSE(err) << tc.name;
  }
}

TEST(OgrVrtGeometryTypesTest, GetGeometryTypeModifiers) {
  int err = 1;
  EXPECT_EQ(wkbPoint25D, OGRVRTGetGeometryType("wkbPoint25D", &err));
  EXPECT_FALSE(err);

  EXPECT_EQ(wkbPoint25D, OGRVRTGetGeometryType("wkbPointZ", &err));
  EXPECT_FALSE(err);

  EXPECT_EQ(wkbPointM, OGRVRTGetGeometryType("wkbPointM", &err));
  EXPECT_FALSE(err);

  EXPECT_EQ(wkbPointZM, OGRVRTGetGeometryType("wkbPointZM", &err));
  EXPECT_FALSE(err);

  // String ending with 'M' at length - 2 (e.g. "wkbPointMZ")
  EXPECT_EQ(wkbPointZM, OGRVRTGetGeometryType("wkbPointMZ", &err));
  EXPECT_FALSE(err);
}

TEST(OgrVrtGeometryTypesTest, GetSerializedGeometryType) {
  EXPECT_EQ("wkbPoint", OGRVRTGetSerializedGeometryType(wkbPoint));
  EXPECT_EQ("wkbPoint25D", OGRVRTGetSerializedGeometryType(wkbPoint25D));
  EXPECT_EQ("wkbPointM", OGRVRTGetSerializedGeometryType(wkbPointM));
  EXPECT_EQ("wkbPointZM", OGRVRTGetSerializedGeometryType(wkbPointZM));

  EXPECT_EQ("wkbCircularString",
            OGRVRTGetSerializedGeometryType(wkbCircularString));
  EXPECT_EQ("wkbCircularStringZ",
            OGRVRTGetSerializedGeometryType(wkbCircularStringZ));
  EXPECT_EQ("wkbCircularStringM",
            OGRVRTGetSerializedGeometryType(wkbCircularStringM));
  EXPECT_EQ("wkbCircularStringZM",
            OGRVRTGetSerializedGeometryType(wkbCircularStringZM));

  EXPECT_EQ("", OGRVRTGetSerializedGeometryType(
                    static_cast<OGRwkbGeometryType>(99999)));
}

}  // namespace
}  // namespace autotest2
