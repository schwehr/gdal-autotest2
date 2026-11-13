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
// Unit tests for NPP Suomi OMPS Total Column L2 ingestion
// (libharp/harp-ingest-npp-suomi-omps_totals_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-npp-suomi-omps_totals_l2.c

#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr std::string_view kModuleName = "NPP_SUOMI_L2_OMPS_EDR_OOTC";
constexpr long kMaxCrosstracks = 105;
constexpr long kMaxAlongtracksPerGranule = 15;
constexpr double kMicrosecondsPerSecond = 1000000.0;
constexpr double kSecondsFrom1958To2000 = 1325376000.0;

void PopulateOmpsTotalsProduct(CodaMemProduct& prod, long num_granules,
                               long start_row, long num_valid_rows,
                               long num_valid_cols) {
  CodaMemNode* all_data = prod.MakeRecord();
  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* edr = prod.MakeRecord();

  const long total_rows = num_granules * kMaxAlongtracksPerGranule;
  const long mid_dims[] = {total_rows};
  const long geo2d_dims[] = {total_rows, kMaxCrosstracks};
  const long geo3d_dims[] = {total_rows, kMaxCrosstracks, 4};

  const double base_us =
      (kSecondsFrom1958To2000 + 300.0) * kMicrosecondsPerSecond;
  std::vector<double> mid_times(static_cast<size_t>(total_rows), -995.0);
  if (total_rows > 0) {
    mid_times[0] = base_us;
  }
  ASSERT_TRUE(
      prod.AddField(geo, "MidTime", prod.MakeDoubleArray(mid_dims, mid_times)));

  auto make_2d_geo = [&](double base_val) {
    std::vector<double> v(static_cast<size_t>(total_rows * kMaxCrosstracks),
                          -999.5);
    for (long r = start_row; r < start_row + num_valid_rows; ++r) {
      for (long c = 0; c < num_valid_cols; ++c) {
        v[static_cast<size_t>(r * kMaxCrosstracks + c)] =
            base_val + static_cast<double>(r * 10 + c);
      }
    }
    return v;
  };

  auto make_3d_geo = [&](double base_val) {
    std::vector<double> v(static_cast<size_t>(total_rows * kMaxCrosstracks * 4),
                          -999.5);
    for (long r = start_row; r < start_row + num_valid_rows; ++r) {
      for (long c = 0; c < num_valid_cols; ++c) {
        for (long k = 0; k < 4; ++k) {
          v[static_cast<size_t>((r * kMaxCrosstracks + c) * 4 + k)] =
              base_val + static_cast<double>(r * 100 + c * 10 + k);
        }
      }
    }
    return v;
  };

  ASSERT_TRUE(prod.AddField(
      geo, "Latitude", prod.MakeDoubleArray(geo2d_dims, make_2d_geo(10.0))));
  ASSERT_TRUE(prod.AddField(
      geo, "Longitude", prod.MakeDoubleArray(geo2d_dims, make_2d_geo(100.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "LatitudeCorners",
                    prod.MakeDoubleArray(geo3d_dims, make_3d_geo(1.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "LongitudeCorners",
                    prod.MakeDoubleArray(geo3d_dims, make_3d_geo(50.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "SatelliteAzimuthAngle",
                    prod.MakeDoubleArray(geo2d_dims, make_2d_geo(20.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "SatelliteZenithAngle",
                    prod.MakeDoubleArray(geo2d_dims, make_2d_geo(30.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "SolarAzimuthAngle",
                    prod.MakeDoubleArray(geo2d_dims, make_2d_geo(40.0))));
  ASSERT_TRUE(
      prod.AddField(geo, "SolarZenithAngle",
                    prod.MakeDoubleArray(geo2d_dims, make_2d_geo(50.0))));

  const long o3_dims[] = {num_granules * num_valid_rows, num_valid_cols};
  std::vector<double> o3(
      static_cast<size_t>(num_granules * num_valid_rows * num_valid_cols),
      300.0);
  if (!o3.empty()) {
    o3[0] = -999.5;  // Mapped to NaN.
    for (size_t i = 1; i < o3.size(); ++i) {
      o3[i] = 300.0 + static_cast<double>(i);
    }
  }
  ASSERT_TRUE(
      prod.AddField(edr, "ColumnAmountO3", prod.MakeDoubleArray(o3_dims, o3)));

  ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
  ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", edr));
  ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
}

TEST(HarpIngestNppSuomiOmpsTotalsL2Test, IngestOotcSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateOmpsTotalsProduct(prod, /*num_granules=*/1, /*start_row=*/0,
                            /*num_valid_rows=*/2, /*num_valid_cols=*/2);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4, res.dimensions[harp_dimension_time]);

  ASSERT_EQ(4u, res.variables["datetime"].double_values.size());
  EXPECT_THAT(res.variables["datetime"].double_values[0],
              DoubleNear(300.0, 1e-6));
  EXPECT_THAT(res.variables["datetime"].double_values[1],
              DoubleNear(300.0, 1e-6));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[2]));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[3]));

  EXPECT_THAT(res.variables["latitude"].double_values,
              ElementsAre(10.0, 11.0, 20.0, 21.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(100.0, 101.0, 110.0, 111.0));
  EXPECT_THAT(res.variables["latitude_bounds"].double_values,
              ElementsAre(1.0, 2.0, 3.0, 4.0, 11.0, 12.0, 13.0, 14.0, 101.0,
                          102.0, 103.0, 104.0, 111.0, 112.0, 113.0, 114.0));
  EXPECT_THAT(res.variables["longitude_bounds"].double_values,
              ElementsAre(50.0, 51.0, 52.0, 53.0, 60.0, 61.0, 62.0, 63.0, 150.0,
                          151.0, 152.0, 153.0, 160.0, 161.0, 162.0, 163.0));
  EXPECT_THAT(res.variables["sensor_azimuth_angle"].double_values,
              ElementsAre(20.0, 21.0, 30.0, 31.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(30.0, 31.0, 40.0, 41.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(40.0, 41.0, 50.0, 51.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(50.0, 51.0, 60.0, 61.0));

  ASSERT_EQ(4u, res.variables["O3_column_number_density"].double_values.size());
  EXPECT_TRUE(
      std::isnan(res.variables["O3_column_number_density"].double_values[0]));
  EXPECT_DOUBLE_EQ(301.0,
                   res.variables["O3_column_number_density"].double_values[1]);
  EXPECT_DOUBLE_EQ(302.0,
                   res.variables["O3_column_number_density"].double_values[2]);
  EXPECT_DOUBLE_EQ(303.0,
                   res.variables["O3_column_number_density"].double_values[3]);
}

TEST(HarpIngestNppSuomiOmpsTotalsL2Test, LeadingFillRowsAndFullGranule) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  // Row 0 is fill (-999.5), rows 1..14 are valid, and all 105 cols are valid
  // (covers start_row > 0, end_row == dimensions[0], and col == dimensions[1]).
  PopulateOmpsTotalsProduct(prod, /*num_granules=*/1, /*start_row=*/1,
                            /*num_valid_rows=*/14,
                            /*num_valid_cols=*/kMaxCrosstracks);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(14 * kMaxCrosstracks, res.dimensions[harp_dimension_time]);
}

TEST(HarpIngestNppSuomiOmpsTotalsL2Test, InitAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Missing All_Data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing Latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Scalar Latitude (coda_cursor_get_array_dim fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "Latitude", prod.MakeDouble(10.0)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. 1D Latitude (num_dimensions != 2).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {2};
    const double vals[] = {10.0, 20.0};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. Non-numeric 2D Latitude (coda_cursor_read_double_array fails in
  // get_dimensions).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {1, 2};
    const std::string_view vals[] = {"a", "b"};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeStringArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. Latitude contains only fill values (-999.5) -> no data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    const std::vector<double> fill(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, fill)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 7. Variable-level read_variable errors:
  // Missing MidTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    std::vector<double> lats(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    lats[0] = 10.0;
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // Wrong dimension count on Longitude (1D instead of 2D), wrong dim0 on
  // SatelliteAzimuthAngle, missing LatitudeCorners, and string
  // SolarZenithAngle.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long mid_dims[] = {kMaxAlongtracksPerGranule};
    const std::vector<double> mid_vals(kMaxAlongtracksPerGranule, 1.0);
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    std::vector<double> lats(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    lats[0] = 10.0;
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "Longitude",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("longitude", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateOmpsTotalsProduct(prod, /*num_granules=*/1, /*start_row=*/0,
                              /*num_valid_rows=*/1, /*num_valid_cols=*/1);
    // Test missing LatitudeCorners by building a product without it.
    CodaMemProduct prod2;
    ASSERT_TRUE(prod2.ok());
    CodaMemNode* all_data = prod2.MakeRecord();
    CodaMemNode* geo = prod2.MakeRecord();
    const long mid_dims[] = {kMaxAlongtracksPerGranule};
    const std::vector<double> mid_vals(kMaxAlongtracksPerGranule, 1.0);
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    std::vector<double> lats(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    lats[0] = 10.0;
    ASSERT_TRUE(prod2.AddField(geo, "MidTime",
                               prod2.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod2.AddField(geo, "Latitude", prod2.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(
        prod2.AddField(geo, "Longitude", prod2.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod2.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(
        prod2.AddField(all_data, "OMPS_TC_EDR_All", prod2.MakeRecord()));
    ASSERT_TRUE(prod2.AddField(prod2.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod2.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("latitude_bounds", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long mid_dims[] = {kMaxAlongtracksPerGranule};
    const std::vector<double> mid_vals(kMaxAlongtracksPerGranule, 1.0);
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    const long dims3d[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks, 4};
    const long bad_saz_dims[] = {1, kMaxCrosstracks};
    std::vector<double> lats(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    lats[0] = 10.0;
    const std::vector<double> corners(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks * 4),
        1.0);
    const std::vector<double> bad_saz(kMaxCrosstracks, 1.0);
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "LatitudeCorners",
                              prod.MakeDoubleArray(dims3d, corners)));
    ASSERT_TRUE(prod.AddField(geo, "LongitudeCorners",
                              prod.MakeDoubleArray(dims3d, corners)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(bad_saz_dims, bad_saz)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("sensor_azimuth_angle", res.failed_variable);
  }
  {
    // Bad ColumnAmountO3 (wrong type -> coda_cursor_read_double_array fails).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long mid_dims[] = {kMaxAlongtracksPerGranule};
    const std::vector<double> mid_vals(kMaxAlongtracksPerGranule, 1.0);
    const long dims[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks};
    const long dims3d[] = {kMaxAlongtracksPerGranule, kMaxCrosstracks, 4};
    std::vector<double> lats(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks),
        -999.5);
    lats[0] = 10.0;
    const std::vector<double> corners(
        static_cast<size_t>(kMaxAlongtracksPerGranule * kMaxCrosstracks * 4),
        1.0);
    const long o3_dims[] = {1, 1};
    const std::string_view bad_o3[] = {"bad"};
    ASSERT_TRUE(prod.AddField(geo, "MidTime",
                              prod.MakeDoubleArray(mid_dims, mid_vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "LatitudeCorners",
                              prod.MakeDoubleArray(dims3d, corners)));
    ASSERT_TRUE(prod.AddField(geo, "LongitudeCorners",
                              prod.MakeDoubleArray(dims3d, corners)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                              prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                              prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                              prod.MakeDoubleArray(dims, lats)));
    ASSERT_TRUE(prod.AddField(edr, "ColumnAmountO3",
                              prod.MakeStringArray(o3_dims, bad_o3)));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "OMPS_TC_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("O3_column_number_density", res.failed_variable);
  }
}

}  // namespace
}  // namespace stcorp_harp
