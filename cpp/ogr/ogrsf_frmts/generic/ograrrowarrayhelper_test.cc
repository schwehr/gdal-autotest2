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

// Tests for OGRArrowArrayHelper.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ograrrowarrayhelper.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ograrrowarrayhelper.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ograrrowarrayhelper.h"

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_recordbatch.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(OGRArrowArrayHelperTest, StaticMethodsAndLimits) {
  uint32_t mem_limit = OGRArrowArrayHelper::GetMemLimit();
  EXPECT_GT(mem_limit, 0);

  {
    CPLConfigOptionSetter mem_limit_setter("OGR_ARROW_MEM_LIMIT", "50000",
                                           false);
    EXPECT_EQ(50000, OGRArrowArrayHelper::GetMemLimit());
  }

  CPLStringList options;
  EXPECT_EQ(65536, OGRArrowArrayHelper::GetMaxFeaturesInBatch(options));

  options.SetNameValue("MAX_FEATURES_IN_BATCH", "128");
  EXPECT_EQ(128, OGRArrowArrayHelper::GetMaxFeaturesInBatch(options));

  options.SetNameValue("MAX_FEATURES_IN_BATCH", "0");
  EXPECT_EQ(1, OGRArrowArrayHelper::GetMaxFeaturesInBatch(options));

  options.SetNameValue("MAX_FEATURES_IN_BATCH", "2147483647");
  EXPECT_EQ(INT_MAX - 1, OGRArrowArrayHelper::GetMaxFeaturesInBatch(options));
}

TEST(OGRArrowArrayHelperTest, FillDictEdgeCases) {
  struct ArrowArray dict_array;
  memset(&dict_array, 0, sizeof(dict_array));

  // Failure with non-integer code
  std::vector<OGRCodedValue> non_int_codes;
  OGRCodedValue cv1;
  cv1.pszCode = CPLStrdup("not_an_int");
  cv1.pszValue = CPLStrdup("val");
  non_int_codes.push_back(cv1);

  OGRCodedFieldDomain non_int_domain("non_int", "desc", OFTInteger, OFSTNone,
                                     std::move(non_int_codes));
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(OGRArrowArrayHelper::FillDict(&dict_array, &non_int_domain));
  }

  // Failure with gap > 100
  std::vector<OGRCodedValue> gap_codes;
  OGRCodedValue cv_gap1;
  cv_gap1.pszCode = CPLStrdup("0");
  cv_gap1.pszValue = CPLStrdup("zero");
  gap_codes.push_back(cv_gap1);
  OGRCodedValue cv_gap2;
  cv_gap2.pszCode = CPLStrdup("200");
  cv_gap2.pszValue = CPLStrdup("two_hundred");
  gap_codes.push_back(cv_gap2);

  OGRCodedFieldDomain gap_domain("gap", "desc", OFTInteger, OFSTNone,
                                 std::move(gap_codes));
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(OGRArrowArrayHelper::FillDict(&dict_array, &gap_domain));
  }

  // Failure with descending codes (nCode <= nLastCode)
  std::vector<OGRCodedValue> desc_codes;
  OGRCodedValue cv_d1;
  cv_d1.pszCode = CPLStrdup("5");
  cv_d1.pszValue = CPLStrdup("five");
  desc_codes.push_back(cv_d1);
  OGRCodedValue cv_d2;
  cv_d2.pszCode = CPLStrdup("3");
  cv_d2.pszValue = CPLStrdup("three");
  desc_codes.push_back(cv_d2);
  OGRCodedFieldDomain desc_domain("desc_codes", "desc", OFTInteger, OFSTNone,
                                  std::move(desc_codes));
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(OGRArrowArrayHelper::FillDict(&dict_array, &desc_domain));
  }

  // Failure with empty string value (nCountChars == 0)
  std::vector<OGRCodedValue> empty_val_codes;
  OGRCodedValue cv_empty;
  cv_empty.pszCode = CPLStrdup("0");
  cv_empty.pszValue = CPLStrdup("");
  empty_val_codes.push_back(cv_empty);
  OGRCodedFieldDomain empty_val_domain("empty_val", "desc", OFTInteger,
                                       OFSTNone, std::move(empty_val_codes));
  {
    WithQuietHandler quiet_handler;
    EXPECT_FALSE(OGRArrowArrayHelper::FillDict(&dict_array, &empty_val_domain));
  }
}

TEST(OGRArrowArrayHelperTest, FullHelperLifecycle) {
  auto ds = std::make_unique<MEMDataset>();

  // Add coded domain
  std::vector<OGRCodedValue> coded_values;
  OGRCodedValue v0;
  v0.pszCode = CPLStrdup("0");
  v0.pszValue = CPLStrdup("zero");
  coded_values.push_back(v0);
  OGRCodedValue v2;
  v2.pszCode = CPLStrdup("2");
  v2.pszValue = CPLStrdup("two");
  coded_values.push_back(v2);
  OGRCodedValue v3;
  v3.pszCode = CPLStrdup("3");
  v3.pszValue = nullptr;  // null value
  coded_values.push_back(v3);

  std::string failureReason;
  auto domain = std::make_unique<OGRCodedFieldDomain>(
      "valid_dom", "desc", OFTInteger, OFSTNone, std::move(coded_values));
  ds->AddFieldDomain(std::move(domain), failureReason);

  OGRField sMin;
  sMin.Integer = 0;
  OGRField sMax;
  sMax.Integer = 100;
  auto range_domain = std::make_unique<OGRRangeFieldDomain>(
      "range_dom", "desc", OFTInteger, OFSTNone, sMin, true, sMax, true);
  ds->AddFieldDomain(std::move(range_domain), failureReason);

  auto defn = std::make_unique<OGRFeatureDefn>("test_defn");

  OGRFieldDefn f_int("f_int", OFTInteger);
  defn->AddFieldDefn(&f_int);

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&f_bool);

  OGRFieldDefn f_int16("f_int16", OFTInteger);
  f_int16.SetSubType(OFSTInt16);
  defn->AddFieldDefn(&f_int16);

  OGRFieldDefn f_dom("f_dom", OFTInteger);
  f_dom.SetDomainName("valid_dom");
  defn->AddFieldDefn(&f_dom);

  OGRFieldDefn f_range_dom("f_range_dom", OFTInteger);
  f_range_dom.SetDomainName("range_dom");
  defn->AddFieldDefn(&f_range_dom);

  OGRFieldDefn f_missing_dom("f_missing_dom", OFTInteger);
  f_missing_dom.SetDomainName("missing_dom");
  defn->AddFieldDefn(&f_missing_dom);

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  defn->AddFieldDefn(&f_int64);

  OGRFieldDefn f_real("f_real", OFTReal);
  defn->AddFieldDefn(&f_real);

  OGRFieldDefn f_float("f_float", OFTReal);
  f_float.SetSubType(OFSTFloat32);
  defn->AddFieldDefn(&f_float);

  OGRFieldDefn f_str("f_str", OFTString);
  defn->AddFieldDefn(&f_str);

  OGRFieldDefn f_bin("f_bin", OFTBinary);
  defn->AddFieldDefn(&f_bin);

  OGRFieldDefn f_date("f_date", OFTDate);
  defn->AddFieldDefn(&f_date);

  OGRFieldDefn f_dt("f_dt", OFTDateTime);
  defn->AddFieldDefn(&f_dt);

  OGRFieldDefn f_time("f_time", OFTTime);
  defn->AddFieldDefn(&f_time);

  OGRFieldDefn f_intlist("f_intlist", OFTIntegerList);
  defn->AddFieldDefn(&f_intlist);

  OGRFieldDefn f_ignored("f_ignored", OFTInteger);
  f_ignored.SetIgnored(TRUE);
  defn->AddFieldDefn(&f_ignored);

  OGRGeomFieldDefn g_fld("geom", wkbPoint);
  defn->AddGeomFieldDefn(&g_fld);

  OGRGeomFieldDefn g_ignored("geom_ignored", wkbPoint);
  g_ignored.SetIgnored(TRUE);
  defn->AddGeomFieldDefn(&g_ignored);

  CPLStringList stream_options;
  stream_options.SetNameValue("INCLUDE_FID", "YES");
  stream_options.SetNameValue("MAX_FEATURES_IN_BATCH", "4");
  stream_options.SetNameValue("TIMEZONE", "");

  struct ArrowArray out_array;
  memset(&out_array, 0, sizeof(out_array));

  {
    OGRArrowArrayHelper helper(ds.get(), defn.get(), stream_options,
                               &out_array);
    EXPECT_TRUE(helper.m_bIncludeFID);
    EXPECT_EQ(4, helper.m_nMaxBatchSize);

    // Call inline setters
    auto* int_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_int")]];
    OGRArrowArrayHelper::SetInt32(int_arr, 0, 42);

    auto* bool_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_bool")]];
    OGRArrowArrayHelper::SetBoolOn(bool_arr, 0);

    auto* int16_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_int16")]];
    OGRArrowArrayHelper::SetInt16(int16_arr, 0, 16);

    auto* int64_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_int64")]];
    OGRArrowArrayHelper::SetInt64(int64_arr, 0, 1234567890123LL);

    auto* real_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_real")]];
    OGRArrowArrayHelper::SetDouble(real_arr, 0, 3.14159);

    auto* float_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_float")]];
    OGRArrowArrayHelper::SetFloat(float_arr, 0, 2.5f);

    struct tm brokenDown;
    memset(&brokenDown, 0, sizeof(brokenDown));
    OGRField sDateField;
    sDateField.Date.Year = 2026;
    sDateField.Date.Month = 1;
    sDateField.Date.Day = 15;
    auto* date_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_date")]];
    OGRArrowArrayHelper::SetDate(date_arr, 0, brokenDown, sDateField);

    OGRField sDTField;
    sDTField.Date.Year = 2026;
    sDTField.Date.Month = 1;
    sDTField.Date.Day = 15;
    sDTField.Date.Hour = 10;
    sDTField.Date.Minute = 30;
    sDTField.Date.Second = 0;
    sDTField.Date.TZFlag = OGR_TZFLAG_UTC + 4;
    auto* dt_arr =
        out_array.children[helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex(
            "f_dt")]];
    OGRArrowArrayHelper::SetDateTime(dt_arr, 0, brokenDown, OGR_TZFLAG_MIXED_TZ,
                                     sDTField);

    int str_arrow_idx =
        helper.m_mapOGRFieldToArrowField[defn->GetFieldIndex("f_str")];
    // Reallocate string buffer with large string length
    GByte* ptr = helper.GetPtrForStringOrBinary(str_arrow_idx, 0, 1000);
    ASSERT_NE(nullptr, ptr);
    memset(ptr, 'A', 1000);

    auto* str_arr = out_array.children[str_arrow_idx];
    OGRArrowArrayHelper::SetEmptyStringOrBinary(str_arr, 1);

    // SetNull on uninitialized null buffer, then on initialized
    EXPECT_TRUE(helper.SetNull(str_arrow_idx, 2));
    EXPECT_TRUE(helper.SetNull(str_arrow_idx, 3));

    // Call remaining inline setters
    OGRArrowArrayHelper::SetInt8(int_arr, 0, 8);
    OGRArrowArrayHelper::SetUInt8(int_arr, 0, 8);
    OGRArrowArrayHelper::SetUInt16(int_arr, 0, 16);
    OGRArrowArrayHelper::SetUInt32(int_arr, 0, 32);
    OGRArrowArrayHelper::SetUInt64(int_arr, 0, 64);

    helper.Shrink(2);
    EXPECT_EQ(2, out_array.length);

    helper.ClearArray();
  }

  // Second helper with DATETIME_AS_STRING=YES, TIMEZONE=unknown, and
  // INCLUDE_FID=NO
  CPLStringList stream_options2;
  stream_options2.SetNameValue("INCLUDE_FID", "NO");
  stream_options2.SetNameValue("DATETIME_AS_STRING", "YES");
  stream_options2.SetNameValue("TIMEZONE", "unknown");
  stream_options2.SetNameValue("MAX_FEATURES_IN_BATCH", "2");

  struct ArrowArray out_array2;
  memset(&out_array2, 0, sizeof(out_array2));
  {
    OGRArrowArrayHelper helper2(ds.get(), defn.get(), stream_options2,
                                &out_array2);
    EXPECT_FALSE(helper2.m_bIncludeFID);
    helper2.ClearArray();
  }
}

// TODO: b/335317901 - Cover remaining error branches in
// ograrrowarrayhelper.cpp.

}  // namespace
}  // namespace autotest2
