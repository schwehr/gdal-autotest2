// Copyright 2018 Google Inc. All Rights Reserved.
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
// RAT == Raster Attribute Table

#include "third_party/gdal/gcore/gdal_rat.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"

extern "C" int json_object_put(struct json_object* obj);

namespace gdal {
namespace {

// Custom subclass of GDALRasterAttributeTable that delegates cell storage to
// an internal GDALDefaultRasterAttributeTable while allowing us to test
// base-class methods (ValuesIO, protected helpers, base fallbacks, etc.).
class CustomBaseRAT : public GDALRasterAttributeTable {
 public:
  CustomBaseRAT() = default;
  ~CustomBaseRAT() override = default;

  GDALRasterAttributeTable* Clone() const override { return inner_.Clone(); }
  int GetColumnCount() const override {
    return fake_col_count_ >= 0 ? fake_col_count_ : inner_.GetColumnCount();
  }
  const char* GetNameOfCol(int iCol) const override {
    return inner_.GetNameOfCol(iCol);
  }
  GDALRATFieldUsage GetUsageOfCol(int iCol) const override {
    return inner_.GetUsageOfCol(iCol);
  }
  GDALRATFieldType GetTypeOfCol(int iCol) const override {
    return inner_.GetTypeOfCol(iCol);
  }
  int GetColOfUsage(GDALRATFieldUsage eUsage) const override {
    return inner_.GetColOfUsage(eUsage);
  }
  int GetRowCount() const override {
    return fake_row_count_ >= 0 ? fake_row_count_ : inner_.GetRowCount();
  }
  const char* GetValueAsString(int iRow, int iField) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      return "";
    }
    return inner_.GetValueAsString(iRow, iField);
  }
  int GetValueAsInt(int iRow, int iField) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      return 0;
    }
    return inner_.GetValueAsInt(iRow, iField);
  }
  double GetValueAsDouble(int iRow, int iField) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      return 0.0;
    }
    return inner_.GetValueAsDouble(iRow, iField);
  }
  bool GetValueAsBoolean(int iRow, int iField) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      return false;
    }
    return inner_.GetValueAsBoolean(iRow, iField);
  }
  GDALRATDateTime GetValueAsDateTime(int iRow, int iField) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      return GDALRATDateTime();
    }
    return inner_.GetValueAsDateTime(iRow, iField);
  }
  const GByte* GetValueAsWKBGeometry(int iRow, int iField,
                                     size_t& nWKBSize) const override {
    if (emit_error_on_get_) {
      CPLError(CE_Failure, CPLE_AppDefined, "Forced get error");
      nWKBSize = 0;
      return nullptr;
    }
    return inner_.GetValueAsWKBGeometry(iRow, iField, nWKBSize);
  }
  CPLErr SetValue(int iRow, int iField, const char* pszValue) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, pszValue);
  }
  CPLErr SetValue(int iRow, int iField, int nValue) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, nValue);
  }
  CPLErr SetValue(int iRow, int iField, double dfValue) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, dfValue);
  }
  CPLErr SetValue(int iRow, int iField, bool bValue) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, bValue);
  }
  CPLErr SetValue(int iRow, int iField,
                  const GDALRATDateTime& sDateTime) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, sDateTime);
  }
  CPLErr SetValue(int iRow, int iField, const void* pabyWKB,
                  size_t nWKBSize) override {
    if (fail_set_) return CE_Failure;
    return inner_.SetValue(iRow, iField, pabyWKB, nWKBSize);
  }
  int ChangesAreWrittenToFile() override { return 0; }
  CPLErr SetTableType(const GDALRATTableType eInTableType) override {
    return inner_.SetTableType(eInTableType);
  }
  GDALRATTableType GetTableType() const override {
    return inner_.GetTableType();
  }
  void RemoveStatistics() override { inner_.RemoveStatistics(); }

  void SetRowCount(int iCount) override {
    GDALRasterAttributeTable::SetRowCount(iCount);
    if (!use_base_fallbacks_) {
      inner_.SetRowCount(iCount);
    }
  }
  CPLErr CreateColumn(const char* pszFieldName, GDALRATFieldType eFieldType,
                      GDALRATFieldUsage eFieldUsage) override {
    if (use_base_fallbacks_) {
      return GDALRasterAttributeTable::CreateColumn(pszFieldName, eFieldType,
                                                    eFieldUsage);
    }
    return inner_.CreateColumn(pszFieldName, eFieldType, eFieldUsage);
  }

  // Expose protected helpers for testing.
  using GDALRasterAttributeTable::ValuesIOBooleanFromIntoInt;
  using GDALRasterAttributeTable::ValuesIODateTimeFromIntoString;
  using GDALRasterAttributeTable::ValuesIOWKBGeometryFromIntoString;

  GDALDefaultRasterAttributeTable inner_;
  bool use_base_fallbacks_ = false;
  bool emit_error_on_get_ = false;
  bool fail_set_ = false;
  int fake_row_count_ = -1;
  int fake_col_count_ = -1;
};

TEST(GdalRatTest, NullPointerCAPIValidation) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDestroyRasterAttributeTable(nullptr);
  EXPECT_EQ(0, GDALRATGetColumnCount(nullptr));
  EXPECT_EQ(nullptr, GDALRATGetNameOfCol(nullptr, 0));
  EXPECT_EQ(GFU_Generic, GDALRATGetUsageOfCol(nullptr, 0));
  EXPECT_EQ(GFT_Integer, GDALRATGetTypeOfCol(nullptr, 0));
  EXPECT_EQ(0, GDALRATGetColOfUsage(nullptr, GFU_Generic));
  EXPECT_EQ(0, GDALRATGetRowCount(nullptr));
  GDALRATSetRowCount(nullptr, 5);

  EXPECT_EQ(nullptr, GDALRATGetValueAsString(nullptr, 0, 0));
  EXPECT_EQ(0, GDALRATGetValueAsInt(nullptr, 0, 0));
  EXPECT_EQ(0.0, GDALRATGetValueAsDouble(nullptr, 0, 0));
  EXPECT_FALSE(GDALRATGetValueAsBoolean(nullptr, 0, 0));

  GDALRATDateTime dt;
  EXPECT_EQ(CE_Failure, GDALRATGetValueAsDateTime(nullptr, 0, 0, &dt));
  GDALDefaultRasterAttributeTable valid_rat;
  EXPECT_EQ(CE_Failure,
            GDALRATGetValueAsDateTime(
                GDALRasterAttributeTable::ToHandle(&valid_rat), 0, 0, nullptr));

  size_t wkb_size = 0;
  EXPECT_EQ(nullptr, GDALRATGetValueAsWKBGeometry(nullptr, 0, 0, &wkb_size));
  EXPECT_EQ(nullptr,
            GDALRATGetValueAsWKBGeometry(
                GDALRasterAttributeTable::ToHandle(&valid_rat), 0, 0, nullptr));

  GDALRATSetValueAsString(nullptr, 0, 0, "x");
  GDALRATSetValueAsInt(nullptr, 0, 0, 1);
  GDALRATSetValueAsDouble(nullptr, 0, 0, 1.0);
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsBoolean(nullptr, 0, 0, true));
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsDateTime(nullptr, 0, 0, &dt));
  EXPECT_EQ(CE_Failure,
            GDALRATSetValueAsDateTime(
                GDALRasterAttributeTable::ToHandle(&valid_rat), 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsWKBGeometry(nullptr, 0, 0, "x", 1));

  double dval = 0.0;
  int ival = 0;
  char* sval = nullptr;
  bool bval = false;
  GByte* wkb_ptr = nullptr;
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDouble(nullptr, GF_Read, 0, 0, 1, &dval));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsInteger(nullptr, GF_Read, 0, 0, 1, &ival));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsString(nullptr, GF_Read, 0, 0, 1, &sval));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsBoolean(nullptr, GF_Read, 0, 0, 1, &bval));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDateTime(nullptr, GF_Read, 0, 0, 1, &dt));
  EXPECT_EQ(CE_Failure, GDALRATValuesIOAsWKBGeometry(nullptr, GF_Read, 0, 0, 1,
                                                     &wkb_ptr, &wkb_size));

  EXPECT_FALSE(GDALRATChangesAreWrittenToFile(nullptr));
  EXPECT_EQ(CE_Failure, GDALRATSetLinearBinning(nullptr, 0.0, 1.0));
  double r0 = 0.0, bs = 0.0;
  EXPECT_FALSE(GDALRATGetLinearBinning(nullptr, &r0, &bs));
  EXPECT_FALSE(GDALRATGetLinearBinning(
      GDALRasterAttributeTable::ToHandle(&valid_rat), nullptr, &bs));
  EXPECT_FALSE(GDALRATGetLinearBinning(
      GDALRasterAttributeTable::ToHandle(&valid_rat), &r0, nullptr));

  EXPECT_EQ(CE_Failure, GDALRATSetTableType(nullptr, GRTT_THEMATIC));
  EXPECT_EQ(GRTT_THEMATIC, GDALRATGetTableType(nullptr));
  EXPECT_EQ(CE_Failure, GDALRATInitializeFromColorTable(nullptr, nullptr));
  EXPECT_EQ(nullptr, GDALRATTranslateToColorTable(nullptr, -1));
  GDALRATDumpReadable(nullptr, nullptr);
  EXPECT_EQ(CE_Failure,
            GDALRATCreateColumn(nullptr, "col", GFT_Integer, GFU_Generic));
  EXPECT_EQ(0, GDALRATGetRowOfValue(nullptr, 1.0));
  EXPECT_EQ(nullptr, GDALRATClone(nullptr));
  EXPECT_EQ(nullptr, GDALRATSerializeJSON(nullptr));
  GDALRATRemoveStatistics(nullptr);
  CPLPopErrorHandler();
}

TEST(GdalRatTest, AllTypesGetAndSetAndConversions) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALRasterAttributeTableH hRAT = GDALCreateRasterAttributeTable();
  ASSERT_NE(nullptr, hRAT);

  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "int_col", GFT_Integer, GFU_Generic));
  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "real_col", GFT_Real, GFU_Generic));
  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "str_col", GFT_String, GFU_Name));
  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "bool_col", GFT_Boolean, GFU_Generic));
  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "dt_col", GFT_DateTime, GFU_Generic));
  EXPECT_EQ(CE_None,
            GDALRATCreateColumn(hRAT, "wkb_col", GFT_WKBGeometry, GFU_Generic));
  // Color columns force GFT_Integer even if requested as GFT_Real.
  EXPECT_EQ(CE_None, GDALRATCreateColumn(hRAT, "red_col", GFT_Real, GFU_Red));
  EXPECT_EQ(GFT_Integer, GDALRATGetTypeOfCol(hRAT, 6));

  EXPECT_EQ(7, GDALRATGetColumnCount(hRAT));
  EXPECT_STREQ("int_col", GDALRATGetNameOfCol(hRAT, 0));
  EXPECT_STREQ("", GDALRATGetNameOfCol(hRAT, -1));
  EXPECT_STREQ("", GDALRATGetNameOfCol(hRAT, 99));
  EXPECT_EQ(GFU_Name, GDALRATGetUsageOfCol(hRAT, 2));
  EXPECT_EQ(GFU_Generic, GDALRATGetUsageOfCol(hRAT, -1));
  EXPECT_EQ(GFU_Generic, GDALRATGetUsageOfCol(hRAT, 99));
  EXPECT_EQ(GFT_Real, GDALRATGetTypeOfCol(hRAT, 1));
  EXPECT_EQ(GFT_Integer, GDALRATGetTypeOfCol(hRAT, -1));
  EXPECT_EQ(GFT_Integer, GDALRATGetTypeOfCol(hRAT, 99));
  EXPECT_EQ(2, GDALRATGetColOfUsage(hRAT, GFU_Name));
  EXPECT_EQ(-1, GDALRATGetColOfUsage(hRAT, GFU_BlueMax));

  GDALRATSetRowCount(hRAT, 2);
  GDALRATSetRowCount(hRAT, 2);  // No-op when equal.
  EXPECT_EQ(2, GDALRATGetRowCount(hRAT));

  // Set values via string on all 6 columns (row 0).
  GDALRATSetValueAsString(hRAT, 0, 0, "42");
  GDALRATSetValueAsString(hRAT, 0, 1, "3.25");
  GDALRATSetValueAsString(hRAT, 0, 2, "POINT (1 2)");
  GDALRATSetValueAsString(hRAT, 0, 3, "true");
  GDALRATSetValueAsString(hRAT, 0, 4, "2025-06-15T12:30:45.500+02:15");
  GDALRATSetValueAsString(hRAT, 0, 5, "POINT (10 20)");

  // Out-of-bounds SetValue checks for all types.
  auto* poRAT = GDALRasterAttributeTable::FromHandle(hRAT);
  EXPECT_EQ(CE_Failure, poRAT->SetValue(-1, 0, "x"));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(99, 0, "x"));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, -1, "x"));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, 99, "x"));

  EXPECT_EQ(CE_Failure, poRAT->SetValue(-1, 0, 1));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(99, 0, 1));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, -1, 1));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, 99, 1));

  EXPECT_EQ(CE_Failure, poRAT->SetValue(-1, 0, 1.0));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(99, 0, 1.0));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, -1, 1.0));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, 99, 1.0));

  EXPECT_EQ(CE_Failure, poRAT->SetValue(-1, 0, true));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(99, 0, true));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, -1, true));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, 99, true));

  GDALRATDateTime dt;
  dt.bIsValid = true;
  dt.nYear = 2024;
  dt.nMonth = 1;
  dt.nDay = 2;
  dt.nHour = 3;
  dt.nMinute = 4;
  dt.fSecond = 5.0f;
  dt.nTimeZoneHour = 0;
  dt.nTimeZoneMinute = 0;
  dt.bPositiveTimeZone = false;
  EXPECT_EQ(CE_Failure, poRAT->SetValue(-1, 4, dt));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(99, 4, dt));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, -1, dt));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(0, 99, dt));

  size_t wkb_size = 0;
  const GByte* wkb_data = GDALRATGetValueAsWKBGeometry(hRAT, 0, 5, &wkb_size);
  ASSERT_NE(nullptr, wkb_data);
  EXPECT_GT(wkb_size, 0U);
  std::vector<GByte> saved_wkb(wkb_data, wkb_data + wkb_size);

  EXPECT_EQ(CE_Failure,
            poRAT->SetValue(-1, 5, saved_wkb.data(), saved_wkb.size()));
  EXPECT_EQ(CE_Failure,
            poRAT->SetValue(99, 5, saved_wkb.data(), saved_wkb.size()));
  EXPECT_EQ(CE_Failure,
            poRAT->SetValue(0, -1, saved_wkb.data(), saved_wkb.size()));
  EXPECT_EQ(CE_Failure,
            poRAT->SetValue(0, 99, saved_wkb.data(), saved_wkb.size()));

  // Set values via int, double, boolean, DateTime, WKBGeometry across
  // compatible and incompatible columns on row 1, plus auto-expanding row 2!
  GDALRATSetValueAsInt(hRAT, 1, 0, 10);
  GDALRATSetValueAsInt(hRAT, 1, 1, 20);
  GDALRATSetValueAsInt(hRAT, 1, 2, 30);
  GDALRATSetValueAsInt(hRAT, 1, 3, 1);
  EXPECT_EQ(CE_Failure, poRAT->SetValue(1, 4, 1));  // Incompatible DateTime
  EXPECT_EQ(CE_Failure, poRAT->SetValue(1, 5, 1));  // Incompatible WKB

  GDALRATSetValueAsDouble(hRAT, 2, 0, 11.5);  // Auto-expands to 3 rows!
  EXPECT_EQ(3, GDALRATGetRowCount(hRAT));
  GDALRATSetValueAsDouble(hRAT, 2, 1, 22.5);
  GDALRATSetValueAsDouble(hRAT, 2, 2, 33.5);
  GDALRATSetValueAsDouble(hRAT, 2, 3, 0.0);
  EXPECT_EQ(CE_Failure, poRAT->SetValue(2, 4, 1.0));
  EXPECT_EQ(CE_Failure, poRAT->SetValue(2, 5, 1.0));

  EXPECT_EQ(CE_None, GDALRATSetValueAsBoolean(hRAT, 3, 0, true));  // Row 3
  EXPECT_EQ(CE_None, GDALRATSetValueAsBoolean(hRAT, 3, 1, true));
  EXPECT_EQ(CE_None, GDALRATSetValueAsBoolean(hRAT, 3, 2, false));
  EXPECT_EQ(CE_None, GDALRATSetValueAsBoolean(hRAT, 3, 3, true));
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsBoolean(hRAT, 3, 4, true));
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsBoolean(hRAT, 3, 5, true));

  EXPECT_EQ(CE_None, GDALRATSetValueAsDateTime(hRAT, 4, 4, &dt));  // Row 4
  EXPECT_EQ(CE_None,
            GDALRATSetValueAsDateTime(hRAT, 4, 2, &dt));  // Into String
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsDateTime(hRAT, 4, 0, &dt));

  EXPECT_EQ(CE_None, GDALRATSetValueAsWKBGeometry(hRAT, 5, 5, saved_wkb.data(),
                                                  saved_wkb.size()));
  EXPECT_EQ(CE_None, GDALRATSetValueAsWKBGeometry(hRAT, 5, 2, saved_wkb.data(),
                                                  saved_wkb.size()));
  EXPECT_EQ(CE_None, GDALRATSetValueAsWKBGeometry(hRAT, 1, 5, nullptr, 0));
  EXPECT_EQ(CE_Failure, GDALRATSetValueAsWKBGeometry(
                            hRAT, 5, 0, saved_wkb.data(), saved_wkb.size()));

  // Read values back in all types across all columns.
  for (int col = 0; col < 6; ++col) {
    (void)GDALRATGetValueAsString(hRAT, 0, col);
    (void)GDALRATGetValueAsInt(hRAT, 0, col);
    (void)GDALRATGetValueAsDouble(hRAT, 0, col);
    (void)GDALRATGetValueAsBoolean(hRAT, 0, col);
    GDALRATDateTime out_dt;
    (void)GDALRATGetValueAsDateTime(hRAT, 0, col, &out_dt);
    size_t out_sz = 0;
    (void)GDALRATGetValueAsWKBGeometry(hRAT, 0, col, &out_sz);
  }
  // Also test GetValueAsWKBGeometry on empty WKB (row 1, col 5) and invalid WKT
  // string (row 1, col 2).
  size_t empty_sz = 99;
  EXPECT_EQ(nullptr, GDALRATGetValueAsWKBGeometry(hRAT, 1, 5, &empty_sz));
  EXPECT_EQ(0U, empty_sz);
  EXPECT_EQ(nullptr, GDALRATGetValueAsWKBGeometry(hRAT, 1, 2, &empty_sz));

  // Out-of-bounds GetValueAs* checks.
  for (int bad_idx : {-1, 99}) {
    EXPECT_STREQ("", GDALRATGetValueAsString(hRAT, bad_idx, 0));
    EXPECT_STREQ("", GDALRATGetValueAsString(hRAT, 0, bad_idx));
    EXPECT_EQ(0, GDALRATGetValueAsInt(hRAT, bad_idx, 0));
    EXPECT_EQ(0, GDALRATGetValueAsInt(hRAT, 0, bad_idx));
    EXPECT_EQ(0.0, GDALRATGetValueAsDouble(hRAT, bad_idx, 0));
    EXPECT_EQ(0.0, GDALRATGetValueAsDouble(hRAT, 0, bad_idx));
    EXPECT_FALSE(GDALRATGetValueAsBoolean(hRAT, bad_idx, 0));
    EXPECT_FALSE(GDALRATGetValueAsBoolean(hRAT, 0, bad_idx));
    GDALRATDateTime out_dt;
    EXPECT_EQ(CE_Failure, GDALRATGetValueAsDateTime(hRAT, bad_idx, 0, &out_dt));
    EXPECT_EQ(CE_Failure, GDALRATGetValueAsDateTime(hRAT, 0, bad_idx, &out_dt));
    size_t out_sz = 0;
    EXPECT_EQ(nullptr, GDALRATGetValueAsWKBGeometry(hRAT, bad_idx, 0, &out_sz));
    EXPECT_EQ(nullptr, GDALRATGetValueAsWKBGeometry(hRAT, 0, bad_idx, &out_sz));
  }

  GDALDestroyRasterAttributeTable(hRAT);
  CPLPopErrorHandler();
}

TEST(GdalRatTest, DateTimeAndWKBStaticHelpers) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALRATDateTime dt;
  EXPECT_FALSE(GDALRasterAttributeTable::StringToDateTime("invalid", dt));
  EXPECT_STREQ("", GDALRasterAttributeTable::DateTimeToString(dt).c_str());

  // UTC Z timezone with integer seconds.
  EXPECT_TRUE(
      GDALRasterAttributeTable::StringToDateTime("2025-12-31T23:59:05Z", dt));
  EXPECT_STREQ("2025-12-31T23:59:05.000+00:00",
               GDALRasterAttributeTable::DateTimeToString(dt).c_str());

  // Negative timezone with fractional seconds.
  EXPECT_TRUE(GDALRasterAttributeTable::StringToDateTime(
      "2025-12-31T23:59:05.250-05:30", dt));
  EXPECT_STREQ("2025-12-31T23:59:05.250-05:30",
               GDALRasterAttributeTable::DateTimeToString(dt).c_str());

  // Positive timezone with 00:00 and bPositiveTimeZone = true / false.
  dt.nTimeZoneHour = 0;
  dt.nTimeZoneMinute = 0;
  dt.bPositiveTimeZone = true;
  dt.fSecond = 10.0f;
  EXPECT_STREQ("2025-12-31T23:59:10.000+00:00",
               GDALRasterAttributeTable::DateTimeToString(dt).c_str());
  dt.nTimeZoneHour = 1;
  dt.nTimeZoneMinute = 30;
  EXPECT_STREQ("2025-12-31T23:59:10.000+01:30",
               GDALRasterAttributeTable::DateTimeToString(dt).c_str());

  // No timezone in parsed string (TZFlag <= 2).
  EXPECT_TRUE(
      GDALRasterAttributeTable::StringToDateTime("2025-12-31T23:59:10", dt));
  EXPECT_STREQ("2025-12-31T23:59:10.000-00:00",
               GDALRasterAttributeTable::DateTimeToString(dt).c_str());

  // WKT/WKB helpers.
  EXPECT_STREQ("",
               GDALRasterAttributeTable::WKBGeometryToWKT(nullptr, 0).c_str());
  const GByte bad_wkb[] = {0x99, 0x99, 0x99};
  EXPECT_STREQ("",
               GDALRasterAttributeTable::WKBGeometryToWKT(bad_wkb, 3).c_str());
  EXPECT_TRUE(GDALRasterAttributeTable::WKTGeometryToWKB("").empty());
  EXPECT_TRUE(GDALRasterAttributeTable::WKTGeometryToWKB("NOT_A_WKT").empty());

  auto wkb = GDALRasterAttributeTable::WKTGeometryToWKB("POINT (3 4)");
  EXPECT_FALSE(wkb.empty());
  EXPECT_STREQ("POINT (3 4)", GDALRasterAttributeTable::WKBGeometryToWKT(
                                  wkb.data(), wkb.size())
                                  .c_str());
  CPLPopErrorHandler();
}

TEST(GdalRatTest, ValuesIOAndProtectedHelpers) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CustomBaseRAT rat;
  rat.CreateColumn("int_col", GFT_Integer, GFU_Generic);
  rat.CreateColumn("real_col", GFT_Real, GFU_Generic);
  rat.CreateColumn("str_col", GFT_String, GFU_Generic);
  rat.CreateColumn("bool_col", GFT_Boolean, GFU_Generic);
  rat.CreateColumn("dt_col", GFT_DateTime, GFU_Generic);
  rat.CreateColumn("wkb_col", GFT_WKBGeometry, GFU_Generic);
  rat.SetRowCount(10);

  GDALRasterAttributeTableH hRAT = GDALRasterAttributeTable::ToHandle(&rat);

  // Test ValuesIO on base GDALRasterAttributeTable::ValuesIO.
  double dvals[2] = {1.5, 2.5};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsDouble(hRAT, GF_Write, 1, 1, 2, dvals));
  EXPECT_EQ(10, rat.GetRowCount());
  double dvals_read[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsDouble(hRAT, GF_Read, 1, 1, 2, dvals_read));
  EXPECT_DOUBLE_EQ(1.5, dvals_read[0]);
  EXPECT_DOUBLE_EQ(2.5, dvals_read[1]);

  int ivals[2] = {100, 200};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsInteger(hRAT, GF_Write, 0, 2, 2, ivals));
  EXPECT_EQ(10, rat.GetRowCount());
  int ivals_read[2] = {0, 0};
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsInteger(hRAT, GF_Read, 0, 2, 2, ivals_read));
  EXPECT_EQ(100, ivals_read[0]);
  EXPECT_EQ(200, ivals_read[1]);

  const char* svals_in[2] = {"hello", "world"};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsString(hRAT, GF_Write, 2, 3, 2,
                                             const_cast<char**>(svals_in)));
  EXPECT_EQ(10, rat.GetRowCount());
  char* svals_out[2] = {nullptr, nullptr};
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsString(hRAT, GF_Read, 2, 3, 2, svals_out));
  EXPECT_STREQ("hello", svals_out[0]);
  EXPECT_STREQ("world", svals_out[1]);
  CPLFree(svals_out[0]);
  CPLFree(svals_out[1]);
  svals_out[0] = nullptr;
  svals_out[1] = nullptr;

  bool bvals[2] = {true, false};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsBoolean(hRAT, GF_Write, 3, 4, 2, bvals));
  bool bvals_read[2] = {false, true};
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsBoolean(hRAT, GF_Read, 3, 4, 2, bvals_read));
  EXPECT_TRUE(bvals_read[0]);
  EXPECT_FALSE(bvals_read[1]);

  GDALRATDateTime dt_vals[2];
  dt_vals[0].bIsValid = true;
  dt_vals[0].nYear = 2025;
  dt_vals[0].nMonth = 3;
  dt_vals[0].nDay = 4;
  dt_vals[1].bIsValid = false;
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsDateTime(hRAT, GF_Write, 4, 5, 2, dt_vals));
  GDALRATDateTime dt_read[2];
  EXPECT_EQ(CE_None,
            GDALRATValuesIOAsDateTime(hRAT, GF_Read, 4, 5, 2, dt_read));
  EXPECT_TRUE(dt_read[0].bIsValid);
  EXPECT_EQ(2025, dt_read[0].nYear);

  auto wkb = GDALRasterAttributeTable::WKTGeometryToWKB("POINT (5 6)");
  GByte* wkb_in[2] = {wkb.data(), nullptr};
  size_t wkb_sizes_in[2] = {wkb.size(), 0};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsWKBGeometry(hRAT, GF_Write, 5, 6, 2,
                                                  wkb_in, wkb_sizes_in));
  GByte* wkb_out[2] = {nullptr, nullptr};
  size_t wkb_sizes_out[2] = {0, 0};
  EXPECT_EQ(CE_None, GDALRATValuesIOAsWKBGeometry(hRAT, GF_Read, 5, 6, 2,
                                                  wkb_out, wkb_sizes_out));
  EXPECT_NE(nullptr, wkb_out[0]);
  EXPECT_EQ(wkb.size(), wkb_sizes_out[0]);
  EXPECT_EQ(nullptr, wkb_out[1]);
  CPLFree(wkb_out[0]);
  wkb_out[0] = nullptr;

  // Out-of-bounds iStartRow + iLength > GetRowCount() for all 6 ValuesIO
  // overloads.
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDouble(hRAT, GF_Read, 1, 100, 1, dvals));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsInteger(hRAT, GF_Read, 0, 100, 1, ivals));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsString(hRAT, GF_Read, 2, 100, 1, svals_out));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsBoolean(hRAT, GF_Read, 3, 100, 1, bvals));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDateTime(hRAT, GF_Read, 4, 100, 1, dt_vals));
  EXPECT_EQ(CE_Failure, GDALRATValuesIOAsWKBGeometry(hRAT, GF_Read, 5, 100, 1,
                                                     wkb_out, wkb_sizes_out));

  // Test error propagation on GF_Write inside ValuesIO loops (fail_set_).
  rat.fail_set_ = true;
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDouble(hRAT, GF_Write, 1, 0, 1, dvals));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsInteger(hRAT, GF_Write, 0, 0, 1, ivals));
  EXPECT_EQ(CE_Failure, GDALRATValuesIOAsString(hRAT, GF_Write, 2, 0, 1,
                                                const_cast<char**>(svals_in)));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsBoolean(hRAT, GF_Write, 3, 0, 1, bvals));
  EXPECT_EQ(CE_Failure,
            GDALRATValuesIOAsDateTime(hRAT, GF_Write, 4, 0, 1, dt_vals));
  EXPECT_EQ(CE_Failure, GDALRATValuesIOAsWKBGeometry(hRAT, GF_Write, 5, 0, 1,
                                                     wkb_in, wkb_sizes_in));
  rat.fail_set_ = false;

  // Test protected helper methods on CustomBaseRAT:
  // 1. ValuesIOBooleanFromIntoInt
  EXPECT_EQ(CE_None, rat.ValuesIOBooleanFromIntoInt(GF_Write, 0, 0, 2, bvals));
  EXPECT_EQ(CE_None,
            rat.ValuesIOBooleanFromIntoInt(GF_Read, 0, 0, 2, bvals_read));
  EXPECT_TRUE(bvals_read[0]);
  EXPECT_FALSE(bvals_read[1]);

  // 2. ValuesIODateTimeFromIntoString
  EXPECT_EQ(CE_None,
            rat.ValuesIODateTimeFromIntoString(GF_Write, 2, 0, 2, dt_vals));
  EXPECT_EQ(CE_None,
            rat.ValuesIODateTimeFromIntoString(GF_Read, 2, 0, 2, dt_read));
  EXPECT_TRUE(dt_read[0].bIsValid);
  EXPECT_FALSE(dt_read[1].bIsValid);
  // Invalid datetime string in col 2 yields an invalid DateTime on GF_Read.
  rat.SetValue(0, 2, "not_a_datetime");
  EXPECT_EQ(CE_None,
            rat.ValuesIODateTimeFromIntoString(GF_Read, 2, 0, 1, dt_read));
  EXPECT_FALSE(dt_read[0].bIsValid);

  // 3. ValuesIOWKBGeometryFromIntoString
  EXPECT_EQ(CE_None, rat.ValuesIOWKBGeometryFromIntoString(
                         GF_Write, 2, 0, 2, wkb_in, wkb_sizes_in));
  EXPECT_EQ(CE_None, rat.ValuesIOWKBGeometryFromIntoString(
                         GF_Read, 2, 0, 2, wkb_out, wkb_sizes_out));
  CPLFree(wkb_out[0]);
  // Invalid WKB on write writes empty string.
  const GByte bad_wkb[] = {0x99, 0x99};
  GByte* bad_wkb_in[1] = {const_cast<GByte*>(bad_wkb)};
  size_t bad_wkb_sz[1] = {sizeof(bad_wkb)};
  EXPECT_EQ(CE_None, rat.ValuesIOWKBGeometryFromIntoString(
                         GF_Write, 2, 0, 1, bad_wkb_in, bad_wkb_sz));
  // Invalid WKT in col 2 yields nullptr WKB on GF_Read.
  rat.SetValue(0, 2, "not_a_wkt");
  EXPECT_EQ(CE_None, rat.ValuesIOWKBGeometryFromIntoString(
                         GF_Read, 2, 0, 1, wkb_out, wkb_sizes_out));
  EXPECT_EQ(nullptr, wkb_out[0]);

  // Base fallback methods on CustomBaseRAT.
  rat.use_base_fallbacks_ = true;
  EXPECT_EQ(-1, rat.GetRowOfValue(1.0));
  EXPECT_EQ(-1, rat.GetRowOfValue(1));
  EXPECT_EQ(CE_Failure, rat.CreateColumn("x", GFT_Integer, GFU_Generic));
  EXPECT_EQ(CE_Failure, rat.SetLinearBinning(0.0, 1.0));
  double r0 = 0.0, bs = 0.0;
  EXPECT_FALSE(rat.GetLinearBinning(&r0, &bs));
  CPLPopErrorHandler();
}

TEST(GdalRatTest, AnalyseColumnsAndGetRowOfValue) {
  // 1. Linear binning
  {
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("val", GFT_Integer, GFU_Generic);
    rat.SetRowCount(5);
    GDALRasterAttributeTableH hRAT = GDALRasterAttributeTable::ToHandle(&rat);
    double r0 = 0.0, bs = 0.0;
    EXPECT_FALSE(GDALRATGetLinearBinning(hRAT, &r0, &bs));
    EXPECT_EQ(CE_None, GDALRATSetLinearBinning(hRAT, 10.0, 2.0));
    EXPECT_TRUE(GDALRATGetLinearBinning(hRAT, &r0, &bs));
    EXPECT_DOUBLE_EQ(10.0, r0);
    EXPECT_DOUBLE_EQ(2.0, bs);

    EXPECT_EQ(-1, GDALRATGetRowOfValue(hRAT, 9.0));
    EXPECT_EQ(0, GDALRATGetRowOfValue(hRAT, 10.5));
    EXPECT_EQ(2, rat.GetRowOfValue(15));
    EXPECT_EQ(-1, GDALRATGetRowOfValue(hRAT, 25.0));
  }

  // 2. GFU_MinMax (GFT_Integer)
  {
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("val", GFT_Integer, GFU_MinMax);
    rat.SetValue(0, 0, 10);
    rat.SetValue(1, 0, 20);
    EXPECT_EQ(0, rat.GetRowOfValue(10.0));
    EXPECT_EQ(1, rat.GetRowOfValue(20));
    EXPECT_EQ(-1, rat.GetRowOfValue(15.0));
    EXPECT_EQ(-1, rat.GetRowOfValue(25.0));
  }

  // 3. Separate GFU_Min and GFU_Max (GFT_Real)
  {
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("min", GFT_Real, GFU_Min);
    rat.CreateColumn("max", GFT_Real, GFU_Max);
    rat.SetValue(0, 0, 0.0);
    rat.SetValue(0, 1, 10.0);
    rat.SetValue(1, 0, 10.0);
    rat.SetValue(1, 1, 20.0);
    EXPECT_EQ(-1, rat.GetRowOfValue(-1.0));
    EXPECT_EQ(0, rat.GetRowOfValue(5.0));
    EXPECT_EQ(1, rat.GetRowOfValue(15.0));
    EXPECT_EQ(-1, rat.GetRowOfValue(25.0));
  }

  // 4. No Min or Max columns
  {
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("name", GFT_String, GFU_Name);
    rat.SetValue(0, 0, "foo");
    EXPECT_EQ(-1, rat.GetRowOfValue(1.0));
  }
}

TEST(GdalRatTest, ColorTableAndStatisticsAndTableType) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDefaultRasterAttributeTable rat;
  GDALRasterAttributeTableH hRAT = GDALRasterAttributeTable::ToHandle(&rat);

  EXPECT_EQ(GRTT_THEMATIC, GDALRATGetTableType(hRAT));
  EXPECT_EQ(CE_None, GDALRATSetTableType(hRAT, GRTT_ATHEMATIC));
  EXPECT_EQ(GRTT_ATHEMATIC, GDALRATGetTableType(hRAT));
  EXPECT_FALSE(GDALRATChangesAreWrittenToFile(hRAT));

  GDALColorTable ct;
  GDALColorEntry e0 = {10, 20, 30, 255};
  GDALColorEntry e1 = {40, 50, 60, 128};
  ct.SetColorEntry(0, &e0);
  ct.SetColorEntry(1, &e1);

  EXPECT_EQ(CE_None, GDALRATInitializeFromColorTable(
                         hRAT, GDALColorTable::ToHandle(&ct)));
  EXPECT_EQ(2, GDALRATGetRowCount(hRAT));

  GDALColorTableH hCTOut = GDALRATTranslateToColorTable(hRAT, -1);
  ASSERT_NE(nullptr, hCTOut);
  EXPECT_EQ(2, GDALGetColorEntryCount(hCTOut));
  const GDALColorEntry* pe1 = GDALGetColorEntry(hCTOut, 1);
  ASSERT_NE(nullptr, pe1);
  EXPECT_EQ(40, pe1->c1);
  EXPECT_EQ(50, pe1->c2);
  EXPECT_EQ(60, pe1->c3);
  EXPECT_EQ(128, pe1->c4);
  GDALDestroyColorTable(hCTOut);

  // Add statistics columns and remove them.
  rat.CreateColumn("Histogram", GFT_Real, GFU_Generic);
  rat.CreateColumn("Count", GFT_Integer, GFU_PixelCount);
  rat.CreateColumn("RedMin", GFT_Integer, GFU_RedMin);
  rat.CreateColumn("RedMax", GFT_Integer, GFU_RedMax);
  rat.CreateColumn("Name", GFT_String, GFU_Name);
  int cols_before = rat.GetColumnCount();
  GDALRATRemoveStatistics(hRAT);
  EXPECT_LT(rat.GetColumnCount(), cols_before);
  EXPECT_EQ(-1, rat.GetColOfUsage(GFU_PixelCount));
  EXPECT_NE(-1, rat.GetColOfUsage(GFU_Name));

  // TranslateToColorTable on RAT without color columns returns nullptr.
  GDALDefaultRasterAttributeTable empty_rat;
  EXPECT_EQ(nullptr, empty_rat.TranslateToColorTable());
  CPLPopErrorHandler();
}

TEST(GdalRatTest, SerializeXMLAndJSONAndDumpReadableAndClone) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDefaultRasterAttributeTable rat;
  rat.SetTableType(GRTT_ATHEMATIC);
  rat.SetLinearBinning(1.0, 0.5);
  rat.CreateColumn("int_col", GFT_Integer, GFU_MinMax);
  rat.CreateColumn("real_col", GFT_Real, GFU_Generic);
  rat.CreateColumn("str_col", GFT_String, GFU_Name);
  rat.CreateColumn("bool_col", GFT_Boolean, GFU_Generic);
  rat.CreateColumn("dt_col", GFT_DateTime, GFU_Generic);
  rat.CreateColumn("wkb_col", GFT_WKBGeometry, GFU_Generic);

  rat.SetValue(0, 0, 1);
  rat.SetValue(0, 1, 2.5);
  rat.SetValue(0, 2, "row0");
  rat.SetValue(0, 3, true);
  rat.SetValue(0, 4, "2025-01-02T03:04:05Z");
  rat.SetValue(0, 5, "POINT (1 2)");

  rat.SetValue(1, 0, 2);
  rat.SetValue(1, 1, 3.5);
  rat.SetValue(1, 2, "row1");
  rat.SetValue(1, 3, false);

  // Clone
  GDALRasterAttributeTableH hRAT = GDALRasterAttributeTable::ToHandle(&rat);
  GDALRasterAttributeTableH hClone = GDALRATClone(hRAT);
  ASSERT_NE(nullptr, hClone);
  EXPECT_EQ(6, GDALRATGetColumnCount(hClone));
  EXPECT_EQ(2, GDALRATGetRowCount(hClone));
  GDALDestroyRasterAttributeTable(hClone);

  // DumpReadable to tmpfile
  FILE* fp = std::tmpfile();
  ASSERT_NE(nullptr, fp);
  GDALRATDumpReadable(hRAT, fp);
  std::fclose(fp);

  // Serialize to XML and XMLInit round-trip
  CPLXMLTreeCloser xml(rat.Serialize());
  ASSERT_NE(nullptr, xml.get());

  GDALDefaultRasterAttributeTable rat2;
  EXPECT_EQ(CE_None, rat2.XMLInit(xml.get(), ""));
  EXPECT_EQ(GRTT_THEMATIC, rat2.GetTableType());
  double r0 = 0.0, bs = 0.0;
  EXPECT_TRUE(rat2.GetLinearBinning(&r0, &bs));
  EXPECT_DOUBLE_EQ(1.0, r0);
  EXPECT_DOUBLE_EQ(0.5, bs);
  EXPECT_EQ(6, rat2.GetColumnCount());
  EXPECT_EQ(2, rat2.GetRowCount());
  EXPECT_STREQ("row0", rat2.GetValueAsString(0, 2));
  EXPECT_TRUE(rat2.GetValueAsBoolean(0, 3));
  size_t wkb_sz = 0;
  EXPECT_NE(nullptr, rat2.GetValueAsWKBGeometry(0, 5, wkb_sz));
  EXPECT_GT(wkb_sz, 0U);

  // SerializeJSON
  json_object* jsobj = static_cast<json_object*>(GDALRATSerializeJSON(hRAT));
  ASSERT_NE(nullptr, jsobj);
  json_object_put(jsobj);

  // Empty RAT Serialize returns nullptr.
  GDALDefaultRasterAttributeTable empty_rat;
  EXPECT_EQ(nullptr, empty_rat.Serialize());
  CPLPopErrorHandler();
}

}  // namespace
}  // namespace gdal
