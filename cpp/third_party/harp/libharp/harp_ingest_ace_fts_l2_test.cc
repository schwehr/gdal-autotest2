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
// Unit tests for ACE-FTS Level 2 ingestion modules
// (libharp/harp-ingest-ace_fts_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-ace_fts_l2.c

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr std::string_view kMainSpecies[] = {
    "H2O",    "O3",  "N2O",    "CO",     "CH4",  "NO",     "NO2",
    "HNO3",   "HF",  "HCl",    "OCS",    "N2O5", "ClONO2", "HCN",
    "CH3Cl",  "CF4", "CCl2F2", "CCl3F",  "COF2", "C2H6",   "C2H2",
    "CHF2Cl", "SF6", "ClO",    "HO2NO2", "H2O2", "HOCl",   "N2",
};

constexpr std::string_view kIsoSpeciesHeader[] = {
    "H2O (181)", "181_err",   "H2O (171)", "171_err",   "H2O (162)",
    "162_err",   "CH4 (311)", "311_err",   "CH4 (212)", "212_err",
};

constexpr std::string_view kIsoHarpSpecies[] = {
    "H2O_181", "H2O_171", "H2O_162", "CH4_311", "CH4_212",
};

void AddHeaderFields(CodaMemProduct& prod, std::string_view date_str,
                     std::string_view start_str, std::string_view end_str,
                     double lat, double lon) {
  ASSERT_TRUE(prod.AddField(prod.root(), "date", prod.MakeString(date_str)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "start_time", prod.MakeString(start_str)));
  ASSERT_TRUE(prod.AddField(prod.root(), "end_time", prod.MakeString(end_str)));
  ASSERT_TRUE(prod.AddField(prod.root(), "latitude", prod.MakeDouble(lat)));
  ASSERT_TRUE(prod.AddField(prod.root(), "longitude", prod.MakeDouble(lon)));
}

void AddFieldDataLines(CodaMemProduct& prod,
                       const std::vector<std::string>& lines) {
  std::vector<CodaMemNode*> records;
  records.reserve(lines.size());
  for (const std::string& line : lines) {
    CodaMemNode* rec = prod.MakeRecord();
    ASSERT_NE(nullptr, rec);
    ASSERT_TRUE(prod.AddField(rec, "line", prod.MakeString(line)));
    records.push_back(rec);
  }
  const long dims[] = {static_cast<long>(records.size())};
  ASSERT_TRUE(prod.AddField(prod.root(), "field_data",
                            prod.MakeNodeArray(dims, records)));
}

TEST(HarpIngestAceFtsL2Test, IngestMainAllSpeciesAndFillValues) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ACE_FTS_L2_main");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod(coda_format_xml, "/data/ss12345_v3.5.asc");
  ASSERT_TRUE(prod.ok());
  AddHeaderFields(prod, "2020-05-15 12:34:56.50+00", "2020-05-15 12:30:00+00",
                  "2020-05-15 12:40:00+00", 45.5, -120.25);

  std::string field_names = "z T P (atm) dens";
  for (std::string_view sp : kMainSpecies) {
    absl::StrAppend(&field_names, " ", sp, " ", sp, "_err");
  }
  ASSERT_TRUE(
      prod.AddField(prod.root(), "field_names", prod.MakeString(field_names)));

  // Row 0: normal values; Row 1: -999 (no data -> NaN) and -888 (estimate).
  std::string row0 = "10.5 250.0 0.5 1.2e19";
  std::string row1 = "-999 220.0 -888 9.0e18";
  for (size_t i = 0; i < std::size(kMainSpecies); ++i) {
    const double vmr = 1.0e-6 * static_cast<double>(i + 1);
    const double err = 1.0e-7 * static_cast<double>(i + 1);
    absl::StrAppend(&row0, " ", vmr, " ", err);
    absl::StrAppend(&row1, " -999 -888");
  }
  AddFieldDataLines(prod, {row0, row1});

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(45.5, 45.5));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-120.25, -120.25));

  EXPECT_EQ(2u, res.variables["altitude"].double_values.size());
  EXPECT_DOUBLE_EQ(10.5, res.variables["altitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["altitude"].double_values[1]));

  EXPECT_THAT(res.variables["temperature"].double_values,
              ElementsAre(250.0, 220.0));
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(DoubleNear(0.5 * 1013.25, 1e-9), -888.0));
  EXPECT_THAT(res.variables["number_density"].double_values,
              ElementsAre(1.2e19, 9.0e18));
  EXPECT_EQ(1u, res.variables["datetime"].double_values.size());
  EXPECT_EQ(1u, res.variables["datetime_start"].double_values.size());
  EXPECT_EQ(1u, res.variables["datetime_stop"].double_values.size());
  EXPECT_LT(res.variables["datetime_start"].double_values[0],
            res.variables["datetime"].double_values[0]);
  EXPECT_LT(res.variables["datetime"].double_values[0],
            res.variables["datetime_stop"].double_values[0]);

  for (size_t i = 0; i < std::size(kMainSpecies); ++i) {
    const std::string vmr_name =
        absl::StrCat(kMainSpecies[i], "_volume_mixing_ratio");
    const std::string err_name =
        absl::StrCat(kMainSpecies[i], "_volume_mixing_ratio_uncertainty");
    ASSERT_EQ(2u, res.variables[vmr_name].double_values.size());
    EXPECT_NEAR(static_cast<double>(i + 1),
                res.variables[vmr_name].double_values[0], 1e-9);
    EXPECT_TRUE(std::isnan(res.variables[vmr_name].double_values[1]));

    ASSERT_EQ(2u, res.variables[err_name].double_values.size());
    EXPECT_NEAR(0.1 * static_cast<double>(i + 1),
                res.variables[err_name].double_values[0], 1e-9);
    EXPECT_DOUBLE_EQ(-888.0, res.variables[err_name].double_values[1]);
  }
}

TEST(HarpIngestAceFtsL2Test, IngestIsoAllSpeciesAndFillScalarLatLon) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ACE_FTS_L2_iso");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod(coda_format_xml, "sr00099.asc");
  ASSERT_TRUE(prod.ok());
  // Use -999 for latitude and longitude to cover scalar fill value -> NaN.
  AddHeaderFields(prod, "2020-05-15 12:34:56.123456+00:00",
                  "2020-05-15 12:30:00+00:00", "2020-05-15 12:40:00+00:00",
                  -999.0, -999.0);

  std::string field_names = "z T P (atm) dens";
  for (std::string_view col : kIsoSpeciesHeader) {
    absl::StrAppend(&field_names, " ", col);
  }
  ASSERT_TRUE(
      prod.AddField(prod.root(), "field_names", prod.MakeString(field_names)));

  std::string row0 = "15.0 210.0 0.25 5.0e18";
  for (size_t i = 0; i < std::size(kIsoHarpSpecies); ++i) {
    absl::StrAppend(&row0, " ", 2.0e-6 * static_cast<double>(i + 1), " ",
                    2.0e-7 * static_cast<double>(i + 1));
  }
  AddFieldDataLines(prod, {row0});

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(99));
  EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[0]));
  EXPECT_TRUE(std::isnan(res.variables["longitude"].double_values[0]));

  for (size_t i = 0; i < std::size(kIsoHarpSpecies); ++i) {
    const std::string vmr_name =
        absl::StrCat(kIsoHarpSpecies[i], "_volume_mixing_ratio");
    const std::string err_name =
        absl::StrCat(kIsoHarpSpecies[i], "_volume_mixing_ratio_uncertainty");
    EXPECT_NEAR(2.0 * static_cast<double>(i + 1),
                res.variables[vmr_name].double_values[0], 1e-9);
    EXPECT_NEAR(0.2 * static_cast<double>(i + 1),
                res.variables[err_name].double_values[0], 1e-9);
  }
}

TEST(HarpIngestAceFtsL2Test, MissingDataSectionsYieldZeroAltitudes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ACE_FTS_L2_main");
  ASSERT_NE(nullptr, mod);

  // 1. Missing field_names.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    AddHeaderFields(prod, "2020-05-15 12:34:56.50+00", "2020-05-15 12:30:00+00",
                    "2020-05-15 12:40:00+00", 10.0, 20.0);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }

  // 2. Missing field_data.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    AddHeaderFields(prod, "2020-05-15 12:34:56.50+00", "2020-05-15 12:30:00+00",
                    "2020-05-15 12:40:00+00", 10.0, 20.0);
    ASSERT_TRUE(prod.AddField(prod.root(), "field_names",
                              prod.MakeString("z T P (atm) dens")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }

  // 3. field_data is scalar (not an array) -> get_array_dim fails -> 0.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    AddHeaderFields(prod, "2020-05-15 12:34:56.50+00", "2020-05-15 12:30:00+00",
                    "2020-05-15 12:40:00+00", 10.0, 20.0);
    ASSERT_TRUE(prod.AddField(prod.root(), "field_names",
                              prod.MakeString("z T P (atm) dens")));
    ASSERT_TRUE(prod.AddField(prod.root(), "field_data",
                              prod.MakeString("not_an_array")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }
}

TEST(HarpIngestAceFtsL2Test, ErrorPathsInitAndVariableReads) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("ACE_FTS_L2_main");
  ASSERT_NE(nullptr, mod);

  // 1. Wrong type for field_names -> read_string fails in get_dimensions.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "field_names", prod.MakeInt32(42)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Empty field_data array -> goto_first_array_element fails.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "field_names", prod.MakeString("z T")));
    CodaMemNode* tmpl = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(tmpl, "line", prod.MakeString("1 2")));
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(prod.AddField(prod.root(), "field_data",
                              prod.MakeNodeArray(kZeroDims, {}, tmpl)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. field_data element is an empty record -> goto_record_field_by_index(0).
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "field_names", prod.MakeString("z T")));
    CodaMemNode* empty_rec = prod.MakeRecord();
    constexpr long kOneDim[] = {1};
    CodaMemNode* elems[] = {empty_rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "field_data",
                              prod.MakeNodeArray(kOneDim, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. field_data[0] field 0 is int32 -> get_string_length fails.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "field_names", prod.MakeString("z T")));
    CodaMemNode* rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(rec, "line", prod.MakeInt32(1)));
    constexpr long kOneDim[] = {1};
    CodaMemNode* elems[] = {rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "field_data",
                              prod.MakeNodeArray(kOneDim, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Line too long (>= 2047) -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "field_names", prod.MakeString("z T")));
    AddFieldDataLines(prod, {std::string(2050, '1')});
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Missing header fields, invalid date string, wrong scalar types, and
  // missing data field ("H2O" absent from field_names).
  {
    CodaMemProduct prod(coda_format_xml, "ss100.asc");
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddField(prod.root(), "date", prod.MakeString("bad-date")));
    ASSERT_TRUE(prod.AddField(prod.root(), "start_time", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "latitude", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "field_names", prod.MakeString("z T")));
    AddFieldDataLines(prod, {"10.0 250.0"});

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(HARP_SUCCESS, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["H2O_volume_mixing_ratio"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
