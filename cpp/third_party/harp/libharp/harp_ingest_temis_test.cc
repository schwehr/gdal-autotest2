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
// Unit tests for TEMIS ingestion modules (libharp/harp-ingest-temis.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-temis.c

#include <cstdint>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

void PopulateTemisProduct(CodaMemProduct& prod, bool is_mean,
                          bool six_element_date, long num_lat, long num_lon) {
  constexpr long kDate2Dims[] = {2};
  constexpr int32_t kDate2[] = {2020, 5};
  constexpr long kDate6Dims[] = {6};
  constexpr int32_t kDate6[] = {2020, 5, 15, 12, 0, 0};
  if (six_element_date) {
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDate6Dims, kDate6)));
  } else {
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDate2Dims, kDate2)));
  }

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                prod.MakeInt32(static_cast<int32_t>(num_lon))));
  constexpr long kRangeDims[] = {2};
  constexpr double kLonRange[] = {-180.0, 180.0};
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                prod.MakeDoubleArray(kRangeDims, kLonRange)));

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                prod.MakeInt32(static_cast<int32_t>(num_lat))));
  constexpr double kLatRange[] = {-90.0, 90.0};
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "Latitude_range",
                                prod.MakeDoubleArray(kRangeDims, kLatRange)));

  const long grid_dims[] = {num_lat, num_lon};
  const double o3_vals[] = {300.0, 310.0, 320.0, 330.0};
  const double std_vals[] = {5.0, 6.0, 7.0, 8.0};
  ASSERT_TRUE(prod.AddField(prod.root(),
                            is_mean ? "Average_O3_column" : "O3_column",
                            prod.MakeDoubleArray(grid_dims, o3_vals)));
  ASSERT_TRUE(prod.AddField(prod.root(), is_mean ? "Average_O3_std" : "O3_std",
                            prod.MakeDoubleArray(grid_dims, std_vals)));
}

TEST(HarpIngestTemisTest, IngestO3FieldSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("TEMIS_o3field");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateTemisProduct(prod, /*is_mean=*/false, /*six_element_date=*/false, 2,
                       2);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_latitude]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_longitude]);
  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(-90.0, 90.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(-180.0, 180.0));
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(300.0, 310.0, 320.0, 330.0));
  EXPECT_THAT(
      res.variables["O3_column_number_density_uncertainty"].double_values,
      ElementsAre(5.0, 6.0, 7.0, 8.0));
}

TEST(HarpIngestTemisTest, IngestO3MeanSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("TEMIS_o3mean");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateTemisProduct(prod, /*is_mean=*/true, /*six_element_date=*/true, 2, 2);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_THAT(res.variables["O3_column_number_density"].double_values,
              ElementsAre(300.0, 310.0, 320.0, 330.0));
}

TEST(HarpIngestTemisTest, ErrorPathsMissingAndInvalidFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("TEMIS_o3field");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @Ozone_field_date.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 2. Invalid @Ozone_field_date length (1 instead of 2 or 6).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {1};
    constexpr int32_t kDate[] = {2020};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Invalid date parts for coda_time_parts_to_double.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {6};
    constexpr int32_t kBadDate[] = {2020, 99, 1, 0, 0, 0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kBadDate)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 4. Missing Number_of_longitudes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 5. Invalid Longitude_range element count.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDateDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDateDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    constexpr long kBadDims[] = {1};
    constexpr double kBadRange[] = {0.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kBadDims, kBadRange)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Missing Number_of_latitudes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-180.0, 180.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 7. Invalid Latitude_range element count.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-180.0, 180.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeInt32(2)));
    constexpr long kBadDims[] = {1};
    constexpr double kBadRange[] = {0.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Latitude_range",
                                  prod.MakeDoubleArray(kBadDims, kBadRange)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 8. Missing O3_column and mismatched O3_std size.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-90.0, 90.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Latitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    constexpr long kBadGrid[] = {1};
    constexpr double kBadVals[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "O3_std",
                              prod.MakeDoubleArray(kBadGrid, kBadVals)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(
        -1, res.variables["O3_column_number_density_uncertainty"].read_status);
  }

  // 9. Non-integer @Ozone_field_date array triggers read_int32_array failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr std::string_view kBadDate[] = {"2020", "05"};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeStringArray(kDims, kBadDate)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 10. Wrong type for @Number_of_longitudes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeString("bad")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 11. Missing @Longitude_range.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 12. Wrong type for @Longitude_range (string array of size 2).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr std::string_view kBadStrs[] = {"-180", "180"};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeStringArray(kDims, kBadStrs)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 13. Wrong type for @Number_of_latitudes.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-180.0, 180.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeString("bad")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 14. Missing @Latitude_range.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-180.0, 180.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeInt32(2)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 15. Wrong type for @Latitude_range (string array of size 2) + O3_column
  // wrong element type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-180.0, 180.0};
    constexpr std::string_view kBadStrs[] = {"-90", "90"};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Latitude_range",
                                  prod.MakeStringArray(kDims, kBadStrs)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDims[] = {2};
    constexpr int32_t kDate[] = {2020, 5};
    constexpr double kRange[] = {-90.0, 90.0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Ozone_field_date",
                                  prod.MakeInt32Array(kDims, kDate)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_longitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Longitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Number_of_latitudes",
                                  prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "Latitude_range",
                                  prod.MakeDoubleArray(kDims, kRange)));
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadGrid[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(prod.root(), "O3_column",
                              prod.MakeStringArray(kGridDims, kBadGrid)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
