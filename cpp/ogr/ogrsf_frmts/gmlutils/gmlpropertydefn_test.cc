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
// Tests for GMLPropertyDefn and GMLGeometryPropertyDefn
// (ogr/ogrsf_frmts/gmlutils/gmlpropertydefn.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/gmlpropertydefn.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py

#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"

namespace autotest2 {
namespace {

TEST(GmlPropertyDefnTest, BasicGettersAndSetters) {
  GMLPropertyDefn defn("prop_name", "src_elem");
  EXPECT_STREQ("prop_name", defn.GetName());
  EXPECT_STREQ("src_elem", defn.GetSrcElement());
  EXPECT_EQ(8, defn.GetSrcElementLen());
  EXPECT_EQ(GMLPT_Untyped, defn.GetType());
  EXPECT_EQ(OFSTNone, defn.GetSubType());
  EXPECT_EQ(0, defn.GetWidth());
  EXPECT_EQ(0, defn.GetPrecision());
  EXPECT_EQ(nullptr, defn.GetCondition());
  EXPECT_TRUE(defn.IsNullable());
  EXPECT_FALSE(defn.IsUnique());
  EXPECT_TRUE(defn.GetDocumentation().empty());

  defn.SetName("new_name");
  EXPECT_STREQ("new_name", defn.GetName());

  defn.SetType(GMLPT_Integer);
  EXPECT_EQ(GMLPT_Integer, defn.GetType());

  defn.SetSubType(OFSTBoolean);
  EXPECT_EQ(OFSTBoolean, defn.GetSubType());

  defn.SetWidth(10);
  EXPECT_EQ(10, defn.GetWidth());

  defn.SetPrecision(2);
  EXPECT_EQ(2, defn.GetPrecision());

  defn.SetSrcElement("new_src");
  EXPECT_STREQ("new_src", defn.GetSrcElement());
  EXPECT_EQ(7, defn.GetSrcElementLen());

  defn.SetSrcElement(nullptr);
  EXPECT_EQ(nullptr, defn.GetSrcElement());
  EXPECT_EQ(0, defn.GetSrcElementLen());

  defn.SetCondition("attr='val'");
  EXPECT_STREQ("attr='val'", defn.GetCondition());

  defn.SetCondition(nullptr);
  EXPECT_EQ(nullptr, defn.GetCondition());

  defn.SetNullable(false);
  EXPECT_FALSE(defn.IsNullable());

  defn.SetUnique(true);
  EXPECT_TRUE(defn.IsUnique());

  defn.SetDocumentation("Some docs");
  EXPECT_EQ("Some docs", defn.GetDocumentation());

  EXPECT_TRUE(GMLPropertyDefn::IsSimpleType(GMLPT_String));
  EXPECT_TRUE(GMLPropertyDefn::IsSimpleType(GMLPT_Integer));
  EXPECT_TRUE(GMLPropertyDefn::IsSimpleType(GMLPT_Real));
  EXPECT_FALSE(GMLPropertyDefn::IsSimpleType(GMLPT_Untyped));
  EXPECT_FALSE(GMLPropertyDefn::IsSimpleType(GMLPT_Complex));
}

void CheckAnalyseSingle(GMLPropertyDefn& defn, const char* val,
                        bool set_width = true) {
  char* subprops[1] = {const_cast<char*>(val)};
  GMLProperty prop;
  prop.nSubProperties = 1;
  prop.papszSubProperties = subprops;
  prop.aszSubProperties[0] = subprops[0];
  prop.aszSubProperties[1] = nullptr;
  defn.AnalysePropertyValue(&prop, set_width);
}

void CheckAnalyseMulti(GMLPropertyDefn& defn,
                       const std::vector<const char*>& vals,
                       bool set_width = true) {
  std::vector<char*> subprops;
  subprops.reserve(vals.size());
  for (const char* v : vals) {
    subprops.push_back(const_cast<char*>(v));
  }
  GMLProperty prop;
  prop.nSubProperties = static_cast<int>(subprops.size());
  prop.papszSubProperties = subprops.data();
  defn.AnalysePropertyValue(&prop, set_width);
}

TEST(GmlPropertyDefnTest, AnalysePropertyValueEmptyAndBoolean) {
  GMLPropertyDefn defn("prop");

  // Empty string does not change type.
  CheckAnalyseSingle(defn, "");
  EXPECT_EQ(GMLPT_Untyped, defn.GetType());

  // "true" promotes Untyped to Boolean.
  CheckAnalyseSingle(defn, "true");
  EXPECT_EQ(GMLPT_Boolean, defn.GetType());

  // "false" keeps Boolean.
  CheckAnalyseSingle(defn, "false");
  EXPECT_EQ(GMLPT_Boolean, defn.GetType());

  // Non-boolean string turns Boolean into String.
  CheckAnalyseSingle(defn, "not_bool");
  EXPECT_EQ(GMLPT_String, defn.GetType());
  EXPECT_EQ(8, defn.GetWidth());
}

TEST(GmlPropertyDefnTest, AnalysePropertyValueIntegerAndReal) {
  // 1. Integer to Integer64 to Real.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "123");
    EXPECT_EQ(GMLPT_Integer, defn.GetType());

    CheckAnalyseSingle(defn, "3000000000");  // > 2^31 - 1
    EXPECT_EQ(GMLPT_Integer64, defn.GetType());

    CheckAnalyseSingle(defn, "456");  // stays Integer64
    EXPECT_EQ(GMLPT_Integer64, defn.GetType());

    CheckAnalyseSingle(defn, "12.34");  // real
    EXPECT_EQ(GMLPT_Real, defn.GetType());
  }

  // 2. Direct Untyped to Integer64.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "3000000000");
    EXPECT_EQ(GMLPT_Integer64, defn.GetType());
  }

  // 3. Direct Untyped to Real.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "3.1415");
    EXPECT_EQ(GMLPT_Real, defn.GetType());
  }
}

TEST(GmlPropertyDefnTest, AnalysePropertyValueDateDateTimeAndTime) {
  // Date and DateTime.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03");
    EXPECT_EQ(GMLPT_Date, defn.GetType());

    CheckAnalyseSingle(defn, "2026-11-03T10:20:30Z");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());

    // Another Date keeps DateTime.
    CheckAnalyseSingle(defn, "2026-11-03");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());
  }

  // Various DateTime endings.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03T10:20:30");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());
  }
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03T10:20:30.123");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());
  }
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03T10:20:30+02:00");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());
  }
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03T10:20:30-05:00");
    EXPECT_EQ(GMLPT_DateTime, defn.GetType());
  }
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "2026-11-03T10:20:30X");  // invalid end
    EXPECT_EQ(GMLPT_String, defn.GetType());
  }

  // Time.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseSingle(defn, "12:34:56");
    EXPECT_EQ(GMLPT_Time, defn.GetType());

    CheckAnalyseSingle(defn, "12:34:56.789");
    EXPECT_EQ(GMLPT_Time, defn.GetType());

    CheckAnalyseSingle(defn, "12:34:56X");  // invalid
    EXPECT_EQ(GMLPT_String, defn.GetType());
  }
}

TEST(GmlPropertyDefnTest, AnalysePropertyValueStringWidth) {
  GMLPropertyDefn defn("prop");
  defn.SetType(GMLPT_String);

  CheckAnalyseSingle(defn, "hello", /*set_width=*/true);
  EXPECT_EQ(5, defn.GetWidth());

  CheckAnalyseSingle(defn, "hi", /*set_width=*/true);
  EXPECT_EQ(5, defn.GetWidth());  // stays 5

  CheckAnalyseSingle(defn, "supercalifragilistic", /*set_width=*/true);
  EXPECT_EQ(20, defn.GetWidth());

  CheckAnalyseSingle(defn, "even longer string than before",
                     /*set_width=*/false);
  EXPECT_EQ(20, defn.GetWidth());  // unchanged when set_width is false
}

TEST(GmlPropertyDefnTest, AnalysePropertyValueMultiLists) {
  // Integer -> IntegerList -> Integer64List -> RealList -> StringList.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseMulti(defn, {"10", "20"});
    EXPECT_EQ(GMLPT_IntegerList, defn.GetType());

    CheckAnalyseMulti(defn, {"10", "3000000000"});
    EXPECT_EQ(GMLPT_Integer64List, defn.GetType());

    CheckAnalyseMulti(defn, {"10", "2.5"});
    EXPECT_EQ(GMLPT_RealList, defn.GetType());

    CheckAnalyseMulti(defn, {"10", "abc"});
    EXPECT_EQ(GMLPT_StringList, defn.GetType());
  }

  // Direct Integer64 multi.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseMulti(defn, {"3000000000", "4000000000"});
    EXPECT_EQ(GMLPT_Integer64List, defn.GetType());
  }

  // Direct Real multi.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseMulti(defn, {"1.5", "2.5"});
    EXPECT_EQ(GMLPT_RealList, defn.GetType());
  }

  // Direct String multi.
  {
    GMLPropertyDefn defn("prop");
    defn.SetType(GMLPT_String);
    defn.SetWidth(10);
    CheckAnalyseMulti(defn, {"abc", "def"});
    EXPECT_EQ(GMLPT_StringList, defn.GetType());
    EXPECT_EQ(0, defn.GetWidth());
  }

  // Boolean multi.
  {
    GMLPropertyDefn defn("prop");
    CheckAnalyseMulti(defn, {"true", "false"});
    EXPECT_EQ(GMLPT_BooleanList, defn.GetType());

    // Non-boolean in BooleanList downgrades to StringList.
    CheckAnalyseMulti(defn, {"true", "not_a_bool"});
    EXPECT_EQ(GMLPT_StringList, defn.GetType());
  }
}

TEST(GmlPropertyDefnTest, GeometryPropertyDefnConstructorAndNameFallback) {
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;

  GMLGeometryPropertyDefn geom_defn("geom_name", "src_element", wkbPoint, 2,
                                    false, prec);
  EXPECT_STREQ("geom_name", geom_defn.GetName());
  EXPECT_STREQ("src_element", geom_defn.GetSrcElement());
  EXPECT_EQ(wkbPoint, geom_defn.GetType());
  EXPECT_EQ(2, geom_defn.GetAttributeIndex());
  EXPECT_FALSE(geom_defn.IsNullable());
  EXPECT_DOUBLE_EQ(0.001, geom_defn.GetCoordinatePrecision().dfXYResolution);

  geom_defn.SetType(wkbMultiPoint);
  EXPECT_EQ(wkbMultiPoint, geom_defn.GetType());

  // Empty name falls back to pszSrcElement.
  GMLGeometryPropertyDefn fallback_defn("", "fallback_elem", wkbPolygon, -1,
                                        true);
  EXPECT_STREQ("fallback_elem", fallback_defn.GetName());
  EXPECT_STREQ("fallback_elem", fallback_defn.GetSrcElement());

  // Null name falls back to pszSrcElement.
  GMLGeometryPropertyDefn null_defn(nullptr, "fallback_elem2", wkbPolygon, -1,
                                    true);
  EXPECT_STREQ("fallback_elem2", null_defn.GetName());
}

TEST(GmlPropertyDefnTest, GeometryPropertyDefnSRSNameMerge) {
  GMLGeometryPropertyDefn geom_defn("geom", "src", wkbUnknown, -1, true);
  EXPECT_TRUE(geom_defn.GetSRSName().empty());

  geom_defn.MergeSRSName("EPSG:4326");
  EXPECT_EQ("EPSG:4326", geom_defn.GetSRSName());

  geom_defn.MergeSRSName("EPSG:4326");
  EXPECT_EQ("EPSG:4326", geom_defn.GetSRSName());

  // Mismatch makes SRS name inconsistent and clears it.
  geom_defn.MergeSRSName("EPSG:3857");
  EXPECT_TRUE(geom_defn.GetSRSName().empty());

  // Subsequent merges are ignored once inconsistent.
  geom_defn.MergeSRSName("EPSG:4326");
  EXPECT_TRUE(geom_defn.GetSRSName().empty());

  // SetSRSName resets consistency.
  geom_defn.SetSRSName("EPSG:32631");
  EXPECT_EQ("EPSG:32631", geom_defn.GetSRSName());
  geom_defn.MergeSRSName("EPSG:32631");
  EXPECT_EQ("EPSG:32631", geom_defn.GetSRSName());
}

// TODO: b/335317901 - GMLPropertyDefn achieves 100% line coverage. Track any
// upstream additions to GMLPropertyDefn methods.

}  // namespace
}  // namespace autotest2
